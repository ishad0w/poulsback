// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_RESOURCE_H
#define POULSBACK_RESOURCE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct pipe_screen;
struct pipe_context;
struct pipe_resource;
struct poulsback_resource_domain;
struct poulsback_ws_render;
struct poulsback_ws_image;
struct poulsback_ws_depth;

/* Private immutable CPU capture identity. This describes an attachment, not
 * an allocated depth image, hardware layout, stride, handle or GPU address. */
#define POULSBACK_DEPTH_CAPTURE_VERSION 1u
#define POULSBACK_DEPTH_CAPTURE_D16 1u
#define POULSBACK_DEPTH_CAPTURE_D24S8 2u
/* Logical depth24/stencil0; the upper8 bits have no API value. This is not a
 * native storage layout or a promise about their contents/preservation. */
#define POULSBACK_DEPTH_CAPTURE_D24X8 3u
struct poulsback_depth_capture_description {
    uint32_t version, format, width, height, samples, storage_samples;
};
/* Authenticates canonical object membership/type and all retained template
 * fields before returning the copied descriptor. Caller holds a resource ref;
 * mutable same-screen or ordinary color/buffer objects never authenticate. */
int poulsback_resource_depth_capture_description(struct pipe_resource *,
                                                 struct poulsback_depth_capture_description *out);
/* Explicit post-CPU-accept lease of this exact canonical depth attachment.
 * First use lazily creates the Z24X8 or Z24S8 storage (one D24S8 layout) in
 * the domain's private ws; the resource retains its owner, shared by healthy
 * same-ws render contexts. D16 and other descriptors remain capture-only. The caller holds the
 * ordinary pipe_resource reference and releases the returned native reference
 * with ws_depth_unref. Resource destruction drops only its anchor; jobs retain
 * their own references. No submission, GPU/cache effect or fence is created.
 * Allocation failure before any ioctl is retryable. Uncertain creation or
 * post-creation source mutation retains owners and forbids replacement/retry.
 * Output must be a separate NULL pointer slot; all failures preserve it. */
int poulsback_resource_depth_acquire(struct pipe_resource *, struct poulsback_ws_render *,
                                     struct poulsback_ws_depth **);

/* Installs the Gallium resource callbacks on the screen. Owns a new
 * image-client namespace and reserves screen->winsys_priv; fd remains borrowed.
 * Screen and all contexts must outlive resources/transfers and final domain
 * destroy. Callbacks serialize image operations; caller holds normal Gallium
 * references. */
int poulsback_resource_domain_create(struct pipe_screen *, int fd,
                                     struct poulsback_resource_domain **);
/* Refuses while resources, transfers or image-client cleanup objects remain. */
int poulsback_resource_domain_destroy(struct poulsback_resource_domain *);
int poulsback_resource_domain_error(struct poulsback_resource_domain *);
/* Installs synchronous level0, one-layer BGRA/XRGB texture_subdata using the
 * existing canonical image transfer/CPU access boundary. Source rows must be
 * readable/immutable for the call; strides, extents and overlap are checked.
 * Failed END retains the complete transfer/image in CPU cleanup HOLD.
 * Buffers are slab storage (poulsback_slab.h, docs/graphics.md#buffer-storage).
 * A CPU write into storage a job may still read renames it (copy on write) when
 * it discards or replaces its range; any other synchronised write map waits.
 * buffer_subdata copies only into buffers through their canonical
 * map/unmap path. Source bytes remain readable/immutable for the call; active
 * transfers, invalid spans and unsupported usage remain refused. A zero-sized
 * upload is a no-op and does not inspect the source or resource.
 * resource_copy_region supports only same-domain buffers, level0
 * and 1D byte ranges. It excludes active source/destination transfers and
 * handles same-buffer overlap, without image/GPU access. An
 * empty valid 1D box is a no-op. Failure preserves bytes and records the
 * domain's existing sticky error; the caller owns normal resource references
 * and keeps the input box stable throughout the call.
 * clear_buffer repeats an owned 1..16-byte pattern over a bounded range of a
 * canonical buffer, including a partial last element. Active
 * transfers, foreign/image/depth resources and invalid spans refuse without
 * changing bytes. A zero-size clear inspects no resource or pattern. Source
 * pattern bytes are readable/immutable for the call; overlapping buffer data
 * is copied before mutation. This has no image/cache/fence/GPU effect. */
void poulsback_resource_context_init(struct pipe_context *);
/* Driver-internal identity query. No GEM handle or GPU address escapes. */
int poulsback_resource_layout(struct pipe_resource *, uint32_t *stride, uint64_t *size);
/* The CPU buffer's own bytes for synchronous reading during one call (the
 * draw module), without a copy; refused while a transfer maps it. */
int poulsback_resource_buffer_borrow(struct pipe_resource *, const void **out, size_t *bytes);
/* Buffer storage that the open frame's job SEQUENCE
 * (poulsback_ws_render_frame_sequence) reads: busy until that job completes,
 * so a CPU write renames it (poulsback_slab.h). CPU is valid during the call's
 * draw only, like a borrow. */
struct poulsback_resource_gpu {
    uint32_t handle;
    uint64_t gpu_va;
    size_t bytes;
    const void *cpu;
};
int poulsback_resource_buffer_gpu(struct pipe_resource *, uint64_t sequence,
                                  struct poulsback_resource_gpu *out);
/* A copy of [offset, offset + bytes) only. */
int poulsback_resource_buffer_snapshot_range(struct pipe_resource *, size_t offset, size_t bytes,
                                             void **out);
/* Canonical color-image lease and rendering share the domain's exact private
 * image-client namespace. Lease must be released with ws_image_unref. */
int poulsback_resource_image_acquire(struct pipe_resource *, struct poulsback_ws_image **);
int poulsback_resource_render_create(struct pipe_screen *, struct poulsback_ws_render **);
#endif
