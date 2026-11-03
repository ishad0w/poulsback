// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// Interface contract: docs/native-2d.md

#include "sgx535_2d_encoder.h"

/* Block heads (bits 31:28). */
#define FENCE 0x70000000u
#define FLUSH 0xf0000000u
#define DST_SURF 0xa0000000u
#define SRC_SURF 0x90000000u
#define SRC_OFF 0x30000000u
#define BLIT 0x80000000u
/* A copy's colour word. */
#define COPY_COLOUR 0xffffffffu
/* Positions and sizes are 12-bit fields. */
#define MAX_EXTENT 4095u
/* The project's allocation policy, not a hardware minimum. */
#define OFFSET_ALIGN 8u
#define STRIDE_ALIGN 128u
/* Ternary ROP operand: pattern (bit 2 of the code index) or source (bit 1). */
#define PATTERN 2u
#define SOURCE 1u

/* A clipped command: starts and the common size, all inside 12 bits. */
struct geometry {
    uint32_t dst_x, dst_y, src_x, src_y, width, height;
};

/* Parameter range [lo, hi) of one axis of the rectangle. */
struct axis {
    int64_t lo, hi;
};

/* Surface word format code (bits 18:15), or 0 for an unknown format. */
static uint32_t format_code(enum sgx535_2d_raster_format format)
{
    switch (format) {
    case SGX535_2D_RASTER_ARGB8888:
        return 12u << 15;
    case SGX535_2D_RASTER_XRGB8888:
        return 11u << 15;
    case SGX535_2D_RASTER_RGB565:
        return 10u << 15;
    }
    return 0;
}

static uint32_t bytes_per_pixel(enum sgx535_2d_raster_format format)
{
    return format == SGX535_2D_RASTER_RGB565 ? 2u : 4u;
}

/* Every limit keeps the products below 2^32. */
static enum sgx535_2d_raster_status check_surface(const struct sgx535_2d_raster_surface *s)
{
    uint32_t row;

    if (!s->storage_id)
        return SGX535_2D_RASTER_UNPROVEN_ALIAS;
    if (!format_code(s->format))
        return SGX535_2D_RASTER_UNSUPPORTED;
    if (s->width - 1u >= MAX_EXTENT || s->height - 1u >= MAX_EXTENT ||
        s->stride_bytes - 1u >= SGX535_2D_RASTER_MAX_STRIDE ||
        s->offset_bytes >= SGX535_2D_RASTER_ADDRESS_BYTES || !s->allocation_bytes ||
        s->allocation_bytes > SGX535_2D_RASTER_ADDRESS_BYTES - s->offset_bytes)
        return SGX535_2D_RASTER_OUT_OF_RANGE;
    if ((s->offset_bytes & (OFFSET_ALIGN - 1u)) || (s->stride_bytes & (STRIDE_ALIGN - 1u)))
        return SGX535_2D_RASTER_BAD_ALIGNMENT;
    row = s->width * bytes_per_pixel(s->format);
    if (row > s->stride_bytes || (s->height - 1u) * s->stride_bytes + row > s->allocation_bytes)
        return SGX535_2D_RASTER_OUT_OF_RANGE;
    return SGX535_2D_RASTER_OK;
}

/* One object is one view; different objects keep their allocations apart. */
static int identity_ok(const struct sgx535_2d_raster_surface *src,
                       const struct sgx535_2d_raster_surface *dst)
{
    if (src->storage_id == dst->storage_id)
        return src->offset_bytes == dst->offset_bytes &&
               src->allocation_bytes == dst->allocation_bytes && src->width == dst->width &&
               src->height == dst->height && src->stride_bytes == dst->stride_bytes;
    return src->offset_bytes + src->allocation_bytes <= dst->offset_bytes ||
           dst->offset_bytes + dst->allocation_bytes <= src->offset_bytes;
}

/* Keep origin + t inside [first, end). */
static void keep_inside(struct axis *a, int64_t origin, int64_t first, int64_t end)
{
    if (first - origin > a->lo)
        a->lo = first - origin;
    if (end - origin < a->hi)
        a->hi = end - origin;
}

/* Clip dst_rect to the destination, src_rect (when src is given) to the
 * source and both to the destination-space clip, keeping their displacement.
 * Returns 0 when nothing is left. */
static int clip_geometry(const struct sgx535_2d_raster_surface *src,
                         const struct sgx535_2d_raster_rect *src_rect,
                         const struct sgx535_2d_raster_surface *dst,
                         const struct sgx535_2d_raster_rect *dst_rect,
                         const struct sgx535_2d_raster_rect *clip, struct geometry *g)
{
    struct axis x = {0, dst_rect->width}, y = {0, dst_rect->height};

    keep_inside(&x, dst_rect->x, 0, dst->width);
    keep_inside(&y, dst_rect->y, 0, dst->height);
    if (src) {
        keep_inside(&x, src_rect->x, 0, src->width);
        keep_inside(&y, src_rect->y, 0, src->height);
    }
    if (clip) {
        keep_inside(&x, dst_rect->x, clip->x, (int64_t)clip->x + clip->width);
        keep_inside(&y, dst_rect->y, clip->y, (int64_t)clip->y + clip->height);
    }
    if (x.lo >= x.hi || y.lo >= y.hi)
        return 0;
    g->dst_x = (uint32_t)(dst_rect->x + x.lo);
    g->dst_y = (uint32_t)(dst_rect->y + y.lo);
    g->src_x = src ? (uint32_t)(src_rect->x + x.lo) : 0u;
    g->src_y = src ? (uint32_t)(src_rect->y + y.lo) : 0u;
    g->width = (uint32_t)(x.hi - x.lo);
    g->height = (uint32_t)(y.hi - y.lo);
    return 1;
}

/* Position (x, y) or size (width, height) word. */
static uint32_t pair(uint32_t a, uint32_t b)
{
    return (a << 12) | b;
}

static uint32_t surface_word(uint32_t head, const struct sgx535_2d_raster_surface *s)
{
    return head | format_code(s->format) | s->stride_bytes;
}

/* Bit i of the code is GX function g applied to the operand bit
 * (i >> operand) & 1 and the destination bit i & 1. */
static uint32_t ternary_rop(enum sgx535_2d_raster_rop rop, uint32_t operand)
{
    uint32_t g = (uint32_t)rop, code = 0, i;

    for (i = 0; i < 8u; i++) {
        uint32_t s = (i >> operand) & 1u, d = i & 1u;
        code |= ((g >> (2u * (1u - s) + (1u - d))) & 1u) << i;
    }
    return code;
}

static uint32_t blit_word(uint32_t order, uint32_t code)
{
    return BLIT | (order << 23) | (code << 8) | code;
}

/* RGB565 field widened to 8 bits; the vacated low bits copy its lowest bit. */
static uint32_t widen(uint32_t field, uint32_t bits)
{
    uint32_t pad = 8u - bits;

    return (field << pad) | ((field & 1u) ? (1u << pad) - 1u : 0u);
}

/* The colour word is A8R8G8B8 whatever the destination format. */
static uint32_t fill_colour(enum sgx535_2d_raster_format format, uint32_t pixel)
{
    if (format == SGX535_2D_RASTER_ARGB8888)
        return pixel;
    if (format == SGX535_2D_RASTER_XRGB8888)
        return pixel | 0xff000000u;
    return 0xff000000u | (widen(pixel >> 11, 5) << 16) | (widen((pixel >> 5) & 63u, 6) << 8) |
           widen(pixel & 31u, 5);
}

enum sgx535_2d_raster_status sgx535_2d_raster_fill(const struct sgx535_2d_raster_surface *dst,
                                                   const struct sgx535_2d_raster_rect *rect,
                                                   const struct sgx535_2d_raster_rect *clip,
                                                   uint32_t pixel, enum sgx535_2d_raster_rop rop,
                                                   uint32_t *out, size_t capacity, size_t *written)
{
    enum sgx535_2d_raster_status status;
    struct geometry g;
    uint32_t code;

    if (!dst || !rect || !out || !written)
        return SGX535_2D_RASTER_INVALID_ARGUMENT;
    if ((uint32_t)rop > (uint32_t)SGX535_2D_RASTER_SET)
        return SGX535_2D_RASTER_UNSUPPORTED;
    status = check_surface(dst);
    if (status != SGX535_2D_RASTER_OK)
        return status;
    if (dst->format == SGX535_2D_RASTER_RGB565 && pixel > 0xffffu)
        return SGX535_2D_RASTER_OUT_OF_RANGE;
    if (!clip_geometry(NULL, NULL, dst, rect, clip, &g)) {
        *written = 0;
        return SGX535_2D_RASTER_OK;
    }
    if (capacity < SGX535_2D_RASTER_FILL_WORDS)
        return SGX535_2D_RASTER_NO_SPACE;
    code = ternary_rop(rop, PATTERN);
    out[0] = FENCE;
    out[1] = surface_word(DST_SURF, dst);
    out[2] = (uint32_t)dst->offset_bytes;
    out[3] = blit_word(0, code);
    out[4] = fill_colour(dst->format, pixel);
    out[5] = pair(g.dst_x, g.dst_y);
    out[6] = pair(g.width, g.height);
    out[7] = FLUSH;
    *written = SGX535_2D_RASTER_FILL_WORDS;
    return SGX535_2D_RASTER_OK;
}

enum sgx535_2d_raster_status sgx535_2d_raster_copy(const struct sgx535_2d_raster_surface *src,
                                                   const struct sgx535_2d_raster_rect *src_rect,
                                                   const struct sgx535_2d_raster_surface *dst,
                                                   const struct sgx535_2d_raster_rect *dst_rect,
                                                   const struct sgx535_2d_raster_rect *clip,
                                                   enum sgx535_2d_raster_rop rop, uint32_t *out,
                                                   size_t capacity, size_t *written)
{
    enum sgx535_2d_raster_status status;
    struct geometry g;
    uint32_t code, order = 0;

    if (!src || !src_rect || !dst || !dst_rect || !out || !written)
        return SGX535_2D_RASTER_INVALID_ARGUMENT;
    if ((uint32_t)rop > (uint32_t)SGX535_2D_RASTER_SET)
        return SGX535_2D_RASTER_UNSUPPORTED;
    status = check_surface(src);
    if (status == SGX535_2D_RASTER_OK)
        status = check_surface(dst);
    if (status != SGX535_2D_RASTER_OK)
        return status;
    if (src->format != dst->format || src_rect->width != dst_rect->width ||
        src_rect->height != dst_rect->height)
        return SGX535_2D_RASTER_UNSUPPORTED;
    if (!identity_ok(src, dst))
        return SGX535_2D_RASTER_UNPROVEN_ALIAS;
    if (!clip_geometry(src, src_rect, dst, dst_rect, clip, &g)) {
        *written = 0;
        return SGX535_2D_RASTER_OK;
    }
    if (capacity < SGX535_2D_RASTER_COPY_WORDS)
        return SGX535_2D_RASTER_NO_SPACE;
    /* Inside one view, run against the displacement from the far corner:
     * COPYORDER 1 reverses x and y, 2 only x, 3 only y. */
    if (src->storage_id == dst->storage_id) {
        int reverse_x = g.src_x < g.dst_x, reverse_y = g.src_y < g.dst_y;

        if (reverse_x) {
            g.src_x += g.width - 1u;
            g.dst_x += g.width - 1u;
        }
        if (reverse_y) {
            g.src_y += g.height - 1u;
            g.dst_y += g.height - 1u;
        }
        order = reverse_x ? (reverse_y ? 1u : 2u) : (reverse_y ? 3u : 0u);
    }
    code = ternary_rop(rop, SOURCE);
    out[0] = FENCE;
    out[1] = surface_word(DST_SURF, dst);
    out[2] = (uint32_t)dst->offset_bytes;
    out[3] = surface_word(SRC_SURF, src);
    out[4] = (uint32_t)src->offset_bytes;
    out[5] = SRC_OFF | pair(g.src_x, g.src_y);
    out[6] = blit_word(order, code);
    out[7] = COPY_COLOUR;
    out[8] = pair(g.dst_x, g.dst_y);
    out[9] = pair(g.width, g.height);
    out[10] = FLUSH;
    *written = SGX535_2D_RASTER_COPY_WORDS;
    return SGX535_2D_RASTER_OK;
}
