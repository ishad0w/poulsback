// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* xf86-video-poulsback: state shared by the driver and its KMS display layer. */
#ifndef POULSBACK_DRV_H
#define POULSBACK_DRV_H

#include "xorg-server.h"
#include "xf86.h"
#include "list.h"
#include <stdint.h>
#include <xf86drmMode.h>

#define POULSBACK_NAME "poulsback"
#define POULSBACK_CURSOR_SIZE 64

/* A pixmap's storage, EXA's driver private (poulsback_exa.c): X's own in
 * system memory or a 2D-class WB BO, or a BO in X's file with a persistent CPU
 * mapping, its flags and class from BO_INFO. A DRI3 import holds its dma-buf;
 * the front or one of X's BOs
 * gets one when a client asks for it (then it is shared too) or when a 2D job
 * names it. CPU access waits on the dma-buf when the BO is shared or a 2D job
 * of X's is pending on it, and first submits X's open job when that names it
 * (queued). */
struct poulsback_pixmap {
    void *sys;
    void *map;
    size_t size;
    int fd;
    uint32_t handle, flags, bo_class;
    /* Its KMS framebuffer, made at the first flip; a refused one is not retried. */
    uint32_t fb;
    Bool fb_refused, front, shared, pending, queued;
    /* Bumped by every write X makes to it (CPU access, a 2D operation, a
     * stippled fill): a staged GetImage copy of an older generation is stale. */
    uint32_t generation;
};

struct poulsback {
    int fd;
    char *render_node;
    /* The screen: the panel's size, a SCANOUT BO (WC, 2D class) and its typed
     * KMS framebuffer, which Present can flip to and from. */
    int width, height;
    uint32_t fb, handle, pitch;
    void *front;
    size_t front_size;
    /* GEM handles of the BOs imported for DRI3 pixmaps (struct handle_ref). */
    struct xorg_list handles;
    /* The 64x64 ARGB cursor image, a gma500 dumb buffer (the kernel copies it at SetCursor). */
    uint32_t cursor_handle;
    uint32_t *cursor;
    /* Present: queued vblank events and the serial that names the next one;
     * the one flip in flight (Present orders them) and the counts of flips,
     * unflips and refused flips, logged at exit. */
    struct xorg_list vblanks;
    uint32_t vblank_serial;
    struct {
        void *crtc; /* xf86CrtcPtr */
        uint64_t event_id;
        uint32_t serial;
        Bool pending;
    } flip;
    unsigned long flips, unflips, flips_refused;
    /* EXA (poulsback_exa.c): its driver record, X's 2D context (made at the
     * first 2D job) and whether 2D jobs are on (off after an error other than a
     * refusal: the GPU is lost until reboot). */
    void *exa;
    uint32_t context;
    Bool accel;
    /* errno of the last refused mode set (poulsback_display.c): after the GPU
     * loss, a front Present cannot show again for any reason but EBUSY is a
     * display failure, and X stops (poulsback_present.c). */
    int mode_error;
    CloseScreenProcPtr close_screen;
    CreateScreenResourcesProcPtr create_screen_resources;
    ScreenBlockHandlerProcPtr block_handler;
    CreateGCProcPtr create_gc;
};

/* An xf86 CRTC's driver_private: the kernel CRTC, its index (the pipe), the
 * framebuffer and mode it showed before X (fbcon's: a typed framebuffer leaves
 * the CRTC only for it), whether its outputs are off (DPMS), the cursor state
 * to restore, and the kernel's 32-bit vblank sequence extended to a 64-bit MSC. */
struct poulsback_crtc {
    uint32_t id, index, initial_fb;
    drmModeModeInfo initial_mode;
    Bool off, cursor_up, msc_valid;
    int cursor_x, cursor_y;
    uint32_t msc_last;
    uint64_t msc;
};

static inline struct poulsback *pb_of(ScrnInfoPtr scrn)
{
    return scrn->driverPrivate;
}

/* poulsback_display.c: xf86Crtc/RandR 1.2 over the legacy KMS ioctls. */
Bool poulsback_display_pre_init(ScrnInfoPtr scrn, struct poulsback *pb);
Bool poulsback_display_screen_init(ScreenPtr screen, struct poulsback *pb);
/* Before the front is removed: each CRTC back on its initial framebuffer. */
void poulsback_display_close_screen(ScreenPtr screen, struct poulsback *pb);

/* The KMS framebuffer of a DRI3 pixmap the CRTC can show (a SCANOUT BO in the
 * front's size, pitch and XRGB8888 format), made at its first flip; 0 if none. */
uint32_t poulsback_pixmap_scanout(ScreenPtr screen, PixmapPtr pixmap);

/* X's own pixmap storage: a 2D-class WB BO of at least size bytes, mapped for
 * the CPU, its handle counted like an import's (a client may hand it back). */
Bool poulsback_pixmap_create_bo(struct poulsback *pb, struct poulsback_pixmap *priv, size_t size);

/* Unmaps a pixmap's BO and closes its framebuffer, dma-buf and X's handle
 * reference. */
void poulsback_pixmap_release_bo(struct poulsback *pb, struct poulsback_pixmap *priv);

/* poulsback_exa.c: EXA in driver-pixmap mode, after fbScreenInit and
 * fbPictureInit; the close hook submits the open job and logs its counters,
 * free runs after the screen's CloseScreen. */
Bool poulsback_exa_init(ScreenPtr screen, struct poulsback *pb);
/* Submits X's open 2D job (Present's flush hook: before a copied pixmap is idle). */
void poulsback_exa_flush(struct poulsback *pb);
void poulsback_exa_close_screen(ScreenPtr screen, struct poulsback *pb);
void poulsback_exa_free(struct poulsback *pb);

/* poulsback_present.c: Present on the kernel's vblank events, with page flips. */
Bool poulsback_present_screen_init(ScreenPtr screen, struct poulsback *pb);
void poulsback_present_close_screen(struct poulsback *pb);

#endif
