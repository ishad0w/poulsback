// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_normalized_color0_task.h"
#include "sgx535_secondary_upload_internal.h"
#include "sgx535_packet_internal.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_normalized_color0_mask.h"
#include "sgx535_window_mesh_internal.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_vertex_dma.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int span(const void *p, unsigned long n)
{
    return p && n <= ~(unsigned long)0 - (unsigned long)p;
}
static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int arguments(const struct sgx535_normalized_color0_request *r,
                     const struct sgx535_normalized_color0_task *out,
                     const struct sgx535_normalized_color0_workspace *w)
{
    unsigned long n;
    if (!r || !out || !w || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    n = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r, sizeof(*r)) && span(out, sizeof(*out)) && span(w, sizeof(*w)) &&
           span(r->fragment, n) && !overlap(r, sizeof(*r), out, sizeof(*out)) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(out, sizeof(*out), w, sizeof(*w)) &&
           !overlap(r->fragment, n, out, sizeof(*out)) && !overlap(r->fragment, n, w, sizeof(*w));
}
static int tile16_arguments(const struct sgx535_normalized_color0_tile16_request *r,
                            const struct sgx535_normalized_color0_task *out,
                            const struct sgx535_normalized_color0_workspace *w)
{
    unsigned long n;
    if (!r || !out || !w || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    n = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r, sizeof(*r)) && span(out, sizeof(*out)) && span(w, sizeof(*w)) &&
           span(r->fragment, n) && !overlap(r, sizeof(*r), out, sizeof(*out)) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(out, sizeof(*out), w, sizeof(*w)) &&
           !overlap(r->fragment, n, out, sizeof(*out)) && !overlap(r->fragment, n, w, sizeof(*w));
}
/* One little-endian word: a single store (the byte loop was one store per
 * byte). Every target, the i686 kernel and userspace, is little-endian. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian words");
static void put(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}
static enum sgx535_task_status compose_encoding(uint32_t version, uint32_t profile,
                                                struct sgx535_normalized_color0_workspace *w,
                                                struct sgx535_normalized_pa0_task *sa,
                                                const uint32_t *uniforms)
{
    struct sgx535_normalized_color0_task *out = &w->result;
    struct sgx535_task_packet *p = &out->packet;
    struct sgx535_fragment_normalized_candidate *program = &out->fragment;
    uint32_t i, j, code_bytes, temp, allocation, instances, pixels;
    uint32_t fragment = SGX535_TASK_PACKET_SECTIONS, event = SGX535_TASK_PACKET_SECTIONS;
    uint32_t old_event = 0, event_bytes = 0, event_offset;
    if (program->primary_register_count != (sa ? 0u : 4u) ||
        program->secondary_register_count != (sa ? sa->physical_sa_words : 0u) ||
        program->encoding.temp_register_count > SGX535_NIR_TEMP_COUNT ||
        !program->encoding.temp_register_count || !program->encoding.word_count ||
        program->encoding.word_count > SGX535_NIR_MAX_WORDS || (program->encoding.word_count & 1u))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; !sa && i < 4; i++)
        if (program->uniform_words_used[i])
            return SGX535_TASK_UNSUPPORTED;
    code_bytes = program->encoding.word_count * 4u;
    temp = program->encoding.temp_register_count;
    allocation = (temp * 4u + 15u) & ~15u;
    /* Exact matched DRI allocator, PA4/SA0: budget=((0x4df)&0xffffffa0)/3
     * is384. PA rounds to4, hence the PA instance limit and pixel count24.
     * TEMP allocation is in bytes, rounded to16; zero instances clamp to1. */
    instances = 0xc0u / allocation;
    if (instances > 4)
        instances = 4;
    if (!instances)
        instances = 1;
    pixels = sa ? 32u : 24u;
    if ((pixels + 3u < 32u ? pixels + 3u : 32u) > instances * 8u)
        pixels = instances * 8u - 3u;
    event_offset = (code_bytes + 31u) & ~31u;
    if (sa) {
        uint32_t s = sa->physical_sa_words;
        struct sgx535_ordinary_secondary_dms dms;
        if (s != sa->reserved_sa_words + sa->uniform_sa_words + sa->literal_sa_words +
                     sa->sampler_state_sa_words ||
            !sgx535_ordinary_secondary_dms(0, temp, s, &dms))
            return SGX535_TASK_UNSUPPORTED;
        sa->dms_sa_budget_words = dms.sa_budget_words;
        sa->dms_word = dms.word;
        sa->dummy_use_offset_bytes = s ? event_offset + 32u : 0;
        sa->dummy_use_bytes = s ? 8u : 0;
        if (event_offset > 4096u - 32u - sa->dummy_use_bytes)
            return SGX535_TASK_UNSUPPORTED;
    }
    for (i = 0; i < w->seed.section_count; i++) {
        const struct sgx535_task_packet_section *s = &w->seed.sections[i];
        if (s->kind == SGX535_PACKET_FRAGMENT_USE) {
            if (fragment != SGX535_TASK_PACKET_SECTIONS || s->resource_byte_offset ||
                s->role != SGX535_TASK_USE_FRAGMENT)
                return SGX535_TASK_UNSUPPORTED;
            fragment = i;
        } else if (s->kind == SGX535_PACKET_EVENT_USE) {
            if (event != SGX535_TASK_PACKET_SECTIONS || s->role != SGX535_TASK_USE_FRAGMENT ||
                s->byte_count != 32 || s->alignment != 32)
                return SGX535_TASK_UNSUPPORTED;
            event = i;
            old_event = s->resource_byte_offset;
            event_bytes = s->byte_count;
        }
    }
    if (fragment == SGX535_TASK_PACKET_SECTIONS || event == SGX535_TASK_PACKET_SECTIONS)
        return SGX535_TASK_UNSUPPORTED;
    *p = w->seed;
    p->version = version;
    p->profile = profile;
    p->missing_contracts = SGX535_VARYING_MISSING_ALL;
    p->byte_count = 0;
    memset(p->bytes, 0, sizeof(p->bytes));
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *old = &w->seed.sections[i];
        struct sgx535_task_packet_section *s = &p->sections[i];
        uint32_t offset = (p->byte_count + 15u) & ~15u;
        uint32_t bytes = i == fragment ? code_bytes : old->byte_count;
        if (sa && i == event)
            bytes += sa->dummy_use_bytes;
        if (sa && old->kind == SGX535_PACKET_SECONDARY_PDS && sa->physical_sa_words)
            bytes = sa->secondary_capacity_bytes + sa->upload_bytes;
        if (offset > SGX535_TASK_PACKET_BYTES || bytes > SGX535_TASK_PACKET_BYTES - offset)
            return SGX535_TASK_UNSUPPORTED;
        s->byte_offset = offset;
        s->byte_count = bytes;
        if (i == fragment) {
            for (j = 0; j < program->encoding.word_count; j++)
                put(p->bytes + offset + j * 4u, program->encoding.words[j]);
        } else {
            memcpy(p->bytes + offset, w->seed.bytes + old->byte_offset, old->byte_count);
            if (i == event)
                s->resource_byte_offset = event_offset;
            else if (s->role == SGX535_TASK_USE_FRAGMENT)
                return SGX535_TASK_UNSUPPORTED;
            if (s->kind == SGX535_PACKET_FRAGMENT_PDS) {
                if (bytes != (sa ? 56u : 60u))
                    return SGX535_TASK_UNSUPPORTED;
                /* Matched primary producer splits TEMP at bit5. Preserve
                 * the selected seed's independent fixed task control0x20. */
                put(p->bytes + offset + 4u, temp << 27);
                put(p->bytes + offset + 32u, (temp >> 5) | 0x20u);
            } else if (s->kind == SGX535_PACKET_MTE_STATE) {
                if (bytes != 40 &&
                    !(bytes == 92 && version == SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION &&
                      profile == SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE))
                    return SGX535_TASK_UNSUPPORTED;
                put(p->bytes + offset + 12u,
                    sa ? sa->dms_word : 4u | (instances - 1u) << 16 | (pixels & 31u) << 25);
                if (sa && sa->physical_sa_words)
                    put(p->bytes + offset + 8u, (sa->secondary_data_words & 0xfcu) << 24);
            }
            if (sa && i == event && sa->dummy_use_bytes) {
                /* Fixed empty USE task from the selected task contract. It
                 * has no shader arithmetic or caller-selected control. */
                put(p->bytes + offset + 32u, 0);
                put(p->bytes + offset + 36u, 0xf8040140u);
            }
            if (sa && s->kind == SGX535_PACKET_SECONDARY_PDS && sa->physical_sa_words) {
                struct sgx535_vertex_dma_plan dma;
                if (!sgx535_ordinary_secondary_plan(sa->physical_sa_words, &dma) ||
                    (16u + dma.transfer_count + 2u) * 4u != sa->secondary_program_bytes ||
                    !sgx535_ordinary_secondary_emit(sa->physical_sa_words, uniforms,
                                                    p->bytes + offset, bytes))
                    return SGX535_TASK_UNSUPPORTED;
                s->resource_byte_offset = sa->secondary_resource_offset_bytes;
            }
        }
        p->byte_count = offset + bytes;
    }
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *rel = &p->relocations[i];
        uint32_t kind = p->sections[rel->section].kind;
        if (rel->role != SGX535_TASK_USE_FRAGMENT)
            continue;
        if (kind == SGX535_PACKET_FRAGMENT_PDS && !rel->resource_byte_offset)
            rel->span_bytes = code_bytes;
        else if (kind == SGX535_PACKET_EVENT_PDS && rel->resource_byte_offset >= old_event &&
                 rel->resource_byte_offset - old_event < event_bytes &&
                 rel->span_bytes <= event_bytes - (rel->resource_byte_offset - old_event))
            rel->resource_byte_offset = event_offset + rel->resource_byte_offset - old_event;
        else
            return SGX535_TASK_UNSUPPORTED;
    }
    out->fragment_code_bytes = code_bytes;
    out->fragment_temp_words = temp;
    out->dms_temp_allocation_bytes = allocation;
    out->event_use_offset_bytes = event_offset;
    out->event_use_bytes = event_bytes;
    out->fragment_use_extent_bytes = event_offset + event_bytes;
    if (sa)
        out->fragment_use_extent_bytes += sa->dummy_use_bytes;
    /* Full canonical allocations stay page sized, even when code grows. */
    if (p->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes < out->fragment_use_extent_bytes)
        p->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes =
            (out->fragment_use_extent_bytes + 4095u) & ~4095u;
    return SGX535_TASK_BLOCKED;
}
static enum sgx535_task_status
compose_program(const struct sgx535_fragment_instruction *fragment_ir, uint32_t fragment_count,
                uint32_t version, uint32_t profile, int raw_sign,
                struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_fragment_linkage linkage = {0};
    enum sgx535_nir_status status;
    memset(&w->result, 0, sizeof(w->result));
    linkage.count = 1;
    linkage.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    status = raw_sign ? sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                            fragment_ir, fragment_count, &linkage, &w->result.fragment)
                      : sgx535_fragment_compile_dri_color0_normalized_candidate(
                            fragment_ir, fragment_count, &linkage, &w->result.fragment);
    if (status != SGX535_NIR_OK)
        return status == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    return compose_encoding(version, profile, w, NULL, NULL);
}
static enum sgx535_task_status construct(const struct sgx535_normalized_color0_request *r,
                                         struct sgx535_normalized_color0_workspace *w)
{
    enum sgx535_task_status status;
    if (r->version != SGX535_NORMALIZED_COLOR0_TASK_VERSION ||
        r->profile != SGX535_NORMALIZED_COLOR0_TASK_PROFILE || r->reserved[0] || r->reserved[1] ||
        r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    status =
        sgx535_varying_window_seed(r->core_id, r->revision, &r->vertices, &w->seed, &w->varying);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return compose_program(r->fragment, r->fragment_count, r->version, r->profile, 0, w);
}
static enum sgx535_task_status
tile16_construct(const struct sgx535_normalized_color0_tile16_request *r,
                 struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_varying_request seed = {0};
    enum sgx535_task_status status;
    if (r->version != SGX535_NORMALIZED_COLOR0_TILE16_VERSION ||
        r->profile != SGX535_NORMALIZED_COLOR0_TILE16_PROFILE || r->reserved[0] || r->reserved[1] ||
        r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    seed.version = SGX535_VARYING_PACKET_VERSION;
    seed.profile = SGX535_VARYING_W1_COLOR0;
    seed.core_id = r->core_id;
    seed.revision = r->revision;
    seed.interpolation = r->interpolation;
    memcpy(seed.clip_position, r->clip_position, sizeof(seed.clip_position));
    memcpy(seed.rgba, r->rgba, sizeof(seed.rgba));
    status = sgx535_varying_clip16_seed(&seed, &w->seed, &w->varying);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return compose_program(r->fragment, r->fragment_count, r->version, r->profile, 0, w);
}
enum sgx535_task_status
sgx535_normalized_color0_task_build(const struct sgx535_normalized_color0_request *r,
                                    struct sgx535_normalized_color0_task *out,
                                    struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_request frozen;
    enum sgx535_task_status status;
    if (!arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status
sgx535_normalized_color0_task_validate(const struct sgx535_normalized_color0_request *r,
                                       const struct sgx535_normalized_color0_task *out,
                                       struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_request frozen;
    enum sgx535_task_status status;
    if (!arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = construct(&frozen, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_UNSUPPORTED : status;
}
enum sgx535_task_status
sgx535_normalized_color0_tile16_task_build(const struct sgx535_normalized_color0_tile16_request *r,
                                           struct sgx535_normalized_color0_task *out,
                                           struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_tile16_request frozen;
    enum sgx535_task_status status;
    if (!tile16_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = tile16_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_tile16_task_validate(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *out, struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_tile16_request frozen;
    enum sgx535_task_status status;
    if (!tile16_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = tile16_construct(&frozen, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_UNSUPPORTED : status;
}
#include "sgx535_normalized_color0_mask.inc"

static int pa0_arguments(const struct sgx535_normalized_pa0_request *r, const void *out,
                         size_t out_bytes, const struct sgx535_normalized_pa0_workspace *w)
{
    size_t ir_bytes, uniform_bytes;
    if (!r || !out || !w || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
        r->uniform_word_count > SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!r->uniforms != !!r->uniform_word_count))
        return 0;
    ir_bytes = (size_t)r->fragment_count * sizeof(*r->fragment);
    uniform_bytes = (size_t)r->uniform_word_count * sizeof(*r->uniforms);
    return span(r, sizeof(*r)) && span(out, out_bytes) && span(w, sizeof(*w)) &&
           span(r->fragment, ir_bytes) && (!uniform_bytes || span(r->uniforms, uniform_bytes)) &&
           !overlap(out, out_bytes, w, sizeof(*w)) && !overlap(r, sizeof(*r), out, out_bytes) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) &&
           !overlap(r->fragment, ir_bytes, out, out_bytes) &&
           !overlap(r->fragment, ir_bytes, w, sizeof(*w)) &&
           (!uniform_bytes || (!overlap(r->uniforms, uniform_bytes, out, out_bytes) &&
                               !overlap(r->uniforms, uniform_bytes, w, sizeof(*w))));
}

static int pa0_seed_check(const struct sgx535_task_packet *p, uint32_t *end)
{
    static const uint32_t kind[16] = {
        SGX535_PACKET_VERTICES,     SGX535_PACKET_INDICES,         SGX535_PACKET_VERTEX_USE,
        SGX535_PACKET_FRAGMENT_USE, SGX535_PACKET_STATE_USE,       SGX535_PACKET_EVENT_USE,
        SGX535_PACKET_VERTEX_PDS,   SGX535_PACKET_FRAGMENT_PDS,    SGX535_PACKET_SECONDARY_PDS,
        SGX535_PACKET_STATE_PDS,    SGX535_PACKET_TERMINATE_PDS,   SGX535_PACKET_EVENT_PDS,
        SGX535_PACKET_MTE_STATE,    SGX535_PACKET_TERMINATE_STATE, SGX535_PACKET_PBE_STATE,
        SGX535_PACKET_TA_STREAM};
    static const uint32_t role[16] = {3, 3, 1, 2, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const uint32_t offset[16] = {0,   64,  0,   0,   32,  32,  0,   64,
                                        128, 160, 224, 288, 448, 496, 512, 544};
    static const uint32_t bytes[16] = {48, 8, 16, 8, 48, 32, 64, 56, 4, 60, 60, 148, 40, 8, 16, 40};
    uint32_t i, last = 0;
    if (p->version != SGX535_TASK_PACKET_DRI_VERSION ||
        p->profile != SGX535_TASK_W1_PACKED_CONSTANT || p->section_count != 16)
        return 0;
    for (i = 0; i < 16; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        if (s->kind != kind[i] || s->role != role[i] || s->resource_byte_offset != offset[i] ||
            s->byte_count != bytes[i])
            return 0;
        if (s->role == SGX535_TASK_PDS && s->resource_byte_offset + s->byte_count > last)
            last = s->resource_byte_offset + s->byte_count;
    }
    *end = last;
    return 1;
}

static int pa0_intervals(const struct sgx535_task_packet *p)
{
    uint32_t i, j;
    if (p->byte_count > SGX535_TASK_PACKET_BYTES || p->section_count != 16 ||
        p->relocation_count > SGX535_TASK_PACKET_RELOCS)
        return 0;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        if (s->role >= SGX535_TASK_IMAGE_ROLES || s->byte_offset > p->byte_count ||
            s->byte_count > p->byte_count - s->byte_offset || s->resource_byte_offset > 4096 ||
            s->byte_count > 4096 - s->resource_byte_offset || !s->alignment ||
            (s->alignment & (s->alignment - 1u)) || (s->resource_byte_offset & (s->alignment - 1u)))
            return 0;
        for (j = 0; j < i; j++) {
            const struct sgx535_task_packet_section *a = &p->sections[j];
            if (overlap(p->bytes + s->byte_offset, s->byte_count, p->bytes + a->byte_offset,
                        a->byte_count) ||
                (s->role == a->role &&
                 s->resource_byte_offset < a->resource_byte_offset + a->byte_count &&
                 a->resource_byte_offset < s->resource_byte_offset + s->byte_count))
                return 0;
        }
    }
    return 1;
}

static int pa0_relocation(struct sgx535_task_packet *p, uint32_t kind, uint32_t section,
                          uint32_t offset, uint32_t role, uint32_t target, uint32_t bytes,
                          uint32_t align, uint32_t mask, uint32_t right, uint32_t left)
{
    if (p->relocation_count >= SGX535_TASK_PACKET_RELOCS || section >= p->section_count ||
        offset > p->sections[section].byte_count || 4u > p->sections[section].byte_count - offset ||
        target > 4096 || bytes > 4096 - target)
        return 0;
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        kind, section, offset, role, target, bytes, align, mask, right, left};
    return 1;
}

static enum sgx535_task_status pa0_construct(struct sgx535_normalized_pa0_workspace *w)
{
    const struct sgx535_normalized_pa0_request *r = &w->frozen;
    struct sgx535_normalized_pa0_task *out = &w->result;
    struct sgx535_normalized_color0_workspace *c = &w->common;
    struct sgx535_task_request seed = {0};
    struct sgx535_task_packet *p = &c->result.packet;
    struct sgx535_vertex_dma_plan dma;
    enum sgx535_nir_status status;
    enum sgx535_task_status result;
    uint32_t i, end, secondary = 16;
    if (r->policy != SGX535_NORMALIZED_PA0_RESERVED_SA0_V1 || r->constant_flags || r->reserved[0] ||
        r->reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    memset(&c->result, 0, sizeof(c->result));
    status = sgx535_fragment_compile_no_input_normalized_candidate(w->instructions,
                                                                   r->fragment_count, &w->logical);
    if (status != SGX535_NIR_OK)
        return status == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    out->logical_uniform_extent = w->logical.secondary_register_count;
    memcpy(out->logical_uniform_words_used, w->logical.uniform_words_used,
           sizeof(out->logical_uniform_words_used));
    if (out->logical_uniform_extent > r->uniform_word_count ||
        out->logical_uniform_extent >= SGX535_FRAGMENT_UNIFORM_WORDS)
        return SGX535_TASK_UNSUPPORTED;
    if (out->logical_uniform_extent) {
        out->reserved_sa_words = 1;
        out->uniform_sa_words = out->logical_uniform_extent;
        out->physical_sa_words = out->uniform_sa_words + 1u;
    }
    /* The original complete grammar was checked before this private clone
     * changes indices. Exact logical and physical metadata remain distinct. */
    for (i = 0; i < r->fragment_count; i++)
        if (w->instructions[i].opcode == SGX535_FRAGMENT_UNIFORM)
            w->instructions[i].index++;
    status = sgx535_fragment_compile_no_input_normalized_candidate(
        w->instructions, r->fragment_count, &c->result.fragment);
    if (status != SGX535_NIR_OK)
        return status == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    seed.version = SGX535_TASK_PACKET_DRI_VERSION;
    seed.profile = SGX535_TASK_W1_PACKED_CONSTANT;
    seed.core_id = r->core_id;
    seed.revision = r->revision;
    memcpy(seed.clip_position, r->clip_position, sizeof(seed.clip_position));
    result = sgx535_task_packet_build_words(&seed, &c->seed, &c->varying.base, &c->packet_words);
    if (result != SGX535_TASK_BLOCKED)
        return result;
    if (!pa0_seed_check(&c->seed, &end) || !pa0_intervals(&c->seed))
        return SGX535_TASK_UNSUPPORTED;
    if (out->physical_sa_words) {
        if (sgx535_vertex_dma_build(out->physical_sa_words, &dma) != SGX535_VERTEX_DMA_OK)
            return SGX535_TASK_UNSUPPORTED;
        out->secondary_resource_offset_bytes = (end + 31u) & ~31u;
        out->secondary_data_words = 16;
        out->secondary_code_offset_bytes = 64;
        out->secondary_code_bytes = (dma.transfer_count + 2u) * 4u;
        out->secondary_program_bytes = 64u + out->secondary_code_bytes;
        out->secondary_capacity_bytes = 96;
        out->upload_resource_offset_bytes = out->secondary_resource_offset_bytes + 96u;
        out->upload_bytes = out->physical_sa_words * 4u;
        if (out->upload_resource_offset_bytes > 4096u ||
            out->upload_bytes > 4096u - out->upload_resource_offset_bytes)
            return SGX535_TASK_UNSUPPORTED;
    }
    result = compose_encoding(SGX535_NORMALIZED_PA0_TASK_VERSION,
                              SGX535_NORMALIZED_PA0_TASK_PROFILE, c, out, w->uniforms);
    if (result != SGX535_TASK_BLOCKED)
        return result;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == SGX535_PACKET_SECONDARY_PDS)
            secondary = i;
    if (out->physical_sa_words) {
        for (i = 0; i < p->relocation_count; i++) {
            struct sgx535_task_packet_reloc *rel = &p->relocations[i];
            if (p->sections[rel->section].kind == SGX535_PACKET_MTE_STATE &&
                rel->section_byte_offset == 8u && rel->role == SGX535_TASK_PDS) {
                rel->resource_byte_offset = out->secondary_resource_offset_bytes;
                rel->span_bytes = out->secondary_program_bytes;
            }
        }
        for (i = 0; i < dma.transfer_count; i++)
            if (!pa0_relocation(
                    p, SGX535_PACKET_DRI_OFFSET, secondary, i ? 32u : 0u, SGX535_TASK_PDS,
                    out->upload_resource_offset_bytes + dma.transfers[i].source_byte_offset,
                    dma.transfers[i].word_count * 4u, 4, 0xffffffffu, 0, 0))
                return SGX535_TASK_UNSUPPORTED;
        if (!pa0_relocation(p, SGX535_PACKET_USE_BANK, secondary, 8, SGX535_TASK_USE_FRAGMENT,
                            out->dummy_use_offset_bytes, 8, 32, 0xf, 0, 0) ||
            !pa0_relocation(p, SGX535_PACKET_USE_ENTRY, secondary, 8, SGX535_TASK_USE_FRAGMENT,
                            out->dummy_use_offset_bytes, 8, 32, 0xf0, 15, 4) ||
            !pa0_relocation(p, SGX535_PACKET_USE_ENTRY, secondary, 8, SGX535_TASK_USE_FRAGMENT,
                            out->dummy_use_offset_bytes, 8, 32, 0x7ff00, 4, 8))
            return SGX535_TASK_UNSUPPORTED;
    }
    if (!pa0_intervals(p))
        return SGX535_TASK_UNSUPPORTED;
    out->task = c->result;
    return SGX535_TASK_BLOCKED;
}

static enum sgx535_task_status pa0_snapshot(const struct sgx535_normalized_pa0_request *r,
                                            struct sgx535_normalized_pa0_workspace *w)
{
    enum sgx535_task_status status;
    memcpy(&w->frozen, r, sizeof(w->frozen));
    memcpy(w->instructions, w->frozen.fragment,
           (size_t)w->frozen.fragment_count * sizeof(*w->instructions));
    memset(w->uniforms, 0, sizeof(w->uniforms));
    if (w->frozen.uniform_word_count)
        memcpy(w->uniforms, w->frozen.uniforms,
               (size_t)w->frozen.uniform_word_count * sizeof(*w->uniforms));
    status = pa0_construct(w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(r, &w->frozen, sizeof(*r)) ||
        (w->frozen.uniform_word_count &&
         memcmp(w->frozen.uniforms, w->uniforms,
                (size_t)w->frozen.uniform_word_count * sizeof(*w->uniforms))))
        return SGX535_TASK_UNSUPPORTED;
    /* Reconstruct logical indices before comparing the complete source. */
    for (uint32_t i = 0; i < w->frozen.fragment_count; i++)
        if (w->instructions[i].opcode == SGX535_FRAGMENT_UNIFORM)
            w->instructions[i].index--;
    return memcmp(w->frozen.fragment, w->instructions,
                  (size_t)w->frozen.fragment_count * sizeof(*w->instructions))
               ? SGX535_TASK_UNSUPPORTED
               : status;
}
enum sgx535_task_status
sgx535_normalized_pa0_task_build(const struct sgx535_normalized_pa0_request *r,
                                 struct sgx535_normalized_pa0_task *out,
                                 struct sgx535_normalized_pa0_workspace *w)
{
    enum sgx535_task_status status;
    if (!pa0_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    status = pa0_snapshot(r, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status
sgx535_normalized_pa0_task_validate(const struct sgx535_normalized_pa0_request *r,
                                    const struct sgx535_normalized_pa0_task *out,
                                    struct sgx535_normalized_pa0_workspace *w)
{
    enum sgx535_task_status status;
    if (!pa0_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    status = pa0_snapshot(r, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_UNSUPPORTED : status;
}
