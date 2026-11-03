// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_clear.h"
#include "sgx535_engine121.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t f32(uint32_t x)
{
    float value = (float)x;
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static const struct sgx535_task_packet_section *section(const struct sgx535_depth_clear_packet *p,
                                                        uint32_t kind)
{
    uint32_t i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return p->sections + i;
    abort();
}
static uint32_t sw(const struct sgx535_depth_clear_packet *p, uint32_t kind, uint32_t index)
{
    const struct sgx535_task_packet_section *s = section(p, kind);
    CHECK(index < s->byte_count / 4u);
    return word(p->bytes + s->byte_offset + index * 4u);
}
static void refuse(const struct sgx535_depth_request *r, struct sgx535_depth_clear_packet *out,
                   struct sgx535_depth_clear_workspace *w)
{
    struct sgx535_depth_clear_packet *saved = malloc(sizeof(*saved));
    CHECK(saved != NULL);
    *saved = *out;
    CHECK(sgx535_depth_clear_build(r, out, w) == SGX535_DEPTH_INVALID);
    CHECK(memcmp(saved, out, sizeof(*saved)) == 0);
    free(saved);
}
int main(void)
{
    struct sgx535_depth_request r = {1, 64, 64, SGX535_DEPTH_D24S8, 0, 1, SGX535_DEPTH_LESS,
                                     1, {0}};
    struct sgx535_depth_clear_packet *p = malloc(sizeof(*p)), *saved = malloc(sizeof(*saved));
    struct sgx535_depth_clear_workspace *w = malloc(sizeof(*w));
    struct sgx535_task_packet *ordinary = malloc(sizeof(*ordinary)), *pw = malloc(sizeof(*pw));
    struct sgx535_task_request draw = {3,
                                       1,
                                       SGX535_ENGINE121_CORE_ID,
                                       SGX535_ENGINE121_REVISION,
                                       {{0xbf000000, 0xbf000000, 0, 0x3f800000},
                                        {0x3f000000, 0xbf000000, 0, 0x3f800000},
                                        {0, 0x3f000000, 0, 0x3f800000}},
                                       0xff123456,
                                       {0}};
    const uint32_t mte[11] = {0x54c5, 0x01e00100, 0x0e000000, 0,       0x30000, 0x0c000000,
                              0,      0,          0x04001000, 0x10000, 0};
    const uint32_t ta[7] = {0x40000000, 0x0c02a203, 0x81400006, 0, 0, 0, 0x02000103};
    const uint32_t indices[6] = {0, 1, 2, 3, 2, 1};
    uint32_t i, j, dim;
    size_t b;
    CHECK(p && saved && w && ordinary && pw);
    CHECK(sgx535_depth_clear_build(&r, p, w) == SGX535_DEPTH_BLOCKED);
    CHECK(sgx535_depth_clear_validate(&r, p, w) == SGX535_DEPTH_BLOCKED);
    CHECK(p->version == 1);
    CHECK(p->section_count == 11);
    CHECK(p->relocation_count == 16);
    /*64+12+16+8+16+64+56+4+60+44+28 bytes, excluding owner-placement gaps. */
    CHECK(p->byte_count == 372);
    CHECK(p->missing_packet_contracts == SGX535_PACKET_MISSING_ALL);
    CHECK(p->depth.missing_contracts == SGX535_DEPTH_MISSING_ALL &&
          p->depth.allocation_bytes == 16384);
    CHECK(p->depth.zls_control == 0x004503d0 && p->depth.isp_set_bits == 0x00400000);
    for (i = 0; i < 11; i++)
        CHECK(sw(p, SGX535_PACKET_MTE_STATE, i) == mte[i]);
    for (i = 0; i < 7; i++)
        CHECK(sw(p, SGX535_PACKET_TA_STREAM, i) == ta[i]);
    CHECK(sw(p, SGX535_PACKET_STATE_USE, 1) == 0x28a1a001 &&
          sw(p, SGX535_PACKET_STATE_USE, 2) == 0xa0200580);
    CHECK(sw(p, SGX535_PACKET_STATE_PDS, 1) == 0x8140000au);
    for (i = 0; i < 6; i++) {
        const struct sgx535_task_packet_section *s = section(p, SGX535_PACKET_INDICES);
        const uint8_t *bytes = p->bytes + s->byte_offset + i * 2u;
        CHECK(((uint32_t)bytes[0] | (uint32_t)bytes[1] << 8) == indices[i]);
    }
    for (dim = 1; dim <= 2048; dim += 17) {
        r.width = dim;
        r.height = 2049u - dim;
        CHECK(sgx535_depth_clear_build(&r, p, w) == SGX535_DEPTH_BLOCKED);
        for (i = 0; i < 4; i++) {
            CHECK(sw(p, SGX535_PACKET_VERTICES, i * 4u) == (i & 1u ? f32(r.width) : 0u));
            CHECK(sw(p, SGX535_PACKET_VERTICES, i * 4u + 1u) == (i & 2u ? f32(r.height) : 0u));
            CHECK(sw(p, SGX535_PACKET_VERTICES, i * 4u + 2u) == 0x3f800000);
            CHECK(sw(p, SGX535_PACKET_VERTICES, i * 4u + 3u) == 0x3f800000);
        }
    }
    r.width = r.height = 64;
    CHECK(sgx535_depth_clear_build(&r, p, w) == SGX535_DEPTH_BLOCKED);
    CHECK(sgx535_task_packet_build(&draw, ordinary, pw) == SGX535_TASK_BLOCKED);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = p->sections + i;
        CHECK(s->byte_offset + s->byte_count <= p->byte_count);
        CHECK(s->resource_byte_offset % s->alignment == 0);
        CHECK((uint64_t)s->resource_byte_offset + s->byte_count <=
              p->requirements[s->role].minimum_bytes);
        for (j = 0; j < ordinary->section_count; j++) {
            const struct sgx535_task_packet_section *o = ordinary->sections + j;
            if (s->role == o->role)
                CHECK(s->resource_byte_offset + s->byte_count <= o->resource_byte_offset ||
                      o->resource_byte_offset + o->byte_count <= s->resource_byte_offset);
        }
        for (j = i + 1; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *o = p->sections + j;
            if (s->role == o->role)
                CHECK(s->resource_byte_offset + s->byte_count <= o->resource_byte_offset ||
                      o->resource_byte_offset + o->byte_count <= s->resource_byte_offset);
        }
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *l = p->relocations + i;
        CHECK(l->section < p->section_count && l->role < SGX535_TASK_IMAGE_ROLES);
        CHECK(l->section_byte_offset + 4u <= p->sections[l->section].byte_count);
        CHECK(l->resource_byte_offset % l->alignment == 0);
        CHECK((uint64_t)l->resource_byte_offset + l->span_bytes <=
              p->requirements[l->role].minimum_bytes);
    }
    *saved = *p;
    for (b = 0; b < sizeof(*p); b++) {
        ((uint8_t *)p)[b] ^= 1;
        CHECK(sgx535_depth_clear_validate(&r, p, w) == SGX535_DEPTH_INVALID);
        ((uint8_t *)p)[b] ^= 1;
    }
    CHECK(memcmp(p, saved, sizeof(*p)) == 0);
    r.width = 128;
    CHECK(memcmp(p, saved, sizeof(*p)) == 0);
    CHECK(sgx535_depth_clear_validate(&r, p, w) == SGX535_DEPTH_INVALID);
    r.width = 64;
    r.preserve_after = 0;
    refuse(&r, p, w);
    r.preserve_after = 1;
    r.content = 1;
    refuse(&r, p, w);
    r.content = 0;
    r.width = 0;
    refuse(&r, p, w);
    r.width = 2049;
    refuse(&r, p, w);
    r.width = 64;
    r.format = 3;
    refuse(&r, p, w);
    r.format = 2;
    for (i = 0; i < 4; i++) {
        r.reserved[i] = 1;
        refuse(&r, p, w);
        r.reserved[i] = 0;
    }
    refuse(NULL, p, w);
    CHECK(sgx535_depth_clear_build(&r, p, (struct sgx535_depth_clear_workspace *)p) ==
          SGX535_DEPTH_INVALID);
    CHECK(memcmp(p, saved, sizeof(*p)) == 0);
    CHECK(sgx535_depth_clear_build((const struct sgx535_depth_request *)p, p, w) ==
          SGX535_DEPTH_INVALID);
    CHECK(memcmp(p, saved, sizeof(*p)) == 0);
    CHECK(sgx535_depth_clear_build((const struct sgx535_depth_request *)w, p, w) ==
          SGX535_DEPTH_INVALID);
    CHECK(memcmp(p, saved, sizeof(*p)) == 0);
    printf(
        "PASS depth clear prefix: %lu checks; geometry/11-word MTE/programs/16relocs, no GPU admission\n",
        checks);
    free(pw);
    free(ordinary);
    free(w);
    free(saved);
    free(p);
    return 0;
}
