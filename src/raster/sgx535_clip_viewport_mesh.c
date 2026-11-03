// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_clip_viewport_mesh.h"
#include "sgx535_clip_viewport_mesh_internal.h"
#include "sgx535_clip_viewport_generic_f32.h"
#include "sgx535_window_mesh_internal.h"
#include "sgx535_background_internal.h"
#include "sgx535_clip_w1.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_generic_f32.h"
#include "sgx535_heap_layout.h"
#include "sgx535_pbe_state.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_vertex_dma.h"
#include "sgx535_vertex_use.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#include "sgx535_task_launch_prefix_internal.h"

struct cv_span {
    const void *p;
    unsigned long bytes;
};
static int cv_span_ok(struct cv_span s)
{
    return s.p && s.bytes && s.bytes <= ~(unsigned long)0 - (unsigned long)s.p;
}
static int cv_overlap(struct cv_span a, struct cv_span b)
{
    unsigned long av = (unsigned long)a.p, bv = (unsigned long)b.p;
    return av <= bv ? bv - av < a.bytes : av - bv < b.bytes;
}
static uint32_t cv_word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void cv_put(uint8_t *p, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}
static int cv_same(const struct sgx535_clip_viewport_mesh_request *a,
                   const struct sgx535_clip_viewport_mesh_request *b)
{
    return a->version == b->version && a->profile == b->profile && a->core_id == b->core_id &&
           a->revision == b->revision && a->width == b->width && a->height == b->height &&
           a->interpolation == b->interpolation &&
           !memcmp(&a->conventions, &b->conventions, sizeof(a->conventions)) &&
           a->vertices == b->vertices && a->window_sidecar == b->window_sidecar &&
           a->indices == b->indices && a->vertex_count == b->vertex_count &&
           a->index_count == b->index_count && a->fragment == b->fragment &&
           a->fragment_count == b->fragment_count &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved));
}
static int cv_args(const void *outer, unsigned long outer_bytes,
                   const struct sgx535_clip_viewport_mesh_request *r, const void *out,
                   unsigned long out_bytes, const uint8_t *geometry, uint32_t bytes,
                   struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap,
                   const struct cv_span *extra, uint32_t nr)
{
    struct cv_span a[13];
    uint32_t i, j, n = 8;
    if (!cv_span_ok((struct cv_span){outer, outer_bytes}) ||
        !cv_span_ok((struct cv_span){r, sizeof(*r)}) || r->vertex_count != 6 ||
        r->index_count != 6 || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS || bytes < 280 || cap < 280 || nr > 4)
        return 0;
    a[0] = (struct cv_span){outer, outer_bytes};
    a[1] = (struct cv_span){r->vertices, 6u * sizeof(*r->vertices)};
    a[2] = (struct cv_span){r->window_sidecar, 6u * sizeof(*r->window_sidecar)};
    a[3] = (struct cv_span){r->indices, 6u * sizeof(*r->indices)};
    a[4] = (struct cv_span){r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment)};
    a[5] = (struct cv_span){out, out_bytes};
    a[6] = (struct cv_span){geometry, bytes};
    a[7] = (struct cv_span){w, sizeof(*w)};
    a[n++] = (struct cv_span){scratch, cap};
    for (i = 0; i < nr; i++)
        a[n++] = extra[i];
    for (i = 0; i < n; i++) {
        if (!cv_span_ok(a[i]))
            return 0;
        for (j = 0; j < i; j++)
            if (cv_overlap(a[i], a[j]))
                return 0;
    }
    return 1;
}
/* This bounded exact grid prevents silently replacing Mesa's ordered F32
 * viewport by a general one-rounding mathematical reconstruction. At64²,
 * products and sums of these source coordinates are exactly representable. */
static int cv_grid(uint32_t v)
{
    uint32_t m = v & 0x7fffffffu, e = (m >> 23) & 255u, shift;
    if (!m)
        return 1;
    if (m > 0x3f800000u || e < 120u)
        return 0;
    shift = 143u - e;
    return !((m | 0x800000u) & ((1u << shift) - 1u));
}
static int cv_payload(const struct sgx535_clip_viewport_mesh_request *r, const uint8_t *g,
                      const struct sgx535_window_mesh_storage *p,
                      const struct sgx535_fragment_instruction *ir)
{
    uint32_t i, j, projected[4];
    for (i = 0; i < 6; i++) {
        if (r->vertices[i].clip_xyzw[3] != 0x3f800000u || r->indices[i] != i)
            return 0;
        for (j = 0; j < 3; j++)
            if (!cv_grid(r->vertices[i].clip_xyzw[j]))
                return 0;
        if (sgx535_clip_w1_encode(r->vertices[i].clip_xyzw, 64, 64, projected) !=
                SGX535_CLIP_W1_OK ||
            memcmp(projected, r->window_sidecar[i].window_xyz_reciprocal_w, 16) ||
            memcmp(r->vertices[i].rgba, r->window_sidecar[i].rgba, 16))
            return 0;
        for (j = 0; j < 4; j++) {
            if (r->vertices[i].rgba[j] > 0x3f800000u)
                return 0;
            if (g && (cv_word(g + i * 32u + j * 4u) != r->vertices[i].clip_xyzw[j] ||
                      cv_word(g + i * 32u + 16u + j * 4u) != r->vertices[i].rgba[j]))
                return 0;
        }
        if (g && ((uint32_t)g[p->foreground_index_offset_bytes + i * 2u] |
                  (uint32_t)g[p->foreground_index_offset_bytes + i * 2u + 1u] << 8) != i)
            return 0;
    }
    if (r->interpolation == SGX535_VARYING_FLAT_REPLICATED) {
        for (i = 0; i < 6; i += 3)
            for (j = 1; j < 3; j++)
                if (memcmp(r->vertices[i].rgba, r->vertices[i + j].rgba, 16))
                    return 0;
    } else if (r->interpolation != SGX535_VARYING_SMOOTH)
        return 0;
    return !ir || !memcmp(r->fragment, ir, (unsigned long)r->fragment_count * sizeof(*ir));
}
static uint8_t *cv_bytes(struct sgx535_clip_viewport_mesh_task *p, uint32_t source, uint32_t offset,
                         uint32_t bytes)
{
    uint8_t *base;
    uint32_t cap;
    if (source == SGX535_WINDOW_MESH_PDS) {
        base = p->pds;
        cap = sizeof(p->pds);
    } else if (source == SGX535_WINDOW_MESH_VERTEX_USE) {
        base = p->vertex_use;
        cap = sizeof(p->vertex_use);
    } else if (source == SGX535_WINDOW_MESH_FRAGMENT_USE) {
        base = p->fragment_use;
        cap = sizeof(p->fragment_use);
    } else
        return 0;
    return offset <= cap && bytes <= cap - offset ? base + offset : 0;
}
static void cv_state_use(uint32_t count, uint8_t *p)
{
    cv_put(p, 0xa0000000u);
    cv_put(p + 4, 0x28a10001u | (count - 1u) << 12);
    cv_put(p + 8, 0xa0200000u | count << 7);
    cv_put(p + 12, 0xfb274000u);
}
/* Matched EMGD fixed state program: two bounded MOV chunks and state EMIT24.
 * The typed vertex producer derives the MOV bank/register/repeat fields; the
 * existing state-EMIT field supplies the fixed24 output count. Padding is an
 * explicit CPU construction policy, not a claim about hardware consumption. */
static int cv_generic_state_use(uint8_t *p)
{
    struct sgx535_vertex_use_projection use;
    uint32_t i;
    if (sgx535_vertex_use_project(24, &use) != SGX535_VERTEX_USE_OK || use.instruction_count != 3)
        return 0;
    for (i = 0; i < 4; i++)
        cv_put(p + i * 4u, use.words[i]);
    cv_put(p + 16, 0xa0200000u | 24u << 7);
    cv_put(p + 20, 0xfb274000u);
    return 1;
}
static int cv_state_pds(uint32_t count, uint8_t *p)
{
    struct sgx535_vertex_dma_plan dma;
    if (sgx535_vertex_dma_build(count, &dma) != SGX535_VERTEX_DMA_OK || dma.transfer_count != 1)
        return 0;
    memset(p, 0, 60);
    cv_put(p + 4, dma.transfers[0].descriptor_word);
    cv_put(p + 8, 0x00200000u);
    cv_put(p + 48, 0x07030223u);
    cv_put(p + 52, 0x07042345u);
    cv_put(p + 56, 0xaf000000u);
    return 1;
}
/* Only copied bytes are shared. Each typed caller must independently retain
 * and revalidate its semantic source contract before publishing workspace. */
static int cv_copied_payload(const struct sgx535_clip_viewport_mesh_request *r, const uint8_t *g,
                             const struct sgx535_window_mesh_storage *p,
                             const struct sgx535_fragment_instruction *ir)
{
    uint32_t i, j;
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 4; j++)
            if (cv_word(g + i * 32u + j * 4u) != r->vertices[i].clip_xyzw[j] ||
                cv_word(g + i * 32u + 16u + j * 4u) != r->vertices[i].rgba[j])
                return 0;
        if (((uint32_t)g[p->foreground_index_offset_bytes + i * 2u] |
             (uint32_t)g[p->foreground_index_offset_bytes + i * 2u + 1u] << 8) != r->indices[i])
            return 0;
    }
    return !memcmp(r->fragment, ir, (unsigned long)r->fragment_count * sizeof(*ir));
}
static enum sgx535_task_status cv_emit_profile(const struct sgx535_clip_viewport_mesh_request *r,
                                               struct sgx535_clip_viewport_mesh_workspace *w,
                                               uint8_t *g, int generic_f32)
{
    static const uint32_t scale[3] = {0x42000000, 0xc2000000, 0x3f000000};
    static const uint32_t translate[3] = {0x42000000, 0x42000000, 0x3f000000};
    static const uint32_t mask[6] = {0, 1, 2, 3, 2, 1};
    struct sgx535_clip_viewport_mesh_task *p;
    const struct sgx535_task_packet *seed;
    struct sgx535_fragment_linkage linkage = {0};
    struct sgx535_pbe_state pbe;
    const struct sgx535_pbe_surface surface = {64, 64, 256, 0, 0, 16384};
    enum sgx535_task_status st;
    enum sgx535_nir_status compiled;
    uint32_t i, j, allocation, instances, pixels, changed = 0, generic_relocations = 0;
    const uint32_t mask_offset = generic_f32 ? 672u : 640u;
    const uint32_t pbe_offset = generic_f32 ? 864u : 688u;
    const uint32_t ta_offset = generic_f32 ? 880u : 704u;
    if (!r || !w || !g || !r->vertices || !r->window_sidecar || !r->indices || !r->fragment ||
        r->vertex_count != 6 || r->index_count != 6 || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_TASK_INVALID;
    p = &w->result.foreground;
    if (r->version != 15 || r->profile != 14 || r->core_id != SGX535_ENGINE121_CORE_ID ||
        r->revision != SGX535_ENGINE121_REVISION || r->width != 64 || r->height != 64 ||
        r->reserved[0] || r->reserved[1] || r->reserved[2] || r->conventions.origin != 1 ||
        r->conventions.half_pixel_center != 1 || r->conventions.bottom_edge_rule != 1 ||
        r->conventions.min_samples != 1 || r->conventions.sample_mask != 0xffffffffu ||
        r->conventions.reserved || memcmp(r->conventions.viewport_scale_f32, scale, 12) ||
        memcmp(r->conventions.viewport_translate_f32, translate, 12))
        return SGX535_TASK_UNSUPPORTED;
    memset(&w->result, 0, sizeof(w->result));
    if (sgx535_window_mesh_storage_plan(6, 6, &p->geometry) != SGX535_TASK_BLOCKED ||
        p->geometry.bytes != 280)
        return SGX535_TASK_UNSUPPORTED;
    memset(g, 0, p->geometry.bytes);
    for (i = 0; i < 6; i++)
        for (j = 0; j < 4; j++) {
            cv_put(g + i * 32u + j * 4u, r->vertices[i].clip_xyzw[j]);
            cv_put(g + i * 32u + 16u + j * 4u, r->vertices[i].rgba[j]);
        }
    for (i = 0; i < 4; i++) {
        uint8_t *v = g + p->geometry.mask_vertex_offset_bytes + i * 16u;
        cv_put(v, (i & 1u) ? 0x42800000u : 0);
        cv_put(v + 4, (i & 2u) ? 0x42800000u : 0);
        cv_put(v + 8, 0x3f800000u);
        cv_put(v + 12, 0x3f800000u);
    }
    for (i = 0; i < 12; i++) {
        uint32_t index = i < 6 ? mask[i] : r->indices[i - 6u];
        uint8_t *v = g + p->geometry.index_offset_bytes + i * 2u;
        v[0] = (uint8_t)index;
        v[1] = (uint8_t)(index >> 8);
    }
    memcpy(w->fragment, r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment));
    st = sgx535_normalized_color0_mask_mesh_template(r->core_id, r->revision, &w->seed.seed,
                                                     &w->seed);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    seed = &w->seed.seed;
    linkage.count = 1;
    linkage.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    compiled = generic_f32 ? sgx535_fragment_compile_generic_f32_normalized_candidate(
                                 w->fragment, r->fragment_count, &linkage, &p->fragment)
                           : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                                 w->fragment, r->fragment_count, &linkage, &p->fragment);
    if (compiled != SGX535_NIR_OK)
        return compiled == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    if (p->fragment.primary_register_count != 4 || p->fragment.secondary_register_count ||
        !p->fragment.encoding.temp_register_count ||
        p->fragment.encoding.temp_register_count > SGX535_NIR_TEMP_COUNT ||
        !p->fragment.encoding.word_count ||
        p->fragment.encoding.word_count > SGX535_NIR_MAX_WORDS ||
        (p->fragment.encoding.word_count & 1u))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 4; i++)
        if (p->fragment.uniform_words_used[i])
            return SGX535_TASK_UNSUPPORTED;
    p->fragment_code_bytes = p->fragment.encoding.word_count * 4u;
    p->fragment_temp_words = p->fragment.encoding.temp_register_count;
    allocation = (p->fragment_temp_words * 4u + 15u) & ~15u;
    p->dms_temp_allocation_bytes = allocation;
    p->event_use_offset_bytes = (p->fragment_code_bytes + 31u) & ~31u;
    p->fragment_use_extent_bytes = p->event_use_offset_bytes + 32u;
    if (p->fragment_use_extent_bytes > sizeof(p->fragment_use))
        return SGX535_TASK_UNSUPPORTED;
    instances = 0xc0u / allocation;
    if (instances > 4)
        instances = 4;
    if (!instances)
        instances = 1;
    pixels = 24;
    if (pixels + 3u > instances * 8u)
        pixels = instances * 8u - 3u;
    p->version = 15;
    p->profile = 14;
    p->core_id = r->core_id;
    p->revision = r->revision;
    p->width = p->height = 64;
    p->stride_bytes = 256;
    p->interpolation = r->interpolation;
    p->vertex_count = p->index_count = 6;
    p->conventions = r->conventions;
    for (i = 0; i < 3; i++) {
        p->viewport_words[i * 2u] = translate[i];
        p->viewport_words[i * 2u + 1u] = scale[i];
    }
    p->missing_packet_contracts = SGX535_VARYING_MISSING_ALL;
    p->missing_mesh_contracts = SGX535_WINDOW_MESH_MISSING_ALL;
    if (sgx535_engine121_scene_build(64, 64, &p->scene) != SGX535_ENGINE121_OK ||
        sgx535_pbe_state_project(&surface, &pbe))
        return SGX535_TASK_UNSUPPORTED;
    memcpy(p->requirements, seed->requirements, sizeof(p->requirements));
    p->requirements[SGX535_TASK_VERTEX_DATA].minimum_bytes = p->geometry.allocation_bytes;
    p->requirements[SGX535_TASK_SCENE].minimum_bytes = (p->scene.allocation_bytes + 4095u) & ~4095u;
    p->requirements[SGX535_TASK_TARGET].minimum_bytes = 16384;
    p->requirements[SGX535_TASK_PDS].minimum_bytes = 4096;
    p->section_count = seed->section_count;
    p->relocation_count = seed->relocation_count;
    p->ta_stream_section = seed->ta_stream_section;
    p->event_pds_section = seed->event_pds_section;
    if (p->section_count != 16 || p->relocation_count != 40)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &seed->sections[i];
        struct sgx535_window_mesh_section *d = &p->sections[i];
        uint8_t *dst;
        if (s->byte_offset > seed->byte_count || s->byte_count > seed->byte_count - s->byte_offset)
            return SGX535_TASK_UNSUPPORTED;
        *d = (struct sgx535_window_mesh_section){s->kind,       0,       s->resource_byte_offset,
                                                 s->byte_count, s->role, s->resource_byte_offset,
                                                 s->alignment};
        if (s->kind == SGX535_PACKET_VERTICES) {
            d->byte_count = p->geometry.vertex_bytes;
            continue;
        }
        if (s->kind == SGX535_PACKET_INDICES) {
            d->byte_offset = d->resource_byte_offset = p->geometry.index_offset_bytes;
            d->byte_count = p->geometry.index_bytes;
            continue;
        }
        if (s->role == SGX535_TASK_PDS)
            d->source = SGX535_WINDOW_MESH_PDS;
        else if (s->role == SGX535_TASK_USE_VERTEX)
            d->source = SGX535_WINDOW_MESH_VERTEX_USE;
        else if (s->role == SGX535_TASK_USE_FRAGMENT)
            d->source = SGX535_WINDOW_MESH_FRAGMENT_USE;
        else
            return SGX535_TASK_UNSUPPORTED;
        if (s->kind == SGX535_PACKET_FRAGMENT_USE)
            d->byte_count = p->fragment_code_bytes;
        if (s->kind == SGX535_PACKET_EVENT_USE)
            d->byte_offset = d->resource_byte_offset = p->event_use_offset_bytes;
        if (s->kind == SGX535_PACKET_MTE_STATE)
            d->byte_count = generic_f32 ? 192u : 108u;
        if (generic_f32 && s->kind == SGX535_PACKET_STATE_USE)
            d->byte_count = 24;
        if (generic_f32 && s->kind == SGX535_PACKET_TERMINATE_STATE) {
            d->byte_offset = d->resource_byte_offset = 768;
            d->byte_count = 96;
        }
        if (s->kind == SGX535_PACKET_PBE_STATE)
            d->byte_offset = d->resource_byte_offset = pbe_offset;
        if (s->kind == SGX535_PACKET_TA_STREAM)
            d->byte_offset = d->resource_byte_offset = ta_offset;
        dst = cv_bytes(p, d->source, d->byte_offset, d->byte_count);
        if (!dst)
            return SGX535_TASK_UNSUPPORTED;
        if (s->kind == SGX535_PACKET_FRAGMENT_USE) {
            for (j = 0; j < p->fragment.encoding.word_count; j++)
                cv_put(dst + j * 4u, p->fragment.encoding.words[j]);
        } else if (s->kind == SGX535_PACKET_MTE_STATE) {
            if (s->byte_count != 92 || cv_word(seed->bytes + s->byte_offset) != 0x54c1u ||
                cv_word(seed->bytes + s->byte_offset + 32) != 0x10000u)
                return SGX535_TASK_UNSUPPORTED;
            cv_put(dst, generic_f32 ? 0xd5c1u : 0x55c1u);
            cv_put(dst + 4, 0x01d00000u);
            cv_put(dst + 12, 4u | (instances - 1u) << 16 | (pixels & 31u) << 25);
            cv_put(dst + 16, 0x0c000000u);
            for (j = 0; j < 6; j++)
                cv_put(dst + 28u + j * 4u, p->viewport_words[j]);
            /* The generic link is one coupled source-derived construction,
             * never a modification of a validated primary-color packet. */
            cv_put(dst + 52, generic_f32 ? 0x08001000u : 0x08001800u);
            cv_put(dst + 56, 0);
            cv_put(dst + 60, generic_f32 ? 7u : 0u);
            if (generic_f32)
                cv_put(dst + 64, 0); /* explicit TEXFLOAT, following TEXSIZE */
            memcpy(dst + mask_offset - 576u, seed->bytes + s->byte_offset + 48, 44);
        } else if (!generic_f32 || s->kind != SGX535_PACKET_STATE_USE)
            memcpy(dst, seed->bytes + s->byte_offset, s->byte_count);
        if (s->kind == SGX535_PACKET_STATE_USE) {
            if (s->byte_count != 48)
                return SGX535_TASK_UNSUPPORTED;
            if (generic_f32) {
                if (!cv_generic_state_use(dst))
                    return SGX535_TASK_UNSUPPORTED;
            } else {
                cv_state_use(11, dst);
                cv_state_use(16, dst + 16);
                cv_state_use(2, dst + 32);
            }
        } else if (s->kind == SGX535_PACKET_STATE_PDS) {
            if (d->byte_count != 124 || !cv_state_pds(generic_f32 ? 24u : 11u, dst) ||
                !cv_state_pds(generic_f32 ? 24u : 16u, dst + 64))
                return SGX535_TASK_UNSUPPORTED;
        } else if (generic_f32 && s->kind == SGX535_PACKET_TERMINATE_PDS) {
            if (!cv_state_pds(24, dst))
                return SGX535_TASK_UNSUPPORTED;
        } else if (s->kind == SGX535_PACKET_FRAGMENT_PDS) {
            if (d->byte_count != 60)
                return SGX535_TASK_UNSUPPORTED;
            if (generic_f32) {
                if (cv_word(dst + 36) != 0x0fd0a00fu)
                    return SGX535_TASK_UNSUPPORTED;
                cv_put(dst + 36, 0x0fd0000fu);
            }
            cv_put(dst + 4, p->fragment_temp_words << 27);
            cv_put(dst + 32, (p->fragment_temp_words >> 5) | 0x20u);
        } else if (s->kind == SGX535_PACKET_TERMINATE_STATE)
            cv_put(dst + 4, 0x00040004);
        else if (s->kind == SGX535_PACKET_PBE_STATE)
            for (j = 0; j < 4; j++)
                cv_put(dst + j * 4u, pbe.words[j]);
        else if (s->kind == SGX535_PACKET_TA_STREAM) {
            cv_put(dst + 36, 0x81400006u);
            if (generic_f32) {
                /* Matched EMGD13cea0/134db0: allocated primary bytes96,
                 * encoded in16-byte units independently of PDS data size. */
                const uint32_t state_bytes = 24u * 4u;
                const uint32_t attributes = (state_bytes + 15u) >> 4;
                if (!attributes || attributes > 0xffu || (attributes << 4) < state_bytes)
                    return SGX535_TASK_UNSUPPORTED;
                for (j = 4; j <= 60; j += 28) {
                    uint32_t word = cv_word(dst + j);
                    if (word != (j == 60 ? 0x0c02a201u : 0x0c02a203u))
                        return SGX535_TASK_UNSUPPORTED;
                    cv_put(dst + j, (word & 0xffffff00u) | attributes);
                }
            } else {
                /* CLIP foreground state DMA/USE is16 DWORDs. The distinct
                 * TA primary-allocation field uses16-byte units. */
                const uint32_t state_bytes = 16u * 4u;
                const uint32_t attributes = (state_bytes + 15u) >> 4;
                uint32_t word = cv_word(dst + 32);
                if (!attributes || attributes > 0xffu || (attributes << 4) < state_bytes ||
                    word != 0x0c02a203u)
                    return SGX535_TASK_UNSUPPORTED;
                cv_put(dst + 32, (word & 0xffffff00u) | attributes);
            }
        }
    }
    memcpy(p->relocations, seed->relocations, sizeof(p->relocations));
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *a = &p->relocations[i];
        uint32_t kind = p->sections[a->section].kind;
        if (a->role == SGX535_TASK_VERTEX_DATA) {
            if (kind == SGX535_PACKET_VERTEX_PDS && !a->section_byte_offset &&
                !a->resource_byte_offset && a->span_bytes == 96) {
                a->span_bytes = 192;
                changed++;
            } else if (kind == SGX535_PACKET_VERTEX_PDS && a->section_byte_offset == 64 &&
                       a->resource_byte_offset == 96 && a->span_bytes == 64) {
                a->resource_byte_offset = 192;
                changed++;
            } else if (kind == SGX535_PACKET_TA_STREAM && a->section_byte_offset == 12 &&
                       a->resource_byte_offset == 160 && a->span_bytes == 12) {
                a->resource_byte_offset = 256;
                changed++;
            } else if (kind == SGX535_PACKET_TA_STREAM && a->section_byte_offset == 40 &&
                       a->resource_byte_offset == 172 && a->span_bytes == 6) {
                a->resource_byte_offset = 268;
                a->span_bytes = 12;
                changed++;
            } else
                return SGX535_TASK_UNSUPPORTED;
        } else if (a->role == SGX535_TASK_USE_FRAGMENT) {
            if (kind == SGX535_PACKET_FRAGMENT_PDS && !a->resource_byte_offset)
                a->span_bytes = p->fragment_code_bytes;
            else if (kind == SGX535_PACKET_EVENT_PDS && a->resource_byte_offset >= 32 &&
                     a->resource_byte_offset < 64 && a->span_bytes <= 64u - a->resource_byte_offset)
                a->resource_byte_offset = p->event_use_offset_bytes + a->resource_byte_offset - 32u;
            else
                return SGX535_TASK_UNSUPPORTED;
        } else if (generic_f32 && a->role == SGX535_TASK_USE_VERTEX &&
                   (kind == SGX535_PACKET_STATE_PDS || kind == SGX535_PACKET_TERMINATE_PDS)) {
            uint32_t old_entry = kind == SGX535_PACKET_TERMINATE_PDS ? 64u
                                 : a->section_byte_offset == 8       ? 32u
                                 : a->section_byte_offset == 72      ? 48u
                                                                     : 0u;
            if (!old_entry || a->resource_byte_offset != old_entry || a->span_bytes != 16)
                return SGX535_TASK_UNSUPPORTED;
            a->resource_byte_offset = 32;
            a->span_bytes = 24;
            generic_relocations++;
        } else if (kind == SGX535_PACKET_STATE_PDS && !a->section_byte_offset &&
                   a->role == SGX535_TASK_PDS) {
            a->resource_byte_offset = mask_offset;
            a->span_bytes = generic_f32 ? 96u : 44u;
            changed++;
        } else if (kind == SGX535_PACKET_STATE_PDS && a->section_byte_offset == 64 &&
                   a->role == SGX535_TASK_PDS) {
            a->resource_byte_offset = 576;
            a->span_bytes = generic_f32 ? 96u : 64u;
            changed++;
        } else if (generic_f32 && kind == SGX535_PACKET_TERMINATE_PDS && !a->section_byte_offset &&
                   a->role == SGX535_TASK_PDS) {
            if (a->resource_byte_offset != 208 || a->span_bytes != 8)
                return SGX535_TASK_UNSUPPORTED;
            a->resource_byte_offset = 768;
            a->span_bytes = 96;
            generic_relocations++;
        } else if (kind == SGX535_PACKET_EVENT_PDS && a->section_byte_offset == 8 &&
                   a->role == SGX535_TASK_PDS) {
            a->resource_byte_offset = pbe_offset;
            changed++;
        } else if (a->role == SGX535_TASK_TARGET) {
            if (kind != SGX535_PACKET_PBE_STATE || a->section_byte_offset != 4 ||
                a->span_bytes != 2048)
                return SGX535_TASK_UNSUPPORTED;
            a->span_bytes = 16384;
        }
    }
    return changed == 7 && generic_relocations == (generic_f32 ? 10u : 0u) &&
                   cv_copied_payload(r, g, &p->geometry, w->fragment)
               ? SGX535_TASK_BLOCKED
               : SGX535_TASK_UNSUPPORTED;
}
enum sgx535_task_status
sgx535_clip_viewport_internal_emit(const struct sgx535_clip_viewport_mesh_request *r,
                                   struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *g)
{
    return cv_emit_profile(r, w, g, 0);
}
static enum sgx535_task_status cv_construct(const struct sgx535_clip_viewport_mesh_request *r,
                                            struct sgx535_clip_viewport_mesh_workspace *w,
                                            uint8_t *g)
{
    static const uint32_t scale[3] = {0x42000000, 0xc2000000, 0x3f000000};
    static const uint32_t translate[3] = {0x42000000, 0x42000000, 0x3f000000};
    enum sgx535_task_status status;
    if (r->version != 15 || r->profile != 14 || r->core_id != SGX535_ENGINE121_CORE_ID ||
        r->revision != SGX535_ENGINE121_REVISION || r->width != 64 || r->height != 64 ||
        r->reserved[0] || r->reserved[1] || r->reserved[2] || r->conventions.origin != 1 ||
        r->conventions.half_pixel_center != 1 || r->conventions.bottom_edge_rule != 1 ||
        r->conventions.min_samples != 1 || r->conventions.sample_mask != 0xffffffffu ||
        r->conventions.reserved || memcmp(r->conventions.viewport_scale_f32, scale, 12) ||
        memcmp(r->conventions.viewport_translate_f32, translate, 12) || !cv_payload(r, 0, 0, 0))
        return SGX535_TASK_UNSUPPORTED;
    status = sgx535_clip_viewport_internal_emit(r, w, g);
    return status == SGX535_TASK_BLOCKED &&
                   !cv_payload(r, g, &w->result.foreground.geometry, w->fragment)
               ? SGX535_TASK_UNSUPPORTED
               : status;
}
static enum sgx535_task_status cv_regenerate(const struct sgx535_clip_viewport_mesh_request *r,
                                             struct sgx535_clip_viewport_mesh_workspace *w,
                                             uint8_t *scratch)
{
    struct sgx535_clip_viewport_mesh_request f = *r;
    enum sgx535_task_status st = cv_construct(&f, w, scratch);
    return st == SGX535_TASK_BLOCKED && !cv_same(r, &f) ? SGX535_TASK_UNSUPPORTED : st;
}

static int cv_binding(const struct sgx535_clip_viewport_mesh_task *p,
                      const struct sgx535_window_mesh_binding *b)
{
    struct sgx535_heap_layout_input h;
    struct sgx535_heap_layout_projection hp;
    uint32_t i, j;
    if (!b->epoch || b->reserved[0] || b->reserved[1] || b->bank_index[0] < 3 ||
        b->bank_index[0] > 15 || b->bank_index[1] < 3 || b->bank_index[1] > 15 ||
        b->bank_index[0] == b->bank_index[1])
        return 0;
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
        const struct sgx535_task_launch_role *r = &b->roles[i];
        const struct sgx535_task_requirement *q = &p->requirements[i];
        if (!r->storage_id || r->reserved || r->access != q->access ||
            !sgx535_task_role_placed(i, r->gpu_va_bytes, r->allocation_bytes) ||
            r->allocation_bytes < q->minimum_bytes || (r->gpu_va_bytes & (q->alignment - 1u)) ||
            (r->allocation_bytes & 4095u))
            return 0;
        for (j = 0; j < i; j++)
            if (r->storage_id == b->roles[j].storage_id ||
                (r->gpu_va_bytes < b->roles[j].gpu_va_bytes + b->roles[j].allocation_bytes &&
                 b->roles[j].gpu_va_bytes < r->gpu_va_bytes + r->allocation_bytes))
                return 0;
    }
#define CV_HEAP(role)                                                                              \
    (struct sgx535_heap_extent){b->roles[role].storage_id, b->roles[role].gpu_va_bytes,            \
                                b->roles[role].allocation_bytes}
    h.pds = CV_HEAP(SGX535_TASK_PDS);
    h.parameters = CV_HEAP(SGX535_TASK_DPM_PARAMETERS);
    h.tables = CV_HEAP(SGX535_TASK_DPM_TABLES);
    h.scene = CV_HEAP(SGX535_TASK_SCENE);
#undef CV_HEAP
    return sgx535_heap_layout_project(&h, &hp) == SGX535_HEAP_LAYOUT_OK;
}
static enum sgx535_task_status cv_bind_image(const struct sgx535_clip_viewport_mesh_task *p,
                                             const struct sgx535_window_mesh_binding *b,
                                             struct sgx535_clip_viewport_mesh_bound *out)
{
    uint32_t i, j;
    if (!cv_binding(p, b))
        return SGX535_TASK_INVALID;
    memset(out, 0, sizeof(*out));
    out->image = *p;
    out->binding = *b;
    out->missing_launch_contracts =
        SGX535_LAUNCH_MISSING_ALL &
        ~(SGX535_LAUNCH_MISSING_EVENT_PROFILE | SGX535_LAUNCH_MISSING_CALLER_SCALARS);
    sgx535_launch_prefix_write(&out->stages[0], 0xab8, 0x20000000u, 0);
    for (i = 0; i < 2; i++) {
        const struct sgx535_task_launch_role *r = &b->roles[SGX535_TASK_USE_VERTEX + i];
        struct sgx535_use_code_bank_input code = {(uint32_t)r->gpu_va_bytes,
                                                  (uint32_t)r->allocation_bytes,
                                                  (uint32_t)r->gpu_va_bytes, b->bank_index[i], i};
        struct sgx535_use_code_bank_projection bank;
        if (sgx535_use_code_bank_project(&code, &bank) != SGX535_USE_CODE_BANK_OK)
            return SGX535_TASK_INVALID;
        out->bank_base[i] = bank.bank_base_va_bytes;
        sgx535_launch_prefix_write(&out->stages[0], bank.register_offset_bytes, bank.register_word,
                                   0);
    }
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &p->sections[i];
        const struct sgx535_task_launch_role *r = &b->roles[s->role];
        if (s->resource_byte_offset > r->allocation_bytes ||
            s->byte_count > r->allocation_bytes - s->resource_byte_offset ||
            ((r->gpu_va_bytes + s->resource_byte_offset) & (s->alignment - 1u)))
            return SGX535_TASK_INVALID;
        for (j = 0; j < i; j++)
            if (p->sections[j].role == s->role &&
                s->resource_byte_offset <
                    p->sections[j].resource_byte_offset + p->sections[j].byte_count &&
                p->sections[j].resource_byte_offset < s->resource_byte_offset + s->byte_count)
                return SGX535_TASK_INVALID;
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        const struct sgx535_window_mesh_section *s = &p->sections[r->section];
        const struct sgx535_task_launch_role *owner = &b->roles[r->role];
        uint64_t va = owner->gpu_va_bytes + r->resource_byte_offset, value;
        uint8_t *dst;
        if (r->resource_byte_offset > owner->allocation_bytes ||
            r->span_bytes > owner->allocation_bytes - r->resource_byte_offset ||
            (va & (r->alignment - 1u)) || r->section_byte_offset > s->byte_count ||
            4u > s->byte_count - r->section_byte_offset)
            return SGX535_TASK_INVALID;
        dst = cv_bytes(&out->image, s->source, s->byte_offset + r->section_byte_offset, 4);
        if (!dst)
            return SGX535_TASK_INVALID;
        if (r->kind == SGX535_PACKET_DRI_OFFSET)
            value = (va >> r->right_shift) << r->left_shift;
        else {
            uint32_t index;
            struct sgx535_use_code_bank_input code;
            struct sgx535_pds_use_link_projection link;
            if (r->role != SGX535_TASK_USE_VERTEX && r->role != SGX535_TASK_USE_FRAGMENT)
                return SGX535_TASK_INVALID;
            index = r->role - SGX535_TASK_USE_VERTEX;
            code = (struct sgx535_use_code_bank_input){(uint32_t)owner->gpu_va_bytes,
                                                       (uint32_t)owner->allocation_bytes,
                                                       (uint32_t)va, b->bank_index[index], index};
            if (sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
                return SGX535_TASK_INVALID;
            if (r->kind == SGX535_PACKET_USE_BANK)
                value = link.bank_selector_fields;
            else if (r->kind == SGX535_PACKET_USE_ENTRY)
                value = link.address_fields;
            else
                return SGX535_TASK_INVALID;
        }
        cv_put(dst, (cv_word(dst) & ~r->mask) | ((uint32_t)value & r->mask));
        out->resolved_relocations++;
    }
    out->ta_stream_va_bytes = (uint32_t)b->roles[SGX535_TASK_PDS].gpu_va_bytes +
                              p->sections[p->ta_stream_section].resource_byte_offset;
    out->event_pds_va_bytes = (uint32_t)b->roles[SGX535_TASK_PDS].gpu_va_bytes +
                              p->sections[p->event_pds_section].resource_byte_offset;
    if (!sgx535_launch_prefix_build_convention(
            &out->stages[1], &out->stages[2], out->ta_stream_va_bytes, out->event_pds_va_bytes, 1,
            SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, 0, SGX535_LAUNCH_PREFIX_WIN7_GL_WINDOW))
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}

enum sgx535_task_status
sgx535_clip_viewport_mesh_build(const struct sgx535_clip_viewport_mesh_request *r,
                                struct sgx535_clip_viewport_mesh_task *out, uint8_t *g,
                                uint32_t bytes, struct sgx535_clip_viewport_mesh_workspace *w,
                                uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status st;
    if (!cv_args(r, sizeof(*r), r, out, sizeof(*out), g, bytes, w, scratch, cap, 0, 0))
        return SGX535_TASK_INVALID;
    st = cv_regenerate(r, w, scratch);
    if (st == SGX535_TASK_BLOCKED) {
        *out = w->result.foreground;
        memcpy(g, scratch, out->geometry.bytes);
    }
    return st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_validate(
    const struct sgx535_clip_viewport_mesh_request *r,
    const struct sgx535_clip_viewport_mesh_task *out, const uint8_t *g, uint32_t bytes,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status st;
    if (!cv_args(r, sizeof(*r), r, out, sizeof(*out), g, bytes, w, scratch, cap, 0, 0))
        return SGX535_TASK_INVALID;
    st = cv_regenerate(r, w, scratch);
    return st == SGX535_TASK_BLOCKED &&
                   (bytes != w->result.foreground.geometry.bytes ||
                    memcmp(out, &w->result.foreground, sizeof(*out)) || memcmp(g, scratch, bytes))
               ? SGX535_TASK_INVALID
               : st;
}
static enum sgx535_task_status cv_bind_construct(const struct sgx535_clip_viewport_mesh_request *r,
                                                 const struct sgx535_clip_viewport_mesh_task *p,
                                                 const uint8_t *g, uint32_t bytes,
                                                 const struct sgx535_window_mesh_binding *binding,
                                                 struct sgx535_clip_viewport_mesh_workspace *w,
                                                 uint8_t *scratch)
{
    struct sgx535_window_mesh_binding b = *binding;
    enum sgx535_task_status st = cv_regenerate(r, w, scratch);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    if (bytes != w->result.foreground.geometry.bytes ||
        memcmp(p, &w->result.foreground, sizeof(*p)) || memcmp(g, scratch, bytes))
        return SGX535_TASK_INVALID;
    st = cv_bind_image(&w->result.foreground, &b, &w->bound.foreground);
    return st == SGX535_TASK_BLOCKED && memcmp(binding, &b, sizeof(b)) ? SGX535_TASK_INVALID : st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_bind(
    const struct sgx535_clip_viewport_mesh_request *r,
    const struct sgx535_clip_viewport_mesh_task *p, const uint8_t *g, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, struct sgx535_clip_viewport_mesh_bound *out,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[2] = {{p, sizeof(*p)}, {b, sizeof(*b)}};
    enum sgx535_task_status st;
    if (!cv_args(r, sizeof(*r), r, out, sizeof(*out), g, bytes, w, scratch, cap, extra, 2))
        return SGX535_TASK_INVALID;
    st = cv_bind_construct(r, p, g, bytes, b, w, scratch);
    if (st == SGX535_TASK_BLOCKED)
        *out = w->bound.foreground;
    return st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_bound_validate(
    const struct sgx535_clip_viewport_mesh_request *r,
    const struct sgx535_clip_viewport_mesh_task *p, const uint8_t *g, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_clip_viewport_mesh_bound *out,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[2] = {{p, sizeof(*p)}, {b, sizeof(*b)}};
    enum sgx535_task_status st;
    if (!cv_args(r, sizeof(*r), r, out, sizeof(*out), g, bytes, w, scratch, cap, extra, 2))
        return SGX535_TASK_INVALID;
    st = cv_bind_construct(r, p, g, bytes, b, w, scratch);
    return st == SGX535_TASK_BLOCKED && memcmp(out, &w->bound.foreground, sizeof(*out))
               ? SGX535_TASK_INVALID
               : st;
}
static struct sgx535_background_request
cv_bg_request(const struct sgx535_clip_viewport_mesh_background_request *r)
{
    struct sgx535_background_request b = {0};
    b.version = SGX535_BACKGROUND_CLIP_VIEWPORT_VERSION;
    b.core_id = r->foreground.core_id;
    b.revision = r->foreground.revision;
    b.width = b.height = 64;
    b.packed_argb = r->packed_background_argb;
    return b;
}
static enum sgx535_task_status
cv_bg_construct(const struct sgx535_clip_viewport_mesh_background_request *r,
                struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch)
{
    struct sgx535_clip_viewport_mesh_background_request f = *r;
    struct sgx535_background_request b = cv_bg_request(&f);
    enum sgx535_task_status st;
    if (f.version != 7 || f.reserved[0] || f.reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    st = cv_regenerate(&f.foreground, w, scratch);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    st = sgx535_background_clip_viewport_seed(&b, &w->result.background);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    w->result.version = 7;
    w->result.missing_background_contracts = SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL;
    return r->version != f.version || r->packed_background_argb != f.packed_background_argb ||
                   memcmp(r->reserved, f.reserved, sizeof(f.reserved)) ||
                   !cv_same(&r->foreground, &f.foreground)
               ? SGX535_TASK_UNSUPPORTED
               : st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_background_build(
    const struct sgx535_clip_viewport_mesh_background_request *r,
    struct sgx535_clip_viewport_mesh_background_image *out, uint8_t *g, uint32_t bytes,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status st;
    if (!r)
        return SGX535_TASK_INVALID;
    if (!cv_args(r, sizeof(*r), &r->foreground, out, sizeof(*out), g, bytes, w, scratch, cap, 0, 0))
        return SGX535_TASK_INVALID;
    st = cv_bg_construct(r, w, scratch);
    if (st == SGX535_TASK_BLOCKED) {
        *out = w->result;
        memcpy(g, scratch, out->foreground.geometry.bytes);
    }
    return st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_background_validate(
    const struct sgx535_clip_viewport_mesh_background_request *r,
    const struct sgx535_clip_viewport_mesh_background_image *out, const uint8_t *g, uint32_t bytes,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status st;
    if (!r)
        return SGX535_TASK_INVALID;
    if (!cv_args(r, sizeof(*r), &r->foreground, out, sizeof(*out), g, bytes, w, scratch, cap, 0, 0))
        return SGX535_TASK_INVALID;
    st = cv_bg_construct(r, w, scratch);
    return st == SGX535_TASK_BLOCKED &&
                   (bytes != w->result.foreground.geometry.bytes ||
                    memcmp(out, &w->result, sizeof(*out)) || memcmp(g, scratch, bytes))
               ? SGX535_TASK_INVALID
               : st;
}
enum sgx535_task_status
sgx535_clip_viewport_internal_bind_pair(const struct sgx535_background_request *request,
                                        const struct sgx535_window_mesh_binding *binding,
                                        const struct sgx535_background_binding *background,
                                        struct sgx535_clip_viewport_mesh_workspace *w)
{
    struct sgx535_window_mesh_binding b = *binding;
    struct sgx535_background_binding bg = *background;
    enum sgx535_task_status st;
    uint32_t i, j;
    st = cv_bind_image(&w->result.foreground, &b, &w->bound.foreground);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    if (bg.epoch != b.epoch ||
        bg.parameter_storage_id != b.roles[SGX535_TASK_DPM_PARAMETERS].storage_id ||
        bg.parameter_va != b.roles[SGX535_TASK_DPM_PARAMETERS].gpu_va_bytes ||
        bg.parameter_bytes != b.roles[SGX535_TASK_DPM_PARAMETERS].allocation_bytes ||
        bg.fragment_bank_index != b.bank_index[1])
        return SGX535_TASK_INVALID;
    for (i = 0; i < 2; i++) {
        uint32_t role = i ? SGX535_TASK_PDS : SGX535_TASK_USE_FRAGMENT;
        if (bg.resources[i].storage_id != b.roles[role].storage_id ||
            bg.resources[i].gpu_va_bytes != b.roles[role].gpu_va_bytes ||
            bg.resources[i].allocation_bytes != b.roles[role].allocation_bytes ||
            bg.resources[i].access != b.roles[role].access)
            return SGX535_TASK_INVALID;
    }
    for (i = 0; i < 8; i++)
        if (bg.resources[2].storage_id == b.roles[i].storage_id ||
            (bg.resources[2].gpu_va_bytes < b.roles[i].gpu_va_bytes + b.roles[i].allocation_bytes &&
             b.roles[i].gpu_va_bytes <
                 bg.resources[2].gpu_va_bytes + bg.resources[2].allocation_bytes))
            return SGX535_TASK_INVALID;
    for (i = 0; i < w->result.background.section_count; i++) {
        const struct sgx535_background_section *s = &w->result.background.sections[i];
        uint32_t role =
            s->role == SGX535_BACKGROUND_USE ? SGX535_TASK_USE_FRAGMENT : SGX535_TASK_PDS;
        if (s->role == SGX535_BACKGROUND_RASTGEOM)
            continue;
        for (j = 0; j < w->result.foreground.section_count; j++) {
            const struct sgx535_window_mesh_section *t = &w->result.foreground.sections[j];
            if (t->role == role && s->resource_offset < t->resource_byte_offset + t->byte_count &&
                t->resource_byte_offset < s->resource_offset + s->byte_count)
                return SGX535_TASK_INVALID;
        }
    }
    st = sgx535_background_clip_viewport_bind_seed(request, &w->result.background, &bg,
                                                   &w->bound.background);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    memset(&w->bound.foreground.stages[1], 0, sizeof(w->bound.foreground.stages[1]));
    memset(&w->bound.foreground.stages[2], 0, sizeof(w->bound.foreground.stages[2]));
    if (!sgx535_launch_prefix_build_convention(
            &w->bound.foreground.stages[1], &w->bound.foreground.stages[2],
            w->bound.foreground.ta_stream_va_bytes, w->bound.foreground.event_pds_va_bytes, 1,
            SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, &w->bound.background,
            SGX535_LAUNCH_PREFIX_WIN7_GL_WINDOW))
        return SGX535_TASK_UNSUPPORTED;
    w->bound.missing_background_contracts = SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL;
    return SGX535_TASK_BLOCKED;
}
static enum sgx535_task_status
cv_bg_bind_construct(const struct sgx535_clip_viewport_mesh_background_request *r,
                     const struct sgx535_clip_viewport_mesh_background_image *p, const uint8_t *g,
                     uint32_t bytes, const struct sgx535_window_mesh_binding *binding,
                     const struct sgx535_background_binding *background,
                     struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch)
{
    struct sgx535_window_mesh_binding b = *binding;
    struct sgx535_background_binding bg = *background;
    struct sgx535_background_request request = cv_bg_request(r);
    enum sgx535_task_status st;
    st = cv_bg_construct(r, w, scratch);
    if (st != SGX535_TASK_BLOCKED)
        return st;
    if (bytes != w->result.foreground.geometry.bytes || memcmp(p, &w->result, sizeof(*p)) ||
        memcmp(g, scratch, bytes))
        return SGX535_TASK_INVALID;
    st = sgx535_clip_viewport_internal_bind_pair(&request, &b, &bg, w);
    return st == SGX535_TASK_BLOCKED &&
                   (memcmp(binding, &b, sizeof(b)) || memcmp(background, &bg, sizeof(bg)))
               ? SGX535_TASK_INVALID
               : st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_background_bind(
    const struct sgx535_clip_viewport_mesh_background_request *r,
    const struct sgx535_clip_viewport_mesh_background_image *p, const uint8_t *g, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    struct sgx535_clip_viewport_mesh_background_bound *out,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[3] = {{p, sizeof(*p)}, {b, sizeof(*b)}, {bg, sizeof(*bg)}};
    enum sgx535_task_status st;
    if (!r)
        return SGX535_TASK_INVALID;
    if (!cv_args(r, sizeof(*r), &r->foreground, out, sizeof(*out), g, bytes, w, scratch, cap, extra,
                 3))
        return SGX535_TASK_INVALID;
    st = cv_bg_bind_construct(r, p, g, bytes, b, bg, w, scratch);
    if (st == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return st;
}
enum sgx535_task_status sgx535_clip_viewport_mesh_background_bound_validate(
    const struct sgx535_clip_viewport_mesh_background_request *r,
    const struct sgx535_clip_viewport_mesh_background_image *p, const uint8_t *g, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    const struct sgx535_clip_viewport_mesh_background_bound *out,
    struct sgx535_clip_viewport_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[3] = {{p, sizeof(*p)}, {b, sizeof(*b)}, {bg, sizeof(*bg)}};
    enum sgx535_task_status st;
    if (!r)
        return SGX535_TASK_INVALID;
    if (!cv_args(r, sizeof(*r), &r->foreground, out, sizeof(*out), g, bytes, w, scratch, cap, extra,
                 3))
        return SGX535_TASK_INVALID;
    st = cv_bg_bind_construct(r, p, g, bytes, b, bg, w, scratch);
    return st == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out)) ? SGX535_TASK_INVALID
                                                                             : st;
}

#include "sgx535_clip_viewport_generic_f32.inc"
