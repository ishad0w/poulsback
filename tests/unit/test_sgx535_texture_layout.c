// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint32_t seed = 0x5eed1234u;

static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void fill(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
}

static uint32_t minify(uint32_t v, uint32_t level)
{
    v >>= level;
    return v ? v : 1;
}

/* --- an independent model of the layout ------------------------------------ */

struct fmt {
    enum sgx535_texel_format format;
    uint32_t bytes, planes, element, block;
    enum sgx535_texture_hw_format hw;
    bool depth;
};

static const struct fmt fmts[] = {
    {SGX535_TEXEL_R8G8B8A8_UNORM, 4, 1, 4, 1, SGX535_TEXFORMAT_B8G8R8A8, false},
    {SGX535_TEXEL_B8G8R8A8_UNORM, 4, 1, 4, 1, SGX535_TEXFORMAT_B8G8R8A8, false},
    {SGX535_TEXEL_R8G8B8X8_UNORM, 4, 1, 4, 1, SGX535_TEXFORMAT_B8G8R8A8, false},
    {SGX535_TEXEL_B8G8R8X8_UNORM, 4, 1, 4, 1, SGX535_TEXFORMAT_B8G8R8A8, false},
    {SGX535_TEXEL_B5G6R5_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_R5G6B5, false},
    {SGX535_TEXEL_B4G4R4A4_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_A4R4G4B4, false},
    {SGX535_TEXEL_A4B4G4R4_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_A4R4G4B4, false},
    {SGX535_TEXEL_B5G5R5A1_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_A1R5G5B5, false},
    {SGX535_TEXEL_A1B5G5R5_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_A1R5G5B5, false},
    {SGX535_TEXEL_U8, 1, 1, 1, 1, SGX535_TEXFORMAT_U8, false},
    {SGX535_TEXEL_U8U8, 2, 1, 2, 1, SGX535_TEXFORMAT_U88, false},
    {SGX535_TEXEL_Z16_UNORM, 2, 1, 2, 1, SGX535_TEXFORMAT_U16, true},
    {SGX535_TEXEL_Z24X8_UNORM, 4, 1, 4, 1, SGX535_TEXFORMAT_F32, true},
    {SGX535_TEXEL_Z32_FLOAT, 4, 1, 4, 1, SGX535_TEXFORMAT_F32, true},
    {SGX535_TEXEL_R16G16B16A16_FLOAT, 8, 2, 4, 1, SGX535_TEXFORMAT_F1616, false},
    {SGX535_TEXEL_R32G32B32A32_FLOAT, 16, 4, 4, 1, SGX535_TEXFORMAT_F32, false},
    {SGX535_TEXEL_DXT1, 8, 1, 8, 4, SGX535_TEXFORMAT_DXT1, false},
    {SGX535_TEXEL_DXT3, 16, 1, 16, 4, SGX535_TEXFORMAT_DXT3, false},
    {SGX535_TEXEL_DXT5, 16, 1, 16, 4, SGX535_TEXFORMAT_DXT5, false},
};
#define NFMTS (sizeof(fmts) / sizeof(fmts[0]))

/* Element index of (x, y, z) in a twiddled grid, one bit at a time:
 * y0 x0 z0 y1 x1 z1 ..., an axis skipped once its size is reached. */
static uint32_t twiddle(uint32_t x, uint32_t y, uint32_t z, uint32_t gx, uint32_t gy, uint32_t gz)
{
    uint32_t out = 0, pos = 0, i;
    for (i = 0; i < 12; i++) {
        if ((1u << i) < gy)
            out |= (y >> i & 1u) << pos++;
        if ((1u << i) < gx)
            out |= (x >> i & 1u) << pos++;
        if ((1u << i) < gz)
            out |= (z >> i & 1u) << pos++;
    }
    return out;
}

/* Byte offset of element (ex, ey, ez) of level, face, plane, from the
 * layout's sizes alone (level and face steps recomputed here). */
static uint32_t model_offset(const struct sgx535_texture_layout *l, const struct fmt *f,
                             uint32_t plane, uint32_t face, uint32_t level, uint32_t ex,
                             uint32_t ey, uint32_t ez)
{
    uint32_t lv, off = 0, face_bytes = 0, plane_bytes, gx, gy, gz, bytes;
    bool stride = l->type == SGX535_TEXTYPE_STRIDE;
    for (lv = 0; lv < l->levels; lv++) {
        gx = (minify(l->width, lv) + f->block - 1) / f->block;
        gy = (minify(l->height, lv) + f->block - 1) / f->block;
        gz = l->target == SGX535_TEXTURE_3D ? minify(l->depth, lv) : 1;
        bytes = stride ? ((gx + 31) / 32 * 32) * gy * f->element : gx * gy * gz * f->element;
        if (lv < level)
            off += bytes;
        face_bytes += bytes;
    }
    plane_bytes = (face_bytes * l->faces + 63) / 64 * 64;
    gx = (minify(l->width, level) + f->block - 1) / f->block;
    gy = (minify(l->height, level) + f->block - 1) / f->block;
    gz = l->target == SGX535_TEXTURE_3D ? minify(l->depth, level) : 1;
    off += plane * plane_bytes + face * face_bytes;
    if (stride)
        return off + ey * ((gx + 31) / 32 * 32) * f->element + ex * f->element;
    return off + twiddle(ex, ey, ez, gx, gy, gz) * f->element;
}

/* The stored bytes of one linear element (plane p), per format. */
static void model_store(const struct fmt *f, const uint8_t *s, uint32_t plane, uint8_t *out)
{
    uint32_t v;
    switch (f->format) {
    case SGX535_TEXEL_R8G8B8A8_UNORM:
    case SGX535_TEXEL_R8G8B8X8_UNORM:
        out[0] = s[2];
        out[1] = s[1];
        out[2] = s[0];
        out[3] = f->format == SGX535_TEXEL_R8G8B8X8_UNORM ? 255 : s[3];
        break;
    case SGX535_TEXEL_B8G8R8X8_UNORM:
        memcpy(out, s, 3);
        out[3] = 255;
        break;
    case SGX535_TEXEL_A4B4G4R4_UNORM: /* R 15:12 G 11:8 B 7:4 A 3:0 -> A R G B */
        v = (uint32_t)s[0] | (uint32_t)s[1] << 8;
        v = (v & 15u) << 12 | (v >> 12 & 15u) << 8 | (v >> 8 & 15u) << 4 | (v >> 4 & 15u);
        out[0] = (uint8_t)v;
        out[1] = (uint8_t)(v >> 8);
        break;
    case SGX535_TEXEL_A1B5G5R5_UNORM: /* R 15:11 G 10:6 B 5:1 A 0 -> A R G B */
        v = (uint32_t)s[0] | (uint32_t)s[1] << 8;
        v = (v & 1u) << 15 | (v >> 11 & 31u) << 10 | (v >> 6 & 31u) << 5 | (v >> 1 & 31u);
        out[0] = (uint8_t)v;
        out[1] = (uint8_t)(v >> 8);
        break;
    case SGX535_TEXEL_Z24X8_UNORM: {
        float z = (float)((double)((uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16) /
                          16777215.0);
        memcpy(out, &z, 4);
        break;
    }
    default:
        memcpy(out, s + plane * f->element, f->element);
        break;
    }
}

/* What a readback in the layout's own format returns for linear element s. */
static void model_back(const struct fmt *f, uint8_t *s)
{
    if (f->format == SGX535_TEXEL_R8G8B8X8_UNORM || f->format == SGX535_TEXEL_B8G8R8X8_UNORM)
        s[3] = 255;
    if (f->format == SGX535_TEXEL_Z24X8_UNORM)
        s[3] = 0;
}

/* --- EMGD texture layout vectors ------------------------------------------ */

static void emgd_layouts(void)
{
    static const uint32_t chain64[] = {0, 0x4000, 0x5000, 0x5400, 0x5500, 0x5540, 0x5550};
    struct sgx535_texture_layout l;
    uint32_t i, k;

    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 64, 64, 1,
                                     6, false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_2D && l.hw_format == SGX535_TEXFORMAT_B8G8R8A8);
    CHECK(l.levels == 7 && l.face_bytes == 0x5554 && !l.chan_replicate);
    for (i = 0; i < 7; i++)
        CHECK(sgx535_texture_offset(&l, 0, 0, i) == chain64[i]);
    /* A partial chain (MAX_LEVEL 2) still stores the whole chain. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 128, 128,
                                     1, 2, false) == SGX535_TEXTURE_OK);
    CHECK(l.levels == 8 && l.face_bytes == 0x15554);
    /* 64x16: x steps 8, 32, 128, 512, 1024, 2048 bytes; y 4, 16, 64, 256; levels
     * at +0x1000, +0x1400, +0x1500. 16x64 transposed. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 64, 16, 1,
                                     6, false) == SGX535_TEXTURE_OK);
    CHECK(l.level_offset[1] == 0x1000 && l.level_offset[2] == 0x1400 &&
          l.level_offset[3] == 0x1500 && l.face_bytes == 0x155c);
    for (k = 0; k < 6; k++)
        CHECK(4 * twiddle(1u << k, 0, 0, 64, 16, 1) == (k < 4 ? 8u << 2 * k : 1024u << (k - 4)));
    for (k = 0; k < 4; k++) {
        CHECK(4 * twiddle(0, 1u << k, 0, 64, 16, 1) == 4u << 2 * k);
        CHECK(4 * twiddle(1u << k, 0, 0, 16, 64, 1) == 8u << 2 * k);
    }
    CHECK(4 * twiddle(0, 16, 0, 16, 64, 1) == 1024 && 4 * twiddle(0, 32, 0, 16, 64, 1) == 2048);
    /* Cube 16x16 RGBA8: faces 1 KiB apart, 0x1800 in all. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_CUBE, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16,
                                     1, 0, false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_CEM && l.faces == 6 && l.size == 0x1800);
    for (i = 0; i < 6; i++)
        CHECK(sgx535_texture_offset(&l, 0, i, 0) == 0x400 * i);
    /* 16^3 chain 0x4924; texel 4 is z = 1. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_3D, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16, 16,
                                     4, false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_3D && l.levels == 5 && l.face_bytes == 0x4924);
    CHECK(twiddle(0, 1, 0, 16, 16, 16) == 1 && twiddle(1, 0, 0, 16, 16, 16) == 2 &&
          twiddle(0, 0, 1, 16, 16, 16) == 4);
    /* NPOT: stride, 32-texel rows: 100x60 RGBA8 512-byte rows. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 100, 60, 1,
                                     0, false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_STRIDE && l.level_pitch[0] == 512 && l.size == 0x7800);
    /* Depth: stride U16 and F32, as EMGD's DOUTT1 says. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_Z16_UNORM, 32, 32, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_STRIDE && l.hw_format == SGX535_TEXFORMAT_U16 &&
          l.level_pitch[0] == 64);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_Z32_FLOAT, 32, 32, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    CHECK(l.type == SGX535_TEXTYPE_STRIDE && l.hw_format == SGX535_TEXFORMAT_F32 &&
          l.level_pitch[0] == 128);
    /* Planes: RGBA16F two F1616 textures, RGBA32F four F32, 0x1000 apart at 32x32. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R16G16B16A16_FLOAT, 32, 32,
                                     1, 0, false) == SGX535_TEXTURE_OK);
    CHECK(l.planes == 2 && l.hw_format == SGX535_TEXFORMAT_F1616 &&
          sgx535_texture_offset(&l, 1, 0, 0) == 0x1000);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R32G32B32A32_FLOAT, 32, 32,
                                     1, 0, false) == SGX535_TEXTURE_OK);
    CHECK(l.planes == 4 && sgx535_texture_offset(&l, 3, 0, 0) == 0x3000);
    /* U8/U88 replicate; DXT by 4x4 block: 32x32 DXT1 is 8x8 blocks of 8 bytes. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_U8U8, 32, 32, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    CHECK(l.chan_replicate && l.hw_format == SGX535_TEXFORMAT_U88);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_DXT1, 32, 32, 1, 5,
                                     false) == SGX535_TEXTURE_OK);
    CHECK(l.block == 4 && l.level_bytes[0] == 512 && l.level_bytes[2] == 32 &&
          l.level_bytes[3] == 8 && l.level_bytes[5] == 8 && l.face_bytes == 512 + 128 + 32 + 8 * 3);
}

static void refusals(void)
{
    struct sgx535_texture_layout l;
    struct sgx535_texture_box box = {0, 0, 0, 4, 4, 1};
    uint8_t storage[64], src[64];

    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_NONE, 4, 4, 1, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_FORMAT_COUNT, 4, 4, 1, 0,
                                     false) == SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_U8, 4096, 4, 1, 0,
                                     false) == SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_U8, 0, 4, 1, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_U8, 4, 4, 2, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_3D, SGX535_TEXEL_U8, 512, 4, 4, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_3D, SGX535_TEXEL_U8, 8, 8, 6, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_CUBE, SGX535_TEXEL_U8, 8, 4, 1, 0, false) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_CUBE, SGX535_TEXEL_U8, 12, 12, 1, 0,
                                     false) == SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_CUBE, SGX535_TEXEL_U8, 8, 8, 1, 0, true) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_CUBE, SGX535_TEXEL_Z16_UNORM, 8, 8, 1, 0,
                                     false) == SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_DXT1, 12, 8, 1, 0,
                                     false) == SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_U8, 8, 8, 1, 4, false) ==
          SGX535_TEXTURE_INVALID);
    /* Boxes and format pairs. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 4, 4, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    memset(src, 0, sizeof(src));
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_B8G8R8X8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_upload(&l, storage, 1, &box, SGX535_TEXEL_R8G8B8A8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_B5G6R5_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_readback(&l, storage, 0, &box, SGX535_TEXEL_B5G6R5_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    box.x = 1;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8A8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    box.x = 0;
    box.depth = 2;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8A8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    box.depth = 1;
    box.width = 0;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8A8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    /* Upload never widens 16 bits into 32; readback does, only into RGBA8/BGRA8. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_B5G6R5_UNORM, 4, 4, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    box.width = 4;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8A8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    CHECK(sgx535_texture_readback(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8X8_UNORM, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    /* DXT boxes on block boundaries, or ending at the level's edge. */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_DXT1, 8, 8, 1, 3, false) ==
          SGX535_TEXTURE_OK);
    box.x = 2;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_DXT1, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    box.x = 4;
    box.width = 3;
    CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_DXT1, src, 16, 64) ==
          SGX535_TEXTURE_INVALID);
    box.x = 0;
    box.width = 2;
    box.height = 2;
    CHECK(sgx535_texture_upload(&l, storage, 2, &box, SGX535_TEXEL_DXT1, src, 8, 64) ==
          SGX535_TEXTURE_OK);
}

/* --- random round trips ----------------------------------------------------- */

static uint32_t pick_size(bool npot, uint32_t max)
{
    uint32_t v = 1u << (rnd() % 9);
    if (npot)
        v = 1 + rnd() % 300;
    return v > max ? max : v;
}

/* One random texture: every level and face uploaded whole, checked element by
 * element against the model, read back exactly; then random sub-boxes over
 * random contents, checked to change exactly their region. */
static void round_trip(const struct fmt *f, enum sgx535_texture_target target, bool npot)
{
    struct sgx535_texture_layout l;
    uint32_t w, h, d, last, level, i;
    uint8_t *storage, *before, *src, *back;
    bool stride = npot || f->depth;

    w = pick_size(npot, target == SGX535_TEXTURE_3D ? 32 : 256);
    h = target == SGX535_TEXTURE_CUBE ? w : pick_size(npot, target == SGX535_TEXTURE_3D ? 32 : 256);
    d = target == SGX535_TEXTURE_3D ? pick_size(false, 32) : 1;
    if (f->block > 1 && (w < 4 || h < 4))
        w = h = target == SGX535_TEXTURE_CUBE ? 8 : 16;
    if (target == SGX535_TEXTURE_2D && !npot && rnd() % 4 == 0)
        stride = !(f->block > 1);
    last = rnd() % 2 && (w > 1 || h > 1 || d > 1) ? 1 : 0;
    CHECK(sgx535_texture_layout_init(&l, target, f->format, w, h, d, last,
                                     stride && target == SGX535_TEXTURE_2D) == SGX535_TEXTURE_OK);
    CHECK(l.hw_format == f->hw && l.planes == f->planes && l.block == f->block);
    CHECK((l.type == SGX535_TEXTYPE_STRIDE) == (target == SGX535_TEXTURE_2D && stride));
    storage = malloc(l.size);
    before = malloc(l.size);
    i = ((w * 16 + 8) * h + 64) * (d > 6 ? d : 6);
    src = malloc(i);
    back = malloc(i);
    CHECK(storage && before && src && back);
    fill(storage, l.size);
    for (level = 0; level < l.levels; level++) {
        uint32_t lw = minify(w, level), lh = minify(h, level);
        uint32_t ld = target == SGX535_TEXTURE_3D ? minify(d, level) : l.faces;
        uint32_t gx = (lw + f->block - 1) / f->block, gy = (lh + f->block - 1) / f->block;
        uint32_t stride_ = gx * f->bytes + rnd() % 8, layer = stride_ * gy + rnd() % 64;
        struct sgx535_texture_box box = {0, 0, 0, lw, lh, ld};
        uint32_t x, y, z, k;
        uint8_t expect[16];
        fill(src, (size_t)layer * ld);
        CHECK(sgx535_texture_upload(&l, storage, level, &box, f->format, src, stride_, layer) ==
              SGX535_TEXTURE_OK);
        for (z = 0; z < ld; z++)
            for (y = 0; y < gy; y++)
                for (x = 0; x < gx; x++)
                    for (k = 0; k < f->planes; k++) {
                        uint32_t face = target == SGX535_TEXTURE_CUBE ? z : 0;
                        uint32_t ez = target == SGX535_TEXTURE_3D ? z : 0;
                        uint32_t off = model_offset(&l, f, k, face, level, x, y, ez);
                        model_store(f, src + z * layer + y * stride_ + x * f->bytes, k, expect);
                        CHECK(off + f->element <= l.size);
                        CHECK(!memcmp(storage + off, expect, f->element));
                    }
        memset(back, 0xa5, (size_t)layer * ld);
        CHECK(sgx535_texture_readback(&l, storage, level, &box, f->format, back, stride_, layer) ==
              SGX535_TEXTURE_OK);
        for (z = 0; z < ld; z++)
            for (y = 0; y < gy; y++)
                for (x = 0; x < gx; x++) {
                    uint8_t *s = src + z * layer + y * stride_ + x * f->bytes;
                    uint8_t *b = back + z * layer + y * stride_ + x * f->bytes;
                    model_back(f, s);
                    CHECK(!memcmp(s, b, f->bytes));
                }
        CHECK(back[gx * f->bytes] == 0xa5 || stride_ == gx * f->bytes);
    }
    for (i = 0; i < 4; i++) {
        uint32_t lvl = rnd() % l.levels, lw = minify(w, lvl), lh = minify(h, lvl);
        uint32_t ld = target == SGX535_TEXTURE_3D ? minify(d, lvl) : l.faces;
        uint32_t bx = rnd() % lw, by = rnd() % lh, bz = rnd() % ld;
        uint32_t bw = 1 + rnd() % (lw - bx), bh = 1 + rnd() % (lh - by), bd = 1 + rnd() % (ld - bz);
        uint32_t s_, layer, x, y, z, ex, ey;
        struct sgx535_texture_box box, whole = {0, 0, 0, lw, lh, ld};
        uint8_t *full = malloc((size_t)lw * lh * ld * f->bytes + 16);
        uint8_t *full2 = malloc((size_t)lw * lh * ld * f->bytes + 16);
        uint32_t gx = (lw + f->block - 1) / f->block, gy = (lh + f->block - 1) / f->block;
        CHECK(full && full2);
        if (f->block > 1) {
            bx &= ~3u;
            by &= ~3u;
            bw = lw - bx < 4 ? lw - bx : (bw + 3) & ~3u;
            bh = lh - by < 4 ? lh - by : (bh + 3) & ~3u;
            bw = bw > lw - bx ? lw - bx : bw;
            bh = bh > lh - by ? lh - by : bh;
        }
        box = (struct sgx535_texture_box){bx, by, bz, bw, bh, bd};
        s_ = ((bw + f->block - 1) / f->block) * f->bytes + rnd() % 5;
        layer = s_ * ((bh + f->block - 1) / f->block) + rnd() % 9;
        fill(src, (size_t)layer * bd);
        CHECK(sgx535_texture_readback(&l, storage, lvl, &whole, f->format, full, gx * f->bytes,
                                      gx * gy * f->bytes) == SGX535_TEXTURE_OK);
        memcpy(before, storage, l.size);
        CHECK(sgx535_texture_upload(&l, storage, lvl, &box, f->format, src, s_, layer) ==
              SGX535_TEXTURE_OK);
        CHECK(sgx535_texture_readback(&l, storage, lvl, &whole, f->format, full2, gx * f->bytes,
                                      gx * gy * f->bytes) == SGX535_TEXTURE_OK);
        for (z = 0; z < ld; z++)
            for (ey = 0; ey < gy; ey++)
                for (ex = 0; ex < gx; ex++) {
                    uint8_t *got = full2 + ((size_t)z * gy + ey) * gx * f->bytes + ex * f->bytes;
                    uint8_t *old = full + ((size_t)z * gy + ey) * gx * f->bytes + ex * f->bytes;
                    x = ex * f->block;
                    y = ey * f->block;
                    if (z >= bz && z < bz + bd && x >= bx && x < bx + bw && y >= by &&
                        y < by + bh) {
                        uint8_t *s = src + (z - bz) * layer + (y - by) / f->block * s_ +
                                     (x - bx) / f->block * f->bytes;
                        model_back(f, s);
                        CHECK(!memcmp(got, s, f->bytes));
                    } else {
                        CHECK(!memcmp(got, old, f->bytes));
                    }
                }
        /* Bytes of other levels, faces and planes are untouched. */
        for (z = 0; z < l.planes * l.faces; z++) {
            uint32_t lo = sgx535_texture_offset(&l, z / l.faces, z % l.faces, lvl);
            memcpy(before + lo, storage + lo, l.level_bytes[lvl]);
        }
        CHECK(!memcmp(before, storage, l.size));
        free(full);
        free(full2);
    }
    free(storage);
    free(before);
    free(src);
    free(back);
}

static void round_trips(void)
{
    uint32_t i, n;
    for (n = 0; n < 6; n++)
        for (i = 0; i < NFMTS; i++) {
            round_trip(&fmts[i], SGX535_TEXTURE_2D, n % 2 && fmts[i].block == 1);
            if (!fmts[i].depth) {
                round_trip(&fmts[i], SGX535_TEXTURE_CUBE, false);
                round_trip(&fmts[i], SGX535_TEXTURE_3D, false);
            }
        }
}

/* --- 16-bit readback expands by bit replication ----------------------------- */

static uint32_t rep5(uint32_t v)
{
    return (v * 0x21u) >> 2; /* v << 3 | v >> 2 */
}

static uint32_t rep6(uint32_t v)
{
    return (v * 0x41u) >> 4; /* v << 2 | v >> 4 */
}

static void replication(void)
{
    static const enum sgx535_texel_format sources[] = {
        SGX535_TEXEL_B5G6R5_UNORM, SGX535_TEXEL_B4G4R4A4_UNORM, SGX535_TEXEL_B5G5R5A1_UNORM};
    struct sgx535_texture_layout l;
    struct sgx535_texture_box box = {0, 0, 0, 256, 256, 1};
    static uint16_t texels[65536];
    static uint8_t rgba[65536 * 4], bgra[65536 * 4];
    uint8_t *storage;
    uint32_t f, v;

    for (v = 0; v < 65536; v++)
        texels[v] = (uint16_t)v;
    for (f = 0; f < 3; f++) {
        CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, sources[f], 256, 256, 1, 0,
                                         false) == SGX535_TEXTURE_OK);
        storage = malloc(l.size);
        CHECK(storage);
        CHECK(sgx535_texture_upload(&l, storage, 0, &box, sources[f], texels, 512, 0) ==
              SGX535_TEXTURE_OK);
        CHECK(sgx535_texture_readback(&l, storage, 0, &box, SGX535_TEXEL_R8G8B8A8_UNORM, rgba, 1024,
                                      0) == SGX535_TEXTURE_OK);
        CHECK(sgx535_texture_readback(&l, storage, 0, &box, SGX535_TEXEL_B8G8R8A8_UNORM, bgra, 1024,
                                      0) == SGX535_TEXTURE_OK);
        for (v = 0; v < 65536; v++) {
            uint32_t r, g, b, a;
            const uint8_t *p = rgba + 4 * v, *q = bgra + 4 * v;
            if (f == 0) {
                r = rep5(v >> 11);
                g = rep6(v >> 5 & 63);
                b = rep5(v & 31);
                a = 255;
            } else if (f == 1) {
                r = (v >> 8 & 15) * 0x11;
                g = (v >> 4 & 15) * 0x11;
                b = (v & 15) * 0x11;
                a = (v >> 12) * 0x11;
            } else {
                r = rep5(v >> 10 & 31);
                g = rep5(v >> 5 & 31);
                b = rep5(v & 31);
                a = v >> 15 ? 255 : 0;
            }
            CHECK(p[0] == r && p[1] == g && p[2] == b && p[3] == a);
            CHECK(q[0] == b && q[1] == g && q[2] == r && q[3] == a);
        }
        /* Full intensity reads 255 (EMGD's glReadPixels gave 248, 252, 240). */
        CHECK(rgba[4 * 0xffff] == 255 && rgba[4 * 0xffff + 1] == 255 &&
              rgba[4 * 0xffff + 2] == 255);
        free(storage);
    }
}

/* --- depth ------------------------------------------------------------------ */

static void depth24(void)
{
    struct sgx535_texture_layout l;
    struct sgx535_texture_box box = {0, 0, 0, 2048, 1, 1};
    static uint32_t z[2048], back[2048];
    uint8_t *storage;
    uint32_t base, i;

    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_Z24X8_UNORM, 2048, 1, 1, 0,
                                     false) == SGX535_TEXTURE_OK);
    storage = malloc(l.size);
    CHECK(storage);
    /* Every 24-bit depth survives the F32 store; X reads back as 0. */
    for (base = 0; base < (1u << 24); base += 2048) {
        for (i = 0; i < 2048; i++)
            z[i] = (base + i) | 0xab000000u;
        CHECK(sgx535_texture_upload(&l, storage, 0, &box, SGX535_TEXEL_Z24X8_UNORM, z, 8192, 0) ==
              SGX535_TEXTURE_OK);
        CHECK(sgx535_texture_readback(&l, storage, 0, &box, SGX535_TEXEL_Z24X8_UNORM, back, 8192,
                                      0) == SGX535_TEXTURE_OK);
        for (i = 0; i < 2048; i++)
            CHECK(back[i] == base + i);
    }
    free(storage);
}

/* The border area (docs/native-3d.md#texture-memory):
 * 32x32 RGBA8 border textures are 0x14c0 bytes, 304 texels before the map,
 * the colour in texels 20-23 and 176-303, zeros elsewhere. */
static void border_area(void)
{
    static uint8_t storage[0x14c0];
    static const uint8_t colour[4] = {0xbf, 0x80, 0x40, 0xff};
    struct sgx535_texture_layout l;
    unsigned coloured = 0;
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 32, 32, 1,
                                     0, false) == SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_layout_border(&l) == SGX535_TEXTURE_OK);
    CHECK(l.size == 0x14c0 && l.border_bytes == 0x4c0 &&
          sgx535_texture_offset(&l, 0, 0, 0) == 0x4c0);
    CHECK(sgx535_texture_layout_border(&l) != SGX535_TEXTURE_OK);
    memset(storage, 0, sizeof(storage));
    CHECK(sgx535_texture_border_fill(&l, storage, colour) == SGX535_TEXTURE_OK);
    for (unsigned t = 0; t < 304; t++) {
        bool in = (t >= 20 && t < 24) || (t >= 176 && t < 304);
        bool set = !memcmp(storage + 4 * t, colour, 4);
        CHECK(in == set);
        coloured += set;
    }
    CHECK(coloured == 132);
    for (unsigned b = 0x4c0; b < sizeof(storage); b++)
        CHECK(!storage[b]);
    /* other textures have no captured border layout */
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 32, 16, 1,
                                     0, false) == SGX535_TEXTURE_OK &&
          sgx535_texture_layout_border(&l) != SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 32, 32, 1,
                                     5, false) == SGX535_TEXTURE_OK &&
          sgx535_texture_layout_border(&l) != SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_layout_init(&l, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 100, 100,
                                     1, 0, false) == SGX535_TEXTURE_OK &&
          sgx535_texture_layout_border(&l) != SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_border_fill(&l, storage, colour) != SGX535_TEXTURE_OK);
}

int main(void)
{
    border_area();
    emgd_layouts();
    refusals();
    round_trips();
    replication();
    depth24();
    printf("PASS: sgx535 texture layout (%lu checks)\n", checks);
    return 0;
}
