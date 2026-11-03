// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// Interface contract: docs/native-2d.md

#include "sgx535_2d_encoder_v1.h"

#define FENCE 0x70000000u
#define FLUSH 0xf0000000u
#define DST_ARGB8888 0xa0060000u
#define SRC_ARGB8888 0x90060000u
#define SRC_OFF 0x30000000u
/* BLIT with the GXcopy fill code and a colour word; with the GXcopy copy code
 * and bit 16 set: no colour word follows. */
#define BLIT_FILL 0x8000f0f0u
#define BLIT_COPY 0x8001ccccu
#define MAX_EXTENT 4095u
#define MAX_STRIDE 32767u
#define BYTES_PER_PIXEL 4u

/* Every limit keeps the products below 2^32. */
static enum sgx535_2d_status check_surface(const struct sgx535_2d_surface *s)
{
    uint32_t row;

    if (s->format != SGX535_2D_ARGB8888)
        return SGX535_2D_UNSUPPORTED;
    if (s->width - 1u >= MAX_EXTENT || s->height - 1u >= MAX_EXTENT ||
        s->stride_bytes - 1u >= MAX_STRIDE || s->offset_bytes >= SGX535_2D_APERTURE_BYTES ||
        !s->allocation_bytes || s->allocation_bytes > SGX535_2D_APERTURE_BYTES - s->offset_bytes)
        return SGX535_2D_OUT_OF_RANGE;
    if ((s->offset_bytes & 7u) || (s->stride_bytes & 127u))
        return SGX535_2D_BAD_ALIGNMENT;
    row = s->width * BYTES_PER_PIXEL;
    if (row > s->stride_bytes || (s->height - 1u) * s->stride_bytes + row > s->allocation_bytes)
        return SGX535_2D_OUT_OF_RANGE;
    return SGX535_2D_OK;
}

static int rect_inside(const struct sgx535_2d_surface *s, const struct sgx535_2d_rect *r)
{
    return r->width && r->height && r->x < s->width && r->width <= s->width - r->x &&
           r->y < s->height && r->height <= s->height - r->y;
}

static uint32_t pair(uint32_t a, uint32_t b)
{
    return (a << 12) | b;
}

/* Bytes from the rectangle's first pixel to just past its last row's end. */
static void touched(const struct sgx535_2d_surface *s, const struct sgx535_2d_rect *r,
                    uint64_t *first, uint64_t *end)
{
    *first = s->offset_bytes + r->y * s->stride_bytes + r->x * BYTES_PER_PIXEL;
    *end = s->offset_bytes + (r->y + r->height - 1u) * s->stride_bytes +
           (r->x + r->width) * BYTES_PER_PIXEL;
}

enum sgx535_2d_status sgx535_2d_build_fill(const struct sgx535_2d_surface *dst,
                                           const struct sgx535_2d_rect *rect, uint32_t argb8888,
                                           uint32_t *out, size_t capacity, size_t *written)
{
    enum sgx535_2d_status status;

    if (!dst || !rect || !out || !written)
        return SGX535_2D_INVALID_ARGUMENT;
    if (capacity < SGX535_2D_FILL_WORDS)
        return SGX535_2D_NO_SPACE;
    status = check_surface(dst);
    if (status != SGX535_2D_OK)
        return status;
    if (!rect_inside(dst, rect))
        return SGX535_2D_OUT_OF_RANGE;
    out[0] = FENCE;
    out[1] = DST_ARGB8888 | dst->stride_bytes;
    out[2] = (uint32_t)dst->offset_bytes;
    out[3] = BLIT_FILL;
    out[4] = argb8888;
    out[5] = pair(rect->x, rect->y);
    out[6] = pair(rect->width, rect->height);
    out[7] = FLUSH;
    *written = SGX535_2D_FILL_WORDS;
    return SGX535_2D_OK;
}

enum sgx535_2d_status sgx535_2d_build_copy(const struct sgx535_2d_surface *src,
                                           const struct sgx535_2d_rect *src_rect,
                                           const struct sgx535_2d_surface *dst,
                                           const struct sgx535_2d_rect *dst_rect, uint32_t *out,
                                           size_t capacity, size_t *written)
{
    enum sgx535_2d_status status;
    uint64_t src_first, src_end, dst_first, dst_end;

    if (!src || !src_rect || !dst || !dst_rect || !out || !written)
        return SGX535_2D_INVALID_ARGUMENT;
    if (capacity < SGX535_2D_COPY_WORDS)
        return SGX535_2D_NO_SPACE;
    status = check_surface(src);
    if (status == SGX535_2D_OK)
        status = check_surface(dst);
    if (status != SGX535_2D_OK)
        return status;
    if (!rect_inside(src, src_rect) || !rect_inside(dst, dst_rect))
        return SGX535_2D_OUT_OF_RANGE;
    if (src_rect->width != dst_rect->width || src_rect->height != dst_rect->height ||
        src_rect->width == 8u || src_rect->height == 8u)
        return SGX535_2D_UNSUPPORTED;
    touched(src, src_rect, &src_first, &src_end);
    touched(dst, dst_rect, &dst_first, &dst_end);
    if (src_first < dst_end && dst_first < src_end)
        return SGX535_2D_OVERLAP;
    out[0] = FENCE;
    out[1] = DST_ARGB8888 | dst->stride_bytes;
    out[2] = (uint32_t)dst->offset_bytes;
    out[3] = SRC_ARGB8888 | src->stride_bytes;
    out[4] = (uint32_t)src->offset_bytes;
    out[5] = SRC_OFF | pair(src_rect->x, src_rect->y);
    out[6] = BLIT_COPY;
    out[7] = pair(dst_rect->x, dst_rect->y);
    out[8] = pair(dst_rect->width, dst_rect->height);
    out[9] = FLUSH;
    *written = SGX535_2D_COPY_WORDS;
    return SGX535_2D_OK;
}
