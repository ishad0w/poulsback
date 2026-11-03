// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The texture words (src/raster/sgx535_texture_words.c) against EMGD-form
 * texture vectors (docs/re/emgd-linux/fragment-path.md#textures), with GL
 * behavior selected where EMGD departs from it. CPU only. */
#include "sgx535_texture_words.h"
#include <math.h>
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

#define BASE UINT32_C(0x60340000)

struct texture {
    uint8_t target, format;
    uint16_t width, height, depth;
    uint8_t last, stride;
};
struct expected {
    const char *name;
    struct texture texture;
    struct sgx535_texture_sampler sampler;
    struct sgx535_texture_view view;
    uint32_t words[2], offset;
};

#define T2D(f, w, h, last) {SGX535_TEXTURE_2D, SGX535_TEXEL_##f, w, h, 1, last, 0}
#define W(s, t, r) {SGX535_WRAP_##s, SGX535_WRAP_##t, SGX535_WRAP_##r}
#define EDGE W(CLAMP_TO_EDGE, CLAMP_TO_EDGE, REPEAT)
#define REP W(REPEAT, REPEAT, REPEAT)
#define N SGX535_FILTER_NEAREST
#define L SGX535_FILTER_LINEAR
#define MIP(m) SGX535_MIP_##m
/* A sampler: wraps, min and mag filters, mip filter, then one more field. */
#define S(w, mn, mg, mp) {.wrap = w, .min_filter = mn, .mag_filter = mg, .mip_filter = mp}
#define SX(w, mn, mg, mp, field, value)                                                            \
    {.wrap = w, .min_filter = mn, .mag_filter = mg, .mip_filter = mp, .field = value}

static const struct expected cases[] = {
    /* formats: one level, CLAMP_TO_EDGE, NEAREST */
    {"rgba8",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x0c050005},
     0},
    {"rgb565",
     T2D(B5G6R5_UNORM, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x05050005},
     0},
    {"rgba4444",
     T2D(B4G4R4A4_UNORM, 16, 16, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x02040004},
     0},
    {"rgba5551",
     T2D(B5G5R5A1_UNORM, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x04050005},
     0},
    {"l8", T2D(U8, 16, 16, 0), S(EDGE, N, N, MIP(NONE)), {0, 0, 0}, {0x43fe0090, 0x00040004}, 0},
    {"la88",
     T2D(U8U8, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x43fe0090, 0x07050005},
     0},
    {"rgba16f plane 1",
     T2D(R16G16B16A16_FLOAT, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 1},
     {0x03fe0090, 0x11050005},
     0x1000},
    {"rgba32f plane 3",
     T2D(R32G32B32A32_FLOAT, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 3},
     {0x03fe0090, 0x12050005},
     0x3000},
    {"srgb8a8",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     SX(EDGE, N, N, MIP(NONE), srgb, 1),
     {0, 0, 0},
     {0x0bfe0090, 0x0c050005},
     0},
    {"dxt1",
     T2D(DXT1, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x14050005},
     0},
    {"dxt5",
     T2D(DXT5, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x16050005},
     0},
    {"depth16",
     T2D(Z16_UNORM, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x6901f01f},
     0},
    {"depth24",
     T2D(Z24X8_UNORM, 32, 32, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x7201f01f},
     0},
    /* filters: a 64x64 chain, REPEAT */
    {"mag-linear",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     S(REP, L, L, MIP(NONE)),
     {0, 6, 0},
     {0x03e01400, 0x0c060006},
     0},
    {"nearest-mipmap-linear",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     S(REP, N, N, MIP(LINEAR)),
     {0, 6, 0},
     {0x03ee0200, 0x0c060006},
     0},
    {"linear-mipmap-linear",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     S(REP, L, L, MIP(LINEAR)),
     {0, 6, 0},
     {0x03ee1600, 0x0c060006},
     0},
    {"anisotropic-4",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     SX(REP, L, L, MIP(LINEAR), max_anisotropy, 4),
     {0, 6, 0},
     {0x03eeaa00, 0x0c060006},
     0},
    /* EMGD copies levels 3-6 into their own allocation; ours names level 3 */
    {"min-lod-3",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     SX(REP, N, N, MIP(NEAREST), min_lod, 3),
     {0, 6, 0},
     {0x03e80000, 0x0c030003},
     0x5400},
    /* GL: levels 1 and 2 are one level apart (EMGD 03e40000: MAX_LEVEL 2 itself) */
    {"base-1-max-2",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     S(REP, N, N, MIP(NEAREST)),
     {1, 2, 0},
     {0x03e20000, 0x0c050005},
     0x4000},
    {"env-lod-bias-1",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     SX(REP, N, N, MIP(NEAREST), lod_bias, 1),
     {0, 6, 0},
     {0x04ee0000, 0x0c060006},
     0},
    {"mag-nearest-mipmapped",
     T2D(R8G8B8A8_UNORM, 64, 64, 6),
     S(REP, L, N, MIP(NEAREST)),
     {0, 6, 0},
     {0x03ee0400, 0x0c060006},
     0},
    /* wrap: 32x32 */
    {"repeat",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(REP, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0000, 0x0c050005},
     0},
    {"mirrored",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(W(MIRRORED_REPEAT, MIRRORED_REPEAT, REPEAT), N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0048, 0x0c050005},
     0},
    /* GL_CLAMP: OGLCLAMP, where EMGD sends CLAMP (03fe0090) */
    {"clamp",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(W(CLAMP, CLAMP, REPEAT), N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe01f8, 0x0c050005},
     0},
    {"clamp-to-border",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(W(CLAMP_TO_BORDER, CLAMP_TO_BORDER, REPEAT), N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0168, 0x0c050005},
     0},
    {"border-s-repeat-t",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(W(CLAMP_TO_BORDER, REPEAT, REPEAT), N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0160, 0x0c050005},
     0},
    {"repeat-s-edge-t",
     T2D(R8G8B8A8_UNORM, 32, 32, 0),
     S(W(REPEAT, CLAMP_TO_EDGE, REPEAT), N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0010, 0x0c050005},
     0},
    /* mips */
    {"64x16",
     T2D(R8G8B8A8_UNORM, 64, 16, 6),
     S(REP, N, N, MIP(NEAREST)),
     {0, 6, 0},
     {0x03ee0000, 0x0c060004},
     0},
    {"128x128-max-level-2",
     T2D(R8G8B8A8_UNORM, 128, 128, 7),
     S(REP, N, N, MIP(NEAREST)),
     {0, 2, 0},
     {0x03e40000, 0x0c070007},
     0},
    {"256x8",
     T2D(R8G8B8A8_UNORM, 256, 8, 8),
     S(REP, N, N, MIP(NEAREST)),
     {0, 8, 0},
     {0x03f20000, 0x0c080003},
     0},
    /* npot: stride surfaces */
    {"100x60",
     T2D(R8G8B8A8_UNORM, 100, 60, 0),
     S(EDGE, N, N, MIP(NONE)),
     {0, 0, 0},
     {0x03fe0090, 0x6c06303b},
     0},
    {"100x60-linear",
     T2D(R8G8B8A8_UNORM, 100, 60, 0),
     S(EDGE, L, L, MIP(NONE)),
     {0, 0, 0},
     {0x03fe1490, 0x6c06303b},
     0},
    /* cube, volume */
    {"cube",
     {SGX535_TEXTURE_CUBE, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16, 1, 0, 0},
     S(EDGE, N, L, MIP(NONE)),
     {0, 0, 0},
     {0x03fe1090, 0x4c040004},
     0},
    {"volume",
     {SGX535_TEXTURE_3D, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16, 16, 4, 0},
     S(REP, N, N, MIP(NEAREST)),
     {0, 4, 0},
     {0x03ea0000, 0x2c044004},
     0},
    {"volume-clamp-r",
     {SGX535_TEXTURE_3D, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16, 16, 4, 0},
     S(W(CLAMP_TO_EDGE, CLAMP_TO_EDGE, CLAMP_TO_EDGE), N, N, MIP(NEAREST)),
     {0, 4, 0},
     {0x03ea0092, 0x2c044004},
     0},
};

static enum sgx535_texture_status words_of(const struct texture *t,
                                           const struct sgx535_texture_sampler *s,
                                           const struct sgx535_texture_view *v, uint32_t w[3])
{
    struct sgx535_texture_layout layout;
    CHECK(sgx535_texture_layout_init(&layout, (enum sgx535_texture_target)t->target,
                                     (enum sgx535_texel_format)t->format, t->width, t->height,
                                     t->depth, t->last, t->stride) == SGX535_TEXTURE_OK);
    /* a border sampler's texture has the border area where it can */
    for (unsigned a = 0; a < 3; a++)
        if (s->wrap[a] == SGX535_WRAP_CLAMP_TO_BORDER) {
            (void)sgx535_texture_layout_border(&layout);
            break;
        }
    return sgx535_texture_words(&layout, s, v, BASE, w);
}

static void emgd_words(void)
{
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct expected *c = &cases[i];
        uint32_t w[3];
        CHECK(words_of(&c->texture, &c->sampler, &c->view, w) == SGX535_TEXTURE_OK);
        if (w[0] != c->words[0] || w[1] != c->words[1] || w[2] != BASE + c->offset)
            fprintf(stderr, "%s: %08x %08x %08x\n", c->name, w[0], w[1], w[2]);
        CHECK(w[0] == c->words[0] && w[1] == c->words[1] && w[2] == BASE + c->offset);
    }
}

static void refusals(void)
{
    static const struct texture
        chain = T2D(R8G8B8A8_UNORM, 64, 64, 6),
        one = T2D(R8G8B8A8_UNORM, 32, 32, 0), npot = T2D(R8G8B8A8_UNORM, 100, 60, 6),
        cube = {SGX535_TEXTURE_CUBE, SGX535_TEXEL_R8G8B8A8_UNORM, 16, 16, 1, 4, 0},
        planes = T2D(R16G16B16A16_FLOAT, 32, 32, 0);
    const struct sgx535_texture_sampler plain = S(EDGE, N, N, MIP(NONE)),
                                        border =
                                            S(W(CLAMP_TO_BORDER, REPEAT, REPEAT), N, N, MIP(NONE)),
                                        mirror_border =
                                            S(W(CLAMP_TO_BORDER, MIRRORED_REPEAT, REPEAT), N, N,
                                              MIP(NONE)),
                                        aniso = SX(EDGE, L, L, MIP(LINEAR), max_anisotropy, 32);
    struct sgx535_texture_layout layout;
    uint32_t w[3] = {1, 2, 3};
    CHECK(words_of(&chain, &plain, &(struct sgx535_texture_view){3, 2, 0}, w) != SGX535_TEXTURE_OK);
    CHECK(words_of(&planes, &plain, &(struct sgx535_texture_view){0, 0, 2}, w) !=
          SGX535_TEXTURE_OK);
    CHECK(words_of(&cube, &plain, &(struct sgx535_texture_view){1, 4, 0}, w) != SGX535_TEXTURE_OK);
    CHECK(words_of(&npot, &plain, &(struct sgx535_texture_view){0, 6, 0}, w) != SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &border, &(struct sgx535_texture_view){0, 6, 0}, w) !=
          SGX535_TEXTURE_OK);
    CHECK(words_of(&one, &mirror_border, &(struct sgx535_texture_view){0, 0, 0}, w) !=
          SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &aniso, &(struct sgx535_texture_view){0, 6, 0}, w) != SGX535_TEXTURE_OK);
    /* one level of a stride chain is a surface */
    CHECK(words_of(&npot, &plain, &(struct sgx535_texture_view){2, 2, 0}, w) == SGX535_TEXTURE_OK &&
          w[0] == 0x03fe0090 && w[1] == (0x6c000000u | 24u << 12 | 14u));
    CHECK(sgx535_texture_layout_init(&layout, SGX535_TEXTURE_2D, SGX535_TEXEL_R8G8B8A8_UNORM, 32,
                                     32, 1, 0, false) == SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_words(&layout, &plain, &(struct sgx535_texture_view){0, 0, 0}, 2, w) !=
          SGX535_TEXTURE_OK);
    CHECK(sgx535_texture_words(NULL, &plain, &(struct sgx535_texture_view){0, 0, 0}, 0, w) !=
          SGX535_TEXTURE_OK);
}

/* LOD state past any texture's (finite floats GL passes unclamped): the
 * conversions are bounded first, so each equals the largest in range. */
static void lod_bounds(void)
{
    static const struct texture chain = T2D(R8G8B8A8_UNORM, 64, 64, 6);
    struct sgx535_texture_sampler far = S(EDGE, N, N, MIP(NEAREST)), near = far;
    uint32_t w[3], v[3];
    far.min_lod = 1.0e30f, near.min_lod = 6.0f;
    CHECK(words_of(&chain, &far, &(struct sgx535_texture_view){0, 6, 0}, w) == SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &near, &(struct sgx535_texture_view){0, 6, 0}, v) == SGX535_TEXTURE_OK);
    CHECK(!memcmp(w, v, sizeof(w)));
    /* a first level near the integer's end with a rebase: refused, no wrap */
    CHECK(words_of(&chain, &near, &(struct sgx535_texture_view){UINT32_MAX - 2u, 6, 0}, w) !=
          SGX535_TEXTURE_OK);
    far.min_lod = near.min_lod = 0.0f;
    far.lod_bias = 1.0e30f, near.lod_bias = 4.0f;
    CHECK(words_of(&chain, &far, &(struct sgx535_texture_view){0, 6, 0}, w) == SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &near, &(struct sgx535_texture_view){0, 6, 0}, v) == SGX535_TEXTURE_OK);
    CHECK(!memcmp(w, v, sizeof(w)) && (w[0] >> 21 & 63u) == 63u);
    far.lod_bias = -1.0e30f, near.lod_bias = -3.875f;
    CHECK(words_of(&chain, &far, &(struct sgx535_texture_view){0, 6, 0}, w) == SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &near, &(struct sgx535_texture_view){0, 6, 0}, v) == SGX535_TEXTURE_OK);
    CHECK(!memcmp(w, v, sizeof(w)) && !(w[0] >> 21 & 63u));
    far.lod_bias = NAN, near.lod_bias = 0.0f;
    CHECK(words_of(&chain, &far, &(struct sgx535_texture_view){0, 6, 0}, w) == SGX535_TEXTURE_OK);
    CHECK(words_of(&chain, &near, &(struct sgx535_texture_view){0, 6, 0}, v) == SGX535_TEXTURE_OK);
    CHECK(!memcmp(w, v, sizeof(w)));
}

int main(void)
{
    emgd_words();
    refusals();
    lod_bounds();
    printf("PASS: sgx535 texture words (%lu checks, %zu EMGD words)\n", checks,
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
