// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_varying_packet.h"
#include "sgx535_packet_internal.h"
#include "sgx535_clip_w1.h"
#include "sgx535_vertex_pds.h"
#include "sgx535_vertex_use.h"
#include "sgx535_pack.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int overlaps(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int arguments(const struct sgx535_varying_request *r, const struct sgx535_task_packet *p,
                     const struct sgx535_varying_workspace *w)
{
    return r && p && w && !overlaps(r, sizeof(*r), p, sizeof(*p)) &&
           !overlaps(r, sizeof(*r), w, sizeof(*w)) && !overlaps(p, sizeof(*p), w, sizeof(*w));
}
static void put(uint8_t *p, uint32_t x)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> (8u * i));
}
static void append(struct sgx535_task_packet *p, struct sgx535_task_packet_section *s,
                   const uint32_t *words, uint32_t count)
{
    uint32_t i;
    s->byte_offset = (p->byte_count + 15u) & ~15u;
    s->byte_count = count * 4u;
    for (i = 0; i < count; i++)
        put(p->bytes + s->byte_offset + i * 4u, words[i]);
    p->byte_count = s->byte_offset + s->byte_count;
}

static enum sgx535_task_status construct_records(const struct sgx535_varying_request *r,
                                                 const uint32_t vertices[24],
                                                 struct sgx535_varying_workspace *w)
{
    struct sgx535_vertex_use_projection vertex;
    struct sgx535_vertex_pds_program vpds;
    uint32_t fragment[16], fpds[15] = {0};
    uint32_t mte[10], ta[10], i, j;
    static const uint32_t bytes[4] = {2, 1, 0, 3};
    static const uint32_t primary[4] = {2, 1, 0, 3};
    struct sgx535_task_packet *p = &w->result;
    if (sgx535_vertex_use_project(8, &vertex) != SGX535_VERTEX_USE_OK ||
        sgx535_vertex_pds_project(8, 8, &vpds) != SGX535_VERTEX_DMA_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 4; i++) {
        const struct sgx535_mov mov = {{SGX535_MOV_TEMP, i}, {SGX535_MOV_PRIMARY, primary[i]}, 0};
        const struct sgx535_pack_f32_u8_channel pack = {
            {SGX535_MOV_OUTPUT, 0}, {SGX535_MOV_TEMP, i}, bytes[i], 1, i == 3u};
        if (sgx535_mov_encode(&mov, fragment + i * 2u) != SGX535_MOV_OK ||
            sgx535_pack_f32_u8_channel_encode(&pack, fragment + 8u + i * 2u) != SGX535_PACK_OK)
            return SGX535_TASK_UNSUPPORTED;
    }
    /* Matched DRI one-iterator path: coordinate 10, four components, source
     * selector 0, smooth flags. Its last dependency is a USE task bit, unlike
     * the distinct generic Boxee PDS codec. Numeric input linkage is kept
     * separately unqualified; neither selector nor mask proves F32 effects. */
    fpds[0] = 0x00080000u;
    fpds[1] = 4u << 27; /* Four temporary words; no vendor prefix/flags. */
    fpds[8] = 0x20;
    fpds[9] = (10u << 12) | 0x0100000fu | (3u << 22) | 0x0c000000u | 0x00100000u | 0x02000000u;
    fpds[12] = 0x07000345u;
    fpds[13] = 0x070418a2u;
    fpds[14] = 0xaf000000u;
    *p = w->base;
    p->version = r->version;
    p->profile = r->profile;
    p->packed_argb = 0;
    p->missing_contracts = SGX535_VARYING_MISSING_ALL;
    p->byte_count = 0;
    memset(p->bytes, 0, sizeof(p->bytes));
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *old = &w->base.sections[i];
        struct sgx535_task_packet_section *s = &p->sections[i];
        switch (s->kind) {
        case SGX535_PACKET_VERTICES:
            append(p, s, vertices, 24);
            break;
        case SGX535_PACKET_INDICES:
            s->resource_byte_offset = 128;
            goto copy;
        case SGX535_PACKET_VERTEX_USE:
            append(p, s, vertex.words, 4);
            break;
        case SGX535_PACKET_FRAGMENT_USE:
            append(p, s, fragment, 16);
            break;
        case SGX535_PACKET_EVENT_USE:
            s->resource_byte_offset = 128;
            goto copy;
        case SGX535_PACKET_VERTEX_PDS:
            append(p, s, vpds.words, 16);
            break;
        case SGX535_PACKET_FRAGMENT_PDS:
            append(p, s, fpds, 15);
            break;
        case SGX535_PACKET_MTE_STATE:
            for (j = 0; j < 10; j++) {
                const uint8_t *b = w->base.bytes + old->byte_offset + j * 4u;
                mte[j] = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                         (uint32_t)b[3] << 24;
            }
            /* Actual DRI shader-state allocator: PA=4, temporary=4, SA=0.
             * limit=((0x4df-0)&0xffffffa0)/3=394; PA rounds to4;
             * instances=min(4,0xc0/16,394/16)=4, pixels=min(32,394/16)=24. */
            mte[3] = 4u | 3u << 16 | 24u << 25;
            mte[7] = (8u << 24) | 0x1800u;
            append(p, s, mte, 10);
            break;
        case SGX535_PACKET_TA_STREAM:
            for (j = 0; j < 10; j++) {
                const uint8_t *b = w->base.bytes + old->byte_offset + j * 4u;
                ta[j] = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                        (uint32_t)b[3] << 24;
            }
            /* Selected index producer:12 PDS data DWORDs,8 outputs,32B stride. */
            ta[6] = 3u | 2u << 25 | 0x20u << 4;
            append(p, s, ta, 10);
            break;
        default:
        copy:
            s->byte_offset = (p->byte_count + 15u) & ~15u;
            memcpy(p->bytes + s->byte_offset, w->base.bytes + old->byte_offset, s->byte_count);
            p->byte_count = s->byte_offset + s->byte_count;
            break;
        }
    }
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *rel = &p->relocations[i];
        uint32_t kind = p->sections[rel->section].kind;
        if (kind == SGX535_PACKET_VERTEX_PDS && rel->kind == SGX535_PACKET_DRI_OFFSET)
            rel->span_bytes = 96;
        if (kind == SGX535_PACKET_FRAGMENT_PDS)
            rel->span_bytes = 64;
        if (kind == SGX535_PACKET_MTE_STATE && rel->resource_byte_offset == 64)
            rel->span_bytes = 60;
        if (kind == SGX535_PACKET_EVENT_PDS && rel->role == SGX535_TASK_USE_FRAGMENT)
            rel->resource_byte_offset += 96;
        if (kind == SGX535_PACKET_TA_STREAM && rel->role == SGX535_TASK_VERTEX_DATA)
            rel->resource_byte_offset = 128;
    }
    return SGX535_TASK_BLOCKED;
}

static enum sgx535_task_status construct(const struct sgx535_varying_request *r,
                                         struct sgx535_varying_workspace *w, uint32_t dimension)
{
    struct sgx535_task_request seed = {0};
    uint32_t vertices[24], i, j;
    if (r->version != SGX535_VARYING_PACKET_VERSION || r->profile != SGX535_VARYING_W1_COLOR0 ||
        (r->interpolation != SGX535_VARYING_SMOOTH &&
         r->interpolation != SGX535_VARYING_FLAT_REPLICATED) ||
        r->reserved[0] || r->reserved[1] || r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    seed.version = SGX535_TASK_PACKET_DRI_VERSION;
    seed.profile = SGX535_TASK_W1_PACKED_CONSTANT;
    seed.core_id = r->core_id;
    seed.revision = r->revision;
    memcpy(seed.clip_position, r->clip_position, sizeof(seed.clip_position));
    for (i = 0; i < 3; i++) {
        if (sgx535_clip_w1_encode(r->clip_position[i], dimension, dimension, vertices + i * 8u) !=
            SGX535_CLIP_W1_OK)
            return SGX535_TASK_UNSUPPORTED;
        for (j = 0; j < 4; j++) {
            /* This deliberately excludes negative zero, infinities and NaNs.
             * The bound is input policy, not a numerical PACK oracle. */
            if (r->rgba[i][j] > 0x3f800000u)
                return SGX535_TASK_UNSUPPORTED;
            if (r->interpolation == SGX535_VARYING_FLAT_REPLICATED &&
                r->rgba[i][j] != r->rgba[0][j])
                return SGX535_TASK_UNSUPPORTED;
            vertices[i * 8u + 4u + j] = r->rgba[i][j];
        }
    }
    if ((dimension == 16
             ? sgx535_task_packet_clip16_seed(r->core_id, r->revision, r->clip_position, &w->base)
             : sgx535_task_packet_build(&seed, &w->base, &w->result)) != SGX535_TASK_BLOCKED)
        return SGX535_TASK_UNSUPPORTED;
    return construct_records(r, vertices, w);
}
enum sgx535_task_status sgx535_varying_clip16_seed(const struct sgx535_varying_request *r,
                                                   struct sgx535_task_packet *out,
                                                   struct sgx535_varying_workspace *w)
{
    enum sgx535_task_status status = construct(r, w, 16);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_varying_window_seed(uint32_t core, uint32_t revision,
                                                   const struct sgx535_window_color0_triangle *in,
                                                   struct sgx535_task_packet *out,
                                                   struct sgx535_varying_workspace *w)
{
    struct sgx535_varying_request metadata = {0};
    uint32_t vertices[24];
    enum sgx535_task_status status;
    if (sgx535_window_color0_encode(in, vertices) != SGX535_WINDOW_VERTEX_OK)
        return SGX535_TASK_UNSUPPORTED;
    status = sgx535_task_packet_window_seed(core, revision, in, &w->base);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    metadata.version = SGX535_WINDOW_COLOR0_PACKET_VERSION;
    metadata.profile = SGX535_WINDOW_COLOR0_PROFILE;
    metadata.interpolation =
        in->replicated_flat ? SGX535_VARYING_FLAT_REPLICATED : SGX535_VARYING_SMOOTH;
    status = construct_records(&metadata, vertices, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}

enum sgx535_task_status sgx535_varying_packet_build(const struct sgx535_varying_request *r,
                                                    struct sgx535_task_packet *out,
                                                    struct sgx535_varying_workspace *w)
{
    enum sgx535_task_status status;
    if (!arguments(r, out, w))
        return SGX535_TASK_INVALID;
    status = construct(r, w, 64);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_varying_packet_validate(const struct sgx535_varying_request *r,
                                                       const struct sgx535_task_packet *p,
                                                       struct sgx535_varying_workspace *w)
{
    enum sgx535_task_status status;
    if (!arguments(r, p, w))
        return SGX535_TASK_INVALID;
    status = construct(r, w, 64);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(p, &w->result, sizeof(*p)) ? SGX535_TASK_UNSUPPORTED : status;
}
