/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_EXEC_UAPI_H
#define POULSBACK_EXEC_UAPI_H
#include <linux/types.h>

/* Execution contexts of the GPU execution engine (docs/kernel/execution.md).
 * The first context in a boot brings the engine up; it then owns the SGX for
 * the boot. */
#define POULSBACK_EXEC_ABI 1U

struct poulsback_context_create {
    __u32 abi, flags, handle;
    __u32 reserved[5];
};
struct poulsback_context_destroy {
    __u32 abi, handle;
    __u32 reserved[2];
};

/* SUBMIT: one TA -> 3D job of `width` x `height` on a context. The BO list
 * names every buffer the job reads or writes (implicit sync through their
 * reservations). Each phase lists every register of its fixed register table
 * once (docs/kernel/execution.md); an address register names a BO by 1 + its
 * index in the list (value 0, bo_offset into the BO), a plain one has bo = 0.
 * The kernel adds the scene, DPM, cache and kick registers itself. */
#define POULSBACK_SUBMIT_BO_WRITE 1U
#define POULSBACK_SUBMIT_MAX_BOS 64U
#define POULSBACK_SUBMIT_MAX_REGS 32U
#define POULSBACK_SUBMIT_MAX_SIZE 2047U

struct poulsback_submit_bo {
    __u32 handle, flags;
};
struct poulsback_submit_reg {
    __u32 offset, value, bo, bo_offset;
};
struct poulsback_submit {
    __u32 abi, flags, context, width, height;
    __u32 bo_count, ta_reg_count, raster_reg_count;
    __u64 bos, ta_regs, raster_regs;
    __u32 in_syncobj, out_syncobj;
    __u32 reserved[4];
};

/* SUBMIT_2D: a list of fills and copies for the 2D core on a context, in
 * order. Surfaces are linear views of 2D-class BOs from the BO list (whose
 * `flags` are 0: the kernel derives reads and writes from the operations).
 * Each operation is clipped to its surfaces and, with POULSBACK_2D_CLIP, to
 * `clip` in destination coordinates; an empty result is a no-op. Fields an
 * operation does not use are 0. */
#define POULSBACK_2D_MAX_BOS 16U
#define POULSBACK_2D_MAX_SURFACES 16U
#define POULSBACK_2D_MAX_OPS 256U
/* Pixels a job may write after clipping: about half a second of the 2D core,
 * a quarter of the job timeout. A client splits larger work into jobs. */
#define POULSBACK_2D_MAX_PIXELS (1U << 25)

#define POULSBACK_2D_ARGB8888 1U
#define POULSBACK_2D_XRGB8888 2U
#define POULSBACK_2D_RGB565 3U

#define POULSBACK_2D_FILL 1U
#define POULSBACK_2D_COPY 2U
#define POULSBACK_2D_CLIP 1U

/* `offset` into the BO is a multiple of 8, `stride` a multiple of
 * POULSBACK_2D_STRIDE_ALIGN up to POULSBACK_2D_MAX_STRIDE bytes and at least a
 * row (so a 32 bpp surface is at most POULSBACK_2D_MAX_WIDTH_32BPP pixels
 * wide); width and height 1 to POULSBACK_2D_MAX_DIM; every row is inside the BO. */
#define POULSBACK_2D_STRIDE_ALIGN 128U
#define POULSBACK_2D_MAX_STRIDE 16256U
#define POULSBACK_2D_MAX_DIM 4095U
#define POULSBACK_2D_MAX_WIDTH_32BPP (POULSBACK_2D_MAX_STRIDE / 4U)
struct poulsback_2d_surface {
    __u32 bo, offset, stride, width, height, format;
    __u32 reserved[2];
};
struct poulsback_2d_rect {
    __s32 x, y;
    __u32 width, height;
};
/* `rop` is an X11 GX function (0-15) of the fill pixel or source and the
 * destination. FILL: `pixel` in the destination's format. COPY: from `src`
 * at (`src_x`, `src_y`), the same format as `dst`; one surface onto itself
 * copies as if through a temporary, two different views of one BO are refused. */
struct poulsback_2d_op {
    __u32 kind, flags, rop, dst, src, pixel;
    struct poulsback_2d_rect rect;
    __s32 src_x, src_y;
    struct poulsback_2d_rect clip;
};
struct poulsback_submit_2d {
    __u32 abi, flags, context, bo_count, surface_count, op_count;
    __u64 bos, surfaces, ops;
    __u32 in_syncobj, out_syncobj;
    __u32 reserved[4];
};

#ifdef __KERNEL__
#include <drm/drm.h>
#else
#include <drm.h>
#endif
#define DRM_IOCTL_PB_CONTEXT_CREATE DRM_IOWR(DRM_COMMAND_BASE + 48, struct poulsback_context_create)
#define DRM_IOCTL_PB_CONTEXT_DESTROY                                                               \
    DRM_IOWR(DRM_COMMAND_BASE + 49, struct poulsback_context_destroy)
#define DRM_IOCTL_PB_SUBMIT DRM_IOWR(DRM_COMMAND_BASE + 50, struct poulsback_submit)
#define DRM_IOCTL_PB_SUBMIT_2D DRM_IOWR(DRM_COMMAND_BASE + 52, struct poulsback_submit_2d)
#endif
