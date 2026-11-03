// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* EXA in driver-pixmap mode (EXA_HANDLES_PIXMAPS without mixed pixmaps).
 *
 * A pixmap is a BO in X's file: one of X's own large enough to pay for it (a
 * 2D-class WB BO), a DRI3 client buffer imported by PRIME, or the screen's
 * SCANOUT front; or X's own in system memory, which fb draws. EXA asks PixmapIsOffscreen whether a pixmap has a BO
 * and calls PrepareAccess before every CPU access to one, never for an
 * accelerated operation. PrepareAccess is X's only CPU wait for the GPU: an
 * imported buffer, or the front once a client holds it, waits for the fences
 * on its dma-buf every time (the client never waits before Present); X's own
 * BO waits only while a 2D job of X's names it. EXA's global marker is unused.
 *
 * Fills and copies on 2D-class BOs at 32 bpp, any GX function, are SUBMIT_2D
 * jobs on X's own context when the CPU would otherwise read write-combined
 * memory or wait for the GPU (docs/graphics.md): the CPU writes the
 * WC front at full speed but reads it slowly, and it fills and copies WB
 * memory faster than the 2D core. So Present's copy of a DRI3 back buffer into
 * the front, copies out of the front and destination-reading fills on it go
 * to the 2D core, ordered against clients' jobs by the BOs' dma_resv with no
 * CPU wait; a planemask fill is two fills (GXand, then GXxor). Operations
 * gather in one job (a large one runs alone) until a limit (16 BOs, 256
 * operations, 2^25 pixels), a CPU access to a BO the job names, a destroyed
 * pixmap it names, Present's flush, the screen's BlockHandler or FlushCallback
 * (before any reply or event reaches a client), so no client learns of an
 * operation before the job that reads its buffer is submitted.
 * GetImage of a WC pixmap goes through a WB staging pixmap. The kernel encodes
 * the operations; this driver names BOs by handle and surfaces by offset,
 * stride, size and format only. A job the kernel refuses (-EINVAL,
 * -EOPNOTSUPP) is drawn by fb. Any other error is the GPU's loss (the kernel
 * publishes -EIO, docs/kernel/execution.md): acceleration goes off
 * for the server's life, X repaints every window once and keeps drawing with
 * fb. Its BOs keep their CPU maps; a lost job's rectangles hold whatever the
 * GPU left until something redraws them. Everything else falls back to fb. */
#include "poulsback_drv.h"
#include "../../include/uapi/poulsback/poulsback_bo_uapi.h"
#include "../../include/uapi/poulsback/poulsback_exec_uapi.h"
#include "exa.h"
#include "fb.h"
#include "fbrop.h"
#include "gcstruct.h"
#include "servermd.h"
#include <linux/dma-buf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

/* Counters logged at exit: CPU accesses that could not wait for their fences,
 * waits and those that blocked (a SYNC), waits on X's own BOs; 2D jobs
 * and their operations; operations of refused jobs and after acceleration
 * went off, drawn by fb; GetImages through the staging pixmap (those served
 * from an earlier copy) and stippled fills written without reads. */
static unsigned long unsynced, waits, blocked, own_waits;
static unsigned long jobs, job_ops, refused_ops, unaccelerated_ops, downloads, staged_downloads,
    stipple_fills;

/* The 2D job being built: its BOs, one surface per pixmap (with the pixmap's
 * driver private, so that a job never looks EXA's private up again: X frees
 * screen-specific privates before CloseScreen), the
 * operations in order and the pixels they write. */
static struct {
    struct poulsback_submit_bo bos[POULSBACK_2D_MAX_BOS];
    struct poulsback_2d_surface surfaces[POULSBACK_2D_MAX_SURFACES];
    PixmapPtr pixmaps[POULSBACK_2D_MAX_SURFACES];
    struct poulsback_pixmap *privs[POULSBACK_2D_MAX_SURFACES];
    struct poulsback_2d_op ops[POULSBACK_2D_MAX_OPS];
    uint32_t bo_count, surface_count, op_count;
    uint64_t pixels;
} job;

/* The operation between Prepare and Done: its pixmaps (no source for a fill)
 * and their surfaces in the job, the GX function, the fill pixel, or for a
 * planemask fill the GXand and GXxor pixels. */
static struct {
    PixmapPtr src, dst;
    uint32_t src_surface, dst_surface;
    int alu;
    Bool masked;
    uint32_t pixel[2];
} op;

static void submit(struct poulsback *pb);

/* The screen, for the repaint after the GPU's loss. */
static ScrnInfoPtr exa_scrn;

/* X's own pixmaps of at least this many pixels get a BO (docs/graphics.md): a BO
 * costs a create, a map and a VA range. */
#define BO_PIXMAP_MIN_PIXELS 4096

static void *CreatePixmap2(ScreenPtr screen, int width, int height, int depth, int usage_hint,
                           int bpp, int *pitch)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *priv = calloc(1, sizeof(*priv));
    (void)usage_hint;
    if (!priv)
        return NULL;
    priv->fd = -1;
    *pitch = 0;
    /* A header-only pixmap (a DRI3 import, the screen, a scratch header) gets
     * its storage later. X's own that the 2D core can draw get a 2D-class WB
     * BO (rows in 128-byte steps up to 16,256 bytes); the rest, or any whose BO
     * fails, system memory padded as fb pads it. */
    if (pb->accel && bpp == 32 && (depth == 24 || depth == 32) &&
        width <= (int)POULSBACK_2D_MAX_WIDTH_32BPP && height > 0 &&
        height <= (int)POULSBACK_2D_MAX_DIM && (long)width * height >= BO_PIXMAP_MIN_PIXELS) {
        size_t stride = ((size_t)width * 4 + 127) & ~(size_t)127;
        if (poulsback_pixmap_create_bo(pb, priv, stride * (size_t)height)) {
            *pitch = (int)stride;
            return priv;
        }
    }
    if (width > 0 && height > 0 && bpp > 0) {
        size_t stride = (((size_t)width * (size_t)bpp + FB_MASK) >> FB_SHIFT) * sizeof(FbBits);
        priv->sys = malloc(stride * (size_t)height);
        if (!priv->sys) {
            free(priv);
            return NULL;
        }
        *pitch = (int)stride;
    }
    return priv;
}

static void DestroyPixmap(ScreenPtr screen, void *driver_priv)
{
    struct poulsback_pixmap *priv = driver_priv;
    if (priv->queued)
        submit(pb_of(xf86ScreenToScrn(screen)));
    if (priv->map && !priv->front)
        poulsback_pixmap_release_bo(pb_of(xf86ScreenToScrn(screen)), priv);
    else if (priv->fd >= 0)
        close(priv->fd);
    free(priv->sys);
    free(priv);
}

/* X's own pixmap keeps its system memory as the pixel pointer: EXA then
 * treats it as not offscreen and never calls PrepareAccess for it. A BO
 * pixmap's pointer is set by PrepareAccess only. */
static Bool ModifyPixmapHeader(PixmapPtr pixmap, int width, int height, int depth, int bpp,
                               int devKind, void *data)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    if (!data && priv && priv->sys)
        data = priv->sys;
    return miModifyPixmapHeader(pixmap, width, height, depth, bpp, devKind, data);
}

static Bool PixmapIsOffscreen(PixmapPtr pixmap)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    return priv && priv->map;
}

/* Waits for the GPU jobs on a BO's dma-buf: its writers before a read, all
 * of them before a write. poll is the idle test (POLLOUT: no job left, so the
 * BO is no longer pending; POLLIN: its writers are done); only a busy BO takes
 * the blocking SYNC START. There is no END: the BO's dma-buf has no CPU-access
 * hooks (kernel/gem/poulsback_bo.c). A refused wait is counted, never fatal
 * (EXA treats a failed PrepareAccess on a pinned pixmap as fatal). */
static void wait_gpu(struct poulsback_pixmap *priv, Bool write)
{
    struct dma_buf_sync sync = {DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ)};
    struct pollfd idle = {.fd = priv->fd, .events = POLLIN | POLLOUT};
    waits++;
    own_waits += !priv->shared;
    if (poll(&idle, 1, 0) == 1 && (idle.revents & POLLOUT)) {
        priv->pending = FALSE;
        return;
    }
    if (!write && (idle.revents & POLLIN))
        return;
    blocked++;
    if (!drmIoctl(priv->fd, DMA_BUF_IOCTL_SYNC, &sync)) {
        if (write)
            priv->pending = FALSE;
    } else if (!unsynced++) {
        xf86Msg(X_WARNING, "%s: CPU access without fence wait: %s\n", POULSBACK_NAME,
                strerror(errno));
    }
}

static Bool PrepareAccess(PixmapPtr pixmap, int index)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    if (!priv || !priv->map)
        return FALSE;
    if (priv->queued)
        submit(pb_of(xf86ScreenToScrn(pixmap->drawable.pScreen)));
    if (index == EXA_PREPARE_DEST || index == EXA_PREPARE_AUX_DEST)
        priv->generation++;
    pixmap->devPrivate.ptr = priv->map;
    if (priv->fd >= 0 && (priv->shared || priv->pending))
        wait_gpu(priv, index == EXA_PREPARE_DEST || index == EXA_PREPARE_AUX_DEST);
    return TRUE;
}

/* A GX function of a source (or fill) and a destination word. */
static uint32_t gx(int alu, uint32_t src, uint32_t dst)
{
    return (alu & 1 ? src & dst : 0) | (alu & 2 ? src & ~dst : 0) | (alu & 4 ? ~src & dst : 0) |
           (alu & 8 ? ~src & ~dst : 0);
}

static Bool reads_dst(int alu)
{
    return alu != GXclear && alu != GXcopy && alu != GXcopyInverted && alu != GXset;
}

static Bool write_combined(const struct poulsback_pixmap *priv)
{
    return (priv->flags & POULSBACK_BO_CPU_WC) != 0;
}

/* Whether a CPU access would wait for the GPU: a client's buffer (its jobs are
 * not X's to see), or one X's open or an unfinished job names. */
static Bool busy(struct poulsback_pixmap *priv)
{
    struct pollfd idle = {.fd = priv->fd, .events = POLLOUT};
    if (priv->pending && priv->fd >= 0 && poll(&idle, 1, 0) == 1)
        priv->pending = FALSE;
    return priv->shared || priv->queued || priv->pending;
}

/* A pixmap the 2D core can draw: a 2D-class BO at 32 bpp within its limits
 * (strides in 128-byte steps up to 16,256, 4,095 rows). */
static Bool twod(PixmapPtr pixmap)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    int stride = pixmap->devKind;
    return priv && priv->map && priv->bo_class == POULSBACK_BO_CLASS_2D &&
           pixmap->drawable.bitsPerPixel == 32 &&
           (pixmap->drawable.depth == 24 || pixmap->drawable.depth == 32) && stride > 0 &&
           stride % POULSBACK_2D_STRIDE_ALIGN == 0 && stride <= (int)POULSBACK_2D_MAX_STRIDE &&
           pixmap->drawable.width <= POULSBACK_2D_MAX_DIM &&
           pixmap->drawable.height <= POULSBACK_2D_MAX_DIM;
}

/* The job's surface for a pixmap, its BO listed once; -1 when the job is full. */
static int surface(PixmapPtr pixmap)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    uint32_t i, bo;
    for (i = 0; i < job.surface_count; i++)
        if (job.pixmaps[i] == pixmap)
            return (int)i;
    for (bo = 0; bo < job.bo_count && job.bos[bo].handle != priv->handle; bo++)
        ;
    if (i == POULSBACK_2D_MAX_SURFACES || bo == POULSBACK_2D_MAX_BOS)
        return -1;
    if (bo == job.bo_count)
        job.bos[job.bo_count++] = (struct poulsback_submit_bo){priv->handle, 0};
    priv->queued = TRUE;
    job.pixmaps[i] = pixmap;
    job.privs[i] = priv;
    job.surfaces[i] = (struct poulsback_2d_surface){
        .bo = bo,
        .stride = (uint32_t)pixmap->devKind,
        .width = pixmap->drawable.width,
        .height = pixmap->drawable.height,
        .format = pixmap->drawable.depth == 32 ? POULSBACK_2D_ARGB8888 : POULSBACK_2D_XRGB8888};
    job.surface_count++;
    return (int)i;
}

/* A BO X's 2D jobs name gets a dma-buf (once) for CPU waits, and is pending.
 * Without one, CPU accesses to it cannot wait: counted and warned once. */
static void mark_pending(struct poulsback *pb, struct poulsback_pixmap *priv)
{
    if (priv->fd < 0 &&
        drmPrimeHandleToFD(pb->fd, priv->handle, DRM_CLOEXEC | DRM_RDWR, &priv->fd)) {
        priv->fd = -1;
        if (!unsynced++)
            xf86Msg(X_WARNING, "%s: no dma-buf for a 2D job's BO, so no fence wait: %s\n",
                    POULSBACK_NAME, strerror(errno));
    }
    priv->pending = TRUE;
}

/* Row y of the job's surface i. */
static FbBits *row(uint32_t i, int y)
{
    return (FbBits *)(void *)((char *)job.privs[i]->map + (size_t)y * job.surfaces[i].stride);
}

/* The operations of a refused job, or of any job once acceleration is off,
 * drawn by fb after waiting as fb's own access would. A copy within one pixmap
 * runs in the direction that reads each pixel before it is written. */
static void draw_on_cpu(void)
{
    for (uint32_t i = 0; i < job.surface_count; i++) {
        struct poulsback_pixmap *priv = job.privs[i];
        if (priv->fd >= 0 && (priv->shared || priv->pending))
            wait_gpu(priv, TRUE);
    }
    for (uint32_t i = 0; i < job.op_count; i++) {
        const struct poulsback_2d_op *o = &job.ops[i];
        FbStride stride = (FbStride)(job.surfaces[o->dst].stride / sizeof(FbBits));
        Bool same = o->src == o->dst;
        if (o->kind == POULSBACK_2D_FILL)
            fbSolid(row(o->dst, o->rect.y), stride, o->rect.x * 32, 32, (int)o->rect.width * 32,
                    (int)o->rect.height, fbAnd((int)o->rop, o->pixel, FB_ALLONES),
                    fbXor((int)o->rop, o->pixel, FB_ALLONES));
        else
            fbBlt(row(o->src, o->src_y), (FbStride)(job.surfaces[o->src].stride / sizeof(FbBits)),
                  o->src_x * 32, row(o->dst, o->rect.y), stride, o->rect.x * 32,
                  (int)o->rect.width * 32, (int)o->rect.height, (int)o->rop, FB_ALLONES, 32,
                  same && o->src_x < o->rect.x, same && o->src_y < o->rect.y);
    }
}

/* The repaint after the loss, a work procedure outside any request: VT
 * switch's pair (no root clip, then the full one) revalidates every window
 * and exposes it, so each client redraws once on the CPU. While switched away
 * EnterVT does the same. */
static Bool repaint(ClientPtr client, void *closure)
{
    ScrnInfoPtr scrn = closure;
    (void)client;
    if (scrn->vtSema && scrn->EnableDisableFBAccess) {
        scrn->EnableDisableFBAccess(scrn, FALSE);
        scrn->EnableDisableFBAccess(scrn, TRUE);
    }
    return TRUE;
}

/* The GPU is lost until the reboot: one line, 2D off, one repaint. */
static void lose(struct poulsback *pb, int err)
{
    xf86Msg(X_ERROR, "%s: GPU lost (%s): 2D acceleration off until reboot, %s\n", POULSBACK_NAME,
            strerror(err), "the desktop continues on the CPU");
    pb->accel = FALSE;
    QueueWorkProc(repaint, NULL, exa_scrn);
}

/* One SUBMIT_2D for the job so far, on X's context (created at the first job:
 * a client's context has brought the engine up by then). */
static void submit(struct poulsback *pb)
{
    struct poulsback_submit_2d args = {.abi = POULSBACK_EXEC_ABI,
                                       .bo_count = job.bo_count,
                                       .surface_count = job.surface_count,
                                       .op_count = job.op_count,
                                       .bos = (uintptr_t)job.bos,
                                       .surfaces = (uintptr_t)job.surfaces,
                                       .ops = (uintptr_t)job.ops};
    int err = 0;
    if (job.op_count && pb->accel && !pb->context) {
        struct poulsback_context_create create = {.abi = POULSBACK_EXEC_ABI};
        if (drmIoctl(pb->fd, DRM_IOCTL_PB_CONTEXT_CREATE, &create))
            err = errno;
        else
            pb->context = create.handle;
    }
    if (job.op_count && pb->accel && !err) {
        args.context = pb->context;
        if (drmIoctl(pb->fd, DRM_IOCTL_PB_SUBMIT_2D, &args))
            err = errno;
    }
    if (!job.op_count) {
    } else if (pb->accel && !err) {
        for (uint32_t i = 0; i < job.surface_count; i++)
            mark_pending(pb, job.privs[i]);
        jobs++;
        job_ops += job.op_count;
    } else {
        if (err == EINVAL || err == EOPNOTSUPP) {
            refused_ops += job.op_count;
        } else {
            if (pb->accel)
                lose(pb, err);
            unaccelerated_ops += job.op_count;
        }
        draw_on_cpu();
    }
    for (uint32_t i = 0; i < job.surface_count; i++)
        job.privs[i]->queued = FALSE;
    job.bo_count = job.surface_count = job.op_count = 0;
    job.pixels = 0;
}

/* The operation's surfaces in the job, submitting it first if it is full. */
static void begin(struct poulsback *pb, PixmapPtr src, PixmapPtr dst)
{
    int s = 0, d = surface(dst);
    if (src)
        s = surface(src);
    if (d < 0 || s < 0) {
        submit(pb);
        d = surface(dst);
        s = src ? surface(src) : 0;
    }
    op.src = src;
    op.dst = dst;
    op.src_surface = (uint32_t)s;
    op.dst_surface = (uint32_t)d;
}

/* An operation of at least this many pixels is a job of its own: within one
 * job each command's FENCE waits for the previous command's writes to drain
 * (HW-2D-003). Large operations run alone to avoid serializing all their
 * writes through the next command's FENCE (docs/graphics.md). */
#define ALONE_PIXELS 65536

/* Appends one operation of the current one, submitting the job first when it
 * has no room (256 operations, 2^25 pixels) or the operation runs alone. */
static void add(struct poulsback *pb, struct poulsback_2d_op o)
{
    uint64_t pixels = (uint64_t)o.rect.width * o.rect.height;
    Bool alone = pixels >= ALONE_PIXELS;
    if ((alone && job.op_count) || job.op_count == POULSBACK_2D_MAX_OPS ||
        job.pixels + pixels > POULSBACK_2D_MAX_PIXELS) {
        submit(pb);
        begin(pb, op.src, op.dst);
    }
    o.dst = op.dst_surface;
    o.src = op.src ? op.src_surface : 0;
    job.privs[o.dst]->generation++;
    job.ops[job.op_count++] = o;
    job.pixels += pixels;
    if (alone) {
        submit(pb);
        begin(pb, op.src, op.dst);
    }
}

/* Fills that read a WC destination, or whose destination a CPU access would
 * wait for, run on the 2D core. A planemask fill is exact as two: each bit of
 * the result is 0, 1, D or not D, so with r(D) the masked result it is
 * (D and A) xor B for B = r(0), A = r(0) xor r(~0). */
static Bool PrepareSolid(PixmapPtr pixmap, int alu, Pixel planemask, Pixel fg)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(pixmap->drawable.pScreen));
    struct poulsback_pixmap *d = exaGetPixmapDriverPrivate(pixmap);
    Bool masked = !EXA_PM_IS_SOLID(&pixmap->drawable, planemask);
    uint32_t pm = (uint32_t)planemask, r0, r1;
    if (!pb->accel || !twod(pixmap) ||
        !(busy(d) || ((reads_dst(alu) || masked) && write_combined(d))))
        return FALSE;
    op.alu = alu;
    op.masked = masked;
    op.pixel[0] = (uint32_t)fg;
    if (masked) {
        r0 = gx(alu, (uint32_t)fg, 0) & pm;
        r1 = (gx(alu, (uint32_t)fg, ~0u) & pm) | ~pm;
        op.pixel[0] = r0 ^ r1;
        op.pixel[1] = r0;
    }
    begin(pb, NULL, pixmap);
    return TRUE;
}

static void Solid(PixmapPtr pixmap, int x1, int y1, int x2, int y2)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(pixmap->drawable.pScreen));
    struct poulsback_2d_rect rect = {x1, y1, (uint32_t)(x2 - x1), (uint32_t)(y2 - y1)};
    if (x2 <= x1 || y2 <= y1)
        return;
    if (!op.masked) {
        add(pb, (struct poulsback_2d_op){.kind = POULSBACK_2D_FILL,
                                         .rop = (uint32_t)op.alu,
                                         .pixel = op.pixel[0],
                                         .rect = rect});
        return;
    }
    add(pb, (struct poulsback_2d_op){
                .kind = POULSBACK_2D_FILL, .rop = GXand, .pixel = op.pixel[0], .rect = rect});
    add(pb, (struct poulsback_2d_op){
                .kind = POULSBACK_2D_FILL, .rop = GXxor, .pixel = op.pixel[1], .rect = rect});
}

static void DoneSolid(PixmapPtr pixmap)
{
    (void)pixmap;
    op.dst = NULL;
}

/* Copies out of WC memory, onto a WC destination they read, or with an operand
 * a CPU access would wait for, run on the 2D core; the rest is fb's. Within
 * one pixmap the kernel orders each box like memmove, and EXA orders the boxes;
 * two pixmaps of one BO are two views the kernel refuses to copy between. A
 * planemask copy stays on fb (three inputs). */
static Bool PrepareCopy(PixmapPtr src, PixmapPtr dst, int xdir, int ydir, int alu, Pixel planemask)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(dst->drawable.pScreen));
    struct poulsback_pixmap *s = exaGetPixmapDriverPrivate(src),
                            *d = exaGetPixmapDriverPrivate(dst);
    (void)xdir;
    (void)ydir;
    if (!pb->accel || !twod(src) || !twod(dst) || src->drawable.depth != dst->drawable.depth ||
        !EXA_PM_IS_SOLID(&dst->drawable, planemask) || (s->handle == d->handle && src != dst) ||
        !(write_combined(s) || busy(s) || busy(d) || (reads_dst(alu) && write_combined(d))))
        return FALSE;
    op.alu = alu;
    op.masked = FALSE;
    begin(pb, src, dst);
    return TRUE;
}

static void Copy(PixmapPtr dst, int src_x, int src_y, int dst_x, int dst_y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    add(pb_of(xf86ScreenToScrn(dst->drawable.pScreen)),
        (struct poulsback_2d_op){.kind = POULSBACK_2D_COPY,
                                 .rop = (uint32_t)op.alu,
                                 .rect = {dst_x, dst_y, (uint32_t)width, (uint32_t)height},
                                 .src_x = src_x,
                                 .src_y = src_y});
}

static void DoneCopy(PixmapPtr dst)
{
    (void)dst;
    op.src = op.dst = NULL;
}

void poulsback_exa_flush(struct poulsback *pb)
{
    submit(pb);
}

/* GetImage of a WC pixmap: the 2D core copies the rectangle into a WB staging
 * pixmap that the CPU reads at cache speed (docs/graphics.md). Below the BO pixmap
 * size the CPU reads WC memory itself (EXA's fallback). GetImage over the
 * socket reads 64 KiB strips, so a copy takes up to four times
 * the rows asked for, and the next strips come from it while the source's
 * generation is unchanged; a client's buffer is never served from it. The
 * staging pixmap grows to the largest copy and lives until the screen
 * closes. */
static PixmapPtr staging;
static struct poulsback_pixmap *staging_priv, *staged;
static uint32_t staged_generation;
static int staged_x, staged_y, staged_w, staged_h;

static Bool staged_holds(struct poulsback_pixmap *s, int x, int y, int w, int h)
{
    return staged == s && !s->shared && staged_generation == s->generation && x >= staged_x &&
           y >= staged_y && x + w <= staged_x + staged_w && y + h <= staged_y + staged_h;
}

static Bool DownloadFromScreen(PixmapPtr pixmap, int x, int y, int w, int h, char *dst,
                               int dst_pitch)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *s = exaGetPixmapDriverPrivate(pixmap);
    int rows;
    if (!pb->accel || !twod(pixmap) || !write_combined(s) || (long)w * h < BO_PIXMAP_MIN_PIXELS)
        return FALSE;
    if (staged_holds(s, x, y, w, h)) {
        staged_downloads++;
    } else {
        rows = min(pixmap->drawable.height - y, max(h, min(4 * h, 262144 / w)));
        if (staging && (staging->drawable.width < w || staging->drawable.height < rows ||
                        staging->drawable.depth != pixmap->drawable.depth)) {
            int width = max(staging->drawable.width, w);
            int height = max(staging->drawable.height, rows);
            screen->DestroyPixmap(staging);
            staging_priv = NULL; /* freed with the pixmap; CloseScreen must not see it */
            staging = screen->CreatePixmap(screen, width, height, pixmap->drawable.depth, 0);
        } else if (!staging) {
            staging = screen->CreatePixmap(screen, w, rows, pixmap->drawable.depth, 0);
        }
        staged = NULL;
        if (!staging || !twod(staging))
            return FALSE;
        staging_priv = exaGetPixmapDriverPrivate(staging);
        op.alu = GXcopy;
        op.masked = FALSE;
        begin(pb, pixmap, staging);
        add(pb, (struct poulsback_2d_op){.kind = POULSBACK_2D_COPY,
                                         .rop = GXcopy,
                                         .rect = {0, 0, (uint32_t)w, (uint32_t)rows},
                                         .src_x = x,
                                         .src_y = y});
        op.src = op.dst = NULL;
        submit(pb);
        if (staging_priv->fd >= 0 && staging_priv->pending)
            wait_gpu(staging_priv, FALSE);
        staged = s;
        staged_generation = s->generation;
        staged_x = x;
        staged_y = y;
        staged_w = w;
        staged_h = rows;
    }
    for (int row_y = 0; row_y < h; row_y++)
        memcpy(dst + (size_t)row_y * (size_t)dst_pitch,
               (char *)staging_priv->map +
                   (size_t)(y - staged_y + row_y) * (size_t)staging->devKind +
                   (size_t)(x - staged_x) * 4,
               (size_t)w * 4);
    downloads++;
    return TRUE;
}

/* Before X sleeps, so that drawing reaches the screen. */
static void ScreenBlockHandler(ScreenPtr screen, void *timeout)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    submit(pb);
    screen->BlockHandler = pb->block_handler;
    screen->BlockHandler(screen, timeout);
    pb->block_handler = screen->BlockHandler;
    screen->BlockHandler = ScreenBlockHandler;
}

/* Before X writes replies and events to clients. */
static void flush_callback(CallbackListPtr *list, void *data, void *call_data)
{
    (void)list;
    (void)call_data;
    submit(data);
}

/* Per-BO waits in PrepareAccess replace EXA's global marker, which would make
 * every CPU access wait for the last job. */
static int MarkSync(ScreenPtr screen)
{
    (void)screen;
    return 0;
}

static void WaitMarker(ScreenPtr screen, int marker)
{
    (void)screen;
    (void)marker;
}

/* GC operations above EXA's, whose own are kept in exa_gc_ops.
 *
 * Stippled fills on WC memory. fb fills a stipple by read-modify-write of every
 * destination word, and the CPU reads WC memory slowly; with GXcopy and a
 * solid planemask each pixel is
 * either written or kept, so this writes the set pixels (and an opaque
 * stipple's background) and reads nothing back. EXA does not accelerate
 * stipples, so the GC's PolyFillRect is wrapped above EXA's; everything else
 * goes to EXA. The stipple's bits are read from its system memory: EXA hides
 * a pixmap's devPrivate.ptr outside an access.
 *
 * Points, thin lines and segments, and solid fills that stay on the CPU under
 * the policy (a destination no CPU access would wait for, and no function or
 * planemask that reads a WC destination) go to fb directly on the exposed
 * map. EXA's fallback builds regions and swaps the GC's operations on every
 * request. EXA turns axis-aligned lines
 * into fills the policy may send to the 2D core, so those stay with it. */
static GCOps gc_ops, exa_gc_ops;

static PixmapPtr drawable_pixmap(DrawablePtr drawable)
{
    return drawable->type == DRAWABLE_WINDOW
               ? drawable->pScreen->GetWindowPixmap((WindowPtr)drawable)
               : (PixmapPtr)drawable;
}

/* The pixmap fb may draw on directly, or NULL to leave the operation to EXA. */
static PixmapPtr cpu_direct(DrawablePtr drawable, GCPtr gc)
{
    PixmapPtr pixmap = drawable_pixmap(drawable);
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    if (!priv || gc->fillStyle != FillSolid)
        return NULL;
    if (!priv->map)
        return priv->sys ? pixmap : NULL;
    if (busy(priv) ||
        ((reads_dst(gc->alu) || !EXA_PM_IS_SOLID(drawable, gc->planemask)) && write_combined(priv)))
        return NULL;
    priv->generation++;
    return pixmap;
}

/* Exposes a pixmap's pixels (a BO's map or X's system memory) to fb for one
 * call; returns the pointer EXA hides outside its own accesses. */
static void *expose(PixmapPtr pixmap)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    void *hidden = pixmap->devPrivate.ptr;
    pixmap->devPrivate.ptr = priv->map ? priv->map : priv->sys;
    return hidden;
}

static void PolyPoint(DrawablePtr drawable, GCPtr gc, int mode, int npt, DDXPointPtr points)
{
    PixmapPtr pixmap = cpu_direct(drawable, gc);
    void *hidden;
    if (!pixmap) {
        exa_gc_ops.PolyPoint(drawable, gc, mode, npt, points);
        return;
    }
    hidden = expose(pixmap);
    fbPolyPoint(drawable, gc, mode, npt, points);
    pixmap->devPrivate.ptr = hidden;
}

static void Polylines(DrawablePtr drawable, GCPtr gc, int mode, int npt, DDXPointPtr points)
{
    PixmapPtr pixmap = gc->lineWidth ? NULL : cpu_direct(drawable, gc);
    void *hidden;
    if (!pixmap) {
        exa_gc_ops.Polylines(drawable, gc, mode, npt, points);
        return;
    }
    hidden = expose(pixmap);
    fbPolyLine(drawable, gc, mode, npt, points);
    pixmap->devPrivate.ptr = hidden;
}

static void PolySegment(DrawablePtr drawable, GCPtr gc, int nseg, xSegment *segments)
{
    PixmapPtr pixmap = gc->lineWidth ? NULL : cpu_direct(drawable, gc);
    void *hidden;
    if (!pixmap) {
        exa_gc_ops.PolySegment(drawable, gc, nseg, segments);
        return;
    }
    hidden = expose(pixmap);
    fbPolySegment(drawable, gc, nseg, segments);
    pixmap->devPrivate.ptr = hidden;
}

static void stipple_fill(PixmapPtr pixmap, int xoff, int yoff, DrawablePtr drawable, GCPtr gc,
                         int nrect, const xRectangle *rects)
{
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    RegionPtr clip = fbGetCompositeClip(gc);
    PixmapPtr stip = gc->stipple;
    const char *bits = ((struct poulsback_pixmap *)exaGetPixmapDriverPrivate(stip))->sys;
    int sw = stip->drawable.width, sh = stip->drawable.height;
    int ox = drawable->x + gc->patOrg.x, oy = drawable->y + gc->patOrg.y;
    Bool opaque = gc->fillStyle == FillOpaqueStippled;
    uint32_t fg = (uint32_t)gc->fgPixel, bg = (uint32_t)gc->bgPixel;
    const BoxRec *boxes = RegionRects(clip);
    int nbox = RegionNumRects(clip);
    if (priv->queued)
        submit(pb_of(xf86ScreenToScrn(pixmap->drawable.pScreen)));
    if (priv->fd >= 0 && (priv->shared || priv->pending))
        wait_gpu(priv, TRUE);
    stipple_fills++;
    priv->generation++;
    for (int r = 0; r < nrect; r++) {
        int rx1 = drawable->x + rects[r].x, ry1 = drawable->y + rects[r].y;
        int rx2 = rx1 + rects[r].width, ry2 = ry1 + rects[r].height;
        for (int b = 0; b < nbox; b++) {
            int x1 = max(rx1, boxes[b].x1), x2 = min(rx2, boxes[b].x2);
            int y1 = max(ry1, boxes[b].y1), y2 = min(ry2, boxes[b].y2);
            for (int y = y1; y < y2; y++) {
                const uint32_t *srow =
                    (const uint32_t *)(const void *)(bits + (size_t)(((y - oy) % sh + sh) % sh) *
                                                                (size_t)stip->devKind);
                uint32_t *drow =
                    (uint32_t *)(void *)((char *)priv->map +
                                         (size_t)(y + yoff) * (size_t)pixmap->devKind) +
                    xoff;
                int sx = ((x1 - ox) % sw + sw) % sw;
                for (int x = x1; x < x2; x++) {
                    if (srow[sx >> 5] >> (sx & 31) & 1)
                        drow[x] = fg;
                    else if (opaque)
                        drow[x] = bg;
                    if (++sx == sw)
                        sx = 0;
                }
            }
        }
    }
}

static void PolyFillRect(DrawablePtr drawable, GCPtr gc, int nrect, xRectangle *rects)
{
    PixmapPtr pixmap = drawable_pixmap(drawable), direct;
    struct poulsback_pixmap *priv = exaGetPixmapDriverPrivate(pixmap), *stip = NULL;
    int xoff = 0, yoff = 0;
    void *hidden;
    if (gc->fillStyle == FillStippled || gc->fillStyle == FillOpaqueStippled)
        stip = exaGetPixmapDriverPrivate(gc->stipple);
#ifdef COMPOSITE
    if (drawable->type == DRAWABLE_WINDOW) {
        xoff = -pixmap->screen_x;
        yoff = -pixmap->screen_y;
    }
#endif
    if (priv && priv->map && write_combined(priv) && pixmap->drawable.bitsPerPixel == 32 && stip &&
        stip->sys && gc->alu == GXcopy && EXA_PM_IS_SOLID(drawable, gc->planemask) &&
        BITMAP_BIT_ORDER == LSBFirst) {
        stipple_fill(pixmap, xoff, yoff, drawable, gc, nrect, rects);
    } else if ((direct = cpu_direct(drawable, gc))) {
        hidden = expose(direct);
        fbPolyFillRect(drawable, gc, nrect, rects);
        direct->devPrivate.ptr = hidden;
    } else {
        exa_gc_ops.PolyFillRect(drawable, gc, nrect, rects);
    }
}

static Bool ScreenCreateGC(GCPtr gc)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(gc->pScreen));
    ScreenPtr screen = gc->pScreen;
    Bool ret;
    screen->CreateGC = pb->create_gc;
    ret = screen->CreateGC(gc);
    pb->create_gc = screen->CreateGC;
    screen->CreateGC = ScreenCreateGC;
    if (ret) {
        if (!exa_gc_ops.PolyFillRect) {
            exa_gc_ops = *gc->ops;
            gc_ops = exa_gc_ops;
            gc_ops.PolyFillRect = PolyFillRect;
            gc_ops.PolyPoint = PolyPoint;
            gc_ops.Polylines = Polylines;
            gc_ops.PolySegment = PolySegment;
        }
        if (gc->ops->PolyFillRect == exa_gc_ops.PolyFillRect)
            gc->ops = &gc_ops;
    }
    return ret;
}

/* The screen pixmap is created by fb's CreateScreenResources over the front's
 * mapping: it becomes the front's BO pixmap. */
static Bool CreateScreenResources(ScreenPtr screen)
{
    struct poulsback *pb = pb_of(xf86ScreenToScrn(screen));
    struct poulsback_pixmap *priv;
    Bool ret;
    screen->CreateScreenResources = pb->create_screen_resources;
    ret = screen->CreateScreenResources(screen);
    pb->create_screen_resources = screen->CreateScreenResources;
    screen->CreateScreenResources = CreateScreenResources;
    if (!ret)
        return FALSE;
    priv = exaGetPixmapDriverPrivate(screen->GetScreenPixmap(screen));
    if (!priv)
        return FALSE;
    priv->map = pb->front;
    priv->size = pb->front_size;
    priv->handle = pb->handle;
    priv->flags = POULSBACK_BO_SCANOUT | POULSBACK_BO_CPU_WC;
    priv->bo_class = POULSBACK_BO_CLASS_2D;
    priv->front = TRUE;
    return TRUE;
}

Bool poulsback_exa_init(ScreenPtr screen, struct poulsback *pb)
{
    ExaDriverPtr exa = exaDriverAlloc();
    if (!exa)
        return FALSE;
    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;
    exa->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS | EXA_SUPPORTS_PREPARE_AUX;
    exa->pixmapOffsetAlign = 8;
    exa->pixmapPitchAlign = 128;
    /* The 2D core's surface limits: a 32-bit row of at most 16,256 bytes. */
    exa->maxX = POULSBACK_2D_MAX_WIDTH_32BPP;
    exa->maxY = POULSBACK_2D_MAX_DIM;
    exa->CreatePixmap2 = CreatePixmap2;
    exa->DestroyPixmap = DestroyPixmap;
    exa->ModifyPixmapHeader = ModifyPixmapHeader;
    exa->PixmapIsOffscreen = PixmapIsOffscreen;
    exa->PrepareAccess = PrepareAccess;
    exa->PrepareSolid = PrepareSolid;
    exa->Solid = Solid;
    exa->DoneSolid = DoneSolid;
    exa->DownloadFromScreen = DownloadFromScreen;
    exa->PrepareCopy = PrepareCopy;
    exa->Copy = Copy;
    exa->DoneCopy = DoneCopy;
    exa->MarkSync = MarkSync;
    exa->WaitMarker = WaitMarker;
    if (!exaDriverInit(screen, exa)) {
        free(exa);
        return FALSE;
    }
    pb->exa = exa;
    pb->accel = TRUE;
    exa_scrn = xf86ScreenToScrn(screen);
    if (!AddCallback(&FlushCallback, flush_callback, pb))
        return FALSE;
    pb->block_handler = screen->BlockHandler;
    screen->BlockHandler = ScreenBlockHandler;
    pb->create_gc = screen->CreateGC;
    screen->CreateGC = ScreenCreateGC;
    pb->create_screen_resources = screen->CreateScreenResources;
    screen->CreateScreenResources = CreateScreenResources;
    return TRUE;
}

void poulsback_exa_close_screen(ScreenPtr screen, struct poulsback *pb)
{
    struct poulsback_context_destroy destroy = {.abi = POULSBACK_EXEC_ABI, .handle = pb->context};
    submit(pb);
    /* Only the staging pixmap's BO goes: X has freed EXA's screen-specific
     * privates before CloseScreen, so the pixmap can neither be destroyed nor
     * looked up here. */
    if (staging_priv)
        poulsback_pixmap_release_bo(pb, staging_priv);
    staging = NULL;
    staging_priv = NULL;
    staged = NULL;
    DeleteCallback(&FlushCallback, flush_callback, pb);
    screen->BlockHandler = pb->block_handler;
    screen->CreateGC = pb->create_gc;
    xf86Msg(X_INFO,
            "%s: EXA: %lu 2D jobs (%lu operations), fb drew %lu refused and %lu "
            "unaccelerated operations; %lu staged GetImages (%lu from an earlier copy), %lu "
            "stippled fills; %lu CPU waits (%lu blocked, %lu on X's own BOs)\n",
            POULSBACK_NAME, jobs, job_ops, refused_ops, unaccelerated_ops, downloads,
            staged_downloads, stipple_fills, waits, blocked, own_waits);
    if (unsynced)
        xf86Msg(X_WARNING, "%s: %lu CPU accesses without fence wait\n", POULSBACK_NAME, unsynced);
    if (pb->context)
        drmIoctl(pb->fd, DRM_IOCTL_PB_CONTEXT_DESTROY, &destroy);
    pb->context = 0;
}

void poulsback_exa_free(struct poulsback *pb)
{
    free(pb->exa);
    pb->exa = NULL;
}
