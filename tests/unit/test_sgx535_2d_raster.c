// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Source-derived literals plus independent CPU pixel oracle; no GPU access. */
#include "sgx535_2d_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks, pixels, operations;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(e)) {                                                                                \
            fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #e);                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

enum { GUARD = 32, BYTES = 4096, WORDS = 16 };
static const struct sgx535_2d_raster_surface base = {
    1, 0x1000, BYTES, 24, 24, 128, SGX535_2D_RASTER_ARGB8888};
static uint32_t random_state = UINT32_C(0x5010d00d);
static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static uint32_t read_pixel(const unsigned char *p, unsigned bpp)
{
    uint32_t v = p[0] | ((uint32_t)p[1] << 8);
    if (bpp == 4)
        v |= ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v;
}
static void write_pixel(unsigned char *p, unsigned bpp, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    if (bpp == 4) {
        p[2] = (unsigned char)(v >> 16);
        p[3] = (unsigned char)(v >> 24);
    }
}
static uint32_t logic(unsigned op, uint32_t s, uint32_t d)
{
    switch (op) {
    case 0:
        return 0;
    case 1:
        return s & d;
    case 2:
        return s & ~d;
    case 3:
        return s;
    case 4:
        return ~s & d;
    case 5:
        return d;
    case 6:
        return s ^ d;
    case 7:
        return s | d;
    case 8:
        return ~(s | d);
    case 9:
        return ~(s ^ d);
    case 10:
        return ~d;
    case 11:
        return s | ~d;
    case 12:
        return ~s;
    case 13:
        return ~s | d;
    case 14:
        return ~(s & d);
    case 15:
        return UINT32_MAX;
    default:
        abort();
    }
}

static int contains(const struct sgx535_2d_raster_rect *r, int64_t x, int64_t y)
{
    return x >= r->x && y >= r->y && x < (int64_t)r->x + r->width && y < (int64_t)r->y + r->height;
}

/* Discover effective size by pixel membership, not production interval
 * clipping. */

/* Decode the emitted truth table per bit, independently of production tables.
 */
static uint32_t truth_table(uint32_t code, uint32_t s, uint32_t d, int fill)
{
    uint32_t v = 0;
    unsigned bit;
    for (bit = 0; bit < 32; ++bit) {
        unsigned sb = (s >> bit) & 1u, db = (d >> bit) & 1u;
        unsigned index = fill ? (sb * 4u + db) : (sb * 2u + db);
        v |= ((code >> index) & 1u) << bit;
    }
    return v;
}

/* Software interpreter of this bounded stream. Does not emulate SGX timing. */
static void interpret(const uint32_t *w, size_t n, unsigned char *src, unsigned char *dst,
                      unsigned bpp)
{
    unsigned dx, dy, sx = 0, sy = 0, width, height, ix, iy;
    unsigned source_stride = 0, destination_stride = w[1] & 0x7fffu;
    unsigned code, order = 0;
    uint32_t color = 0;
    CHECK(w[0] == 0x70000000 && w[n - 1] == 0xf0000000);
    if (n == 8) {
        code = w[3] & 0xffu;
        CHECK(((w[3] >> 8) & 0xffu) == code);
        dx = (w[5] >> 12) & 4095u;
        dy = w[5] & 4095u;
        width = w[6] >> 12;
        height = w[6] & 4095u;
        color = bpp == 2 ? ((w[4] >> 19) & 31u) * 2048u + ((w[4] >> 10) & 63u) * 32u +
                               ((w[4] >> 3) & 31u)
                         : w[4];
    } else {
        CHECK(n == 11 && w[7] == UINT32_MAX);
        code = w[6] & 0xffu;
        CHECK(((w[6] >> 8) & 0xffu) == code);
        order = (w[6] >> 23) & 3u;
        source_stride = w[3] & 0x7fffu;
        sx = (w[5] >> 12) & 4095u;
        sy = w[5] & 4095u;
        dx = (w[8] >> 12) & 4095u;
        dy = w[8] & 4095u;
        width = w[9] >> 12;
        height = w[9] & 4095u;
    }
    for (iy = 0; iy < height; ++iy) {
        for (ix = 0; ix < width; ++ix) {
            unsigned x = (order == 1 || order == 2) ? dx - ix : dx + ix;
            unsigned y = (order == 1 || order == 3) ? dy - iy : dy + iy;
            unsigned char *dp = dst + y * destination_stride + x * bpp;
            uint32_t s = color, d;
            CHECK(y * destination_stride + x * bpp + bpp <= BYTES);
            if (n == 11) {
                unsigned xx = (order == 1 || order == 2) ? sx - ix : sx + ix;
                unsigned yy = (order == 1 || order == 3) ? sy - iy : sy + iy;
                CHECK(yy * source_stride + xx * bpp + bpp <= BYTES);
                s = read_pixel(src + yy * source_stride + xx * bpp, bpp);
            }
            d = read_pixel(dp, bpp);
            write_pixel(dp, bpp, truth_table(code, s, d, n == 8));
        }
    }
}

/* Iterate every destination pixel from the untouched source/destination image.
 */
static void oracle(const struct sgx535_2d_raster_surface *s, const struct sgx535_2d_raster_rect *sr,
                   const struct sgx535_2d_raster_rect *dr, const struct sgx535_2d_raster_rect *clip,
                   const unsigned char *original_source, const unsigned char *original_destination,
                   unsigned char *expected, unsigned op, uint32_t color)
{
    unsigned bpp = s->format == SGX535_2D_RASTER_RGB565 ? 2u : 4u;
    uint32_t mask = s->format == SGX535_2D_RASTER_RGB565 ? 0xffffu : UINT32_MAX;
    unsigned x, y;
    memcpy(expected, original_destination, BYTES);
    for (y = 0; y < s->height; ++y) {
        for (x = 0; x < s->width; ++x) {
            int64_t sx, sy;
            unsigned offset = y * s->stride_bytes + x * bpp;
            uint32_t source = color;
            if (!contains(dr, x, y) || (clip != NULL && !contains(clip, x, y)))
                continue;
            if (sr != NULL) {
                sx = (int64_t)sr->x + (int64_t)x - dr->x;
                sy = (int64_t)sr->y + (int64_t)y - dr->y;
                if (sx < 0 || sy < 0 || sx >= s->width || sy >= s->height)
                    continue;
                source = read_pixel(
                    original_source + (size_t)sy * s->stride_bytes + (size_t)sx * bpp, bpp);
            }
            write_pixel(expected + offset, bpp,
                        logic(op, source, read_pixel(original_destination + offset, bpp)) & mask);
            ++pixels;
        }
    }
}

static void words_equal(const uint32_t *got, const uint32_t *literal, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
        CHECK(got[i] == literal[i]);
}

static void literal_vectors(void)
{
    struct sgx535_2d_raster_surface s = {1, 0x1000, 8192, 64, 32, 256, SGX535_2D_RASTER_ARGB8888};
    struct sgx535_2d_raster_rect dr = {3, 5, 17, 9}, sr;
    uint32_t w[WORDS];
    size_t n = 0;
    unsigned i;
    static const uint32_t fill[] = {0x70000000, 0xa0060100, 0x1000,  0x8000f0f0,
                                    0xff336699, 0x3005,     0x11009, 0xf0000000};
    static const uint32_t copies[4][11] = {
        {0x70000000, 0xa0060100, 0x1000, 0x90060100, 0x1000, 0x30004006, 0x8000cccc, 0xffffffff,
         0x2003, 0x7005, 0xf0000000},
        {0x70000000, 0xa0060100, 0x1000, 0x90060100, 0x1000, 0x30008006, 0x8100cccc, 0xffffffff,
         0xa003, 0x7005, 0xf0000000},
        {0x70000000, 0xa0060100, 0x1000, 0x90060100, 0x1000, 0x30004007, 0x8180cccc, 0xffffffff,
         0x200a, 0x7005, 0xf0000000},
        {0x70000000, 0xa0060100, 0x1000, 0x90060100, 0x1000, 0x30008007, 0x8080cccc, 0xffffffff,
         0xa00a, 0x7005, 0xf0000000}};
    CHECK(sgx535_2d_raster_fill(&s, &dr, NULL, 0xff336699, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          n == 8);
    words_equal(w, fill, 8);
    for (i = 0; i < 4; ++i) {
        sr = (struct sgx535_2d_raster_rect){i & 1u ? 2 : 4, i & 2u ? 3 : 6, 7, 5};
        dr = (struct sgx535_2d_raster_rect){i & 1u ? 4 : 2, i & 2u ? 6 : 3, 7, 5};
        CHECK(sgx535_2d_raster_copy(&s, &sr, &s, &dr, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
                  SGX535_2D_RASTER_OK &&
              n == 11);
        words_equal(w, copies[i], 11);
    }
    dr = (struct sgx535_2d_raster_rect){0, 0, 1, 1};
    s.format = SGX535_2D_RASTER_RGB565;
    CHECK(sgx535_2d_raster_fill(&s, &dr, NULL, 0xa55a, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_OK);
    CHECK(w[1] == 0xa0050100 && w[4] == 0xffa0a8d0);
    s.format = SGX535_2D_RASTER_XRGB8888;
    CHECK(sgx535_2d_raster_fill(&s, &dr, NULL, 0x12336699, SGX535_2D_RASTER_XOR, w, WORDS, &n) ==
          SGX535_2D_RASTER_OK);
    CHECK(w[1] == 0xa0058100 && w[3] == 0x80005a5a && w[4] == 0xff336699);
}

static void memory_cases(void)
{
    unsigned char a[BYTES + GUARD * 2], b[BYTES + GUARD * 2], oa[BYTES], ob[BYTES], expected[BYTES];
    unsigned char ga[GUARD * 2], gb[GUARD * 2];
    struct sgx535_2d_raster_surface s = base, d;
    struct sgx535_2d_raster_rect sr, dr, clip;
    uint32_t w[WORDS], before[WORDS];
    unsigned format, op, iteration, index, self, is_copy;
    size_t n;
    for (format = 1; format <= 3; ++format) {
        s.format = (enum sgx535_2d_raster_format)format;
        for (op = 0; op < 16; ++op) {
            for (iteration = 0; iteration < 180; ++iteration) {
                unsigned bpp = format == 3 ? 2u : 4u;
                uint32_t color = random_word();
                if (format == 3)
                    color &= 0xffffu;
                for (index = 0; index < sizeof(a); ++index) {
                    a[index] = (unsigned char)random_word();
                    b[index] = (unsigned char)random_word();
                }
                memcpy(oa, a + GUARD, BYTES);
                memcpy(ob, b + GUARD, BYTES);
                memcpy(ga, a, GUARD);
                memcpy(ga + GUARD, a + GUARD + BYTES, GUARD);
                memcpy(gb, b, GUARD);
                memcpy(gb + GUARD, b + GUARD + BYTES, GUARD);
                self = iteration < 8 || iteration % 3u == 0;
                is_copy = iteration < 8 || iteration % 2u == 0;
                d = s;
                if (!self) {
                    d.storage_id = 2;
                    d.offset_bytes = 0x8000;
                }
                sr = (struct sgx535_2d_raster_rect){(int32_t)(random_word() % 45u) - 10,
                                                    (int32_t)(random_word() % 45u) - 10,
                                                    random_word() % 30u, random_word() % 30u};
                dr = (struct sgx535_2d_raster_rect){(int32_t)(random_word() % 45u) - 10,
                                                    (int32_t)(random_word() % 45u) - 10, sr.width,
                                                    sr.height};
                clip = (struct sgx535_2d_raster_rect){(int32_t)(random_word() % 35u) - 5,
                                                      (int32_t)(random_word() % 35u) - 5,
                                                      random_word() % 30u, random_word() % 30u};
                if (iteration < 8) {
                    sr = (struct sgx535_2d_raster_rect){iteration & 1u ? 2 : 4,
                                                        iteration & 2u ? 3 : 6, 7, 5};
                    dr = (struct sgx535_2d_raster_rect){iteration & 1u ? 4 : 2,
                                                        iteration & 2u ? 6 : 3, 7, 5};
                    clip = (struct sgx535_2d_raster_rect){0, 0, 24, 24};
                    if (iteration == 4 || iteration == 5)
                        dr.y = sr.y;
                    if (iteration == 6 || iteration == 7)
                        dr.x = sr.x;
                }
                for (index = 0; index < WORDS; ++index)
                    w[index] = 0xdeadbeef;
                memcpy(before, w, sizeof(w));
                n = 123;
                if (is_copy) {
                    enum sgx535_2d_raster_status status = sgx535_2d_raster_copy(
                        &s, &sr, &d, &dr, &clip, (enum sgx535_2d_raster_rop)op, w, WORDS, &n);
                    CHECK(status == SGX535_2D_RASTER_OK);
                    oracle(&s, &sr, &dr, &clip, oa, self ? oa : ob, expected, op, 0);
                    if (n != 0)
                        interpret(w, n, a + GUARD, self ? a + GUARD : b + GUARD, bpp);
                } else {
                    CHECK(sgx535_2d_raster_fill(&d, &dr, &clip, color,
                                                (enum sgx535_2d_raster_rop)op, w, WORDS,
                                                &n) == SGX535_2D_RASTER_OK);
                    oracle(&s, NULL, &dr, &clip, oa, self ? oa : ob, expected, op, color);
                    if (n != 0)
                        interpret(w, n, a + GUARD, self ? a + GUARD : b + GUARD, bpp);
                }
                for (index = 0; index < BYTES; ++index) {
                    unsigned char actual = (self ? a : b)[GUARD + index];
                    /* XRGB top bytes of touched pixels are unspecified. */
                    if (format == 2 && index % 128u < s.width * 4u && index % 4u == 3u) {
                        unsigned x = (index % 128u) / 4u, y = index / 128u;
                        int touched = contains(&dr, x, y) && contains(&clip, x, y) && y < s.height;
                        if (is_copy) {
                            int64_t xx = (int64_t)sr.x + x - dr.x, yy = (int64_t)sr.y + y - dr.y;
                            touched =
                                touched && xx >= 0 && yy >= 0 && xx < s.width && yy < s.height;
                        }
                        if (touched)
                            continue;
                    }
                    CHECK(actual == expected[index]);
                }
                for (index = 0; index < GUARD; ++index) {
                    CHECK(a[index] == ga[index] && a[GUARD + BYTES + index] == ga[GUARD + index]);
                    CHECK(b[index] == gb[index] && b[GUARD + BYTES + index] == gb[GUARD + index]);
                }
                if (!self)
                    CHECK(memcmp(a + GUARD, oa, BYTES) == 0);
                /* Interpreter bounds every write; untouched command suffix is a canary.
                 */
                for (index = (unsigned)n; index < WORDS; ++index)
                    CHECK(w[index] == before[index]);
                ++operations;
            }
        }
    }
}

static void prime(uint32_t *w, size_t *n)
{
    unsigned i;
    for (i = 0; i < WORDS; ++i)
        w[i] = 0x13579bdf;
    *n = 99;
}
static void unchanged(const uint32_t *w, size_t n)
{
    unsigned i;
    CHECK(n == 99);
    for (i = 0; i < WORDS; ++i)
        CHECK(w[i] == 0x13579bdf);
}

static void rejection_and_edges(void)
{
    struct sgx535_2d_raster_surface s = base, d = base;
    struct sgx535_2d_raster_rect r = {0, 0, 7, 5}, q = r, c = {0, 0, 7, 5};
    uint32_t w[WORDS];
    size_t n;
#define REJECT_FILL(expected)                                                                      \
    do {                                                                                           \
        prime(w, &n);                                                                              \
        CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==       \
              (expected));                                                                         \
        unchanged(w, n);                                                                           \
    } while (0)
#define REJECT_COPY(expected)                                                                      \
    do {                                                                                           \
        prime(w, &n);                                                                              \
        CHECK(sgx535_2d_raster_copy(&s, &r, &d, &q, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==  \
              (expected));                                                                         \
        unchanged(w, n);                                                                           \
    } while (0)
    d.storage_id = 0;
    REJECT_FILL(SGX535_2D_RASTER_UNPROVEN_ALIAS);
    d = base;
    d.width = UINT32_MAX;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    d.allocation_bytes = UINT64_MAX;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    d.offset_bytes = UINT64_MAX;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    d.stride_bytes = 16384;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    d.stride_bytes = 132;
    REJECT_FILL(SGX535_2D_RASTER_BAD_ALIGNMENT);
    d = base;
    d.offset_bytes = 0x1004;
    REJECT_FILL(SGX535_2D_RASTER_BAD_ALIGNMENT);
    d = base;
    d.allocation_bytes = 23u * 128u + 95u;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    d.format = (enum sgx535_2d_raster_format)99;
    REJECT_FILL(SGX535_2D_RASTER_UNSUPPORTED);
    d = base;
    d.offset_bytes = 0x1008;
    REJECT_COPY(SGX535_2D_RASTER_UNPROVEN_ALIAS);
    d = base;
    d.stride_bytes = 256;
    d.allocation_bytes = 8192;
    REJECT_COPY(SGX535_2D_RASTER_UNPROVEN_ALIAS);
    d = base;
    d.storage_id = 2;
    REJECT_COPY(SGX535_2D_RASTER_UNPROVEN_ALIAS);
    d = base;
    d.storage_id = 2;
    d.offset_bytes = 0x8000;
    d.format = SGX535_2D_RASTER_RGB565;
    REJECT_COPY(SGX535_2D_RASTER_UNSUPPORTED);
    d = base;
    /* Width and height 8 encode like any other size (HW-2D-001). */
    r.width = 8;
    q = r;
    prime(w, &n);
    CHECK(sgx535_2d_raster_copy(&s, &r, &d, &q, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          n == SGX535_2D_RASTER_COPY_WORDS && w[9] == ((8u << 12) | r.height));
    r = (struct sgx535_2d_raster_rect){0, 0, 7, 8};
    q = r;
    prime(w, &n);
    CHECK(sgx535_2d_raster_copy(&s, &r, &d, &q, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          n == SGX535_2D_RASTER_COPY_WORDS && w[9] == ((7u << 12) | 8u));
    r = (struct sgx535_2d_raster_rect){0, 0, 7, 5};
    q = r;
    q.width = 6;
    REJECT_COPY(SGX535_2D_RASTER_UNSUPPORTED);
    q = r;
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, (enum sgx535_2d_raster_rop) - 1, w, WORDS, &n) ==
          SGX535_2D_RASTER_UNSUPPORTED);
    unchanged(w, n);
    prime(w, &n);
    CHECK(sgx535_2d_raster_copy(&s, &r, &d, &q, NULL, (enum sgx535_2d_raster_rop)16, w, WORDS,
                                &n) == SGX535_2D_RASTER_UNSUPPORTED);
    unchanged(w, n);
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, 7, &n) ==
          SGX535_2D_RASTER_NO_SPACE);
    unchanged(w, n);
    prime(w, &n);
    CHECK(sgx535_2d_raster_copy(&s, &r, &d, &q, NULL, SGX535_2D_RASTER_COPY, w, 10, &n) ==
          SGX535_2D_RASTER_NO_SPACE);
    unchanged(w, n);
    d.format = SGX535_2D_RASTER_RGB565;
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0x10000, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_OUT_OF_RANGE);
    unchanged(w, n);
    d = base;
    r = (struct sgx535_2d_raster_rect){INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX};
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          n == 8 && w[5] == 0 && w[6] == 0x18018);
    r = (struct sgx535_2d_raster_rect){INT32_MAX, INT32_MAX, UINT32_MAX, UINT32_MAX};
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, 0, &n) ==
              SGX535_2D_RASTER_OK &&
          n == 0);
    n = 99;
    unchanged(w, n);
    r = (struct sgx535_2d_raster_rect){0, 0, 0, 1};
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, 0, &n) ==
              SGX535_2D_RASTER_OK &&
          n == 0);
    n = 99;
    unchanged(w, n);
    r = (struct sgx535_2d_raster_rect){-2, -3, 11, 9};
    c = (struct sgx535_2d_raster_rect){2, 1, 3, 4};
    CHECK(sgx535_2d_raster_fill(&d, &r, &c, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          w[5] == 0x2001 && w[6] == 0x3004);
    d = (struct sgx535_2d_raster_surface){1, 0x0ffffff8, 8, 2, 1, 128, SGX535_2D_RASTER_ARGB8888};
    r = (struct sgx535_2d_raster_rect){1, 0, 1, 1};
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK &&
          w[2] == 0x0ffffff8);
    d.allocation_bytes = 9;
    REJECT_FILL(SGX535_2D_RASTER_OUT_OF_RANGE);
    d = base;
    prime(w, &n);
    CHECK(sgx535_2d_raster_fill(NULL, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_fill(&d, NULL, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, NULL, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_fill(&d, &r, NULL, 0, SGX535_2D_RASTER_COPY, w, WORDS, NULL) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(NULL, &r, &d, &r, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(&d, NULL, &d, &r, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(&d, &r, NULL, &r, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(&d, &r, &d, NULL, NULL, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(&d, &r, &d, &r, NULL, SGX535_2D_RASTER_COPY, NULL, WORDS, &n) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
    CHECK(sgx535_2d_raster_copy(&d, &r, &d, &r, NULL, SGX535_2D_RASTER_COPY, w, WORDS, NULL) ==
          SGX535_2D_RASTER_INVALID_ARGUMENT);
    unchanged(w, n);
#undef REJECT_FILL
#undef REJECT_COPY
}

static void rgb565_roundtrip(void)
{
    struct sgx535_2d_raster_surface s = base;
    struct sgx535_2d_raster_rect r = {0, 0, 1, 1};
    uint32_t w[WORDS], pixel;
    size_t n;
    s.format = SGX535_2D_RASTER_RGB565;
    for (pixel = 0; pixel <= 0xffffu; ++pixel) {
        CHECK(sgx535_2d_raster_fill(&s, &r, NULL, pixel, SGX535_2D_RASTER_COPY, w, WORDS, &n) ==
              SGX535_2D_RASTER_OK);
        CHECK((((w[4] >> 19) & 31u) << 11 | ((w[4] >> 10) & 63u) << 5 | ((w[4] >> 3) & 31u)) ==
              pixel);
    }
}

int main(void)
{
    literal_vectors();
    rejection_and_edges();
    rgb565_roundtrip();
    memory_cases();
    printf("PASS CPU_NATIVE2D_RASTER checks=%lu operations=%lu oracle_pixels=%lu\n", checks,
           operations, pixels);
    puts("No MMIO/submission/GPU access.");
    return EXIT_SUCCESS;
}
