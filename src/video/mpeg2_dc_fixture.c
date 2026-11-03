// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "mpeg2_dc_fixture.h"
#include <string.h>

struct bits {
    uint8_t bytes[MPEG2_DC16_STREAM_CAPACITY];
    size_t count;
    int error;
};
static void put(struct bits *w, uint32_t value, unsigned n)
{
    unsigned i;
    if (n > 32u || w->count > sizeof(w->bytes) * 8u - n) {
        w->error = 1;
        return;
    }
    for (i = n; i; --i) {
        w->bytes[w->count / 8u] |= (uint8_t)(((value >> (i - 1u)) & 1u) << (7u - w->count % 8u));
        ++w->count;
    }
}
static void start(struct bits *w, uint8_t code)
{
    if (w->count % 8u)
        put(w, 0, (unsigned)(8u - w->count % 8u));
    put(w, UINT32_C(0x00000100) | code, 32);
}
int mpeg2_dc16_build(struct mpeg2_dc16_fixture *out)
{
    struct bits w = {{0}, 0, 0};
    struct mpeg2_dc16_fixture f;
    unsigned i;
    if (!out)
        return 0;
    start(&w, 0xb3); /* Sequence header, square pixels, 25 fps. */
    put(&w, 16, 12);
    put(&w, 16, 12);
    put(&w, 1, 4);
    put(&w, 3, 4);
    put(&w, 1000, 18);
    put(&w, 1, 1);
    put(&w, 20, 10);
    put(&w, 0, 1);
    put(&w, 0, 1);
    put(&w, 0, 1);
    start(&w, 0xb5); /* Main profile / Main level sequence extension. */
    put(&w, 1, 4);
    put(&w, 0x48, 8);
    put(&w, 1, 1);
    put(&w, 1, 2);
    put(&w, 0, 2);
    put(&w, 0, 2);
    put(&w, 0, 12);
    put(&w, 1, 1);
    put(&w, 0, 8);
    put(&w, 0, 1);
    put(&w, 0, 2);
    put(&w, 0, 5);
    start(&w, 0x00); /* Temporal reference 0, I-picture, vbv_delay unknown. */
    put(&w, 0, 10);
    put(&w, 1, 3);
    put(&w, 0xffff, 16);
    put(&w, 0, 1);
    start(&w, 0xb5); /* Picture coding extension; frame, precision 0. */
    put(&w, 8, 4);
    put(&w, 0xffff, 16);
    put(&w, 0, 2);
    put(&w, 3, 2);
    put(&w, 0, 1);
    put(&w, 1, 1);
    put(&w, 0, 1);
    put(&w, 0, 1);
    put(&w, 0, 1);
    put(&w, 0, 1);
    put(&w, 0, 1);
    put(&w, 1, 1);
    put(&w, 1, 1);
    put(&w, 0, 1);
    start(&w, 0x01); /* Slice: quantiser_scale_code 8 (linear scale 16), no extra information. */
    put(&w, 8, 5);
    put(&w, 0, 1);
    put(&w, 1, 1); /* macroblock_address_increment = 1 */
    put(&w, 1, 1); /* I-picture macroblock_type = intra, no quant change */
    for (i = 0; i < 6; ++i) {
        if (i < 4)
            put(&w, 4, 3); /* Luminance DC-size zero: 100. */
        else
            put(&w, 0, 2); /* Chrominance DC-size zero: 00. */
        put(&w, 2, 2);     /* AC end-of-block: 10, intra_vlc_format=0. */
    }
    start(&w, 0xb7);
    if (w.error)
        return 0;
    memset(&f, 0, sizeof(f));
    f.stream_bytes = w.count / 8u;
    memcpy(f.stream, w.bytes, f.stream_bytes);
    memset(f.yuv420, 128, sizeof(f.yuv420));
    memcpy(out, &f, sizeof(f));
    return 1;
}
static int overlap(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (n > UINTPTR_MAX - x || m > UINTPTR_MAX - y)
        return 1;
    return x < y + m && y < x + n;
}
enum mpeg2_dc16_status mpeg2_dc16_compare(const struct mpeg2_dc16_plane p[3], uint8_t guard,
                                          struct mpeg2_dc16_comparison *out)
{
    struct mpeg2_dc16_comparison c;
    unsigned i, j;
    int different = 0;
    if (!p || !out)
        return MPEG2_DC16_ARGUMENT;
    if (overlap(p, 3u * sizeof(*p), out, sizeof(*out)))
        return MPEG2_DC16_ALIAS;
    for (i = 0; i < 3; ++i) {
        size_t width = i ? 8u : 16u;
        if (!p[i].data || !p[i].bytes || p[i].bytes > MPEG2_DC16_MAX_PLANE_BYTES ||
            p[i].stride < width || p[i].offset > p[i].bytes ||
            p[i].stride > (p[i].bytes - p[i].offset) / (width - 1u) ||
            width > p[i].bytes - p[i].offset - (width - 1u) * p[i].stride)
            return MPEG2_DC16_LAYOUT;
        if (overlap(p[i].data, p[i].bytes, out, sizeof(*out)) ||
            overlap(p[i].data, p[i].bytes, p, 3u * sizeof(*p)))
            return MPEG2_DC16_ALIAS;
        for (j = 0; j < i; ++j)
            if (overlap(p[i].data, p[i].bytes, p[j].data, p[j].bytes))
                return MPEG2_DC16_ALIAS;
    }
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; ++i) {
        size_t at, width = i ? 8u : 16u;
        c.first_bad_byte[i] = SIZE_MAX;
        for (at = 0; at < p[i].bytes; ++at) {
            size_t relative = at >= p[i].offset ? at - p[i].offset : SIZE_MAX;
            int active = relative != SIZE_MAX && relative / p[i].stride < width &&
                         relative % p[i].stride < width;
            uint8_t expected = active ? 128 : guard;
            if (p[i].data[at] != expected) {
                uint32_t delta = p[i].data[at] > expected ? (uint32_t)p[i].data[at] - expected
                                                          : (uint32_t)expected - p[i].data[at];
                different = 1;
                if (c.first_bad_byte[i] == SIZE_MAX)
                    c.first_bad_byte[i] = at;
                if (active) {
                    ++c.sample_errors[i];
                    if (delta > c.maximum_sample_error[i])
                        c.maximum_sample_error[i] = delta;
                } else
                    ++c.guard_errors[i];
            }
        }
    }
    memcpy(out, &c, sizeof(c));
    return different ? MPEG2_DC16_DIFFERENT : MPEG2_DC16_MATCH;
}
