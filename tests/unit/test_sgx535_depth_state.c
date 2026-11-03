// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
static void invalid(const struct sgx535_depth_request *r)
{
    struct sgx535_depth_preparation p, saved;
    memset(&p, 0x5a, sizeof(p));
    saved = p;
    CHECK(sgx535_depth_prepare(r, &p) == SGX535_DEPTH_INVALID);
    CHECK(memcmp(&p, &saved, sizeof(p)) == 0);
}
int main(void)
{
    struct sgx535_depth_request r = {1, 64, 64, SGX535_DEPTH_D16, 0, 0, SGX535_DEPTH_LESS, 1, {0}};
    struct sgx535_depth_preparation p;
    const uint32_t offsets[5] = {0xcb0, 0x484, 0x488, 0x48c, 0x490};
    const uint32_t comparison[8] = {0,         0x400000,  0x800000,  0xc00000,
                                    0x1000000, 0x1400000, 0x1800000, 0x1c00000};
    uint32_t fmt, w, h, i, c, preserve;
    CHECK(sgx535_depth_prepare(&r, &p) == SGX535_DEPTH_BLOCKED);
    CHECK(p.pitch_pixels == 64 && p.stride_bytes == 128 && p.allocation_height == 64);
    CHECK(p.allocation_bytes == 8192 && p.zls_control == 0x000a0300);
    CHECK(p.isp_clear_mask == 0x01d00000 && p.isp_set_bits == 0x00400000);
    CHECK(p.clear_depth_f32 == 0x3f800000 && p.clear_stencil == 0);
    r.format = SGX535_DEPTH_D24S8;
    r.content = SGX535_DEPTH_PREVIOUS_SCENE;
    r.preserve_after = 1;
    CHECK(sgx535_depth_prepare(&r, &p) == SGX535_DEPTH_BLOCKED);
    CHECK(p.allocation_bytes == 16384 && p.zls_control == 0x006503f6);
    for (fmt = 1; fmt <= 2; fmt++)
        for (w = 1; w <= 2048; w += 31)
            for (h = 1; h <= 2048; h += 127) {
                uint64_t columns = (uint64_t)((w - 1u) / 32u + 1u) * 32u;
                uint64_t rows = (uint64_t)((h - 1u) / 32u + 1u) * 32u;
                r.format = fmt;
                r.width = w;
                r.height = h;
                CHECK(sgx535_depth_prepare(&r, &p) == SGX535_DEPTH_BLOCKED);
                CHECK(p.allocation_bytes == columns * rows * (fmt == 1 ? 2u : 4u));
                CHECK(p.stride_bytes == columns * (fmt == 1 ? 2u : 4u));
                CHECK(p.missing_contracts == SGX535_DEPTH_MISSING_ALL);
                CHECK(p.address_pair_count == 5);
                for (i = 0; i < 5; i++) {
                    CHECK(p.address_pairs[i].register_offset == offsets[i]);
                    CHECK(p.address_pairs[i].source_relocation_mask ==
                          (i ? 0x0ffffff0u : 0xf0000000u));
                    CHECK(p.address_pairs[i].resource_byte_offset == 0 &&
                          p.address_pairs[i].alignment == 16);
                    CHECK(p.address_pairs[i].required_span_bytes == p.allocation_bytes);
                }
            }
    r.width = 64;
    r.height = 64;
    for (c = 0; c < 8; c++)
        for (i = 0; i < 2; i++) {
            r.compare = c;
            r.write_enable = i;
            CHECK(sgx535_depth_prepare(&r, &p) == SGX535_DEPTH_BLOCKED);
            CHECK(p.isp_set_bits == (comparison[c] | (i ? 0u : 0x100000u)));
            CHECK((p.isp_set_bits & ~p.isp_clear_mask) == 0);
        }
    r.compare = SGX535_DEPTH_LESS;
    r.write_enable = 1;
    for (c = 0; c <= 2; c += 2)
        for (preserve = 0; preserve < 2; preserve++) {
            r.content = c;
            r.preserve_after = preserve;
            CHECK(sgx535_depth_prepare(&r, &p) == SGX535_DEPTH_BLOCKED);
            CHECK(p.missing_contracts & SGX535_DEPTH_MISSING_INITIALIZATION);
            CHECK(p.missing_contracts & SGX535_DEPTH_MISSING_FORMAT_RESET);
            CHECK((p.zls_control & 0x200026) == (c ? 0x200026u : 0u));
            CHECK((p.zls_control & 0x4000d0) == (preserve ? 0x4000d0u : 0u));
        }
    r.version = 0;
    invalid(&r);
    r.version = 1;
    r.width = 0;
    invalid(&r);
    r.width = 2049;
    invalid(&r);
    r.width = 0xffffffffu;
    invalid(&r);
    r.width = 64;
    r.height = 0;
    invalid(&r);
    r.height = 2049;
    invalid(&r);
    r.height = 0xffffffffu;
    invalid(&r);
    r.height = 64;
    r.format = 0;
    invalid(&r);
    r.format = 3;
    invalid(&r);
    r.format = 2;
    r.content = 1;
    invalid(&r);
    r.content = 3;
    invalid(&r);
    r.content = 0;
    r.preserve_after = 2;
    invalid(&r);
    r.preserve_after = 0;
    r.compare = 8;
    invalid(&r);
    r.compare = 0;
    r.write_enable = 2;
    invalid(&r);
    r.write_enable = 1;
    for (i = 0; i < 4; i++) {
        r.reserved[i] = 1;
        invalid(&r);
        r.reserved[i] = 0;
    }
    invalid(NULL);
    CHECK(sgx535_depth_prepare(&r, NULL) == SGX535_DEPTH_INVALID);
    printf(
        "PASS depth preparation: %lu checks; typed padded storage/ZLS/ISP, no physical admission\n",
        checks);
    return 0;
}
