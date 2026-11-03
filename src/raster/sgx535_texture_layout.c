// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_texture_layout.h"

#include <string.h>

/* Per-texel work is one table lookup per axis: each axis's twiddle (or
 * stride) offsets are built once per call, for the box only, by the
 * scatter-increment ((v | ~mask) + 1) & mask. Rows dispatch on the conversion
 * once; the inner loops are straight lines of loads and stores. */

enum conversion {
    CV_NONE,
    CV_COPY8,
    CV_COPY16,
    CV_COPY32,
    CV_COPY64,
    CV_COPY128,
    CV_SWAP_RB,        /* R8G8B8A8 <-> B8G8R8A8 */
    CV_SWAP_RB_OPAQUE, /* R8G8B8X8 -> B8G8R8A8 */
    CV_OPAQUE,         /* B8G8R8X8 -> B8G8R8A8 */
    CV_ROTR4,          /* A4B4G4R4 -> A4R4G4B4 */
    CV_ROTL4,
    CV_ROTR1, /* A1B5G5R5 -> A1R5G5B5 */
    CV_ROTL1,
    CV_Z24_F32,
    CV_F32_Z24,
    CV_PLANE32,   /* one 32-bit plane of a wider texel */
    CV_565_RGBA8, /* readback expansions by bit replication */
    CV_565_BGRA8,
    CV_4444_RGBA8,
    CV_4444_BGRA8,
    CV_1555_RGBA8,
    CV_1555_BGRA8
};

struct texel_info {
    uint8_t hw_format; /* enum sgx535_texture_hw_format */
    uint8_t bytes;     /* linear bytes per element (texel, or DXT block) */
    uint8_t planes;
    uint8_t element; /* stored bytes per element of one plane */
    uint8_t block;
    uint8_t depth; /* a depth format: stride 2D only */
    uint8_t up;    /* enum conversion: linear -> stored */
    uint8_t down;  /* stored -> linear, exact */
};

static const struct texel_info texels[SGX535_TEXEL_FORMAT_COUNT] = {
    [SGX535_TEXEL_R8G8B8A8_UNORM] = {SGX535_TEXFORMAT_B8G8R8A8, 4, 1, 4, 1, 0, CV_SWAP_RB,
                                     CV_SWAP_RB},
    [SGX535_TEXEL_B8G8R8A8_UNORM] = {SGX535_TEXFORMAT_B8G8R8A8, 4, 1, 4, 1, 0, CV_COPY32,
                                     CV_COPY32},
    [SGX535_TEXEL_R8G8B8X8_UNORM] = {SGX535_TEXFORMAT_B8G8R8A8, 4, 1, 4, 1, 0, CV_SWAP_RB_OPAQUE,
                                     CV_SWAP_RB},
    [SGX535_TEXEL_B8G8R8X8_UNORM] = {SGX535_TEXFORMAT_B8G8R8A8, 4, 1, 4, 1, 0, CV_OPAQUE,
                                     CV_COPY32},
    [SGX535_TEXEL_B5G6R5_UNORM] = {SGX535_TEXFORMAT_R5G6B5, 2, 1, 2, 1, 0, CV_COPY16, CV_COPY16},
    [SGX535_TEXEL_B4G4R4A4_UNORM] = {SGX535_TEXFORMAT_A4R4G4B4, 2, 1, 2, 1, 0, CV_COPY16,
                                     CV_COPY16},
    [SGX535_TEXEL_A4B4G4R4_UNORM] = {SGX535_TEXFORMAT_A4R4G4B4, 2, 1, 2, 1, 0, CV_ROTR4, CV_ROTL4},
    [SGX535_TEXEL_B5G5R5A1_UNORM] = {SGX535_TEXFORMAT_A1R5G5B5, 2, 1, 2, 1, 0, CV_COPY16,
                                     CV_COPY16},
    [SGX535_TEXEL_A1B5G5R5_UNORM] = {SGX535_TEXFORMAT_A1R5G5B5, 2, 1, 2, 1, 0, CV_ROTR1, CV_ROTL1},
    [SGX535_TEXEL_U8] = {SGX535_TEXFORMAT_U8, 1, 1, 1, 1, 0, CV_COPY8, CV_COPY8},
    [SGX535_TEXEL_U8U8] = {SGX535_TEXFORMAT_U88, 2, 1, 2, 1, 0, CV_COPY16, CV_COPY16},
    [SGX535_TEXEL_Z16_UNORM] = {SGX535_TEXFORMAT_U16, 2, 1, 2, 1, 1, CV_COPY16, CV_COPY16},
    [SGX535_TEXEL_Z24X8_UNORM] = {SGX535_TEXFORMAT_F32, 4, 1, 4, 1, 1, CV_Z24_F32, CV_F32_Z24},
    [SGX535_TEXEL_Z32_FLOAT] = {SGX535_TEXFORMAT_F32, 4, 1, 4, 1, 1, CV_COPY32, CV_COPY32},
    [SGX535_TEXEL_R16G16B16A16_FLOAT] = {SGX535_TEXFORMAT_F1616, 8, 2, 4, 1, 0, CV_PLANE32,
                                         CV_PLANE32},
    [SGX535_TEXEL_R32G32B32A32_FLOAT] = {SGX535_TEXFORMAT_F32, 16, 4, 4, 1, 0, CV_PLANE32,
                                         CV_PLANE32},
    [SGX535_TEXEL_DXT1] = {SGX535_TEXFORMAT_DXT1, 8, 1, 8, 4, 0, CV_COPY64, CV_COPY64},
    [SGX535_TEXEL_DXT3] = {SGX535_TEXFORMAT_DXT3, 16, 1, 16, 4, 0, CV_COPY128, CV_COPY128},
    [SGX535_TEXEL_DXT5] = {SGX535_TEXFORMAT_DXT5, 16, 1, 16, 4, 0, CV_COPY128, CV_COPY128},
};

static const struct texel_info *texel(enum sgx535_texel_format format)
{
    if ((unsigned)format >= SGX535_TEXEL_FORMAT_COUNT || !texels[format].bytes)
        return NULL;
    return &texels[format];
}

static bool pot(uint32_t v)
{
    return v && !(v & (v - 1));
}

static uint32_t log2u(uint32_t v)
{
    uint32_t n = 0;
    while (v >>= 1)
        n++;
    return n;
}

static uint32_t minify(uint32_t size, uint32_t level)
{
    size >>= level;
    return size ? size : 1;
}

static uint32_t blocks(uint32_t texels_, uint32_t block)
{
    return (texels_ + block - 1) / block;
}

enum sgx535_texture_status
sgx535_texture_layout_init(struct sgx535_texture_layout *layout, enum sgx535_texture_target target,
                           enum sgx535_texel_format format, uint32_t width, uint32_t height,
                           uint32_t depth, uint32_t last_level, bool stride)
{
    const struct texel_info *t = texel(format);
    uint32_t limit =
        target == SGX535_TEXTURE_3D ? SGX535_TEXTURE_MAX_SIZE_3D : SGX535_TEXTURE_MAX_SIZE;
    uint32_t largest, chain, level, offset = 0;
    struct sgx535_texture_layout l;

    if (!t || !width || !height || !depth || width > limit || height > limit || depth > limit)
        return SGX535_TEXTURE_INVALID;
    if (target != SGX535_TEXTURE_3D && depth != 1)
        return SGX535_TEXTURE_INVALID;
    if (target == SGX535_TEXTURE_CUBE && width != height)
        return SGX535_TEXTURE_INVALID;
    if (target == SGX535_TEXTURE_2D && (!pot(width) || !pot(height) || t->depth))
        stride = true;
    if (stride && (target != SGX535_TEXTURE_2D || t->block > 1))
        return SGX535_TEXTURE_INVALID;
    if (!stride && (!pot(width) || !pot(height) || !pot(depth) || t->depth))
        return SGX535_TEXTURE_INVALID;
    largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    chain = log2u(largest) + 1;
    if (last_level >= chain)
        return SGX535_TEXTURE_INVALID;

    memset(&l, 0, sizeof(l));
    l.target = target;
    l.format = format;
    l.type = stride                          ? SGX535_TEXTYPE_STRIDE
             : target == SGX535_TEXTURE_CUBE ? SGX535_TEXTYPE_CEM
             : target == SGX535_TEXTURE_3D   ? SGX535_TEXTYPE_3D
                                             : SGX535_TEXTYPE_2D;
    l.hw_format = (enum sgx535_texture_hw_format)t->hw_format;
    l.chan_replicate = t->hw_format == SGX535_TEXFORMAT_U8 || t->hw_format == SGX535_TEXFORMAT_U88;
    l.width = width;
    l.height = height;
    l.depth = depth;
    l.levels = last_level ? chain : 1;
    l.faces = target == SGX535_TEXTURE_CUBE ? 6 : 1;
    l.planes = t->planes;
    l.block = t->block;
    l.element_bytes = t->element;
    /* Below 2^30: at most 4 bytes per texel and plane, 2048^2 x 4/3 x 6 faces x 4 planes. */
    for (level = 0; level < l.levels; level++) {
        uint32_t gx = blocks(minify(width, level), t->block);
        uint32_t gy = blocks(minify(height, level), t->block);
        uint32_t gz = target == SGX535_TEXTURE_3D ? minify(depth, level) : 1;
        if (stride) {
            l.level_pitch[level] = ((gx + 31) & ~31u) * t->element;
            l.level_bytes[level] = l.level_pitch[level] * gy;
        } else {
            l.level_bytes[level] = gx * gy * gz * t->element;
        }
        l.level_offset[level] = offset;
        offset += l.level_bytes[level];
    }
    l.face_bytes = offset;
    l.plane_bytes = (offset * l.faces + 63) & ~63u;
    l.size = l.plane_bytes * l.planes;
    *layout = l;
    return SGX535_TEXTURE_OK;
}

uint32_t sgx535_texture_offset(const struct sgx535_texture_layout *layout, uint32_t plane,
                               uint32_t face, uint32_t level)
{
    return plane * layout->plane_bytes + face * layout->face_bytes + layout->level_offset[level];
}

enum sgx535_texture_status sgx535_texture_layout_border(struct sgx535_texture_layout *layout)
{
    if (!layout || layout->target != SGX535_TEXTURE_2D || layout->type == SGX535_TEXTYPE_STRIDE ||
        layout->width != layout->height || layout->levels != 1 || layout->planes != 1 ||
        layout->block != 1 || layout->border_bytes)
        return SGX535_TEXTURE_INVALID;
    layout->border_bytes = (48u + 8u * layout->width) * layout->element_bytes;
    layout->level_offset[0] += layout->border_bytes;
    layout->face_bytes += layout->border_bytes;
    layout->plane_bytes += layout->border_bytes;
    layout->size += layout->border_bytes;
    return SGX535_TEXTURE_OK;
}

enum sgx535_texture_status sgx535_texture_border_fill(const struct sgx535_texture_layout *layout,
                                                      void *storage, const void *element)
{
    uint8_t *area = storage;
    uint32_t n, first[2], count[2] = {4, 0};
    if (!layout || !storage || !element || !layout->border_bytes)
        return SGX535_TEXTURE_INVALID;
    n = layout->width;
    first[0] = 4u * log2u(n);
    first[1] = 52u + 4u * (n - 1u), count[1] = 4u * n;
    for (unsigned k = 0; k < 2; k++)
        for (uint32_t t = first[k]; t < first[k] + count[k]; t++)
            memcpy(area + (size_t)t * layout->element_bytes, element, layout->element_bytes);
    return SGX535_TEXTURE_OK;
}

/* --- addressing ------------------------------------------------------------ */

struct region {
    uint8_t *base; /* plane 0, level, the box's first face or slice excluded */
    uint32_t count[3];
    uint32_t tx[SGX535_TEXTURE_MAX_SIZE];
    uint32_t ty[SGX535_TEXTURE_MAX_SIZE];
    uint32_t tz[SGX535_TEXTURE_MAX_SIZE_3D];
};

/* The element offsets start, start + 1, ... scattered to mask's bits, times
 * the element size (a power of two). */
static void scatter(uint32_t *t, uint32_t start, uint32_t count, uint32_t mask, uint32_t shift)
{
    uint32_t v = 0, bit, i;
    for (bit = 1, i = start; i && bit; bit <<= 1) {
        if (mask & bit) {
            v |= i & 1 ? bit : 0;
            i >>= 1;
        }
    }
    for (i = 0; i < count; i++) {
        t[i] = v << shift;
        v = ((v | ~mask) + 1) & mask;
    }
}

static void linear(uint32_t *t, uint32_t start, uint32_t count, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        t[i] = (start + i) * step;
}

static bool region(const struct sgx535_texture_layout *l, uint8_t *storage, uint32_t level,
                   const struct sgx535_texture_box *box, struct region *r)
{
    uint32_t lw, lh, ld, gx, gy, gz, x0, y0, shift;

    if (level >= l->levels || !box->width || !box->height || !box->depth)
        return false;
    lw = minify(l->width, level);
    lh = minify(l->height, level);
    ld = l->target == SGX535_TEXTURE_3D ? minify(l->depth, level) : l->faces;
    if (box->x >= lw || box->width > lw - box->x || box->y >= lh || box->height > lh - box->y ||
        box->z >= ld || box->depth > ld - box->z)
        return false;
    if (l->block > 1 && (box->x % l->block || box->y % l->block ||
                         (box->width % l->block && box->x + box->width != lw) ||
                         (box->height % l->block && box->y + box->height != lh)))
        return false;
    gx = blocks(lw, l->block);
    gy = blocks(lh, l->block);
    gz = l->target == SGX535_TEXTURE_3D ? ld : 1;
    x0 = box->x / l->block;
    y0 = box->y / l->block;
    r->count[0] = blocks(box->width, l->block);
    r->count[1] = blocks(box->height, l->block);
    r->count[2] = box->depth;
    r->base = storage + l->level_offset[level];
    shift = log2u(l->element_bytes);
    if (l->type == SGX535_TEXTYPE_STRIDE) {
        linear(r->tx, x0, r->count[0], l->element_bytes);
        linear(r->ty, y0, r->count[1], l->level_pitch[level]);
    } else {
        /* Bits from the lowest: y, x, z in turn, an axis dropping out when its
         * bits run out. */
        uint32_t lx = log2u(gx), ly = log2u(gy), lz = log2u(gz), mx = 0, my = 0, mz = 0;
        uint32_t i, pos = 0;
        for (i = 0; i < 12; i++) {
            if (i < ly)
                my |= 1u << pos++;
            if (i < lx)
                mx |= 1u << pos++;
            if (i < lz)
                mz |= 1u << pos++;
        }
        scatter(r->tx, x0, r->count[0], mx, shift);
        scatter(r->ty, y0, r->count[1], my, shift);
        if (l->target == SGX535_TEXTURE_3D)
            scatter(r->tz, box->z, r->count[2], mz, shift);
    }
    if (l->target != SGX535_TEXTURE_3D)
        linear(r->tz, box->z, r->count[2], l->face_bytes);
    return true;
}

/* --- conversions --------------------------------------------------------- */

static inline uint16_t ld16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t ld32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void st16(uint8_t *p, uint32_t v)
{
    uint16_t w = (uint16_t)v;
    memcpy(p, &w, sizeof(w));
}

static inline void st32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static inline uint32_t swap_rb(uint32_t v)
{
    return (v & 0xff00ff00u) | (v >> 16 & 0xffu) | (v & 0xffu) << 16;
}

static inline uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool bgra)
{
    return bgra ? b | g << 8 | r << 16 | a << 24 : r | g << 8 | b << 16 | a << 24;
}

static inline uint32_t from565(uint32_t v, bool bgra)
{
    uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    return rgba8(r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255, bgra);
}

static inline uint32_t from4444(uint32_t v, bool bgra)
{
    return rgba8((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 15) * 17, bgra);
}

static inline uint32_t from1555(uint32_t v, bool bgra)
{
    uint32_t r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
    return rgba8(r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, v >> 15 ? 255 : 0, bgra);
}

static inline uint32_t z24_f32(uint32_t v)
{
    float f = (float)((double)(v & 0xffffffu) * (1.0 / 16777215.0));
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline uint32_t f32_z24(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    if (!(f > 0.0f))
        return 0;
    if (f >= 1.0f)
        return 0xffffffu;
    return (uint32_t)((double)f * 16777215.0 + 0.5);
}

/* Linear src (element step bytes, plane offset off) -> stored elements at d + t[x]. */
static void put_row(enum conversion cv, uint8_t *d, const uint32_t *t, const uint8_t *s, uint32_t n,
                    uint32_t step, uint32_t off)
{
    uint32_t x, v;
    switch (cv) {
    case CV_COPY8:
        for (x = 0; x < n; x++)
            d[t[x]] = s[x];
        break;
    case CV_COPY16:
        for (x = 0; x < n; x++)
            st16(d + t[x], ld16(s + 2 * x));
        break;
    case CV_COPY32:
        for (x = 0; x < n; x++)
            st32(d + t[x], ld32(s + 4 * x));
        break;
    case CV_COPY64:
        for (x = 0; x < n; x++)
            memcpy(d + t[x], s + 8 * x, 8);
        break;
    case CV_COPY128:
        for (x = 0; x < n; x++)
            memcpy(d + t[x], s + 16 * x, 16);
        break;
    case CV_SWAP_RB:
        for (x = 0; x < n; x++)
            st32(d + t[x], swap_rb(ld32(s + 4 * x)));
        break;
    case CV_SWAP_RB_OPAQUE:
        for (x = 0; x < n; x++)
            st32(d + t[x], swap_rb(ld32(s + 4 * x)) | 0xff000000u);
        break;
    case CV_OPAQUE:
        for (x = 0; x < n; x++)
            st32(d + t[x], ld32(s + 4 * x) | 0xff000000u);
        break;
    case CV_ROTR4:
        for (x = 0; x < n; x++) {
            v = ld16(s + 2 * x);
            st16(d + t[x], v >> 4 | v << 12);
        }
        break;
    case CV_ROTR1:
        for (x = 0; x < n; x++) {
            v = ld16(s + 2 * x);
            st16(d + t[x], v >> 1 | v << 15);
        }
        break;
    case CV_Z24_F32:
        for (x = 0; x < n; x++)
            st32(d + t[x], z24_f32(ld32(s + 4 * x)));
        break;
    case CV_PLANE32:
        for (x = 0; x < n; x++)
            st32(d + t[x], ld32(s + step * x + off));
        break;
    default:
        break;
    }
}

/* Stored elements at s + t[x] -> linear d (element step bytes, plane offset off). */
static void get_row(enum conversion cv, uint8_t *d, const uint8_t *s, const uint32_t *t, uint32_t n,
                    uint32_t step, uint32_t off)
{
    uint32_t x, v;
    switch (cv) {
    case CV_COPY8:
        for (x = 0; x < n; x++)
            d[x] = s[t[x]];
        break;
    case CV_COPY16:
        for (x = 0; x < n; x++)
            st16(d + 2 * x, ld16(s + t[x]));
        break;
    case CV_COPY32:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, ld32(s + t[x]));
        break;
    case CV_COPY64:
        for (x = 0; x < n; x++)
            memcpy(d + 8 * x, s + t[x], 8);
        break;
    case CV_COPY128:
        for (x = 0; x < n; x++)
            memcpy(d + 16 * x, s + t[x], 16);
        break;
    case CV_SWAP_RB:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, swap_rb(ld32(s + t[x])));
        break;
    case CV_ROTL4:
        for (x = 0; x < n; x++) {
            v = ld16(s + t[x]);
            st16(d + 2 * x, v << 4 | v >> 12);
        }
        break;
    case CV_ROTL1:
        for (x = 0; x < n; x++) {
            v = ld16(s + t[x]);
            st16(d + 2 * x, v << 1 | v >> 15);
        }
        break;
    case CV_F32_Z24:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, f32_z24(ld32(s + t[x])));
        break;
    case CV_PLANE32:
        for (x = 0; x < n; x++)
            st32(d + step * x + off, ld32(s + t[x]));
        break;
    case CV_565_RGBA8:
    case CV_565_BGRA8:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, from565(ld16(s + t[x]), cv == CV_565_BGRA8));
        break;
    case CV_4444_RGBA8:
    case CV_4444_BGRA8:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, from4444(ld16(s + t[x]), cv == CV_4444_BGRA8));
        break;
    case CV_1555_RGBA8:
    case CV_1555_BGRA8:
        for (x = 0; x < n; x++)
            st32(d + 4 * x, from1555(ld16(s + t[x]), cv == CV_1555_BGRA8));
        break;
    default:
        break;
    }
}

/* The readback of a 16-bit colour layout into 8-bit channels. */
static enum conversion expansion(enum sgx535_texture_hw_format stored, enum sgx535_texel_format dst)
{
    bool bgra = dst == SGX535_TEXEL_B8G8R8A8_UNORM;
    if (dst != SGX535_TEXEL_R8G8B8A8_UNORM && !bgra)
        return CV_NONE;
    switch (stored) {
    case SGX535_TEXFORMAT_R5G6B5:
        return bgra ? CV_565_BGRA8 : CV_565_RGBA8;
    case SGX535_TEXFORMAT_A4R4G4B4:
        return bgra ? CV_4444_BGRA8 : CV_4444_RGBA8;
    case SGX535_TEXFORMAT_A1R5G5B5:
        return bgra ? CV_1555_BGRA8 : CV_1555_RGBA8;
    default:
        return CV_NONE;
    }
}

static enum sgx535_texture_status transfer(const struct sgx535_texture_layout *l, uint8_t *storage,
                                           uint32_t level, const struct sgx535_texture_box *box,
                                           enum sgx535_texel_format format, uint8_t *data,
                                           uint32_t stride, uint32_t layer_stride, bool upload)
{
    const struct texel_info *t = texel(format);
    struct region r;
    enum conversion cv;
    uint32_t plane, z, y, step;

    if (!t || !storage || !data)
        return SGX535_TEXTURE_INVALID;
    if (t->hw_format == l->hw_format && t->planes == l->planes)
        cv = (enum conversion)(upload ? t->up : t->down);
    else
        cv = upload ? CV_NONE : expansion(l->hw_format, format);
    if (cv == CV_NONE || !region(l, storage, level, box, &r))
        return SGX535_TEXTURE_INVALID;
    step = t->bytes;
    for (plane = 0; plane < l->planes; plane++) {
        uint8_t *base = r.base + plane * l->plane_bytes;
        for (z = 0; z < r.count[2]; z++) {
            for (y = 0; y < r.count[1]; y++) {
                uint8_t *row = base + r.tz[z] + r.ty[y];
                uint8_t *lin = data + z * layer_stride + y * stride;
                if (upload)
                    put_row(cv, row, r.tx, lin, r.count[0], step, plane * 4);
                else
                    get_row(cv, lin, row, r.tx, r.count[0], step, plane * 4);
            }
        }
    }
    return SGX535_TEXTURE_OK;
}

enum sgx535_texture_status
sgx535_texture_upload(const struct sgx535_texture_layout *layout, void *storage, uint32_t level,
                      const struct sgx535_texture_box *box, enum sgx535_texel_format src_format,
                      const void *src, uint32_t src_stride, uint32_t src_layer_stride)
{
    return transfer(layout, storage, level, box, src_format, (uint8_t *)(uintptr_t)src, src_stride,
                    src_layer_stride, true);
}

enum sgx535_texture_status sgx535_texture_readback(const struct sgx535_texture_layout *layout,
                                                   const void *storage, uint32_t level,
                                                   const struct sgx535_texture_box *box,
                                                   enum sgx535_texel_format dst_format, void *dst,
                                                   uint32_t dst_stride, uint32_t dst_layer_stride)
{
    return transfer(layout, (uint8_t *)(uintptr_t)storage, level, box, dst_format, dst, dst_stride,
                    dst_layer_stride, false);
}
