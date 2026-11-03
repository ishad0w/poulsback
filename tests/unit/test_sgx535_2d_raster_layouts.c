// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Independent CPU regression for distinct source/destination BO layouts.
 * The oracle visits original destination pixels. The stream interpreter uses
 * each emitted surface's pitch and resolves its raw offset separately. */
#include "sgx535_2d_encoder.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GUARD = 32, WORDS = 16 };
static unsigned long checks, cases, nonempty, noops, oracle_pixels;
static uint32_t rng = UINT32_C(0xdec0d02d);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint32_t random_word(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static uint32_t read_pixel(const unsigned char *p, unsigned bpp)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
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
static uint32_t gx(unsigned op, uint32_t source, uint32_t destination)
{
    switch (op) {
    case 0:
        return 0;
    case 1:
        return source & destination;
    case 2:
        return source & ~destination;
    case 3:
        return source;
    case 4:
        return ~source & destination;
    case 5:
        return destination;
    case 6:
        return source ^ destination;
    case 7:
        return source | destination;
    case 8:
        return ~(source | destination);
    case 9:
        return ~(source ^ destination);
    case 10:
        return ~destination;
    case 11:
        return source | ~destination;
    case 12:
        return ~source;
    case 13:
        return ~source | destination;
    case 14:
        return ~(source & destination);
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
struct membership {
    unsigned count, minx, miny, maxx, maxy;
};
static struct membership oracle(const struct sgx535_2d_raster_surface *src,
                                const struct sgx535_2d_raster_rect *sr,
                                const struct sgx535_2d_raster_surface *dst,
                                const struct sgx535_2d_raster_rect *dr,
                                const struct sgx535_2d_raster_rect *clip,
                                const unsigned char *source, const unsigned char *destination,
                                unsigned char *expected, unsigned char *touched, unsigned op)
{
    unsigned x, y, bpp = src->format == SGX535_2D_RASTER_RGB565 ? 2u : 4u;
    struct membership m = {0, dst->width, dst->height, 0, 0};
    memcpy(expected, destination, (size_t)dst->allocation_bytes);
    memset(touched, 0, (size_t)dst->allocation_bytes);
    for (y = 0; y < dst->height; ++y)
        for (x = 0; x < dst->width; ++x) {
            int64_t sx = (int64_t)sr->x + x - dr->x, sy = (int64_t)sr->y + y - dr->y;
            size_t d, s;
            if (!contains(dr, x, y) || (clip != NULL && !contains(clip, x, y)) || sx < 0 ||
                sy < 0 || sx >= src->width || sy >= src->height)
                continue;
            d = (size_t)y * dst->stride_bytes + (size_t)x * bpp;
            s = (size_t)sy * src->stride_bytes + (size_t)sx * bpp;
            CHECK(d + bpp <= dst->allocation_bytes && s + bpp <= src->allocation_bytes);
            write_pixel(expected + d, bpp,
                        gx(op, read_pixel(source + s, bpp), read_pixel(destination + d, bpp)));
            if (bpp == 4)
                touched[d + 3] = 1;
            if (x < m.minx)
                m.minx = x;
            if (x > m.maxx)
                m.maxx = x;
            if (y < m.miny)
                m.miny = y;
            if (y > m.maxy)
                m.maxy = y;
            ++m.count;
        }
    return m;
}
static uint32_t decode_rop(uint32_t code, uint32_t source, uint32_t destination)
{
    uint32_t result = 0;
    unsigned bit;
    for (bit = 0; bit < 32; ++bit) {
        unsigned index = (((source >> bit) & 1u) << 1) | ((destination >> bit) & 1u);
        result |= ((code >> index) & 1u) << bit;
    }
    return result;
}
static void interpret(const uint32_t *w, size_t count, const struct sgx535_2d_raster_surface *src,
                      const struct sgx535_2d_raster_surface *dst, const unsigned char *source,
                      unsigned char *destination)
{
    unsigned bpp = src->format == SGX535_2D_RASTER_RGB565 ? 2u : 4u;
    unsigned x, y, sx = (w[5] >> 12) & 4095u, sy = w[5] & 4095u;
    unsigned dx = (w[8] >> 12) & 4095u, dy = w[8] & 4095u;
    unsigned width = (w[9] >> 12) & 4095u, height = w[9] & 4095u;
    unsigned sp = w[3] & 0x7fffu, dp = w[1] & 0x7fffu;
    uint32_t code = w[6] & 255u;
    uint32_t format = src->format == SGX535_2D_RASTER_RGB565     ? 0x50000u
                      : src->format == SGX535_2D_RASTER_XRGB8888 ? 0x58000u
                                                                 : 0x60000u;
    CHECK(count == 11 && w[0] == 0x70000000 && w[10] == 0xf0000000);
    CHECK(w[2] == dst->offset_bytes && w[4] == src->offset_bytes);
    CHECK((w[1] & 0xffff8000u) == (0xa0000000u | format));
    CHECK((w[3] & 0xffff8000u) == (0x90000000u | format));
    CHECK(w[5] == (0x30000000u | (sx << 12) | sy));
    CHECK(w[6] == (0x80000000u | code | (code << 8)) && w[7] == UINT32_MAX);
    CHECK(sp == src->stride_bytes && dp == dst->stride_bytes && width && height);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            size_t s = (size_t)(sy + y) * sp + (size_t)(sx + x) * bpp;
            size_t d = (size_t)(dy + y) * dp + (size_t)(dx + x) * bpp;
            CHECK(sx + x < src->width && sy + y < src->height);
            CHECK(dx + x < dst->width && dy + y < dst->height);
            CHECK(s + bpp <= src->allocation_bytes && d + bpp <= dst->allocation_bytes);
            write_pixel(
                destination + d, bpp,
                decode_rop(code, read_pixel(source + s, bpp), read_pixel(destination + d, bpp)));
        }
}
static void one_case(const struct sgx535_2d_raster_surface *src,
                     const struct sgx535_2d_raster_rect *sr,
                     const struct sgx535_2d_raster_surface *dst,
                     const struct sgx535_2d_raster_rect *dr,
                     const struct sgx535_2d_raster_rect *clip, unsigned op)
{
    size_t source_bytes = (size_t)src->allocation_bytes,
           destination_bytes = (size_t)dst->allocation_bytes, i, n = 99;
    unsigned char *s = malloc(source_bytes + 2u * GUARD),
                  *d = malloc(destination_bytes + 2u * GUARD);
    unsigned char *original_source = malloc(source_bytes + 2u * GUARD),
                  *original_destination = malloc(destination_bytes + 2u * GUARD);
    unsigned char *expected = malloc(destination_bytes), *touched = malloc(destination_bytes);
    uint32_t words[WORDS], before[WORDS];
    struct membership m;
    enum sgx535_2d_raster_status status;
    CHECK(s && d && original_source && original_destination && expected && touched);
    for (i = 0; i < source_bytes + 2u * GUARD; ++i)
        s[i] = (unsigned char)random_word();
    for (i = 0; i < destination_bytes + 2u * GUARD; ++i)
        d[i] = (unsigned char)random_word();
    memcpy(original_source, s, source_bytes + 2u * GUARD);
    memcpy(original_destination, d, destination_bytes + 2u * GUARD);
    for (i = 0; i < WORDS; ++i)
        words[i] = 0xcafe0123u;
    memcpy(before, words, sizeof(words));
    m = oracle(src, sr, dst, dr, clip, s + GUARD, d + GUARD, expected, touched, op);
    status = sgx535_2d_raster_copy(src, sr, dst, dr, clip, (enum sgx535_2d_raster_rop)op, words,
                                   WORDS, &n);
    ++cases;
    {
        CHECK(status == SGX535_2D_RASTER_OK);
        oracle_pixels += m.count;
        if (m.count) {
            CHECK(n == 11);
            interpret(words, n, src, dst, s + GUARD, d + GUARD);
            ++nonempty;
        } else {
            CHECK(n == 0);
            ++noops;
        }
        for (i = 0; i < destination_bytes; ++i)
            if (!(dst->format == SGX535_2D_RASTER_XRGB8888 && touched[i]))
                CHECK(d[GUARD + i] == expected[i]);
        for (i = n; i < WORDS; ++i)
            CHECK(words[i] == before[i]);
    }
    CHECK(memcmp(s, original_source, source_bytes + 2u * GUARD) == 0);
    CHECK(memcmp(d, original_destination, GUARD) == 0);
    CHECK(memcmp(d + GUARD + destination_bytes, original_destination + GUARD + destination_bytes,
                 GUARD) == 0);
    free(s);
    free(d);
    free(original_source);
    free(original_destination);
    free(expected);
    free(touched);
}
/* Thin surfaces reach the declared packet limits without a large framebuffer.
 * Coordinates/pitches below are independent contract literals. Final-row
 * padding is deliberately absent; neither software result qualifies hardware. */
static unsigned long boundary_cases, boundary_refusals;

static void boundary_case(const struct sgx535_2d_raster_surface *src,
                          const struct sgx535_2d_raster_rect *sr,
                          const struct sgx535_2d_raster_surface *dst,
                          const struct sgx535_2d_raster_rect *dr, unsigned op)
{
    unsigned char source_before[sizeof(*src)], destination_before[sizeof(*dst)];
    unsigned char sr_before[sizeof(*sr)], dr_before[sizeof(*dr)];
    memcpy(source_before, src, sizeof(*src));
    memcpy(destination_before, dst, sizeof(*dst));
    memcpy(sr_before, sr, sizeof(*sr));
    memcpy(dr_before, dr, sizeof(*dr));
    one_case(src, sr, dst, dr, NULL, op);
    CHECK(!memcmp(source_before, src, sizeof(*src)));
    CHECK(!memcmp(destination_before, dst, sizeof(*dst)));
    CHECK(!memcmp(sr_before, sr, sizeof(*sr)));
    CHECK(!memcmp(dr_before, dr, sizeof(*dr)));
    ++boundary_cases;
}

static void boundary_refusal(const struct sgx535_2d_raster_surface *src,
                             const struct sgx535_2d_raster_surface *dst, unsigned op)
{
    struct sgx535_2d_raster_rect rect = {0, 0, 1, 1};
    unsigned char source_before[sizeof(*src)], destination_before[sizeof(*dst)];
    uint32_t words[WORDS], before[WORDS];
    size_t written = 99;
    unsigned i;
    memcpy(source_before, src, sizeof(*src));
    memcpy(destination_before, dst, sizeof(*dst));
    for (i = 0; i < WORDS; ++i)
        words[i] = UINT32_C(0x5a170000) + i;
    memcpy(before, words, sizeof(words));
    CHECK(sgx535_2d_raster_copy(src, &rect, dst, &rect, NULL, (enum sgx535_2d_raster_rop)op, words,
                                WORDS, &written) == SGX535_2D_RASTER_OUT_OF_RANGE);
    CHECK(written == 99 && !memcmp(before, words, sizeof(words)));
    CHECK(!memcmp(source_before, src, sizeof(*src)));
    CHECK(!memcmp(destination_before, dst, sizeof(*dst)));
    ++boundary_refusals;
}

static void maximum_domain_cases(void)
{
    unsigned format, op, shape, mutation, side;
    for (format = 1; format <= 3; ++format)
        for (op = 0; op < 16; ++op) {
            unsigned bpp = format == 3 ? 2u : 4u;
            for (shape = 0; shape < 5; ++shape) {
                struct sgx535_2d_raster_surface src = {
                    1, UINT64_C(0x1000), 0, 1, 1, 128, (enum sgx535_2d_raster_format)format};
                struct sgx535_2d_raster_surface dst = src;
                struct sgx535_2d_raster_rect sr, dr;
                dst.storage_id = 2;
                dst.offset_bytes = UINT64_C(0x04000000);
                if (shape == 0) {
                    src.width = dst.width = bpp == 2 ? 4095 : 4064;
                    src.stride_bytes = bpp == 2 ? 8192 : 16256;
                    dst.stride_bytes = 16256;
                } else if (shape == 1) {
                    src.height = dst.height = 4095;
                    dst.stride_bytes = 256;
                } else if (shape == 2) {
                    src.width = dst.width = 13;
                    src.height = dst.height = 2;
                    src.stride_bytes = dst.stride_bytes = 16256;
                }
                src.allocation_bytes =
                    (uint64_t)(src.height - 1) * src.stride_bytes + (uint64_t)src.width * bpp;
                dst.allocation_bytes =
                    (uint64_t)(dst.height - 1) * dst.stride_bytes + (uint64_t)dst.width * bpp;
                if (shape == 3) {
                    src.offset_bytes = UINT64_C(0x0ffffff8);
                    src.allocation_bytes = 8;
                } else if (shape == 4) {
                    dst.offset_bytes = UINT64_C(0x0ffffff8);
                    dst.allocation_bytes = 8;
                }
                sr = (struct sgx535_2d_raster_rect){0, 0, src.width, src.height};
                dr = sr;
                boundary_case(&src, &sr, &dst, &dr, op);
                if (shape < 2) {
                    sr = (struct sgx535_2d_raster_rect){(int32_t)src.width - 1,
                                                        (int32_t)src.height - 1, 1, 1};
                    dr = (struct sgx535_2d_raster_rect){0, 0, 1, 1};
                    boundary_case(&src, &sr, &dst, &dr, op);
                    boundary_case(&src, &dr, &dst, &sr, op);
                }
            }
            for (mutation = 0; mutation < 6; ++mutation)
                for (side = 0; side < 2; ++side) {
                    struct sgx535_2d_raster_surface src = {
                        1, UINT64_C(0x1000), bpp, 1, 1, 128, (enum sgx535_2d_raster_format)format};
                    struct sgx535_2d_raster_surface dst = src, *bad;
                    dst.storage_id = 2;
                    dst.offset_bytes = UINT64_C(0x04000000);
                    bad = side ? &dst : &src;
                    if (mutation == 0) {
                        bad->width = 4096;
                        bad->stride_bytes = 16256;
                        bad->allocation_bytes = (uint64_t)4096 * bpp;
                    } else if (mutation == 1) {
                        bad->height = 4096;
                        bad->allocation_bytes = (uint64_t)4095 * 128 + bpp;
                    } else if (mutation == 2) {
                        bad->stride_bytes = 16384;
                    } else if (mutation == 3) {
                        bad->height = 4095;
                        bad->allocation_bytes = (uint64_t)4094 * 128 + bpp - 1;
                    } else if (mutation == 4) {
                        bad->offset_bytes = UINT64_C(0x0ffffff8);
                        bad->allocation_bytes = 9;
                    } else {
                        bad->allocation_bytes = UINT64_MAX;
                    }
                    boundary_refusal(&src, &dst, op);
                }
        }
    CHECK(boundary_cases == 432 && boundary_refusals == 576);
    printf("PASS CPU_NATIVE2D_MAX_DOMAIN cases=%lu refusals=%lu\n", boundary_cases,
           boundary_refusals);
}

int main(void)
{
    static const unsigned layouts[4][6] = {{17, 23, 128, 31, 11, 256},
                                           {53, 9, 256, 13, 31, 128},
                                           {1, 29, 128, 27, 1, 128},
                                           {61, 39, 256, 41, 33, 384}};
    unsigned format, op, layout, iteration;
    for (format = 1; format <= 3; ++format)
        for (op = 0; op < 16; ++op)
            for (layout = 0; layout < 4; ++layout) {
                unsigned bpp = format == 3 ? 2u : 4u;
                struct sgx535_2d_raster_surface src = {1,
                                                       0x1000,
                                                       0,
                                                       layouts[layout][0],
                                                       layouts[layout][1],
                                                       layouts[layout][2],
                                                       (enum sgx535_2d_raster_format)format};
                struct sgx535_2d_raster_surface dst = {2,
                                                       0x10000,
                                                       0,
                                                       layouts[layout][3],
                                                       layouts[layout][4],
                                                       layouts[layout][5],
                                                       (enum sgx535_2d_raster_format)format};
                src.allocation_bytes =
                    (uint64_t)(src.height - 1) * src.stride_bytes + (uint64_t)src.width * bpp;
                dst.allocation_bytes =
                    (uint64_t)(dst.height - 1) * dst.stride_bytes + (uint64_t)dst.width * bpp;
                for (iteration = 0; iteration < 80; ++iteration) {
                    struct sgx535_2d_raster_rect sr = {(int32_t)(random_word() % 70u) - 20,
                                                       (int32_t)(random_word() % 60u) - 20,
                                                       random_word() % 80u, random_word() % 70u};
                    struct sgx535_2d_raster_rect dr = {(int32_t)(random_word() % 70u) - 20,
                                                       (int32_t)(random_word() % 60u) - 20,
                                                       sr.width, sr.height};
                    struct sgx535_2d_raster_rect clip = {(int32_t)(random_word() % 55u) - 10,
                                                         (int32_t)(random_word() % 50u) - 10,
                                                         random_word() % 75u, random_word() % 65u};
                    const struct sgx535_2d_raster_rect *cp = iteration % 2u ? &clip : NULL;
                    if (iteration == 0) {
                        sr = (struct sgx535_2d_raster_rect){0, 0, UINT32_MAX, UINT32_MAX};
                        dr = sr;
                    } else if (iteration == 1) {
                        sr = (struct sgx535_2d_raster_rect){-2, -3, 31, 21};
                        dr = (struct sgx535_2d_raster_rect){-5, -7, 31, 21};
                        clip = (struct sgx535_2d_raster_rect){1, 0, 27, 17};
                    } else if (iteration == 2) {
                        sr = (struct sgx535_2d_raster_rect){INT32_MIN, INT32_MIN, UINT32_MAX,
                                                            UINT32_MAX};
                        dr = sr;
                    } else if (iteration == 3) {
                        sr = (struct sgx535_2d_raster_rect){INT32_MIN, INT32_MAX, UINT32_MAX,
                                                            UINT32_MAX};
                        dr = (struct sgx535_2d_raster_rect){INT32_MAX, INT32_MIN, UINT32_MAX,
                                                            UINT32_MAX};
                        clip = (struct sgx535_2d_raster_rect){INT32_MIN, INT32_MIN, UINT32_MAX,
                                                              UINT32_MAX};
                    } else if (iteration == 4) {
                        sr = (struct sgx535_2d_raster_rect){0, 0, 8, 7};
                        dr = sr;
                    } else if (iteration == 5) {
                        sr = (struct sgx535_2d_raster_rect){0, 0, 9, 7};
                        dr = (struct sgx535_2d_raster_rect){-1, 0, 9, 7};
                        cp = NULL;
                    } else if (iteration == 6) {
                        sr = (struct sgx535_2d_raster_rect){0, 0, 7, 8};
                        dr = sr;
                        cp = NULL;
                    } else if (iteration == 7) {
                        sr = (struct sgx535_2d_raster_rect){0, 0, 0, UINT32_MAX};
                        dr = sr;
                    }
                    one_case(&src, &sr, &dst, &dr, cp, op);
                }
            }
    maximum_domain_cases();
    printf("PASS CPU_NATIVE2D_DISTINCT_LAYOUTS checks=%lu cases=%lu nonempty=%lu "
           "noops=%lu oracle_pixels=%lu\n",
           checks, cases, nonempty, noops, oracle_pixels);
    puts("Independent distinct-layout CPU oracle; no hardware access or timing "
         "proof.");
    return 0;
}
