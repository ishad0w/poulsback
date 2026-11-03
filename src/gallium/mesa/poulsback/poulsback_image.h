// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Shared images and the engine render client; no pipe capability publication. */
#ifndef POULSBACK_WINSYS_IMAGE_H
#define POULSBACK_WINSYS_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
#include "poulsback_engine.h"
#include "poulsback_render_ir.h"

/* An image is a 2D-class BO with a layout kept here. Write-back by default; a
 * SCANOUT image (one that can be page-flipped) is write-combined. */
#define POULSBACK_IMAGE_FORMAT_XRGB8888 0x34325258U
#define POULSBACK_IMAGE_FORMAT_ARGB8888 0x34325241U
#define POULSBACK_IMAGE_MOD_LINEAR 0ULL
#define POULSBACK_IMAGE_MAX_BYTES (16U * 1024U * 1024U)
#define POULSBACK_IMAGE_TOTAL_BYTES (64U * 1024U * 1024U)

struct poulsback_image_info {
    uint32_t handle;
    uint64_t size, modifier;
    uint32_t width, height, stride, format;
};

struct poulsback_ws;
struct poulsback_ws_image;
struct poulsback_ws_render;
struct poulsback_ws_depth;
struct poulsback_scene;
struct poulsback_slab_ops;

struct poulsback_ws_layout {
    uint32_t width, height, stride, format;
    uint64_t modifier, offset;
};

enum poulsback_ws_cpu_access {
    POULSBACK_WS_CPU_READ = 1,
    POULSBACK_WS_CPU_WRITE = 2,
    POULSBACK_WS_CPU_READ_WRITE = 3
};

/* All failures return a negative errno and preserve output arguments. fd is
 * borrowed; the client reopens its stable /proc fd anchor into an independent
 * DRM file and owns that CLOEXEC fd and GEM namespace. Reopen/access failure
 * refuses creation. External callers must not close its GEM handles. Operations serialize
 * internally. A caller must retain an image reference for every operation;
 * final client destruction requires external lifetime serialization. */
int poulsback_ws_create(int fd, struct poulsback_ws **out);
/* Returns EBUSY without changing the client while images still exist. */
int poulsback_ws_destroy(struct poulsback_ws *ws);
/* scanout: a SCANOUT, write-combined BO that the display can page-flip. */
int poulsback_ws_image_create(struct poulsback_ws *ws, const struct poulsback_ws_layout *layout,
                              bool scanout, struct poulsback_ws_image **out);
/* The width, height, stride and format (DRM fourcc) a SCANOUT image needs to
 * be page-flipped, read from the kernel when the winsys was made (zero when
 * nothing was shown). */
void poulsback_ws_display(const struct poulsback_ws *ws, struct poulsback_ws_layout *out);
/* Imported fd is borrowed. Only one plane, zero offset and a matching native
 * metadata record are accepted. Repeated handles share one reference object. */
int poulsback_ws_image_import(struct poulsback_ws *ws, int fd,
                              const struct poulsback_ws_layout *layout,
                              struct poulsback_ws_image **out);
int poulsback_ws_image_ref(struct poulsback_ws_image *image);
/* A failed final GEM_CLOSE preserves the reference for retry. */
int poulsback_ws_image_unref(struct poulsback_ws_image *image);
int poulsback_ws_image_info(struct poulsback_ws_image *image, struct poulsback_image_info *out);
/* The image's GEM handle and GPU address, for a frame that samples it. The
 * caller holds a reference until the frame is submitted. */
int poulsback_ws_image_gpu(struct poulsback_ws_image *image, uint32_t *handle, uint64_t *gpu_va);
/* Successful export transfers a CLOEXEC dma-buf fd to the caller. */
int poulsback_ws_image_export(struct poulsback_ws_image *image, int *fd);
/* One CPU access interval at a time. START/END use DMA_BUF_IOCTL_SYNC; they do
 * not exclude GPU or display use: callers check poulsback_ws_image_gpu_busy.
 * Active mappings retain an internal image reference. END failure retains the
 * mapping/reference and may be retried. If START fails and its munmap cleanup
 * also fails, cleanup_error records that failure: call cpu_end to release the
 * retained mapping even though cpu_begin returned an error. No readback
 * presentation is provided. */
/* True while a submitted job still holds the image (released when reaped). */
bool poulsback_ws_image_gpu_busy(struct poulsback_ws_image *image);
/* Whether a job still holds the depth's planes. */
bool poulsback_ws_depth_gpu_busy(struct poulsback_ws_depth *);
int poulsback_ws_image_cpu_begin(struct poulsback_ws_image *image,
                                 enum poulsback_ws_cpu_access access, void **address,
                                 uint64_t *size);
int poulsback_ws_image_cpu_end(struct poulsback_ws_image *image);

/* Buffer storage BOs (poulsback_slab.h) in the client's namespace: GENERAL,
 * write-back and snooped, mapped for reading and writing. Freeing one is not
 * retried; a failed unmap or GEM_CLOSE is kept as cleanup_error. */
void poulsback_ws_buffer_ops(struct poulsback_ws *, struct poulsback_slab_ops *out);

/* A render is one engine context on the SAME private drm_file as its
 * images; a depth scene is built in userspace and run through SUBMIT. No DRM
 * fd, GEM handle or address escapes these APIs. Any failure holds the render. */
int poulsback_ws_render_create(struct poulsback_ws *, struct poulsback_ws_render **);
/* Destroying the render waits for every frame it submitted. */
int poulsback_ws_render_destroy(struct poulsback_ws_render *);
int poulsback_ws_render_error(struct poulsback_ws_render *);
/* Exact opaque namespace membership plus render health; no FD/handle escapes.
 * The caller owns ws/render lifetime. A render from another ws returns EXDEV,
 * including when both clients happened to reopen the same device node. */
int poulsback_ws_render_check_owner(struct poulsback_ws_render *, struct poulsback_ws *);
/* A DEPTH BO in the same private DRM-file namespace. No handle/address
 * escapes. Jobs retain a reference until reaped; a failed creation holds the
 * depth owner and forbids final destruction/retry. */
int poulsback_ws_depth_create(struct poulsback_ws_render *, uint32_t width, uint32_t height,
                              struct poulsback_ws_depth **);
int poulsback_ws_depth_ref(struct poulsback_ws_depth *);
int poulsback_ws_depth_unref(struct poulsback_ws_depth *);
/* The stored planes (D24S8: depth in a word's low 24 bits, stencil in its
 * high 8, rows of the width rounded up to 32) for the CPU once no job holds
 * them: a write-combined mapping of the BO, *STRIDE bytes per row. EBUSY
 * while a job holds it or it is mapped. */
int poulsback_ws_depth_cpu_begin(struct poulsback_ws_depth *, void **address, uint32_t *stride);
int poulsback_ws_depth_cpu_end(struct poulsback_ws_depth *);
/* POULSBACK_RENDER_DEPTH_SCENE_ABI while the render is healthy, else zero. */
uint32_t poulsback_ws_render_depth_scene_version(struct poulsback_ws_render *);
/* Pending returns EBUSY. A job releases its target and depth only after its
 * fence signalled; a GEM cleanup failure is retained for retry. */
int poulsback_ws_render_reap(struct poulsback_ws_render *);
/* One draw of the render's open frame (poulsback_engine_draw). */
int poulsback_ws_render_draw(struct poulsback_ws_render *, const poulsback_frame_shape *,
                             const struct poulsback_frame_vertices *);
/* A GPU-shaded draw of the render's open frame and space in its stream
 * (poulsback_engine_draw_gpu, _stream); the static index sequence. */
int poulsback_ws_render_draw_gpu(struct poulsback_ws_render *, const poulsback_frame_shape *,
                                 const struct poulsback_frame_gpu_draw *);
int poulsback_ws_render_stream(struct poulsback_ws_render *, uint32_t bytes, uint32_t alignment,
                               void **cpu, uint64_t *address, uint32_t *handle);
void poulsback_ws_render_sequence(struct poulsback_ws_render *, uint64_t *address,
                                  uint32_t *handle);
/* The buffer job number (poulsback_slab.h) of the render's open frame, the
 * one its submit gives the job. */
uint64_t poulsback_ws_render_frame_sequence(struct poulsback_ws_render *);
/* The highest number every job up to which has completed (or never ran): the
 * client's renders' jobs are reaped first. */
uint64_t poulsback_ws_completed(struct poulsback_ws *);
/* Drops the open frame's draws (a full clear replaces them). */
void poulsback_ws_render_discard(struct poulsback_ws_render *);
/* The open frame loads its depth and stencil: it does not start with a full
 * clear. */
void poulsback_ws_render_load(struct poulsback_ws_render *);
/* The open frame's draws, 0 when none is open. */
uint32_t poulsback_ws_render_draws(struct poulsback_ws_render *);
/* Submits the open frame; depth is NULL for a frame without a depth
 * attachment. Success returns a CLOEXEC sync-file FD and a nonzero job
 * identity; the job keeps target and depth until its fence signals. */
int poulsback_ws_render_submit_engine(struct poulsback_ws_render *,
                                      struct poulsback_ws_image *target,
                                      struct poulsback_ws_depth *, int *sync_file,
                                      uint64_t *job_id);
/* One 2D-core copy between two images of this render's client (or within
 * one), after the render's earlier jobs. Success returns a CLOEXEC sync-file
 * FD; the job keeps both images until its fence signals. A failure leaves
 * nothing running and the render healthy; -EINVAL and -EOPNOTSUPP are the
 * kernel's refusals (a non-2D-class BO, a layout or formats the 2D core cannot
 * copy). If only the FD cannot be returned, the job still runs and is reaped. */
int poulsback_ws_render_copy(struct poulsback_ws_render *, struct poulsback_ws_image *dst,
                             struct poulsback_ws_image *src, const struct poulsback_engine_copy *,
                             int *sync_file);
#endif
