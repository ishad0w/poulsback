// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* xf86-video-poulsback: the Xorg driver for poulsback (GMA500/SGX535).
 *
 * Outputs, modes, the hardware cursor and gamma through xf86Crtc/RandR 1.2
 * (poulsback_display.c); the screen is a SCANOUT BO of the panel's size; EXA
 * in driver-pixmap mode over fb (poulsback_exa.c); DRI3 for GL clients (they
 * get the render node; their dma-buf back buffers become BO pixmaps with a
 * CPU mapping) and Present on the kernel's vblanks, flipping fullscreen
 * SCANOUT windows and copying the others (poulsback_present.c). A CPU access
 * to a client's buffer first waits for the GPU jobs that write it (EXA's
 * PrepareAccess: the fences on its dma-buf, DMA_BUF_IOCTL_SYNC).
 *
 * Config: Driver "poulsback", Option "kmsdev" (default /dev/dri/card0).
 */
#include "poulsback_drv.h"
#include "../../include/uapi/poulsback/poulsback_bo_uapi.h"
#include "xf86_OSproc.h"
#include "xf86Crtc.h"
#include "xf86Modes.h"
#include "dri3.h"
#include "exa.h"
#include "fb.h"
#include "micmap.h"
#include "mipointer.h"
#include "misyncshm.h"
#include "present.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define POULSBACK_DRIVER_NAME "poulsback"
#define POULSBACK_VERSION 1

/* X's file holds one GEM handle per BO: how many DRI3 pixmaps share it. */
struct handle_ref {
    struct xorg_list link;
    uint32_t handle, refs;
};

enum { OPTION_KMSDEV };
static const OptionInfoRec options[] = {
    {OPTION_KMSDEV, "kmsdev", OPTV_STRING, {0}, FALSE},
    {-1, NULL, OPTV_NONE, {0}, FALSE},
};

static const char *kmsdev(GDevPtr device)
{
    const char *path = xf86FindOptionValue(device->options, "kmsdev");
    return path ? path : "/dev/dri/card0";
}

/* True when the node is a poulsback DRM device. */
static int open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    drmVersionPtr v;
    if (fd < 0)
        return -1;
    v = drmGetVersion(fd);
    if (!v || strcmp(v->name, POULSBACK_DRIVER_NAME)) {
        if (v)
            drmFreeVersion(v);
        close(fd);
        return -1;
    }
    drmFreeVersion(v);
    return fd;
}

static void Identify(int flags)
{
    (void)flags;
    xf86Msg(X_INFO, "%s: driver for poulsback (GMA500/SGX535) KMS devices\n", POULSBACK_NAME);
}

static const OptionInfoRec *AvailableOptions(int chipid, int busid)
{
    (void)chipid;
    (void)busid;
    return options;
}

static Bool DriverFunc(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data)
{
    (void)scrn;
    if (op == GET_REQUIRED_HW_INTERFACES) {
        *(xorgHWFlags *)data = HW_SKIP_CONSOLE;
        return TRUE;
    }
    return FALSE;
}

static Bool PreInit(ScrnInfoPtr scrn, int flags)
{
    EntityInfoPtr entity;
    struct poulsback *pb;
    rgb zeros = {0, 0, 0};
    Gamma no_gamma = {0.0, 0.0, 0.0};
    if (flags & PROBE_DETECT)
        return FALSE;
    entity = xf86GetEntityInfo(scrn->entityList[0]);
    pb = xnfcalloc(1, sizeof(*pb));
    scrn->driverPrivate = pb;
    pb->fd = open_device(kmsdev(entity->device));
    free(entity);
    if (pb->fd < 0) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "no poulsback device\n");
        return FALSE;
    }
    pb->render_node = drmGetRenderDeviceNameFromFd(pb->fd);
    scrn->monitor = scrn->confScreen->monitor; /* xf86SetGamma reads it */
    if (!xf86SetDepthBpp(scrn, 24, 0, 0, Support32bppFb) || scrn->depth != 24 ||
        scrn->bitsPerPixel != 32) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "depth 24 / 32 bpp only\n");
        return FALSE;
    }
    xf86PrintDepthBpp(scrn);
    if (!xf86SetWeight(scrn, zeros, zeros) || !xf86SetDefaultVisual(scrn, -1) ||
        !xf86SetGamma(scrn, no_gamma))
        return FALSE;
    if (!poulsback_display_pre_init(scrn, pb))
        return FALSE;
    scrn->currentMode = scrn->modes;
    scrn->displayWidth = scrn->virtualX; /* ScreenInit uses the front's pitch */
    scrn->progClock = TRUE;
    scrn->rgbBits = 8;
    xf86SetDpi(scrn, 0, 0);
    return xf86LoadSubModule(scrn, "fb") && xf86LoadSubModule(scrn, "exa");
}

static void gem_close(struct poulsback *pb, uint32_t handle)
{
    struct drm_gem_close close_bo = {.handle = handle};
    drmIoctl(pb->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
}

/* A new BO, mapped for the CPU; NULL (and no handle left) on failure. */
static void *bo_create_map(struct poulsback *pb, struct poulsback_bo_create *create)
{
    void *map;
    if (drmIoctl(pb->fd, DRM_IOCTL_PB_BO_CREATE, create))
        return NULL;
    map = mmap(NULL, (size_t)create->size, PROT_READ | PROT_WRITE, MAP_SHARED, pb->fd,
               (off_t)create->mmap_offset);
    if (map == MAP_FAILED) {
        gem_close(pb, create->handle);
        return NULL;
    }
    return map;
}

/* The screen is a SCANOUT BO: write-combined for the CPU like any scanout page
 * (gma500's dumb front was too), in the 2D class so the 2D core can reach it,
 * and shown through a typed framebuffer that Present can flip to and from. */
static Bool create_front(ScrnInfoPtr scrn, struct poulsback *pb)
{
    uint32_t pitch = ((uint32_t)scrn->virtualX * 4 + 63) & ~63u;
    struct poulsback_bo_create create = {.abi = POULSBACK_BO_ABI,
                                         .flags = POULSBACK_BO_SCANOUT | POULSBACK_BO_CPU_WC,
                                         .bo_class = POULSBACK_BO_CLASS_2D,
                                         .size = (uint64_t)pitch * (uint32_t)scrn->virtualY};
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    pb->front = bo_create_map(pb, &create);
    if (!pb->front)
        return FALSE;
    pb->handle = create.handle;
    pb->pitch = pitch;
    pb->front_size = (size_t)create.size;
    handles[0] = pb->handle;
    pitches[0] = pitch;
    if (drmModeAddFB2(pb->fd, (uint32_t)scrn->virtualX, (uint32_t)scrn->virtualY,
                      DRM_FORMAT_XRGB8888, handles, pitches, offsets, &pb->fb, 0))
        return FALSE;
    scrn->displayWidth = (int)(pitch / 4);
    return TRUE;
}

static void destroy_front(struct poulsback *pb)
{
    if (pb->fb)
        drmModeRmFB(pb->fd, pb->fb);
    if (pb->front)
        munmap(pb->front, pb->front_size);
    if (pb->handle)
        gem_close(pb, pb->handle);
    pb->fb = pb->handle = 0;
    pb->front = NULL;
}

static void release_handle(struct poulsback *pb, uint32_t handle)
{
    struct handle_ref *ref;
    xorg_list_for_each_entry(ref, &pb->handles, link)
    {
        if (ref->handle != handle)
            continue;
        if (!--ref->refs) {
            xorg_list_del(&ref->link);
            free(ref);
            gem_close(pb, handle);
        }
        return;
    }
}

/* The client's dma-buf as a GEM handle in X's file (only our own BOs import)
 * and that BO's flags and class. */
static Bool import_handle(struct poulsback *pb, int fd, uint32_t *handle, uint32_t *flags,
                          uint32_t *bo_class)
{
    struct poulsback_bo_info info = {.abi = POULSBACK_BO_ABI};
    struct handle_ref *ref;
    Bool found = FALSE;
    if (drmPrimeFDToHandle(pb->fd, fd, handle))
        return FALSE;
    xorg_list_for_each_entry(ref, &pb->handles, link)
    {
        if (ref->handle == *handle) {
            ref->refs++;
            found = TRUE;
            break;
        }
    }
    if (!found) {
        ref = calloc(1, sizeof(*ref));
        if (!ref) {
            gem_close(pb, *handle);
            return FALSE;
        }
        ref->handle = *handle;
        ref->refs = 1;
        xorg_list_add(&ref->link, &pb->handles);
    }
    info.handle = *handle;
    if (drmIoctl(pb->fd, DRM_IOCTL_PB_BO_INFO, &info)) {
        release_handle(pb, *handle);
        return FALSE;
    }
    *flags = info.flags;
    *bo_class = info.bo_class;
    return TRUE;
}

Bool poulsback_pixmap_create_bo(struct poulsback *pb, struct poulsback_pixmap *priv, size_t size)
{
    struct poulsback_bo_create create = {
        .abi = POULSBACK_BO_ABI, .bo_class = POULSBACK_BO_CLASS_2D, .size = size};
    struct handle_ref *ref = calloc(1, sizeof(*ref));
    void *map = ref ? bo_create_map(pb, &create) : NULL;
    if (!map) {
        free(ref);
        return FALSE;
    }
    ref->handle = create.handle;
    ref->refs = 1;
    xorg_list_add(&ref->link, &pb->handles);
    priv->map = map;
    priv->size = (size_t)create.size;
    priv->handle = create.handle;
    priv->flags = 0;
    priv->bo_class = POULSBACK_BO_CLASS_2D;
    return TRUE;
}

void poulsback_pixmap_release_bo(struct poulsback *pb, struct poulsback_pixmap *priv)
{
    if (priv->fb)
        drmModeRmFB(pb->fd, priv->fb);
    munmap(priv->map, priv->size);
    if (priv->fd >= 0)
        close(priv->fd);
    release_handle(pb, priv->handle);
    priv->map = NULL;
    priv->fd = -1;
    priv->fb = 0;
}

/* DRI3: clients render on the render node; their buffers come back as dma-bufs. */
static int dri3_open_client(ClientPtr client, ScreenPtr screen, RRProviderPtr provider, int *out)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    int fd;
    (void)client;
    (void)provider;
    fd = pb->render_node ? open(pb->render_node, O_RDWR | O_CLOEXEC) : -1;
    if (fd < 0)
        return BadAlloc;
    *out = fd;
    return Success;
}

static PixmapPtr dri3_pixmap_from_fds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                                      CARD16 width, CARD16 height, const CARD32 *strides,
                                      const CARD32 *offsets, CARD8 depth, CARD8 bpp,
                                      CARD64 modifier)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *priv;
    PixmapPtr pixmap;
    uint32_t handle, flags, bo_class;
    off_t size;
    void *map;
    if (num_fds != 1 || bpp != 32 || (depth != 24 && depth != 32) || offsets[0] ||
        (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR) ||
        strides[0] < (CARD32)width * 4u || !width || !height)
        return NULL;
    size = lseek(fds[0], 0, SEEK_END);
    if (size < (off_t)strides[0] * height || !import_handle(pb, fds[0], &handle, &flags, &bo_class))
        return NULL;
    map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    if (map == MAP_FAILED) {
        xf86Msg(X_WARNING, "%s: dri3 pixmap %ux%u: mmap: %s\n", POULSBACK_NAME, width, height,
                strerror(errno));
        release_handle(pb, handle);
        return NULL;
    }
    /* A header-only EXA pixmap that owns the BO from here: destroying it
     * releases the mapping and the handle. */
    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    priv = pixmap ? exaGetPixmapDriverPrivate(pixmap) : NULL;
    if (!priv) {
        if (pixmap)
            screen->DestroyPixmap(pixmap);
        munmap(map, (size_t)size);
        release_handle(pb, handle);
        return NULL;
    }
    priv->map = map;
    priv->size = (size_t)size;
    priv->handle = handle;
    priv->flags = flags;
    priv->bo_class = bo_class;
    priv->shared = TRUE;
    /* The server closes the request's fd; CPU waits need our own copy. */
    priv->fd = fcntl(fds[0], F_DUPFD_CLOEXEC, 0);
    if (priv->fd < 0)
        xf86Msg(X_WARNING, "%s: dri3 pixmap %ux%u: dup: %s\n", POULSBACK_NAME, width, height,
                strerror(errno));
    if (priv->fd < 0 ||
        !screen->ModifyPixmapHeader(pixmap, width, height, depth, bpp, (int)strides[0], NULL)) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }
    return pixmap;
}

static PixmapPtr dri3_pixmap_from_fd(ScreenPtr screen, int fd, CARD16 width, CARD16 height,
                                     CARD16 stride, CARD8 depth, CARD8 bpp)
{
    CARD32 strides[1] = {stride}, offsets[1] = {0};
    return dri3_pixmap_from_fds(screen, 1, &fd, width, height, strides, offsets, depth, bpp,
                                DRM_FORMAT_MOD_INVALID);
}

/* A BO pixmap gives back its dma-buf: a DRI3 pixmap the client's, the screen or
 * one of X's BO pixmaps a PRIME export of its BO, kept so that X's CPU access
 * to it waits for the client's jobs from then on; X's pixmaps in system memory
 * have none. */
static int dri3_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds, uint32_t *strides,
                                uint32_t *offsets, uint64_t *modifier)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    int fd = -1;
    if (!priv || !priv->map)
        return 0;
    if (priv->fd < 0 &&
        drmPrimeHandleToFD(pb->fd, priv->handle, DRM_CLOEXEC | DRM_RDWR, &priv->fd))
        priv->fd = -1;
    if (priv->fd >= 0)
        fd = fcntl(priv->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    priv->shared = TRUE;
    fds[0] = fd;
    strides[0] = (uint32_t)pixmap->devKind;
    offsets[0] = 0;
    *modifier = DRM_FORMAT_MOD_LINEAR;
    return 1;
}

static int dri3_fd_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, CARD16 *stride, CARD32 *size)
{
    int fds[1];
    uint32_t strides[1], offsets[1];
    uint64_t modifier;
    if (dri3_fds_from_pixmap(screen, pixmap, fds, strides, offsets, &modifier) != 1)
        return -1;
    if (strides[0] > 0xffff) {
        close(fds[0]);
        return -1;
    }
    *stride = (CARD16)strides[0];
    *size = strides[0] * pixmap->drawable.height;
    return fds[0];
}

/* The formats a client may share, each in the LINEAR layout only. */
static const CARD32 dri3_formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};

static int dri3_get_formats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    (void)screen;
    *formats = malloc(sizeof(dri3_formats));
    if (!*formats)
        return FALSE;
    memcpy(*formats, dri3_formats, sizeof(dri3_formats));
    *num_formats = ARRAY_SIZE(dri3_formats);
    return TRUE;
}

static int linear_only(uint32_t format, uint32_t *num, uint64_t **mods)
{
    *num = 0;
    *mods = NULL;
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888)
        return TRUE;
    *mods = malloc(sizeof(**mods));
    if (!*mods)
        return FALSE;
    **mods = DRM_FORMAT_MOD_LINEAR;
    *num = 1;
    return TRUE;
}

static int dri3_get_modifiers(ScreenPtr screen, uint32_t format, uint32_t *num, uint64_t **mods)
{
    (void)screen;
    return linear_only(format, num, mods);
}

/* A window's own list names LINEAR only where Present could flip it, as
 * modesetting does: otherwise the screen's list applies. */
static int dri3_get_drawable_modifiers(DrawablePtr draw, uint32_t format, uint32_t *num,
                                       uint64_t **mods)
{
    if (draw->type != DRAWABLE_WINDOW || !present_can_window_flip((WindowPtr)draw)) {
        *num = 0;
        *mods = NULL;
        return TRUE;
    }
    return linear_only(format, num, mods);
}

static const dri3_screen_info_rec dri3_info = {
    .version = 2,
    .pixmap_from_fd = dri3_pixmap_from_fd,
    .fd_from_pixmap = dri3_fd_from_pixmap,
    .open_client = dri3_open_client,
    .pixmap_from_fds = dri3_pixmap_from_fds,
    .fds_from_pixmap = dri3_fds_from_pixmap,
    .get_formats = dri3_get_formats,
    .get_modifiers = dri3_get_modifiers,
    .get_drawable_modifiers = dri3_get_drawable_modifiers,
};

uint32_t poulsback_pixmap_scanout(ScreenPtr screen, PixmapPtr pixmap)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    uint32_t handles[4] = {priv ? priv->handle : 0}, pitches[4] = {pb->pitch}, offsets[4] = {0};
    if (!priv || !priv->map || priv->front || !(priv->flags & POULSBACK_BO_SCANOUT) ||
        priv->fb_refused || pixmap->drawable.width != pb->width ||
        pixmap->drawable.height != pb->height || pixmap->devKind != (int)pb->pitch ||
        pixmap->drawable.bitsPerPixel != 32 || pixmap->drawable.depth != 24)
        return 0;
    if (!priv->fb && drmModeAddFB2(pb->fd, pb->width, pb->height, DRM_FORMAT_XRGB8888, handles,
                                   pitches, offsets, &priv->fb, 0)) {
        priv->fb = 0;
        priv->fb_refused = TRUE;
    }
    return priv->fb;
}

static Bool CloseScreen(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct poulsback *pb = pb_of(scrn);
    Bool ret;
    poulsback_exa_close_screen(screen, pb);
    poulsback_present_close_screen(pb);
    poulsback_display_close_screen(screen, pb);
    destroy_front(pb);
    drmDropMaster(pb->fd);
    scrn->vtSema = FALSE;
    screen->CloseScreen = pb->close_screen;
    ret = screen->CloseScreen(screen);
    poulsback_exa_free(pb);
    return ret;
}

static Bool ScreenInit(ScreenPtr screen, int argc, char **argv)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct poulsback *pb = pb_of(scrn);
    VisualPtr visual;
    (void)argc;
    (void)argv;
    if (drmSetMaster(pb->fd))
        xf86DrvMsg(scrn->scrnIndex, X_WARNING, "drmSetMaster: %s\n", strerror(errno));
    xorg_list_init(&pb->handles);
    if (!create_front(scrn, pb)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "scanout buffer failed: %s\n", strerror(errno));
        return FALSE;
    }
    miClearVisualTypes();
    if (!miSetVisualTypes(scrn->depth, miGetDefaultVisualMask(scrn->depth), scrn->rgbBits,
                          scrn->defaultVisual) ||
        !miSetPixmapDepths() ||
        !fbScreenInit(screen, pb->front, scrn->virtualX, scrn->virtualY, scrn->xDpi, scrn->yDpi,
                      scrn->displayWidth, scrn->bitsPerPixel))
        return FALSE;
    for (visual = screen->visuals + screen->numVisuals; --visual >= screen->visuals;)
        if ((visual->class | DynamicClass) == DirectColor) {
            visual->offsetRed = scrn->offset.red;
            visual->offsetGreen = scrn->offset.green;
            visual->offsetBlue = scrn->offset.blue;
            visual->redMask = scrn->mask.red;
            visual->greenMask = scrn->mask.green;
            visual->blueMask = scrn->mask.blue;
        }
    if (!fbPictureInit(screen, NULL, 0) || !poulsback_exa_init(screen, pb))
        return FALSE;
    xf86SetBlackWhitePixels(screen);
    xf86SetBackingStore(screen);
    xf86SetSilkenMouse(screen);
    miDCInitialize(screen, xf86GetPointerScreenFuncs());
    if (!miCreateDefColormap(screen) || !poulsback_display_screen_init(screen, pb))
        return FALSE;
    screen->SaveScreen = xf86SaveScreen;
    pb->close_screen = screen->CloseScreen;
    screen->CloseScreen = CloseScreen;
    /* xshmfence-backed sync fences, then DRI3 and Present. */
    if (!miSyncShmScreenInit(screen) || !dri3_screen_init(screen, &dri3_info) ||
        !poulsback_present_screen_init(screen, pb)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "DRI3/Present setup failed\n");
        return FALSE;
    }
    xf86DrvMsg(scrn->scrnIndex, X_INFO, "EXA, DRI3 on %s, Present with flips\n",
               pb->render_node ? pb->render_node : "(no render node)");
    scrn->vtSema = TRUE;
    if (!xf86SetDesiredModes(scrn)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "mode set failed\n");
        return FALSE;
    }
    return TRUE;
}

static Bool EnterVT(ScrnInfoPtr scrn)
{
    drmSetMaster(pb_of(scrn)->fd);
    scrn->vtSema = TRUE;
    return xf86SetDesiredModes(scrn);
}

static void LeaveVT(ScrnInfoPtr scrn)
{
    xf86_hide_cursors(scrn);
    scrn->vtSema = FALSE;
    drmDropMaster(pb_of(scrn)->fd);
}

static void FreeScreen(ScrnInfoPtr scrn)
{
    struct poulsback *pb = pb_of(scrn);
    if (!pb)
        return;
    if (pb->fd >= 0)
        close(pb->fd);
    free(pb->render_node);
    free(pb);
    scrn->driverPrivate = NULL;
}

static Bool Probe(DriverPtr drv, int flags)
{
    GDevPtr *sections;
    int n = xf86MatchDevice(POULSBACK_NAME, &sections);
    Bool found = FALSE;
    for (int i = 0; i < n; i++) {
        int fd = open_device(kmsdev(sections[i])), entity;
        ScrnInfoPtr scrn;
        if (fd < 0)
            continue;
        close(fd);
        found = TRUE;
        if (flags & PROBE_DETECT)
            continue;
        entity = xf86ClaimNoSlot(drv, 0, sections[i], TRUE);
        scrn = xf86ConfigFbEntity(NULL, 0, entity, NULL, NULL, NULL, NULL);
        if (!scrn)
            continue;
        scrn->driverVersion = POULSBACK_VERSION;
        scrn->driverName = (char *)POULSBACK_NAME;
        scrn->name = (char *)"POULSBACK";
        scrn->Probe = Probe;
        scrn->PreInit = PreInit;
        scrn->ScreenInit = ScreenInit;
        scrn->EnterVT = EnterVT;
        scrn->LeaveVT = LeaveVT;
        scrn->FreeScreen = FreeScreen;
    }
    free(sections);
    return found;
}

_X_EXPORT DriverRec POULSBACK = {
    .driverVersion = POULSBACK_VERSION,
    .driverName = POULSBACK_NAME,
    .Identify = Identify,
    .Probe = Probe,
    .AvailableOptions = AvailableOptions,
    .driverFunc = DriverFunc,
};

static void *Setup(void *module, void *opts, int *errmaj, int *errmin)
{
    static Bool done;
    (void)opts;
    (void)errmin;
    if (done) {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }
    done = TRUE;
    xf86AddDriver(&POULSBACK, module, HaveDriverFuncs);
    return (void *)1;
}

static XF86ModuleVersionInfo version = {
    POULSBACK_NAME, MODULEVENDORSTRING, MODINFOSTRING1, MODINFOSTRING2, XORG_VERSION_CURRENT,
    0, 1, 0, ABI_CLASS_VIDEODRV, ABI_VIDEODRV_VERSION, MOD_CLASS_VIDEODRV, {0, 0, 0, 0},
};

_X_EXPORT XF86ModuleData poulsbackModuleData = {&version, Setup, NULL};
