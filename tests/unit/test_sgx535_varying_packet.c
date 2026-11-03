// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_varying_packet.h"
#include "sgx535_engine121.h"
#include "sgx535_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        assert(x);                                                                                 \
    } while (0)
static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static const struct sgx535_task_packet_section *find(const struct sgx535_task_packet *p,
                                                     uint32_t kind)
{
    uint32_t i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    assert(0);
    return NULL;
}
static uint32_t sw(const struct sgx535_task_packet *p, uint32_t kind, uint32_t index)
{
    const struct sgx535_task_packet_section *s = find(p, kind);
    CHECK(index < s->byte_count / 4u);
    return get(p->bytes + s->byte_offset + index * 4u);
}
static void ranges(const struct sgx535_task_packet *p)
{
    uint32_t i, j;
    CHECK(p->section_count == 16 && p->relocation_count == 29);
    CHECK(p->byte_count <= sizeof(p->bytes));
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        CHECK(s->role < SGX535_TASK_IMAGE_ROLES && s->byte_count);
        CHECK(s->byte_offset + s->byte_count <= p->byte_count);
        CHECK(!(s->resource_byte_offset & (s->alignment - 1u)));
        CHECK(s->resource_byte_offset + s->byte_count <= p->requirements[s->role].minimum_bytes);
        for (j = i + 1; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *t = &p->sections[j];
            CHECK(s->byte_offset + s->byte_count <= t->byte_offset ||
                  t->byte_offset + t->byte_count <= s->byte_offset);
            if (s->role == t->role)
                CHECK(s->resource_byte_offset + s->byte_count <= t->resource_byte_offset ||
                      t->resource_byte_offset + t->byte_count <= s->resource_byte_offset);
        }
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        int contained = r->role == SGX535_TASK_TARGET;
        CHECK(r->section < p->section_count && r->role < SGX535_TASK_IMAGE_ROLES);
        CHECK(r->section_byte_offset + 4u <= p->sections[r->section].byte_count);
        CHECK(!(get(p->bytes + p->sections[r->section].byte_offset + r->section_byte_offset) &
                r->mask));
        for (j = 0; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *s = &p->sections[j];
            if (s->role == r->role && r->resource_byte_offset >= s->resource_byte_offset &&
                r->resource_byte_offset + r->span_bytes <= s->resource_byte_offset + s->byte_count)
                contained = 1;
        }
        CHECK(contained);
    }
}
static void program(const struct sgx535_task_packet *p)
{
    static const uint32_t literal[16] = {0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u,
                                         0xa0400000u, 0x28a10000u, 0xa0600180u, 0x28a10000u,
                                         0x20040000u, 0x40a10c11u, 0x20040080u, 0x40a10c09u,
                                         0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
    static const uint32_t byte[4] = {2, 1, 0, 3};
    static const uint32_t primary[4] = {2, 1, 0, 3};
    uint32_t i, words[2];
    for (i = 0; i < 16; i++)
        CHECK(sw(p, SGX535_PACKET_FRAGMENT_USE, i) == literal[i]);
    for (i = 0; i < 4; i++) {
        struct sgx535_mov m;
        struct sgx535_pack_f32_u8_channel pack;
        words[0] = sw(p, SGX535_PACKET_FRAGMENT_USE, i * 2u);
        words[1] = sw(p, SGX535_PACKET_FRAGMENT_USE, i * 2u + 1u);
        CHECK(sgx535_mov_decode(words, &m) == SGX535_MOV_OK);
        CHECK(m.source.bank == SGX535_MOV_PRIMARY && m.source.number == primary[i]);
        CHECK(m.destination.bank == SGX535_MOV_TEMP && m.destination.number == i && !m.end);
        words[0] = sw(p, SGX535_PACKET_FRAGMENT_USE, 8u + i * 2u);
        words[1] = sw(p, SGX535_PACKET_FRAGMENT_USE, 9u + i * 2u);
        CHECK(sgx535_pack_f32_u8_channel_decode(words, &pack) == SGX535_PACK_OK);
        CHECK(pack.source.bank == SGX535_MOV_TEMP && pack.source.number == i);
        CHECK(pack.destination.bank == SGX535_MOV_OUTPUT && !pack.destination.number);
        CHECK(pack.byte_index == byte[i] && pack.scale == 1 && pack.end == (i == 3u));
    }
}
static void replicated_flat(const struct sgx535_varying_request *original)
{
    static const uint32_t color[][4] = {{0, 0, 0, 0},
                                        {0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000},
                                        {1, 0x007fffff, 0x00800000, 0x3f800000},
                                        {0x3e000001, 0x3f000001, 0x3f400001, 0x3f200001}};
    static const uint32_t invalid[] = {0x80000000u, 0xbf000000u, 0x3f800001u,
                                       0x7f800000u, 0xff800000u, 0x7fc00001u};
    struct sgx535_varying_request r = *original, bad;
    struct sgx535_varying_workspace *w = malloc(sizeof(*w));
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *smooth = malloc(sizeof(*smooth));
    uint32_t row, i, j;
    CHECK(w && p && smooth);
    for (row = 0; row < sizeof(color) / sizeof(color[0]); row++) {
        for (i = 0; i < 3; i++)
            memcpy(r.rgba[i], color[row], sizeof(r.rgba[i]));
        r.interpolation = SGX535_VARYING_SMOOTH;
        CHECK(sgx535_varying_packet_build(&r, smooth, w) == SGX535_TASK_BLOCKED);
        r.interpolation = SGX535_VARYING_FLAT_REPLICATED;
        CHECK(sgx535_varying_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_varying_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
        /* A transport invariant changes no shader, iterator, shading state,
         * relocation, owner extent or physical qualification byte. */
        CHECK(!memcmp(p, smooth, sizeof(*p)) && p->missing_contracts == 0x1ff);
    }
    for (i = 1; i < 3; i++)
        for (j = 0; j < 4; j++) {
            bad = r;
            bad.rgba[i][j] ^= 1u; /* An admitted adjacent F32 value. */
            CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
            CHECK(!memcmp(p, smooth, sizeof(*p)));
            CHECK(sgx535_varying_packet_validate(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
        }
    for (row = 0; row < sizeof(invalid) / sizeof(invalid[0]); row++) {
        bad = r;
        for (i = 0; i < 3; i++)
            bad.rgba[i][row % 4u] = invalid[row];
        CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, smooth, sizeof(*p))); /* Equality does not bypass domain checks. */
    }
    bad = r;
    bad.interpolation = 3;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, smooth, sizeof(*p)));
    for (i = 0; i < sizeof(*p); i++) {
        ((uint8_t *)p)[i] ^= 1;
        CHECK(sgx535_varying_packet_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
        ((uint8_t *)p)[i] ^= 1;
    }
    free(smooth);
    free(p);
    free(w);
}
int main(void)
{
    struct sgx535_varying_request r = {5,
                                       2,
                                       SGX535_ENGINE121_CORE_ID,
                                       SGX535_ENGINE121_REVISION,
                                       {{0xbf000000u, 0xbf000000u, 0, 0x3f800000u},
                                        {0x3f000000u, 0xbf000000u, 0, 0x3f800000u},
                                        {0, 0x3f000000u, 0, 0x3f800000u}},
                                       {{0x3f800000u, 0, 0, 0x3f800000u},
                                        {0, 0x3f800000u, 0, 0x3f800000u},
                                        {0, 0, 0x3f800000u, 0x3f800000u}},
                                       1,
                                       {0}};
    struct sgx535_varying_request bad;
    struct sgx535_varying_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *saved = malloc(sizeof(*saved));
    const uint32_t invalid[] = {0x80000000u, 0xbf000000u, 0x3f800001u,
                                0x7f800000u, 0xff800000u, 0x7fc00000u};
    uint32_t i, j;
    CHECK(w && p && saved);
    CHECK(sgx535_varying_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_varying_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->version == 5 && p->profile == 2 && p->missing_contracts == 0x1ff);
    CHECK(p->width == 64 && p->height == 64 && p->stride_bytes == 256);
    ranges(p);
    program(p);
    CHECK(find(p, SGX535_PACKET_VERTICES)->byte_count == 96);
    CHECK(find(p, SGX535_PACKET_INDICES)->resource_byte_offset == 128);
    CHECK(find(p, SGX535_PACKET_EVENT_USE)->resource_byte_offset == 128);
    CHECK(sw(p, SGX535_PACKET_VERTEX_PDS, 8) == 32);
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 0) == 0x80000);
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 1) == 0x20000000);
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 9) == 0x0fd0a00f);
    CHECK(!(sw(p, SGX535_PACKET_FRAGMENT_PDS, 9) & 0x80000));
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 12) == 0x07000345);
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 13) == 0x070418a2);
    CHECK(sw(p, SGX535_PACKET_FRAGMENT_PDS, 14) == 0xaf000000u);
    CHECK(sw(p, SGX535_PACKET_MTE_STATE, 3) == 0x30030004);
    CHECK(sw(p, SGX535_PACKET_MTE_STATE, 7) == 0x08001800);
    CHECK(sw(p, SGX535_PACKET_MTE_STATE, 8) == 0x10000);
    CHECK(sw(p, SGX535_PACKET_TA_STREAM, 6) == 0x04000203);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            CHECK(sw(p, SGX535_PACKET_VERTICES, i * 8u + 4u + j) == r.rgba[i][j]);
    *saved = *p;
    /* Every byte, including flags, unused storage and relocation metadata,
     * participates in regeneration. A cleared missing mask grants nothing. */
    for (i = 0; i < sizeof(*p); i++) {
        ((uint8_t *)p)[i] ^= 1;
        CHECK(sgx535_varying_packet_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
        ((uint8_t *)p)[i] ^= 1;
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        bad = r;
        bad.rgba[i % 3u][i % 4u] = invalid[i];
        CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, saved, sizeof(*p)));
    }
    bad = r;
    bad.interpolation = 0;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    bad = r;
    bad.clip_position[1][3] = 0x40000000;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    bad = r;
    bad.reserved[2] = 1;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    bad = r;
    bad.version = 4;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(sgx535_varying_packet_build(NULL, p, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_varying_packet_build(&r, NULL, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_varying_packet_build(&r, p, NULL) == SGX535_TASK_INVALID);
    CHECK(sgx535_varying_packet_build(&r, &w->result, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_varying_packet_build((const void *)p, p, w) == SGX535_TASK_INVALID);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    /* A different vertex attribute changes copied data, not the generic FS. */
    bad = r;
    bad.rgba[0][0] = 0x3e800000;
    CHECK(sgx535_varying_packet_build(&bad, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sw(p, SGX535_PACKET_VERTICES, 4) == 0x3e800000);
    CHECK(!memcmp(p->bytes + find(p, SGX535_PACKET_FRAGMENT_USE)->byte_offset,
                  saved->bytes + find(saved, SGX535_PACKET_FRAGMENT_USE)->byte_offset, 64));
    CHECK(sw(saved, SGX535_PACKET_VERTICES, 4) == 0x3f800000);
    CHECK(sgx535_varying_packet_validate(&r, saved, w) == SGX535_TASK_BLOCKED);
    replicated_flat(&r);
    free(saved);
    free(p);
    free(w);
    printf("sgx535 varying packet: %u CPU checks PASS; physical execution BLOCKED\n", checks);
    return 0;
}
