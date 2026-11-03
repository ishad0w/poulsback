// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_window_vertex.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct sgx535_window_color0_triangle input(void)
{
    /* Positive homogeneousW2/W4/W0.5 examples after the ordinary64x64 viewport.
     * The input to this encoder is already windowXYZ/reciprocalW, not clip. */
    const struct sgx535_window_color0_triangle value = {
        {{0x42400000, 0x42400000, 0x3f200000, 0x3f000000},
         {0x41800000, 0x41800000, 0x3e800000, 0x3e800000},
         {0x42000000, 0x42000000, 0x3f000000, 0x40000000}},
        {{0x3f800000, 0, 0, 0x3f800000},
         {0, 0x3f000000, 0, 0x3f800000},
         {0, 0, 0x3e800000, 0x3f800000}},
        0};
    return value;
}
int main(void)
{
    struct sgx535_window_color0_triangle in = input(), saved;
    union {
        struct sgx535_window_color0_triangle source;
        uint32_t words[25];
    } alias;
    uint32_t out[24], before[24], expected[24], i, j, mode, checks = 0;
    static const uint32_t bad[] = {0x7f800000, 0xff800000, 0x7fc12345, 0xbf000000};
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) {
            expected[i * 8 + j] = in.window_xyz_reciprocal_w[i][j];
            expected[i * 8 + 4 + j] = in.rgba[i][j];
        }
    assert(sgx535_window_color0_encode(&in, out) == SGX535_WINDOW_VERTEX_OK);
    checks++;
    assert(!memcmp(out, expected, sizeof(out)) && out[3] == 0x3f000000 && out[11] == 0x3e800000 &&
           out[19] == 0x40000000);
    checks++;
    for (mode = 0; mode < 20; mode++) {
        in = input();
        memset(out, 0x9b, sizeof(out));
        memcpy(before, out, sizeof(out));
        if (mode < 4)
            in.window_xyz_reciprocal_w[2][mode] = bad[mode];
        else
            switch (mode) {
            case 4:
                in.window_xyz_reciprocal_w[2][0] = 0x42800001;
                break;
            case 5:
                in.window_xyz_reciprocal_w[2][1] = 0x42800001;
                break;
            case 6:
                in.window_xyz_reciprocal_w[2][2] = 0x3f800001;
                break;
            case 7:
                in.window_xyz_reciprocal_w[2][3] = 0;
                break;
            case 8:
                in.window_xyz_reciprocal_w[2][3] = 0x80000000;
                break;
            case 9:
                in.window_xyz_reciprocal_w[2][3] = 0xbf800000;
                break;
            case 10:
                in.window_xyz_reciprocal_w[2][3] = 0x7fc12345;
                break;
            case 11:
                in.rgba[2][0] = 0x80000000;
                break;
            case 12:
                in.rgba[2][1] = 0x3f800001;
                break;
            case 13:
                in.rgba[2][2] = 0x7fc12345;
                break;
            case 14:
                in.rgba[2][3] = 0x7f800000;
                break;
            case 15:
                in.replicated_flat = 2;
                break;
            default:
                in.replicated_flat = 1;
                for (i = 0; i < 3; i++)
                    for (j = 0; j < 4; j++)
                        in.rgba[i][j] = 0x3f000000;
                in.rgba[2][mode - 16] ^= 1;
                break;
            }
        assert(sgx535_window_color0_encode(&in, out) == SGX535_WINDOW_VERTEX_UNSUPPORTED);
        checks++;
        assert(!memcmp(out, before, sizeof(out)));
        checks++;
    }
    in = input();
    in.replicated_flat = 1;
    for (i = 1; i < 3; i++)
        memcpy(in.rgba[i], in.rgba[0], sizeof(in.rgba[i]));
    assert(sgx535_window_color0_encode(&in, out) == SGX535_WINDOW_VERTEX_OK);
    checks++;
    in.window_xyz_reciprocal_w[0][0] = 0x80000000;
    in.window_xyz_reciprocal_w[0][3] = 1;
    assert(sgx535_window_color0_encode(&in, out) == SGX535_WINDOW_VERTEX_OK &&
           out[0] == 0x80000000 && out[3] == 1);
    checks++;
    /* Full source/output overlap is safe because all24 words are staged first. */
    alias.source = input();
    saved = alias.source;
    assert(sgx535_window_color0_encode(&alias.source, alias.words) == SGX535_WINDOW_VERTEX_OK);
    checks++;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) {
            assert(alias.words[i * 8 + j] == saved.window_xyz_reciprocal_w[i][j]);
            checks++;
            assert(alias.words[i * 8 + 4 + j] == saved.rgba[i][j]);
            checks++;
        }
    memset(out, 0x9b, sizeof(out));
    memcpy(before, out, sizeof(out));
    assert(sgx535_window_color0_encode(NULL, out) == SGX535_WINDOW_VERTEX_INVALID &&
           !memcmp(out, before, sizeof(out)));
    checks++;
    assert(sgx535_window_color0_encode(&saved, NULL) == SGX535_WINDOW_VERTEX_INVALID);
    checks++;
    printf(
        "windowXYZ/reciprocalW: %u CPU checks; complete32-byte vertex records, no native qualification\n",
        checks);
    return 0;
}
