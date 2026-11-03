// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Present on the kernel's vblanks, with page flips of fullscreen windows.
 *
 * Each queued Present event is one DRM vblank event, read from the DRM fd. Its
 * record is found by a serial carried as the request's user data, so an event
 * whose record was aborted is dropped, and one left from a closed screen finds
 * no record. The MSC is the kernel's 32-bit vblank sequence extended to 64 bits
 * per CRTC. Under the typed front a DPMS-off CRTC's pipe keeps running, so its
 * vblanks serve Present too; a request the kernel refuses makes Present fall
 * back to its own clock.
 *
 * A flip shows a client's SCANOUT buffer through its own KMS framebuffer; its
 * DRM event comes at the first vblank that proves the store latched,
 * and Present then notifies the client and releases the previous buffer. An
 * unflip flips back to the front; a refused flip leaves Present copying. */
#include "poulsback_drv.h"
#include "present.h"
#include "xf86Crtc.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>

struct vblank {
    struct xorg_list link;
    xf86CrtcPtr crtc;
    uint64_t event_id;
    uint32_t serial;
    Bool aborted;
};

static unsigned int crtc_bits(const struct poulsback_crtc *c)
{
    return c->index == 1 ? DRM_VBLANK_SECONDARY
                         : (c->index << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
}

/* The 64-bit MSC of a kernel sequence; an older sequence than the last one seen
 * (an event behind a query) does not advance the extension. */
static uint64_t msc_of(struct poulsback_crtc *c, uint32_t sequence)
{
    int32_t delta = (int32_t)(sequence - c->msc_last);
    uint64_t msc = c->msc_valid ? c->msc + (uint64_t)(int64_t)delta : sequence;
    if (!c->msc_valid || delta > 0) {
        c->msc = msc;
        c->msc_last = sequence;
        c->msc_valid = TRUE;
    }
    return msc;
}

/* A refused vblank request leaves Present on its own clock: said once. */
static void refused(xf86CrtcPtr crtc, const char *what, int err)
{
    static Bool reported;
    if (!reported) {
        reported = TRUE;
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING, "vblank %s on CRTC %u refused: %s\n", what,
                   ((struct poulsback_crtc *)crtc->driver_private)->id, strerror(err));
    }
}

static uint64_t ust_of(unsigned long sec, unsigned long usec)
{
    return (uint64_t)sec * 1000000u + usec;
}

/* The enabled CRTC that shows most of the window; none puts the window on
 * Present's own clock (docs/graphics.md). */
static RRCrtcPtr get_crtc(WindowPtr window)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(xf86ScreenToScrn(window->drawable.pScreen));
    RRCrtcPtr best = NULL;
    int64_t best_area = -1;
    int wx = window->drawable.x, wy = window->drawable.y;
    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        int64_t w, h;
        if (!crtc->enabled)
            continue;
        w = min(wx + window->drawable.width, crtc->x + crtc->mode.HDisplay) - max(wx, crtc->x);
        h = min(wy + window->drawable.height, crtc->y + crtc->mode.VDisplay) - max(wy, crtc->y);
        if (w < 0 || h < 0)
            w = h = 0;
        if (w * h > best_area) {
            best_area = w * h;
            best = crtc->randr_crtc;
        }
    }
    return best;
}

static int get_ust_msc(RRCrtcPtr rrcrtc, uint64_t *ust, uint64_t *msc)
{
    xf86CrtcPtr crtc = rrcrtc->devPrivate;
    struct poulsback_crtc *c = crtc->driver_private;
    drmVBlank v = {.request = {.type = DRM_VBLANK_RELATIVE | crtc_bits(c), .sequence = 0}};
    if (!crtc->enabled)
        return BadMatch;
    if (drmWaitVBlank(pb_of(crtc->scrn)->fd, &v)) {
        refused(crtc, "query", errno);
        return BadMatch;
    }
    *ust = ust_of(v.reply.tval_sec, v.reply.tval_usec);
    *msc = msc_of(c, v.reply.sequence);
    return Success;
}

/* The Present core reads this "Bool" as an X status, as modesetting returns it:
 * Success (0) means queued; a nonzero result requests the clock fallback. */
static Bool queue_vblank(RRCrtcPtr rrcrtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr crtc = rrcrtc->devPrivate;
    struct poulsback *pb = pb_of(crtc->scrn);
    struct vblank *v = calloc(1, sizeof(*v));
    drmVBlank request = {.request = {.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT |
                                             crtc_bits(crtc->driver_private),
                                     .sequence = (uint32_t)msc}};
    if (!v || !crtc->enabled)
        goto refused;
    v->crtc = crtc;
    v->event_id = event_id;
    v->serial = ++pb->vblank_serial ? pb->vblank_serial : ++pb->vblank_serial;
    request.request.signal = v->serial;
    if (drmWaitVBlank(pb->fd, &request)) {
        refused(crtc, "event", errno);
        goto refused;
    }
    xorg_list_add(&v->link, &pb->vblanks);
    return Success;
refused:
    free(v);
    return BadAlloc;
}

static void abort_vblank(RRCrtcPtr rrcrtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr crtc = rrcrtc->devPrivate;
    struct poulsback *pb = pb_of(crtc->scrn);
    struct vblank *v;
    (void)msc;
    xorg_list_for_each_entry(v, &pb->vblanks, link) if (v->event_id == event_id) v->aborted = TRUE;
}

/* After each Present copy, before its pixmap is idle to the client. */
static void flush(WindowPtr window)
{
    poulsback_exa_flush(pb_of(xf86ScreenToScrn(window->drawable.pScreen)));
}

/* A page flip with an event; one at a time (Present orders them, and the
 * kernel refuses a second with EBUSY). */
static Bool start_flip(struct poulsback *pb, xf86CrtcPtr crtc, uint32_t fb, uint64_t event_id)
{
    uint32_t serial = ++pb->vblank_serial ? pb->vblank_serial : ++pb->vblank_serial;
    if (pb->flip.pending || !fb ||
        drmModePageFlip(pb->fd, ((struct poulsback_crtc *)crtc->driver_private)->id, fb,
                        DRM_MODE_PAGE_FLIP_EVENT, (void *)(uintptr_t)serial)) {
        if (!pb->flips_refused++)
            xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING, "page flip refused: %s\n",
                       pb->flip.pending ? "one in flight" : strerror(errno));
        return FALSE;
    }
    pb->flip.crtc = crtc;
    pb->flip.event_id = event_id;
    pb->flip.serial = serial;
    pb->flip.pending = TRUE;
    return TRUE;
}

/* Present has checked that the window covers the screen with this pixmap; the
 * pixmap must be a SCANOUT buffer in the front's geometry and format. An async
 * flip (swap interval 0) is accepted too: the BASE store latches at a vblank
 * anyway, so it completes at the next one without a WC readback copy
 * (docs/graphics.md). */
static Bool check_flip(RRCrtcPtr rrcrtc, WindowPtr window, PixmapPtr pixmap, Bool sync_flip)
{
    xf86CrtcPtr crtc = rrcrtc ? rrcrtc->devPrivate : NULL;
    (void)window;
    (void)sync_flip;
    return crtc && crtc->scrn->vtSema && crtc->enabled && !crtc->x && !crtc->y &&
           crtc->mode.HDisplay == pixmap->drawable.width &&
           crtc->mode.VDisplay == pixmap->drawable.height &&
           poulsback_pixmap_scanout(pixmap->drawable.pScreen, pixmap);
}

static Bool flip(RRCrtcPtr rrcrtc, uint64_t event_id, uint64_t target_msc, PixmapPtr pixmap,
                 Bool sync_flip)
{
    xf86CrtcPtr crtc = rrcrtc->devPrivate;
    struct poulsback *pb = pb_of(crtc->scrn);
    (void)target_msc;
    (void)sync_flip;
    if (!start_flip(pb, crtc, poulsback_pixmap_scanout(crtc->scrn->pScreen, pixmap), event_id))
        return FALSE;
    pb->flips++;
    return TRUE;
}

/* Back to the front, which Present has refreshed from the flipped window. If
 * the front is shown already (a same-buffer flip is refused) or the flip is
 * refused, a mode set puts it on the CRTC and the unflip completes now. After
 * the GPU loss (2D off) a front the display refuses for any reason but EBUSY
 * is a display failure: X stops rather than serve a desktop nobody sees
 * (docs/graphics.md). */
static void unflip(ScreenPtr screen, uint64_t event_id)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    struct poulsback *pb = pb_of(scrn);
    xf86CrtcPtr crtc = NULL;
    uint64_t ust = 0, msc = 0;
    for (int i = 0; i < config->num_crtc && !crtc; i++)
        if (config->crtc[i]->enabled)
            crtc = config->crtc[i];
    pb->unflips++;
    if (crtc && scrn->vtSema && start_flip(pb, crtc, pb->fb, event_id))
        return;
    if (crtc && scrn->vtSema &&
        !crtc->funcs->set_mode_major(crtc, &crtc->mode, crtc->rotation, crtc->x, crtc->y) &&
        !pb->accel && pb->mode_error != EBUSY)
        FatalError("%s: the front cannot be shown after the GPU loss: %s\n", POULSBACK_NAME,
                   strerror(pb->mode_error));
    if (crtc)
        get_ust_msc(crtc->randr_crtc, &ust, &msc);
    present_event_notify(event_id, ust, msc);
}

static present_screen_info_rec present_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = get_crtc,
    .get_ust_msc = get_ust_msc,
    .queue_vblank = queue_vblank,
    .abort_vblank = abort_vblank,
    .flush = flush,
    .capabilities = PresentCapabilityAsync,
    .check_flip = check_flip,
    .flip = flip,
    .unflip = unflip,
};

/* drmHandleEvent's handlers take no context of ours: the screen being served. */
static struct poulsback *serving;

static void vblank_event(int fd, unsigned int sequence, unsigned int sec, unsigned int usec,
                         void *user_data)
{
    uint32_t serial = (uint32_t)(uintptr_t)user_data;
    struct vblank *v, *next;
    (void)fd;
    xorg_list_for_each_entry_safe(v, next, &serving->vblanks, link)
    {
        if (v->serial != serial)
            continue;
        xorg_list_del(&v->link);
        if (!v->aborted)
            present_event_notify(v->event_id, ust_of(sec, usec),
                                 msc_of(v->crtc->driver_private, sequence));
        free(v);
        return;
    }
}

static void flip_event(int fd, unsigned int sequence, unsigned int sec, unsigned int usec,
                       void *user_data)
{
    (void)fd;
    if (!serving->flip.pending || serving->flip.serial != (uint32_t)(uintptr_t)user_data)
        return;
    serving->flip.pending = FALSE;
    present_event_notify(serving->flip.event_id, ust_of(sec, usec),
                         msc_of(((xf86CrtcPtr)serving->flip.crtc)->driver_private, sequence));
}

static void drm_readable(int fd, int ready, void *data)
{
    drmEventContext context = {
        .version = 2, .vblank_handler = vblank_event, .page_flip_handler = flip_event};
    (void)ready;
    serving = data;
    drmHandleEvent(fd, &context);
    serving = NULL;
}

Bool poulsback_present_screen_init(ScreenPtr screen, struct poulsback *pb)
{
    xorg_list_init(&pb->vblanks);
    return SetNotifyFd(pb->fd, drm_readable, X_NOTIFY_READ, pb) &&
           present_screen_init(screen, &present_info);
}

void poulsback_present_close_screen(struct poulsback *pb)
{
    struct vblank *v, *next;
    xf86Msg(X_INFO, "%s: Present: %lu flips, %lu unflips, %lu flips refused\n", POULSBACK_NAME,
            pb->flips, pb->unflips, pb->flips_refused);
    RemoveNotifyFd(pb->fd);
    xorg_list_for_each_entry_safe(v, next, &pb->vblanks, link)
    {
        xorg_list_del(&v->link);
        free(v);
    }
}
