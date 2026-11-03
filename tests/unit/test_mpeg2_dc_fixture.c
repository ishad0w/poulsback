// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "mpeg2_dc_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)
static uint32_t take(const uint8_t *p, size_t *position, unsigned n)
{
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < n; ++i) {
        size_t at = (*position)++;
        value = value * 2u + (((uint32_t)p[at / 8u] >> (7u - at % 8u)) & 1u);
    }
    return value;
}
static void syntax(const struct mpeg2_dc16_fixture *f)
{
    size_t at = 0;
    unsigned i;
    CHECK(f->stream_bytes == 52);
    CHECK(take(f->stream, &at, 32) == 0x1b3);
    CHECK(take(f->stream, &at, 12) == 16 && take(f->stream, &at, 12) == 16);
    CHECK(take(f->stream, &at, 4) == 1 && take(f->stream, &at, 4) == 3);
    CHECK(take(f->stream, &at, 18) == 1000 && take(f->stream, &at, 1) == 1);
    CHECK(take(f->stream, &at, 10) == 20 && take(f->stream, &at, 3) == 0);
    CHECK(take(f->stream, &at, 32) == 0x1b5);
    CHECK(take(f->stream, &at, 4) == 1 && take(f->stream, &at, 8) == 0x48);
    CHECK(take(f->stream, &at, 1) == 1 && take(f->stream, &at, 2) == 1);
    CHECK(take(f->stream, &at, 16) == 0 && take(f->stream, &at, 1) == 1);
    CHECK(take(f->stream, &at, 16) == 0);
    CHECK(take(f->stream, &at, 32) == 0x100);
    CHECK(take(f->stream, &at, 10) == 0 && take(f->stream, &at, 3) == 1);
    CHECK(take(f->stream, &at, 16) == 0xffff && take(f->stream, &at, 3) == 0);
    CHECK(take(f->stream, &at, 32) == 0x1b5);
    CHECK(take(f->stream, &at, 4) == 8 && take(f->stream, &at, 16) == 0xffff);
    CHECK(take(f->stream, &at, 4) == 3);      /* precision 0 + frame structure 3 */
    CHECK(take(f->stream, &at, 10) == 0x106); /* frame_pred, chroma420, progressive */
    CHECK(take(f->stream, &at, 6) == 0);
    CHECK(take(f->stream, &at, 32) == 0x101);
    CHECK(take(f->stream, &at, 5) == 8 && take(f->stream, &at, 1) == 0);
    CHECK(take(f->stream, &at, 1) == 1 && take(f->stream, &at, 1) == 1);
    for (i = 0; i < 6; ++i) {
        CHECK(take(f->stream, &at, i < 4 ? 3u : 2u) == (i < 4 ? 4u : 0u));
        CHECK(take(f->stream, &at, 2) == 2);
    }
    CHECK(take(f->stream, &at, 4) == 0);
    CHECK(take(f->stream, &at, 32) == 0x1b7 && at == f->stream_bytes * 8u);
    for (i = 0; i < MPEG2_DC16_YUV_BYTES; ++i)
        CHECK(f->yuv420[i] == 128);
}
static void planes(void)
{
    uint8_t bytes[3][700];
    struct mpeg2_dc16_plane p[3];
    struct mpeg2_dc16_comparison out, before;
    unsigned i;
    size_t at;
    memset(bytes, 0xa5, sizeof(bytes));
    for (i = 0; i < 3; ++i) {
        size_t width = i ? 8u : 16u, row;
        p[i].data = bytes[i];
        p[i].bytes = sizeof(bytes[i]);
        p[i].offset = 3u + i;
        p[i].stride = width + 7u;
        for (row = 0; row < width; ++row)
            memset(bytes[i] + p[i].offset + row * p[i].stride, 128, width);
    }
    CHECK(mpeg2_dc16_compare(p, 0xa5, &out) == MPEG2_DC16_MATCH);
    for (i = 0; i < 3; ++i)
        for (at = 0; at < sizeof(bytes[i]); ++at) {
            int sample = bytes[i][at] == 128;
            bytes[i][at] ^= 1;
            CHECK(mpeg2_dc16_compare(p, 0xa5, &out) == MPEG2_DC16_DIFFERENT);
            CHECK(out.first_bad_byte[i] == at);
            CHECK(out.sample_errors[i] == (uint32_t)sample);
            CHECK(out.guard_errors[i] == (uint32_t)!sample);
            CHECK(out.maximum_sample_error[i] == (uint32_t)sample);
            bytes[i][at] ^= 1;
        }
    memset(&out, 0xa5, sizeof(out));
    before = out;
    p[0].stride = SIZE_MAX;
    CHECK(mpeg2_dc16_compare(p, 0xa5, &out) == MPEG2_DC16_LAYOUT);
    CHECK(!memcmp(&out, &before, sizeof(out)));
    p[0].stride = 23;
    p[0].offset = SIZE_MAX;
    CHECK(mpeg2_dc16_compare(p, 0xa5, &out) == MPEG2_DC16_LAYOUT);
    p[0].offset = 3;
    p[1].data = p[0].data;
    CHECK(mpeg2_dc16_compare(p, 0xa5, &out) == MPEG2_DC16_ALIAS);
    CHECK(!memcmp(&out, &before, sizeof(out)));
}
int main(int argc, char **argv)
{
    struct mpeg2_dc16_fixture f;
    CHECK(mpeg2_dc16_build(&f));
    syntax(&f);
    planes();
    if (argc == 3) {
        FILE *stream = fopen(argv[1], "wbx"), *oracle;
        CHECK(stream);
        CHECK(fwrite(f.stream, 1, f.stream_bytes, stream) == f.stream_bytes);
        CHECK(!fclose(stream));
        oracle = fopen(argv[2], "wbx");
        CHECK(oracle);
        CHECK(fwrite(f.yuv420, 1, sizeof(f.yuv420), oracle) == sizeof(f.yuv420));
        CHECK(!fclose(oracle));
    } else
        CHECK(argc == 1);
    printf(
        "PASS analytic DC-only MPEG-2 fixture/YUV oracle: %u checks; 52-byte stream/384 samples; no hardware\n",
        checks);
    return 0;
}
