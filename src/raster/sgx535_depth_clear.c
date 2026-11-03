// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_clear.h"
#include "sgx535_vertex_pds.h"
#include "sgx535_vertex_use.h"
#include "sgx535_limm_fields.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
enum clear_section {
    C_VERTICES,
    C_INDICES,
    C_VERTEX_USE,
    C_FRAGMENT_USE,
    C_STATE_USE,
    C_VERTEX_PDS,
    C_FRAGMENT_PDS,
    C_SECONDARY_PDS,
    C_STATE_PDS,
    C_MTE,
    C_TA
};
static int overlaps(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long x = (unsigned long)a, y = (unsigned long)b;
    return x <= y ? y - x < an : x - y < bn;
}
static uint32_t integer_f32(uint32_t value)
{
    uint32_t exponent = 0, shifted = value;
    /* Dimensions are positive and <=2048, so every conversion is exact. */
    while (shifted > 1) {
        shifted >>= 1;
        exponent++;
    }
    return (exponent + 127u) << 23 | (value << (23u - exponent) & 0x7fffffu);
}
static void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}
static void section(struct sgx535_depth_clear_packet *p, uint32_t id, uint32_t kind, uint32_t role,
                    uint32_t offset, uint32_t alignment, const uint32_t *words, uint32_t count)
{
    uint32_t i, cursor = p->byte_count;
    p->sections[id] =
        (struct sgx535_task_packet_section){kind, cursor, count * 4u, role, offset, alignment};
    for (i = 0; i < count; i++)
        put32(p->bytes + cursor + i * 4u, words[i]);
    p->byte_count += count * 4u;
    if (p->requirements[role].minimum_bytes < offset + count * 4u)
        p->requirements[role].minimum_bytes = offset + count * 4u;
    if (p->requirements[role].alignment < alignment)
        p->requirements[role].alignment = alignment;
    p->requirements[role].role = role;
    p->requirements[role].access = SGX535_TASK_ACCESS_READ;
}
static void relocation(struct sgx535_depth_clear_packet *p, uint32_t kind, uint32_t sid,
                       uint32_t byte_offset, uint32_t role, uint32_t target, uint32_t span,
                       uint32_t alignment, uint32_t mask, uint32_t right, uint32_t left)
{
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        kind, sid, byte_offset, role, target, span, alignment, mask, right, left};
}
static void address(struct sgx535_depth_clear_packet *p, uint32_t sid, uint32_t offset,
                    uint32_t role, uint32_t target, uint32_t span, uint32_t alignment,
                    uint32_t mask, uint32_t right)
{
    relocation(p, SGX535_PACKET_DRI_OFFSET, sid, offset, role, target, span, alignment, mask, right,
               0);
}
static void use_link(struct sgx535_depth_clear_packet *p, uint32_t sid, uint32_t offset,
                     uint32_t role, uint32_t target, uint32_t span)
{
    relocation(p, SGX535_PACKET_USE_BANK, sid, offset, role, target, span, 16, 0xf, 0, 0);
    relocation(p, SGX535_PACKET_USE_ENTRY, sid, offset, role, target, span, 16, 0xf0, 15, 4);
    relocation(p, SGX535_PACKET_USE_ENTRY, sid, offset, role, target, span, 16, 0x7ff00, 4, 8);
}
static enum sgx535_depth_status construct(const struct sgx535_depth_request *r,
                                          struct sgx535_depth_clear_packet *p)
{
    struct sgx535_vertex_use_projection vertex;
    struct sgx535_vertex_pds_program vpds;
    struct sgx535_vertex_dma_plan dma;
    uint32_t vertices[16] = {0}, fs[2], fpds[14] = {0}, spds[15] = {0};
    static const uint32_t indices[3] = {0x00010000u, 0x00030002u, 0x00010002u};
    /* Count11 state copies primary words to output then commits that count. */
    static const uint32_t state_use[4] = {0xa0000000u, 0x28a1a001u, 0xa0200580u, 0xfb274000u};
    static const uint32_t state[11] = {0x54c5, 0x01e00100, 0x0e000000, 0, 3u << 16, 12u << 24, 0,
                                       0,      0x04001000, 0x00010000, 0};
    static const uint32_t ta[7] = {0x40000000, 0x0c02a203, 0x81400006, 0, 0, 0, 0x02000103};
    static const uint32_t secondary[1] = {0xaf000000u};
    uint32_t i, x, y;
    memset(p, 0, sizeof(*p));
    if (sgx535_depth_prepare(r, &p->depth) != SGX535_DEPTH_BLOCKED || r->preserve_after != 1 ||
        sgx535_vertex_use_project(4, &vertex) != SGX535_VERTEX_USE_OK ||
        sgx535_vertex_pds_project(4, 4, &vpds) != SGX535_VERTEX_DMA_OK ||
        sgx535_vertex_dma_build(11, &dma) != SGX535_VERTEX_DMA_OK || dma.transfer_count != 1)
        return SGX535_DEPTH_INVALID;
    x = integer_f32(r->width);
    y = integer_f32(r->height);
    vertices[4] = vertices[12] = x;
    vertices[9] = vertices[13] = y;
    for (i = 0; i < 4; i++) {
        vertices[i * 4u + 2u] = 0x3f800000u;
        vertices[i * 4u + 3u] = 0x3f800000u;
    }
    sgx535_limm_compose(1, 0, 0xff00ff00u, 1, fs);
    fpds[8] = 0x20;
    fpds[12] = 0x07000345;
    fpds[13] = 0xaf000000u;
    spds[1] = dma.transfers[0].descriptor_word;
    spds[2] = 0x00200000;
    spds[12] = 0x07030223;
    spds[13] = 0x07042345;
    spds[14] = 0xaf000000u;
    p->version = SGX535_DEPTH_CLEAR_VERSION;
    p->section_count = SGX535_DEPTH_CLEAR_SECTIONS;
    p->missing_packet_contracts = SGX535_PACKET_MISSING_ALL;
    p->ta_prefix_section = C_TA;
    section(p, C_VERTICES, SGX535_PACKET_VERTICES, SGX535_TASK_VERTEX_DATA, 256, 16, vertices, 16);
    section(p, C_INDICES, SGX535_PACKET_INDICES, SGX535_TASK_VERTEX_DATA, 320, 2, indices, 3);
    section(p, C_VERTEX_USE, SGX535_PACKET_VERTEX_USE, SGX535_TASK_USE_VERTEX, 128, 32,
            vertex.words, 4);
    section(p, C_FRAGMENT_USE, SGX535_PACKET_FRAGMENT_USE, SGX535_TASK_USE_FRAGMENT, 128, 32, fs,
            2);
    section(p, C_STATE_USE, SGX535_PACKET_STATE_USE, SGX535_TASK_USE_VERTEX, 160, 32, state_use, 4);
    section(p, C_VERTEX_PDS, SGX535_PACKET_VERTEX_PDS, SGX535_TASK_PDS, 1024, 32, vpds.words, 16);
    section(p, C_FRAGMENT_PDS, SGX535_PACKET_FRAGMENT_PDS, SGX535_TASK_PDS, 1088, 32, fpds, 14);
    section(p, C_SECONDARY_PDS, SGX535_PACKET_SECONDARY_PDS, SGX535_TASK_PDS, 1152, 32, secondary,
            1);
    section(p, C_STATE_PDS, SGX535_PACKET_STATE_PDS, SGX535_TASK_PDS, 1184, 32, spds, 15);
    section(p, C_MTE, SGX535_PACKET_MTE_STATE, SGX535_TASK_PDS, 1248, 16, state, 11);
    section(p, C_TA, SGX535_PACKET_TA_STREAM, SGX535_TASK_PDS, 1296, 16, ta, 7);
    address(p, C_VERTEX_PDS, 0, SGX535_TASK_VERTEX_DATA, 256, 64, 16, 0xffffffffu, 0);
    use_link(p, C_VERTEX_PDS, 16, SGX535_TASK_USE_VERTEX, 128, 16);
    use_link(p, C_FRAGMENT_PDS, 0, SGX535_TASK_USE_FRAGMENT, 128, 8);
    address(p, C_STATE_PDS, 0, SGX535_TASK_PDS, 1248, 44, 16, 0xffffffffu, 0);
    use_link(p, C_STATE_PDS, 8, SGX535_TASK_USE_VERTEX, 160, 16);
    address(p, C_MTE, 12, SGX535_TASK_PDS, 1152, 4, 32, 0xffffffu, 4);
    address(p, C_MTE, 20, SGX535_TASK_PDS, 1088, 56, 32, 0xffffffu, 4);
    address(p, C_TA, 0, SGX535_TASK_PDS, 1184, 60, 32, 0xfffffffu, 4);
    address(p, C_TA, 12, SGX535_TASK_VERTEX_DATA, 320, 12, 2, 0xffffffffu, 0);
    address(p, C_TA, 20, SGX535_TASK_PDS, 1024, 64, 32, 0xfffffffu, 4);
    return SGX535_DEPTH_BLOCKED;
}
static int arguments(const struct sgx535_depth_request *r, const void *p,
                     struct sgx535_depth_clear_workspace *w)
{
    return r && p && w && !overlaps(r, sizeof(*r), p, sizeof(struct sgx535_depth_clear_packet)) &&
           !overlaps(r, sizeof(*r), w, sizeof(*w)) &&
           !overlaps(p, sizeof(struct sgx535_depth_clear_packet), w, sizeof(*w));
}
enum sgx535_depth_status sgx535_depth_clear_build(const struct sgx535_depth_request *r,
                                                  struct sgx535_depth_clear_packet *out,
                                                  struct sgx535_depth_clear_workspace *w)
{
    enum sgx535_depth_status status;
    if (!arguments(r, out, w))
        return SGX535_DEPTH_INVALID;
    status = construct(r, &w->result);
    if (status == SGX535_DEPTH_BLOCKED)
        memcpy(out, &w->result, sizeof(*out));
    return status;
}
enum sgx535_depth_status sgx535_depth_clear_validate(const struct sgx535_depth_request *r,
                                                     const struct sgx535_depth_clear_packet *p,
                                                     struct sgx535_depth_clear_workspace *w)
{
    if (!arguments(r, p, w) || construct(r, &w->result) != SGX535_DEPTH_BLOCKED ||
        memcmp(p, &w->result, sizeof(*p)))
        return SGX535_DEPTH_INVALID;
    return SGX535_DEPTH_BLOCKED;
}
