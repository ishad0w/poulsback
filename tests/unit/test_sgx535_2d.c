// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_2d_encoder_v1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define SENTINEL UINT32_C(0x5a5a5a5a)

static const struct sgx535_2d_surface destination = {UINT64_C(0x1000),  8192, 64, 32, 256,
                                                     SGX535_2D_ARGB8888};
static const struct sgx535_2d_rect rectangle = {3, 5, 17, 9};

static void prime(uint32_t out[12], size_t *written)
{
    size_t i;
    for (i = 0; i < 12; ++i)
        out[i] = SENTINEL;
    *written = 99;
}

static void untouched(const uint32_t out[12], size_t written)
{
    size_t i;
    CHECK(written == 99);
    for (i = 0; i < 12; ++i)
        CHECK(out[i] == SENTINEL);
}

static void reject_fill(struct sgx535_2d_surface dst, struct sgx535_2d_rect rect,
                        enum sgx535_2d_status expected)
{
    uint32_t out[12];
    size_t written;
    prime(out, &written);
    CHECK(sgx535_2d_build_fill(&dst, &rect, UINT32_C(0xff336699), out, 12, &written) == expected);
    untouched(out, written);
}

static void reject_copy(struct sgx535_2d_surface src, struct sgx535_2d_rect src_rect,
                        struct sgx535_2d_surface dst, struct sgx535_2d_rect dst_rect,
                        enum sgx535_2d_status expected)
{
    uint32_t out[12];
    size_t written;
    prime(out, &written);
    CHECK(sgx535_2d_build_copy(&src, &src_rect, &dst, &dst_rect, out, 12, &written) == expected);
    untouched(out, written);
}

static void historical_vectors(void)
{
    /*
     * Literal, manually evaluated vectors from the archived historical driver,
     * not constructed with implementation macros. psb_fb.c:490-523 and 616-671,
     * commit 98b5307e5158a9ac401b29128ddd1184ae06b4d7. These are source-derived
     * reference words, NOT captured successful hardware traces.
     */
    static const uint32_t fill_expected[] = {0x70000000, 0xa0060100, 0x00001000, 0x8000f0f0,
                                             0xff336699, 0x00003005, 0x00011009, 0xf0000000};
    static const uint32_t copy_expected[] = {0x70000000, 0xa0060100, 0x00008000, 0x90060100,
                                             0x00002000, 0x30004006, 0x8001cccc, 0x00002003,
                                             0x00007005, 0xf0000000};
    struct sgx535_2d_surface src = destination, dst = destination;
    const struct sgx535_2d_rect src_rect = {4, 6, 7, 5};
    const struct sgx535_2d_rect dst_rect = {2, 3, 7, 5};
    uint32_t out[12];
    size_t written;

    prime(out, &written);
    CHECK(sgx535_2d_build_fill(&destination, &rectangle, 0xff336699, out, 12, &written) ==
          SGX535_2D_OK);
    CHECK(written == 8);
    CHECK(memcmp(out, fill_expected, sizeof(fill_expected)) == 0);
    CHECK(out[8] == SENTINEL && out[11] == SENTINEL);

    src.offset_bytes = 0x2000;
    dst.offset_bytes = 0x8000;
    prime(out, &written);
    CHECK(sgx535_2d_build_copy(&src, &src_rect, &dst, &dst_rect, out, 12, &written) ==
          SGX535_2D_OK);
    CHECK(written == 10);
    CHECK(memcmp(out, copy_expected, sizeof(copy_expected)) == 0);
    CHECK(out[10] == SENTINEL && out[11] == SENTINEL);
}

static void surface_and_rectangle_limits(void)
{
    struct sgx535_2d_surface dst;
    struct sgx535_2d_rect rect;
    uint32_t out[12];
    size_t written;

    dst = destination;
    dst.format = (enum sgx535_2d_format)0;
    reject_fill(dst, rectangle, SGX535_2D_UNSUPPORTED);
    dst.format = (enum sgx535_2d_format)2;
    reject_fill(dst, rectangle, SGX535_2D_UNSUPPORTED);
    dst = destination;
    dst.width = 0;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.height = 0;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.width = 4096;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.height = UINT32_MAX;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.stride_bytes = 0;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.stride_bytes = 32768;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.stride_bytes = UINT32_MAX;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.stride_bytes = 252;
    reject_fill(dst, rectangle, SGX535_2D_BAD_ALIGNMENT);
    dst = destination;
    dst.stride_bytes = 128;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.offset_bytes = 0x1004;
    reject_fill(dst, rectangle, SGX535_2D_BAD_ALIGNMENT);
    dst = destination;
    dst.offset_bytes = UINT64_MAX;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.offset_bytes = 0x10000000;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.allocation_bytes = UINT64_MAX;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.allocation_bytes = 0;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);
    dst = destination;
    dst.allocation_bytes = 8191;
    reject_fill(dst, rectangle, SGX535_2D_OUT_OF_RANGE);

    rect = rectangle;
    rect.width = 0;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = rectangle;
    rect.height = 0;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = rectangle;
    rect.x = UINT32_MAX;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = rectangle;
    rect.y = UINT32_MAX;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = rectangle;
    rect.width = UINT32_MAX;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = rectangle;
    rect.height = UINT32_MAX;
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = (struct sgx535_2d_rect){63, 31, 2, 1};
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);
    rect = (struct sgx535_2d_rect){63, 31, 1, 2};
    reject_fill(destination, rect, SGX535_2D_OUT_OF_RANGE);

    dst = (struct sgx535_2d_surface){0,     4095u * UINT64_C(16384), 4095, 4095,
                                     16384, SGX535_2D_ARGB8888};
    rect = (struct sgx535_2d_rect){0, 0, 4095, 4095};
    CHECK(sgx535_2d_build_fill(&dst, &rect, UINT32_MAX, out, 8, &written) == SGX535_2D_OK);
    CHECK(out[6] == 0x00ffffff && out[4] == UINT32_MAX);
    rect = (struct sgx535_2d_rect){4094, 4094, 1, 1};
    CHECK(sgx535_2d_build_fill(&dst, &rect, 0, out, 8, &written) == SGX535_2D_OK);
    CHECK(out[5] == 0x00ffeffe && out[6] == 0x00001001);

    /* Highest byte-stride that fits the field AND the conservative alignment. */
    dst = (struct sgx535_2d_surface){0,     4095u * UINT64_C(32640), 4095, 4095,
                                     32640, SGX535_2D_ARGB8888};
    CHECK(sgx535_2d_build_fill(&dst, &rect, 0, out, 8, &written) == SGX535_2D_OK);
    CHECK(out[1] == 0xa0067f80);

    /* Exact allocation/aperture boundary; last-row padding is not touched. */
    dst = (struct sgx535_2d_surface){0x0ffffff8, 8, 2, 1, 128, SGX535_2D_ARGB8888};
    rect = (struct sgx535_2d_rect){1, 0, 1, 1};
    CHECK(sgx535_2d_build_fill(&dst, &rect, 0, out, 8, &written) == SGX535_2D_OK);
    CHECK(out[2] == 0x0ffffff8);
    dst.allocation_bytes = 9;
    reject_fill(dst, rect, SGX535_2D_OUT_OF_RANGE);
    dst.allocation_bytes = 7;
    reject_fill(dst, rect, SGX535_2D_OUT_OF_RANGE);
}

static void copy_limits_and_aliases(void)
{
    struct sgx535_2d_surface dst = destination;
    struct sgx535_2d_surface invalid = destination;
    struct sgx535_2d_rect from = {0, 0, 7, 5}, to = {0, 0, 7, 5};
    uint32_t out[12];
    size_t written;

    dst.offset_bytes = 0x8000;
    invalid.format = (enum sgx535_2d_format)2;
    reject_copy(invalid, from, dst, to, SGX535_2D_UNSUPPORTED);
    reject_copy(destination, from, invalid, to, SGX535_2D_UNSUPPORTED);
    invalid = destination;
    invalid.allocation_bytes = UINT64_MAX;
    reject_copy(invalid, from, dst, to, SGX535_2D_OUT_OF_RANGE);
    reject_copy(destination, from, invalid, to, SGX535_2D_OUT_OF_RANGE);
    from.width = UINT32_MAX;
    reject_copy(destination, from, dst, to, SGX535_2D_OUT_OF_RANGE);
    from = to;
    to.height = 0;
    reject_copy(destination, from, dst, to, SGX535_2D_OUT_OF_RANGE);
    to = from;
    to.width = 6;
    reject_copy(destination, from, dst, to, SGX535_2D_UNSUPPORTED);
    to = from;
    to.height = 4;
    reject_copy(destination, from, dst, to, SGX535_2D_UNSUPPORTED);
    from.width = 8;
    to = from;
    reject_copy(destination, from, dst, to, SGX535_2D_UNSUPPORTED);
    from.width = 7;
    from.height = 8;
    to = from;
    reject_copy(destination, from, dst, to, SGX535_2D_UNSUPPORTED);

    from = (struct sgx535_2d_rect){0, 0, 7, 5};
    to = from;
    reject_copy(destination, from, destination, to, SGX535_2D_OVERLAP);
    to.x = 1;
    reject_copy(destination, from, destination, to, SGX535_2D_OVERLAP);
    to = from;
    to.y = 1;
    reject_copy(destination, from, destination, to, SGX535_2D_OVERLAP);
    /* Different surface bases can still address overlapping bytes. */
    dst = destination;
    dst.offset_bytes += 8;
    reject_copy(destination, from, dst, from, SGX535_2D_OVERLAP);
    /* Disjoint horizontal pixels across rows still have overlapping spans. */
    from = (struct sgx535_2d_rect){0, 0, 1, 2};
    to = (struct sgx535_2d_rect){2, 0, 1, 2};
    reject_copy(destination, from, destination, to, SGX535_2D_OVERLAP);

    /* Adjacent one-pixel intervals do not overlap; same allocation is allowed. */
    from = (struct sgx535_2d_rect){0, 0, 1, 1};
    to = (struct sgx535_2d_rect){1, 0, 1, 1};
    CHECK(sgx535_2d_build_copy(&destination, &from, &destination, &to, out, 10, &written) ==
          SGX535_2D_OK);
    CHECK(written == 10 && out[6] == 0x8001cccc);
    /* Destination before source is equally valid for nonoverlapping spans. */
    CHECK(sgx535_2d_build_copy(&destination, &to, &destination, &from, out, 10, &written) ==
          SGX535_2D_OK);
    CHECK(out[6] == 0x8001cccc);
}

static void invalid_arguments_and_capacity(void)
{
    uint32_t out[12];
    size_t written;
    prime(out, &written);
    CHECK(sgx535_2d_build_fill(NULL, &rectangle, 0, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_fill(&destination, NULL, 0, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_fill(&destination, &rectangle, 0, NULL, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_fill(&destination, &rectangle, 0, out, 12, NULL) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_fill(&destination, &rectangle, 0, out, 7, &written) ==
          SGX535_2D_NO_SPACE);
    CHECK(sgx535_2d_build_copy(NULL, &rectangle, &destination, &rectangle, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, NULL, &destination, &rectangle, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, &rectangle, NULL, &rectangle, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, &rectangle, &destination, NULL, out, 12, &written) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, &rectangle, &destination, &rectangle, NULL, 12,
                               &written) == SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, &rectangle, &destination, &rectangle, out, 12, NULL) ==
          SGX535_2D_INVALID_ARGUMENT);
    CHECK(sgx535_2d_build_copy(&destination, &rectangle, &destination, &rectangle, out, 9,
                               &written) == SGX535_2D_NO_SPACE);
    untouched(out, written);
}

int main(void)
{
    historical_vectors();
    surface_and_rectangle_limits();
    copy_limits_and_aliases();
    invalid_arguments_and_capacity();
    puts("sgx535_2d: CPU packet serialization PASS; no hardware dispatch");
    return EXIT_SUCCESS;
}
