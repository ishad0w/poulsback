// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// Interface contract: docs/native-2d.md

#ifndef SGX535_2D_ENCODER_V1_H
#define SGX535_2D_ENCODER_V1_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* The older, narrower interface (spec section 10): ARGB8888 GXcopy fills and
 * forward copies, no clipping, no storage identity. No product caller; the
 * general encoder is sgx535_2d_encoder.h. Its width-8 refusal and stride limit
 * of 32,767 predate HW-2D-001/002. */
#define SGX535_2D_FILL_WORDS 8u
#define SGX535_2D_COPY_WORDS 10u
#define SGX535_2D_APERTURE_BYTES 0x10000000ull

enum sgx535_2d_format { SGX535_2D_ARGB8888 = 1 };

enum sgx535_2d_status {
    SGX535_2D_OK = 0,
    SGX535_2D_INVALID_ARGUMENT = 1,
    SGX535_2D_UNSUPPORTED = 2,
    SGX535_2D_OUT_OF_RANGE = 3,
    SGX535_2D_BAD_ALIGNMENT = 4,
    SGX535_2D_OVERLAP = 5,
    SGX535_2D_NO_SPACE = 6
};

struct sgx535_2d_surface {
    uint64_t offset_bytes;
    uint64_t allocation_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    enum sgx535_2d_format format;
};

/* Nonempty and wholly inside its surface, else OUT_OF_RANGE. */
struct sgx535_2d_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/* Capacity is checked before anything else but null pointers. On OK exactly
 * SGX535_2D_FILL_WORDS (or _COPY_WORDS) words are written; on any error out
 * and *written are unchanged. */
enum sgx535_2d_status sgx535_2d_build_fill(const struct sgx535_2d_surface *dst,
                                           const struct sgx535_2d_rect *rect, uint32_t argb8888,
                                           uint32_t *out, size_t capacity, size_t *written);

/* Copies 8 wide or high and copies whose touched bytes overlap are refused. */
enum sgx535_2d_status sgx535_2d_build_copy(const struct sgx535_2d_surface *src,
                                           const struct sgx535_2d_rect *src_rect,
                                           const struct sgx535_2d_surface *dst,
                                           const struct sgx535_2d_rect *dst_rect, uint32_t *out,
                                           size_t capacity, size_t *written);

#endif
