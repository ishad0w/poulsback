// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Engine mode: frames run on the kernel's engine through SUBMIT
 * (docs/kernel/execution.md). The frame writer (poulsback_frame.h) writes each
 * draw into a frame slot's buffers as it comes; a submit sends the frame with
 * its target and depth buffers. Image copies are SUBMIT_2D jobs on the same
 * context, ordered against rendering by BO reservations. */
#ifndef POULSBACK_ENGINE_H
#define POULSBACK_ENGINE_H
#include <stdint.h>
#include "poulsback_frame.h"

struct poulsback_engine;

/* A BO of the caller's that a frame renders into (target) or tests against. */
struct poulsback_engine_buffer {
    uint32_t handle, stride;
    uint64_t gpu_va, size;
};

/* A BO of BO_CLASS with FLAGS and at least BYTES on fd, mapped with PROT
 * unless that is PROT_NONE (out->map is then NULL). A failed check or map
 * closes the handle; a failed close is stored in *cleanup when given. The
 * engine's frame buffers and the winsys's images, buffers and depth. */
int poulsback_bo_create(int fd, uint32_t bo_class, uint32_t flags, uint64_t bytes, int prot,
                        struct poulsback_frame_bo *out, int *cleanup);
/* One engine context on fd (borrowed; it outlives the engine). */
int poulsback_engine_create(int fd, struct poulsback_engine **out);
/* Waits for the engine's frames, then frees its buffers and its context. */
void poulsback_engine_destroy(struct poulsback_engine *);
/* One draw of the open frame; the first opens it in the next frame slot,
 * after that slot's last job. A refusal leaves the frame as it was. */
int poulsback_engine_draw(struct poulsback_engine *, const poulsback_frame_shape *,
                          const struct poulsback_frame_vertices *);
/* A GPU-shaded draw of the open frame (poulsback_frame_draw_gpu), and space
 * in its stream; either opens the frame. */
int poulsback_engine_draw_gpu(struct poulsback_engine *, const poulsback_frame_shape *,
                              const struct poulsback_frame_gpu_draw *);
int poulsback_engine_stream(struct poulsback_engine *, uint32_t bytes, uint32_t alignment,
                            void **cpu, uint64_t *address, uint32_t *handle);
void poulsback_engine_sequence(struct poulsback_engine *, uint64_t *address, uint32_t *handle);
/* Drops the open frame's draws (a full clear replaces them). */
void poulsback_engine_discard(struct poulsback_engine *);
/* The open frame loads its depth and stencil (poulsback_frame_load). */
void poulsback_engine_load(struct poulsback_engine *);
/* The open frame's draws (poulsback_frame_draws). */
uint32_t poulsback_engine_draws(const struct poulsback_engine *);
/* Submits the open frame; it closes either way. *sync_file is the job's
 * completion fence; target and depth stay in use until it signals. depth is
 * NULL for a frame without a depth attachment. */
int poulsback_engine_submit(struct poulsback_engine *, const struct poulsback_engine_buffer *target,
                            const struct poulsback_engine_buffer *depth, int *sync_file);

/* A 2D-class BO seen as one linear image; format is a POULSBACK_2D_* value. */
struct poulsback_engine_image {
    uint32_t handle, stride, width, height, format;
};
/* A width x height rectangle at (x, y) of dst from (src_x, src_y) of src. */
struct poulsback_engine_copy {
    int32_t x, y, src_x, src_y;
    uint32_t width, height;
};
/* One 2D-core copy (GXcopy) from src to dst, which may be the same image.
 * *sync_file is the job's completion fence; reads and writes are fenced on
 * the BOs' reservations. A failure leaves nothing running (a queued job whose
 * fence cannot be exported is waited for): -EINVAL and -EOPNOTSUPP are the
 * kernel's refusals of what the 2D core cannot copy. */
int poulsback_engine_copy(struct poulsback_engine *, const struct poulsback_engine_image *dst,
                          const struct poulsback_engine_image *src,
                          const struct poulsback_engine_copy *, int *sync_file);
#endif
