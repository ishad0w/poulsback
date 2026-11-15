/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_VIDEO_UAPI_H
#define POULSBACK_VIDEO_UAPI_H
#include <linux/types.h>

#define POULSBACK_VIDEO_ABI 1U
#define POULSBACK_MPEG2_PROGRESSIVE_SEQUENCE 1U
#define POULSBACK_MPEG2_MAX_DIMENSION 2048U
#define POULSBACK_MPEG2_MAX_SLICES 2048U
#define POULSBACK_MPEG2_MAX_BITSTREAM (4U << 20)

/* One complete slice, including its start code. Offsets count bytes except
 * macroblock_offset, which counts bits from the slice's first byte. */
struct poulsback_mpeg2_slice {
    __u32 offset, bytes, macroblock_offset, vertical_position;
    __u32 quantiser_scale_code, reserved;
};

/* Synchronous MPEG-2 4:2:0 frame-picture decode. Output and references are
 * native GEM BOs with the same NV12 layout: Y at zero, UV at pitch *
 * ALIGN(height, 32). Pitch is 512, 1024, 1280 or 2048. A successful return
 * means device completion and CPU visibility, not display presentation.
 * coding_extension uses the H.262/libva picture bit layout documented in
 * docs/video-protocol.md; picture_structure must be frame and first_field 1.
 * Matrices are raster order. No raw commands or device addresses enter here. */
struct poulsback_mpeg2_decode {
    __u32 abi, flags, width, height;
    __u32 output, forward_reference, backward_reference, pitch;
    __u32 coding_type, f_code, coding_extension, slice_count;
    __aligned_u64 slices, bitstream;
    __u32 bitstream_bytes, reserved[3];
    __u8 intra_matrix[64], non_intra_matrix[64];
};

#ifdef __KERNEL__
#include <drm/drm.h>
#else
#include <drm.h>
#endif
#define DRM_IOCTL_PB_MPEG2_DECODE DRM_IOWR(DRM_COMMAND_BASE + 53, struct poulsback_mpeg2_decode)
#endif
