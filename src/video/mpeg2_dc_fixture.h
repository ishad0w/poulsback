// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef MPEG2_DC_FIXTURE_H
#define MPEG2_DC_FIXTURE_H
#include <stddef.h>
#include <stdint.h>
#define MPEG2_DC16_STREAM_CAPACITY 128u
#define MPEG2_DC16_YUV_BYTES 384u
#define MPEG2_DC16_MAX_PLANE_BYTES (1024u * 1024u)
struct mpeg2_dc16_fixture {
    size_t stream_bytes;
    uint8_t stream[MPEG2_DC16_STREAM_CAPACITY];
    uint8_t yuv420[MPEG2_DC16_YUV_BYTES];
};
/* Elementary stream: 16x16 progressive 4:2:0 I-picture, one slice,
 * one intra macroblock, six zero-DC-difference/zero-AC blocks. With initial
 * precision-0 DC predictor 128, the base inverse transform is 1024/8=128.
 * H.262 mismatch control adds F[7][7]=1; its ideal per-sample contribution
 * has magnitude <=1/4, so nearest-integer samples remain 128. This fixture's
 * strict oracle uses no tolerance; it is not a universal IDCT conformance rule.
 * This generator is not a general MPEG-2 encoder or a conformance verdict.
 * Outputs need external independent bitstream/decoder verification before a
 * physical test selects them. */
int mpeg2_dc16_build(struct mpeg2_dc16_fixture *);

struct mpeg2_dc16_plane {
    const uint8_t *data;
    size_t bytes, offset, stride;
};
struct mpeg2_dc16_comparison {
    uint32_t sample_errors[3], guard_errors[3], maximum_sample_error[3];
    size_t first_bad_byte[3]; /* SIZE_MAX if that whole plane span matches. */
};
enum mpeg2_dc16_status {
    MPEG2_DC16_MATCH,
    MPEG2_DC16_DIFFERENT,
    MPEG2_DC16_ARGUMENT,
    MPEG2_DC16_LAYOUT,
    MPEG2_DC16_ALIAS
};
/* CPU all-byte oracle for separately supplied Y,U,V spans. Active dimensions
 * are 16x16,8x8,8x8; every other declared byte must equal initialized guard.
 * Planes must be disjoint and their layout/output metadata separate from all
 * byte spans. Full validation before comparison; invalid input preserves out.
 * This flexible stride/offset policy does not specify an MSVDX surface layout,
 * tiling, color conversion, physical cache visibility or decoder completion. */
enum mpeg2_dc16_status mpeg2_dc16_compare(const struct mpeg2_dc16_plane[3], uint8_t guard,
                                          struct mpeg2_dc16_comparison *);
#endif
