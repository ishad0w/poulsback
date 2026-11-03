// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_window_mesh.h"
#include "poulsback_selfcheck.h"
#include "sgx535_window_mesh_gl_window.h"
#include "sgx535_window_mesh_internal.h"
#include "sgx535_single_sample_internal.h"
#include "sgx535_background_internal.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_generic_f32.h"
#include "sgx535_vertex_dma.h"
#include "sgx535_heap_layout.h"
#include "sgx535_pbe_state.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_vertex_pds.h"
#include "sgx535_secondary_upload_internal.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif
#include "sgx535_task_launch_prefix_internal.h"

enum window_convention { WINDOW_LEGACY = 0, WINDOW_WIN7_GL = 1, WINDOW_WIN7_GL_SINGLE_SAMPLE = 2 };
static uint32_t uint_f32(uint32_t value);
static int source_convention(const struct sgx535_window_mesh_request *r,
                             enum window_convention convention)
{
    uint32_t scale[3] = {0x42000000, 0xc2000000, 0x3f000000};
    uint32_t translate[3] = {0x42000000, 0x42000000, 0x3f000000};
    if (r->window.origin != 1 || r->window.reserved)
        return 0;
    if (convention == WINDOW_LEGACY)
        return !r->window.half_pixel_center && !r->window.bottom_edge_rule &&
               (r->window.sample_mask & 1u) && r->window.min_samples <= 1;
    if (convention != WINDOW_WIN7_GL && convention != WINDOW_WIN7_GL_SINGLE_SAMPLE)
        return 0;
    if (convention == WINDOW_WIN7_GL_SINGLE_SAMPLE) {
        if (!r->width || !r->height || r->width > 2047 || r->height > 2047)
            return 0;
        /* Integer extents in the existing scene domain are exact F32 values.
         * Dividing by two changes only the exponent, including odd extents. */
        scale[0] = translate[0] = uint_f32(r->width) - 0x00800000u;
        translate[1] = uint_f32(r->height) - 0x00800000u;
        scale[1] = translate[1] | 0x80000000u;
    } else if (r->width != 64 || r->height != 64)
        return 0;
    return r->window.half_pixel_center == 1 && r->window.bottom_edge_rule == 1 &&
           r->window.sample_mask == 0xffffffffu &&
           r->window.min_samples == (convention == WINDOW_WIN7_GL_SINGLE_SAMPLE ? 0u : 1u) &&
           !memcmp(r->window.viewport_scale_f32, scale, sizeof(scale)) &&
           !memcmp(r->window.viewport_translate_f32, translate, sizeof(translate));
}

static int span(const void *p, unsigned long n)
{
    return p && n <= ~(unsigned long)0 - (unsigned long)p;
}
static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* One little-endian word: a single store (the byte loop was one store per
 * byte). Every target, the i686 kernel and userspace, is little-endian. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian words");
static void put(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}
static uint32_t page_bytes(uint32_t bytes)
{
    return (bytes + 4095u) & ~4095u;
}
static uint32_t uint_f32(uint32_t value)
{
    uint32_t exponent = 0, leading = value;
    if (!value)
        return 0;
    while (leading >>= 1)
        exponent++;
    return ((exponent + 127u) << 23) | ((value << (23u - exponent)) & 0x7fffffu);
}
static int finite(uint32_t v)
{
    return (v & 0x7f800000u) != 0x7f800000u;
}
static int nonnegative_bound(uint32_t v, uint32_t max, int signed_zero)
{
    uint32_t magnitude = v & 0x7fffffffu;
    return magnitude <= max && (!(v >> 31) || (signed_zero && !magnitude));
}
static bool clip_space(const struct sgx535_window_mesh_request *r)
{
    return r->version == SGX535_WINDOW_MESH_CLIP_VERSION;
}
/* A known request version; only clip-space requests cull. */
static bool request_version(const struct sgx535_window_mesh_request *r)
{
    return clip_space(r) ? r->cull <= SGX535_WINDOW_MESH_CULL_CCW
                         : r->version == SGX535_WINDOW_MESH_VERSION && !r->cull;
}
/* Window space: x and y in the target, z in 0..1 and a positive finite 1/W,
 * exactly 1 without inputs. Clip space: any four finite words. */
static bool position(const struct sgx535_window_mesh_request *r, const uint32_t p[4], bool no_input)
{
    if (clip_space(r))
        return finite(p[0]) && finite(p[1]) && finite(p[2]) && finite(p[3]);
    return nonnegative_bound(p[0], uint_f32(r->width), 1) &&
           nonnegative_bound(p[1], uint_f32(r->height), 1) &&
           nonnegative_bound(p[2], 0x3f800000u, 1) && p[3] && p[3] < 0x7f800000u &&
           (!no_input || p[3] == 0x3f800000u);
}

enum sgx535_task_status
sgx535_window_mesh_no_input_source(const struct sgx535_window_mesh_request *r)
{
    uint32_t base, extent = 0;
    if (!span(r, sizeof(*r)))
        return SGX535_TASK_INVALID;
    if (!request_version(r) || r->profile != SGX535_WINDOW_MESH_PROFILE ||
        r->core_id != SGX535_ENGINE121_CORE_ID || r->revision != SGX535_ENGINE121_REVISION ||
        r->interpolation != SGX535_WINDOW_MESH_NO_INPUT ||
        r->order != SGX535_WINDOW_MESH_DEINDEXED || r->vertex_count < 3 ||
        r->vertex_count > SGX535_WINDOW_MESH_MAX_VERTICES || r->vertex_count % 3u ||
        r->index_count != r->vertex_count || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS - SGX535_NORMALIZED_EXTRA_INSTRUCTIONS ||
        !r->width || r->width > 2047 || !r->height || r->height > 2047 || r->reserved[0] ||
        r->reserved[1] || r->reserved[2] ||
        r->uniform_word_count >= SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!r->uniform_word_count != !!r->uniforms))
        return SGX535_TASK_UNSUPPORTED;
    if (!span(r->vertices, (unsigned long)r->vertex_count * sizeof(*r->vertices)) ||
        !span(r->indices, (unsigned long)r->index_count * sizeof(*r->indices)) ||
        !span(r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment)) ||
        (r->uniform_word_count && !span(r->uniforms, (unsigned long)r->uniform_word_count * 4u)))
        return SGX535_TASK_INVALID;
    base = r->fragment_count - 4u;
    /* This finite static-CB0/MAD slice is checked before any source acquisition.
     * Every node, including dead ones, belongs to the empty physical interface. */
    for (uint32_t i = 0; i < r->fragment_count; i++) {
        const struct sgx535_fragment_instruction *in = &r->fragment[i];
        if (in->component ||
            (in->opcode != SGX535_FRAGMENT_MAD && (in->source[1] || in->source[2])))
            return SGX535_TASK_UNSUPPORTED;
        if (i >= base) {
            if (in->opcode != SGX535_FRAGMENT_OUTPUT || in->index != i - base || in->immediate ||
                in->source[0] >= base)
                return SGX535_TASK_UNSUPPORTED;
        } else if (in->opcode == SGX535_FRAGMENT_CONSTANT) {
            if (in->index || in->source[0] || !finite(in->immediate))
                return SGX535_TASK_UNSUPPORTED;
        } else if (in->opcode == SGX535_FRAGMENT_MOV) {
            if (in->immediate || in->index || in->source[0] >= i)
                return SGX535_TASK_UNSUPPORTED;
        } else if (in->opcode == SGX535_FRAGMENT_UNIFORM) {
            if (in->immediate || in->source[0] || in->index >= r->uniform_word_count)
                return SGX535_TASK_UNSUPPORTED;
            if (extent <= in->index)
                extent = in->index + 1u;
        } else if (in->opcode == SGX535_FRAGMENT_MAD) {
            if (in->immediate || in->index || in->source[0] >= i || in->source[1] >= i ||
                in->source[2] >= i)
                return SGX535_TASK_UNSUPPORTED;
        } else
            return SGX535_TASK_UNSUPPORTED;
    }
    if (extent != r->uniform_word_count)
        return SGX535_TASK_UNSUPPORTED;
    for (uint32_t i = 0; i < extent; i++)
        if (!finite(r->uniforms[i]))
            return SGX535_TASK_UNSUPPORTED;
    for (uint32_t i = 0; i < r->vertex_count; i++) {
        const struct sgx535_window_mesh_vertex *v = &r->vertices[i];
        if (!position(r, v->window_xyz_reciprocal_w, true) || r->indices[i] != i)
            return SGX535_TASK_UNSUPPORTED;
        for (uint32_t c = 0; c < 4; c++)
            if (v->rgba[c])
                return SGX535_TASK_UNSUPPORTED;
    }
    return SGX535_TASK_BLOCKED;
}
static enum sgx535_task_status window_uv2_arguments(const struct sgx535_window_mesh_uv2_source *r,
                                                    const void *layout, const uint8_t *geometry,
                                                    uint32_t bytes)
{
    struct {
        const void *p;
        unsigned long bytes;
    } ranges[5];
    uint32_t i, j;
    if (!span(r, sizeof(*r)))
        return SGX535_TASK_INVALID;
    if (!r->width || r->width > 4096 || !r->height || r->height > 4096 || r->vertex_count < 3 ||
        r->vertex_count > SGX535_WINDOW_MESH_MAX_VERTICES || r->vertex_count % 3u ||
        r->logical_slot >= SGX535_FRAGMENT_INPUT_SLOTS)
        return SGX535_TASK_UNSUPPORTED;
    if (bytes < r->vertex_count * SGX535_WINDOW_MESH_UV2_STRIDE_BYTES)
        return SGX535_TASK_INVALID;
    ranges[0].p = r;
    ranges[0].bytes = sizeof(*r);
    ranges[1].p = r->clip_varyings;
    ranges[1].bytes = (unsigned long)r->vertex_count * 24u;
    ranges[2].p = r->window_position;
    ranges[2].bytes = (unsigned long)r->vertex_count * 16u;
    ranges[3].p = layout;
    ranges[3].bytes = sizeof(struct sgx535_window_mesh_uv2_layout);
    ranges[4].p = geometry;
    ranges[4].bytes = bytes;
    for (i = 0; i < 5; i++) {
        if (!span(ranges[i].p, ranges[i].bytes))
            return SGX535_TASK_INVALID;
        for (j = 0; j < i; j++)
            if (overlap(ranges[i].p, ranges[i].bytes, ranges[j].p, ranges[j].bytes))
                return SGX535_TASK_INVALID;
    }
    for (i = 0; i < r->vertex_count; i++) {
        const uint32_t *clip = r->clip_varyings + i * 6u;
        const uint32_t *window = r->window_position + i * 4u;
        for (j = 0; j < 6; j++)
            if (!finite(clip[j]))
                return SGX535_TASK_UNSUPPORTED;
        if (!clip[3] || clip[3] >= 0x7f800000u ||
            !nonnegative_bound(window[0], uint_f32(r->width), 1) ||
            !nonnegative_bound(window[1], uint_f32(r->height), 1) ||
            !nonnegative_bound(window[2], 0x3f800000u, 1) || !window[3] || window[3] >= 0x7f800000u)
            return SGX535_TASK_UNSUPPORTED;
    }
    return SGX535_TASK_BLOCKED;
}

static enum sgx535_task_status window_uv2_layout(const struct sgx535_window_mesh_uv2_source *r,
                                                 struct sgx535_window_mesh_uv2_layout *out)
{
    struct sgx535_window_mesh_uv2_layout p = {0};
    p.logical_slot = r->logical_slot;
    p.physical_set = 0;
    p.vertex_count = r->vertex_count;
    p.geometry_bytes = r->vertex_count * SGX535_WINDOW_MESH_UV2_STRIDE_BYTES;
    p.stride_bytes = SGX535_WINDOW_MESH_UV2_STRIDE_BYTES;
    p.output_dwords = SGX535_WINDOW_MESH_UV2_WORDS;
    /* Four position words plus UV2 share one output-state construction.
     * OUTSELECTS/TEXSIZE/TEXFLOAT describe this exact six-word vertex record,
     * independently of eleven state words or 96 state PRIMARY bytes. */
    p.outselects = p.output_dwords << 24 | 0x1000u;
    p.texsize = 1;
    p.texfloat = 0;
    if (sgx535_vertex_dma_build(p.output_dwords, &p.dma) != SGX535_VERTEX_DMA_OK ||
        sgx535_vertex_use_project(p.output_dwords, &p.use) != SGX535_VERTEX_USE_OK)
        return SGX535_TASK_UNSUPPORTED;
    *out = p;
    return SGX535_TASK_BLOCKED;
}

enum sgx535_task_status sgx535_window_mesh_uv2_build(const struct sgx535_window_mesh_uv2_source *r,
                                                     struct sgx535_window_mesh_uv2_layout *out,
                                                     uint8_t *geometry, uint32_t capacity)
{
    struct sgx535_window_mesh_uv2_layout p;
    enum sgx535_task_status status = window_uv2_arguments(r, out, geometry, capacity);
    uint32_t i, j;
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = window_uv2_layout(r, &p);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    for (i = 0; i < r->vertex_count; i++) {
        for (j = 0; j < 4; j++)
            put(geometry + i * 24u + j * 4u, r->window_position[i * 4u + j]);
        for (j = 0; j < 2; j++)
            put(geometry + i * 24u + 16u + j * 4u, r->clip_varyings[i * 6u + 4u + j]);
    }
    *out = p;
    return SGX535_TASK_BLOCKED;
}

enum sgx535_task_status
sgx535_window_mesh_uv2_validate(const struct sgx535_window_mesh_uv2_source *r,
                                const struct sgx535_window_mesh_uv2_layout *layout,
                                const uint8_t *geometry, uint32_t bytes)
{
    struct sgx535_window_mesh_uv2_layout p;
    enum sgx535_task_status status = window_uv2_arguments(r, layout, geometry, bytes);
    uint32_t i, j;
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = window_uv2_layout(r, &p);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (bytes != p.geometry_bytes || memcmp(layout, &p, sizeof(p)))
        return SGX535_TASK_INVALID;
    for (i = 0; i < r->vertex_count; i++) {
        for (j = 0; j < 4; j++)
            if (word(geometry + i * 24u + j * 4u) != r->window_position[i * 4u + j])
                return SGX535_TASK_INVALID;
        for (j = 0; j < 2; j++)
            if (word(geometry + i * 24u + 16u + j * 4u) != r->clip_varyings[i * 6u + 4u + j])
                return SGX535_TASK_INVALID;
    }
    return SGX535_TASK_BLOCKED;
}

static enum sgx535_task_status storage_plan(uint32_t vertices, uint32_t indices, uint32_t words,
                                            struct sgx535_window_mesh_storage *out)
{
    struct sgx535_window_mesh_storage p = {0};
    if (!out)
        return SGX535_TASK_INVALID;
    if (vertices < 3 || vertices > SGX535_WINDOW_MESH_MAX_VERTICES || indices < 3 ||
        indices > SGX535_WINDOW_MESH_MAX_INDICES || indices % 3u)
        return SGX535_TASK_UNSUPPORTED;
    p.foreground_vertex_bytes = vertices * words * 4u;
    p.mask_vertex_offset_bytes = p.foreground_vertex_bytes;
    p.vertex_bytes = p.foreground_vertex_bytes + 64u;
    p.index_offset_bytes = p.vertex_bytes;
    p.foreground_index_offset_bytes = p.index_offset_bytes + 12u;
    p.foreground_index_bytes = indices * 2u;
    p.index_bytes = (12u + p.foreground_index_bytes + 3u) & ~3u;
    p.bytes = p.index_offset_bytes + p.index_bytes;
    p.allocation_bytes = page_bytes(p.bytes);
    *out = p;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_window_mesh_storage_plan(uint32_t vertices, uint32_t indices,
                                                        struct sgx535_window_mesh_storage *out)
{
    return storage_plan(vertices, indices, 8, out);
}
enum sgx535_task_status
sgx535_window_mesh_input_storage_plan(const struct sgx535_window_mesh_request *r,
                                      struct sgx535_window_mesh_storage *out)
{
    return storage_plan(r->vertex_count, r->index_count,
                        r->interpolation == SGX535_WINDOW_MESH_NO_INPUT ? 4u : 8u, out);
}
struct cpu_span {
    const void *p;
    unsigned long bytes;
};
static int arguments(const struct sgx535_window_mesh_request *r, const void *out,
                     unsigned long out_bytes, const uint8_t *geometry, uint32_t capacity,
                     struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                     uint32_t scratch_capacity)
{
    struct sgx535_window_mesh_storage p;
    struct cpu_span ranges[9];
    uint32_t i, j, count = 8;
    if (!span(r, sizeof(*r)) || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
        r->uniform_word_count >= SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!r->uniform_word_count != !!r->uniforms) ||
        sgx535_window_mesh_input_storage_plan(r, &p) != SGX535_TASK_BLOCKED || capacity < p.bytes ||
        scratch_capacity < p.bytes)
        return 0;
    ranges[0] = (struct cpu_span){r, sizeof(*r)};
    ranges[1] =
        (struct cpu_span){r->vertices, (unsigned long)r->vertex_count * sizeof(*r->vertices)};
    ranges[2] = (struct cpu_span){r->indices, (unsigned long)r->index_count * sizeof(*r->indices)};
    ranges[3] =
        (struct cpu_span){r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment)};
    ranges[4] = (struct cpu_span){out, out_bytes};
    ranges[5] = (struct cpu_span){geometry, capacity};
    ranges[6] = (struct cpu_span){w, sizeof(*w)};
    ranges[7] = (struct cpu_span){scratch, scratch_capacity};
    if (r->uniform_word_count)
        ranges[count++] = (struct cpu_span){r->uniforms, (unsigned long)r->uniform_word_count * 4u};
    for (i = 0; i < count; i++) {
        if (!span(ranges[i].p, ranges[i].bytes))
            return 0;
        for (j = 0; j < i; j++)
            if (overlap(ranges[i].p, ranges[i].bytes, ranges[j].p, ranges[j].bytes))
                return 0;
    }
    return 1;
}
static int extra(const void *p, unsigned long bytes, const struct sgx535_window_mesh_request *r,
                 const void *out, unsigned long out_bytes, const uint8_t *geometry,
                 uint32_t geometry_bytes, struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                 uint32_t scratch_capacity)
{
    return span(p, bytes) && !overlap(p, bytes, r, sizeof(*r)) &&
           !overlap(p, bytes, r->vertices, (unsigned long)r->vertex_count * sizeof(*r->vertices)) &&
           !overlap(p, bytes, r->indices, (unsigned long)r->index_count * sizeof(*r->indices)) &&
           !overlap(p, bytes, r->fragment,
                    (unsigned long)r->fragment_count * sizeof(*r->fragment)) &&
           (!r->uniform_word_count ||
            !overlap(p, bytes, r->uniforms, (unsigned long)r->uniform_word_count * 4u)) &&
           !overlap(p, bytes, out, out_bytes) && !overlap(p, bytes, geometry, geometry_bytes) &&
           !overlap(p, bytes, w, sizeof(*w)) && !overlap(p, bytes, scratch, scratch_capacity);
}
static int same_request(const struct sgx535_window_mesh_request *a,
                        const struct sgx535_window_mesh_request *b)
{
    return a->version == b->version && a->profile == b->profile && a->core_id == b->core_id &&
           a->revision == b->revision && a->width == b->width && a->height == b->height &&
           a->order == b->order && a->interpolation == b->interpolation &&
           !memcmp(&a->window, &b->window, sizeof(a->window)) && a->vertices == b->vertices &&
           a->indices == b->indices && a->vertex_count == b->vertex_count &&
           a->index_count == b->index_count && a->fragment == b->fragment &&
           a->fragment_count == b->fragment_count && a->uniforms == b->uniforms &&
           a->uniform_word_count == b->uniform_word_count && a->cull == b->cull &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved));
}
static int copied_inputs(const struct sgx535_window_mesh_request *r, const uint8_t *geometry,
                         const struct sgx535_window_mesh_storage *p,
                         const struct sgx535_fragment_instruction *ir)
{
    uint32_t i, j, stride = p->foreground_vertex_bytes / r->vertex_count;
    for (i = 0; i < r->vertex_count; i++) {
        for (j = 0; j < 4; j++)
            if (word(geometry + i * stride + j * 4u) != r->vertices[i].window_xyz_reciprocal_w[j] ||
                (stride == 32u
                     ? word(geometry + i * stride + 16u + j * 4u) != r->vertices[i].rgba[j]
                     : r->vertices[i].rgba[j] != 0))
                return 0;
    }
    for (i = 0; i < r->index_count; i++) {
        const uint8_t *v = geometry + p->foreground_index_offset_bytes + i * 2u;
        if (((uint32_t)v[0] | (uint32_t)v[1] << 8) != r->indices[i])
            return 0;
    }
    return !memcmp(r->fragment, ir, (unsigned long)r->fragment_count * sizeof(*ir));
}
static enum sgx535_task_status geometry_build(const struct sgx535_window_mesh_request *r,
                                              struct sgx535_window_mesh_storage *p,
                                              uint8_t *geometry)
{
    static const uint32_t mask[6] = {0, 1, 2, 3, 2, 1};
    uint32_t i, j, width = uint_f32(r->width), height = uint_f32(r->height);
    uint32_t stride = p->foreground_vertex_bytes / r->vertex_count;
    const int no_input = r->interpolation == SGX535_WINDOW_MESH_NO_INPUT;
    if (r->order != SGX535_WINDOW_MESH_INDEXED && r->order != SGX535_WINDOW_MESH_DEINDEXED)
        return SGX535_TASK_UNSUPPORTED;
    if (r->order == SGX535_WINDOW_MESH_DEINDEXED &&
        (r->vertex_count != r->index_count || r->vertex_count % 3u))
        return SGX535_TASK_UNSUPPORTED;
    memset(geometry, 0, p->bytes);
    for (i = 0; i < r->vertex_count; i++) {
        const struct sgx535_window_mesh_vertex *v = &r->vertices[i];
        if (!position(r, v->window_xyz_reciprocal_w, no_input))
            return SGX535_TASK_UNSUPPORTED;
        for (j = 0; j < 4; j++) {
            if (no_input ? v->rgba[j] != 0 : v->rgba[j] > 0x3f800000u)
                return SGX535_TASK_UNSUPPORTED;
            put(geometry + i * stride + j * 4u, v->window_xyz_reciprocal_w[j]);
            if (!no_input)
                put(geometry + i * stride + 16u + j * 4u, v->rgba[j]);
        }
    }
    for (i = 0; i < 4; i++) {
        uint8_t *v = geometry + p->mask_vertex_offset_bytes + i * 16u;
        put(v, (i & 1u) ? width : 0);
        put(v + 4, (i & 2u) ? height : 0);
        put(v + 8, 0x3f800000u);
        put(v + 12, 0x3f800000u);
    }
    for (i = 0; i < 6u + r->index_count; i++) {
        uint32_t index = i < 6 ? mask[i] : r->indices[i - 6u];
        uint8_t *v = geometry + p->index_offset_bytes + i * 2u;
        if (i >= 6 && (index >= r->vertex_count ||
                       (r->order == SGX535_WINDOW_MESH_DEINDEXED && index != i - 6u)))
            return SGX535_TASK_UNSUPPORTED;
        v[0] = (uint8_t)index;
        v[1] = (uint8_t)(index >> 8);
    }
    if (r->interpolation == SGX535_VARYING_FLAT_REPLICATED) {
        for (i = 0; i < r->index_count; i += 3)
            for (j = 1; j < 3; j++)
                if (memcmp(r->vertices[r->indices[i]].rgba, r->vertices[r->indices[i + j]].rgba,
                           16))
                    return SGX535_TASK_UNSUPPORTED;
    } else if (!no_input && r->interpolation != SGX535_VARYING_SMOOTH)
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
static uint8_t *section_bytes(struct sgx535_window_mesh_task *p, uint32_t source, uint32_t offset,
                              uint32_t bytes)
{
    uint8_t *base;
    uint32_t capacity;
    if (source == SGX535_WINDOW_MESH_PDS) {
        base = p->pds;
        capacity = sizeof(p->pds);
    } else if (source == SGX535_WINDOW_MESH_VERTEX_USE) {
        base = p->vertex_use;
        capacity = sizeof(p->vertex_use);
    } else if (source == SGX535_WINDOW_MESH_FRAGMENT_USE) {
        base = p->fragment_use;
        capacity = sizeof(p->fragment_use);
    } else if (source == SGX535_WINDOW_MESH_SECONDARY_UPLOAD) {
        base = p->secondary_upload;
        capacity = sizeof(p->secondary_upload);
    } else
        return 0;
    return offset <= capacity && bytes <= capacity - offset ? base + offset : 0;
}
/* This composition always compiles one COLOR0 varying with the same linkage.
 * Keep its complete zero-tailed input in read-only storage, off the call stack. */
static const struct sgx535_fragment_linkage window_fragment_linkage = {
    .count = 1, .varying = {{SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH}}};
enum sgx535_task_status
sgx535_window_mesh_fragment_layout(uint32_t words, struct sgx535_window_mesh_fragment_layout *out)
{
    struct sgx535_window_mesh_fragment_layout p;
    if (!out)
        return SGX535_TASK_INVALID;
    if (!words || words > SGX535_NIR_MAX_WORDS || (words & 1u))
        return SGX535_TASK_UNSUPPORTED;
    p.code_bytes = words * 4u;
    p.event_offset_bytes = (p.code_bytes + 31u) & ~31u;
    p.foreground_extent_bytes = p.event_offset_bytes + 32u;
    p.background_offset_bytes = p.foreground_extent_bytes < SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES
                                    ? SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES
                                    : p.foreground_extent_bytes;
    p.clear_offset_bytes = p.background_offset_bytes + 32u;
    *out = p;
    return SGX535_TASK_BLOCKED;
}
static int window_secondary_relocation(struct sgx535_window_mesh_task *p, uint32_t kind,
                                       uint32_t section, uint32_t offset, uint32_t role,
                                       uint32_t target, uint32_t bytes, uint32_t alignment,
                                       uint32_t mask, uint32_t right, uint32_t left)
{
    if (p->relocation_count >= SGX535_WINDOW_MESH_RELOCS || section >= p->section_count ||
        offset > p->sections[section].byte_count || 4u > p->sections[section].byte_count - offset ||
        target > p->requirements[role].minimum_bytes ||
        bytes > p->requirements[role].minimum_bytes - target)
        return 0;
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        kind, section, offset, role, target, bytes, alignment, mask, right, left};
    return 1;
}
static enum sgx535_task_status window_secondary_compose(struct sgx535_window_mesh_workspace *w)
{
    struct sgx535_window_mesh_task *p = &w->result.foreground;
    struct sgx535_window_mesh_secondary_layout *s = &p->secondary;
    const struct sgx535_fragment_generic_f32_secondary_candidate *program = &w->secondary;
    struct sgx535_ordinary_secondary_dms dms;
    struct sgx535_vertex_dma_plan dma;
    uint32_t secondary = SGX535_TASK_PACKET_SECTIONS, mte = secondary, event = secondary, i;
    uint32_t pds_end = 0;
    memcpy(s->logical_uniform_words_used, program->logical_uniform_words_used,
           sizeof(s->logical_uniform_words_used));
    s->logical_uniform_extent = program->logical_uniform_extent;
    s->physical_sa_words = program->physical_sa_words;
    s->reserved_sa_words = program->reserved_sa_words;
    s->uniform_sa_words = program->uniform_sa_words;
    s->literal_sa_words = program->literal_sa_words;
    s->sampler_state_sa_words = program->sampler_state_sa_words;
    if (s->logical_uniform_extent > w->uniform_word_count ||
        s->reserved_sa_words != !!s->physical_sa_words ||
        s->uniform_sa_words != s->logical_uniform_extent || s->literal_sa_words ||
        s->sampler_state_sa_words ||
        s->physical_sa_words != s->reserved_sa_words + s->uniform_sa_words + s->literal_sa_words +
                                    s->sampler_state_sa_words)
        return SGX535_TASK_UNSUPPORTED;
    if (!s->physical_sa_words)
        return SGX535_TASK_BLOCKED;
    if (!sgx535_ordinary_secondary_plan(s->physical_sa_words, &dma) ||
        !sgx535_ordinary_secondary_dms(p->fragment.primary_register_count, p->fragment_temp_words,
                                       s->physical_sa_words, &dms))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_window_mesh_section *section = &p->sections[i];
        if (section->role == SGX535_TASK_PDS) {
            if (section->resource_byte_offset > p->requirements[SGX535_TASK_PDS].minimum_bytes ||
                section->byte_count >
                    p->requirements[SGX535_TASK_PDS].minimum_bytes - section->resource_byte_offset)
                return SGX535_TASK_UNSUPPORTED;
            if (pds_end < section->resource_byte_offset + section->byte_count)
                pds_end = section->resource_byte_offset + section->byte_count;
        }
    }
    for (i = 0; i < w->result.background.section_count; i++) {
        const struct sgx535_background_section *section = &w->result.background.sections[i];
        if (section->role == SGX535_BACKGROUND_PDS) {
            if (section->resource_offset > p->requirements[SGX535_TASK_PDS].minimum_bytes ||
                section->byte_count >
                    p->requirements[SGX535_TASK_PDS].minimum_bytes - section->resource_offset)
                return SGX535_TASK_UNSUPPORTED;
            if (pds_end < section->resource_offset + section->byte_count)
                pds_end = section->resource_offset + section->byte_count;
        }
    }
    if (pds_end > UINT32_MAX - 31u)
        return SGX535_TASK_UNSUPPORTED;
    s->secondary_resource_offset_bytes = (pds_end + 31u) & ~31u;
    s->secondary_program_bytes = (16u + dma.transfer_count + 2u) * 4u;
    s->upload_resource_offset_bytes =
        s->secondary_resource_offset_bytes + SGX535_ORDINARY_SECONDARY_CAPACITY;
    s->upload_bytes = s->physical_sa_words * 4u;
    s->empty_use_offset_bytes = p->fragment_use_extent_bytes;
    s->dms_sa_budget_words = dms.sa_budget_words;
    if (s->secondary_resource_offset_bytes > p->requirements[SGX535_TASK_PDS].minimum_bytes ||
        SGX535_ORDINARY_SECONDARY_CAPACITY + s->upload_bytes >
            p->requirements[SGX535_TASK_PDS].minimum_bytes - s->secondary_resource_offset_bytes ||
        s->empty_use_offset_bytes > sizeof(p->fragment_use) ||
        8u > sizeof(p->fragment_use) - s->empty_use_offset_bytes ||
        !sgx535_ordinary_secondary_emit(s->physical_sa_words, w->uniforms, p->secondary_upload,
                                        sizeof(p->secondary_upload)))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < p->section_count; i++) {
        if (p->sections[i].kind == SGX535_PACKET_SECONDARY_PDS)
            secondary = i;
        else if (p->sections[i].kind == SGX535_PACKET_MTE_STATE)
            mte = i;
        else if (p->sections[i].kind == SGX535_PACKET_EVENT_USE)
            event = i;
    }
    if (secondary == SGX535_TASK_PACKET_SECTIONS || mte == SGX535_TASK_PACKET_SECTIONS ||
        event == SGX535_TASK_PACKET_SECTIONS || p->sections[secondary].byte_count != 4u ||
        p->sections[mte].byte_count != (p->version == SGX535_WINDOW_MESH_CLIP_VERSION
                                            ? SGX535_WINDOW_MESH_CLIP_MTE_BYTES
                                            : 92u) ||
        p->sections[event].byte_count != 32u)
        return SGX535_TASK_UNSUPPORTED;
    p->sections[secondary].source = SGX535_WINDOW_MESH_SECONDARY_UPLOAD;
    p->sections[secondary].byte_offset = 0;
    p->sections[secondary].byte_count = SGX535_ORDINARY_SECONDARY_CAPACITY + s->upload_bytes;
    p->sections[secondary].resource_byte_offset = s->secondary_resource_offset_bytes;
    p->sections[event].byte_count += 8u;
    put(p->fragment_use + s->empty_use_offset_bytes, 0);
    put(p->fragment_use + s->empty_use_offset_bytes + 4u, 0xf8040140u);
    p->fragment_use_extent_bytes += 8u;
    p->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes =
        page_bytes(p->fragment_use_extent_bytes);
    put(p->pds + p->sections[mte].byte_offset + 8u, 0x10000000u);
    put(p->pds + p->sections[mte].byte_offset + 12u, dms.word);
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *r = &p->relocations[i];
        if (r->section == mte && r->section_byte_offset == 8u && r->role == SGX535_TASK_PDS) {
            r->resource_byte_offset = s->secondary_resource_offset_bytes;
            r->span_bytes = s->secondary_program_bytes;
        }
    }
    for (i = 0; i < dma.transfer_count; i++)
        if (!window_secondary_relocation(
                p, SGX535_PACKET_DRI_OFFSET, secondary, i ? 32u : 0u, SGX535_TASK_PDS,
                s->upload_resource_offset_bytes + dma.transfers[i].source_byte_offset,
                dma.transfers[i].word_count * 4u, 4, 0xffffffffu, 0, 0))
            return SGX535_TASK_UNSUPPORTED;
    if (!window_secondary_relocation(p, SGX535_PACKET_USE_BANK, secondary, 8,
                                     SGX535_TASK_USE_FRAGMENT, s->empty_use_offset_bytes, 8, 32,
                                     0xf, 0, 0) ||
        !window_secondary_relocation(p, SGX535_PACKET_USE_ENTRY, secondary, 8,
                                     SGX535_TASK_USE_FRAGMENT, s->empty_use_offset_bytes, 8, 32,
                                     0xf0, 15, 4) ||
        !window_secondary_relocation(p, SGX535_PACKET_USE_ENTRY, secondary, 8,
                                     SGX535_TASK_USE_FRAGMENT, s->empty_use_offset_bytes, 8, 32,
                                     0x7ff00, 4, 8))
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
/* The primary-output program depends on the IR and the compile variant
 * alone, and every draw group of every frame compiles it (0.1 ms each on the
 * Z520; a frame's groups mostly share one program): the last one is kept.
 * OUT is zeroed by the caller, so a kept result equals a fresh compile; debug
 * builds compile anyway and confirm it. */
#ifdef __KERNEL__
#include <linux/mutex.h>
static DEFINE_MUTEX(fragment_kept_lock);
#define fragment_kept_take() mutex_lock(&fragment_kept_lock)
#define fragment_kept_drop() mutex_unlock(&fragment_kept_lock)
#else
#include <pthread.h>
static pthread_mutex_t fragment_kept_lock = PTHREAD_MUTEX_INITIALIZER;
#define fragment_kept_take() pthread_mutex_lock(&fragment_kept_lock)
#define fragment_kept_drop() pthread_mutex_unlock(&fragment_kept_lock)
#endif
static struct {
    bool kept, raw_mask;
    enum sgx535_window_fragment_io io;
    uint32_t count;
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_normalized_candidate program;
} fragment_kept;
static bool fragment_kept_same(const struct sgx535_fragment_instruction *in, uint32_t count,
                               enum sgx535_window_fragment_io io, bool raw_mask)
{
    return fragment_kept.kept && fragment_kept.io == io && fragment_kept.raw_mask == raw_mask &&
           fragment_kept.count == count && !memcmp(fragment_kept.fragment, in, count * sizeof(*in));
}
static enum sgx535_nir_status
window_fragment_compile(const struct sgx535_fragment_instruction *in, uint32_t count,
                        enum sgx535_window_fragment_io io, bool raw_mask,
                        struct sgx535_fragment_normalized_candidate *out)
{
    enum sgx535_nir_status status;
    bool kept;
    if (count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    fragment_kept_take();
    kept = fragment_kept_same(in, count, io, raw_mask);
    if (kept && !POULSBACK_SELFCHECK)
        *out = fragment_kept.program;
    fragment_kept_drop();
    if (kept && !POULSBACK_SELFCHECK)
        return SGX535_NIR_OK;
    status = io == SGX535_WINDOW_GENERIC0_F32
                 ? sgx535_fragment_compile_generic_f32_normalized_candidate(
                       in, count, &window_fragment_linkage, out)
             : raw_mask ? sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(
                              in, count, &window_fragment_linkage, out)
                        : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                              in, count, &window_fragment_linkage, out);
    if (status != SGX535_NIR_OK)
        return status;
    fragment_kept_take();
    if (!fragment_kept_same(in, count, io, raw_mask)) {
        memcpy(fragment_kept.fragment, in, count * sizeof(*in));
        fragment_kept.count = count;
        fragment_kept.io = io;
        fragment_kept.raw_mask = raw_mask;
        fragment_kept.program = *out;
        fragment_kept.kept = true;
    } else if (memcmp(&fragment_kept.program, out, sizeof(*out)))
        status = SGX535_NIR_ERR_UNSUPPORTED;
    fragment_kept_drop();
    return status;
}
/* A clip-space image keeps the 768-byte PDS image: the PBE state's and the TA
 * stream's bytes follow the grown MTE state (576..684) packed, while their
 * placements move past it and past the background's PDS programs (768..836).
 * The depth composition places its own stream. */
enum clip_pds { CLIP_PBE_BYTES = 684, CLIP_PBE = 688, CLIP_TA_BYTES = 700, CLIP_TA = 848 };
static bool clip_placement(struct sgx535_window_mesh_section *d)
{
    uint32_t window = d->byte_offset;
    if (d->kind == SGX535_PACKET_MTE_STATE) {
        d->byte_count = SGX535_WINDOW_MESH_CLIP_MTE_BYTES;
        return window == 576;
    }
    if (d->kind == SGX535_PACKET_PBE_STATE) {
        d->byte_offset = CLIP_PBE_BYTES;
        d->resource_byte_offset = CLIP_PBE;
        return window == 672 && d->byte_count == CLIP_TA_BYTES - CLIP_PBE_BYTES;
    }
    if (d->kind == SGX535_PACKET_TA_STREAM) {
        d->byte_offset = CLIP_TA_BYTES;
        d->resource_byte_offset = CLIP_TA;
        return window == 688 && d->byte_count == SGX535_WINDOW_MESH_PDS_BYTES - CLIP_TA_BYTES;
    }
    return true;
}
/* The clip-space foreground MTE record (sgx535_window_mesh_internal.h) from
 * the window-space MTE state WINDOW: viewport words translate then scale per
 * axis, EMGD's W clamp (1e-7) and MTE control, then the mask's record. */
static bool clip_mte(uint8_t *dst, const uint8_t *window,
                     const struct sgx535_window_mesh_request *r, bool generic)
{
    const uint32_t shade = r->interpolation == SGX535_VARYING_FLAT_REPLICATED ? 3u : 0u;
    if (word(window) != 0x54c1u || word(window + 32) != 0x10000u || word(window + 48) != 0x54c5u)
        return false;
    memset(dst, 0, SGX535_WINDOW_MESH_CLIP_MTE_BYTES);
    put(dst, generic ? SGX535_WINDOW_MESH_CLIP_F32_MTE_HEADER : SGX535_WINDOW_MESH_CLIP_MTE_HEADER);
    memcpy(dst + 4, window + 4, 16); /* ISP A, pixel PDS */
    for (uint32_t i = 0; i < 3; i++) {
        put(dst + 20 + i * 8u, r->window.viewport_translate_f32[i]);
        put(dst + 24 + i * 8u, r->window.viewport_scale_f32[i]);
    }
    put(dst + 44, generic ? 0x08001000u : word(window + 28));
    put(dst + 48, 0x33d6bf95u);
    /* DRAWCLIPPEDEDGES, SHADE (flat: vertex 2), WCLAMPEN and CULLMODE;
     * PRETRANSFORM (bit 16) clear. */
    put(dst + 52, 0x00080000u | shade << 17 | 0x8000u | r->cull);
    put(dst + 56, generic ? 7u : word(window + 36));
    /* +60: the generic family's TEXFLOAT 0, else the DMA's sixteenth word. */
    memcpy(dst + SGX535_WINDOW_MESH_CLIP_MASK_MTE, window + 48, 44);
    return true;
}
/* One 16-byte state USE program: COUNT (at most sixteen) attribute words
 * moved to the outputs, then the state emit of COUNT words. */
static bool state_use(uint8_t *dst, uint32_t count)
{
    struct sgx535_vertex_use_projection use;
    if (sgx535_vertex_use_project(count, &use) != SGX535_VERTEX_USE_OK ||
        use.instruction_count != 2)
        return false;
    put(dst, use.words[0]);
    put(dst + 4, use.words[1]);
    put(dst + 8, 0xa0200000u | count << 7);
    put(dst + 12, 0xfb274000u);
    return true;
}
/* The state programs DMA the mask's record from its new place and the
 * foreground's sixteen words; the pixel event reads the moved PBE state. */
static bool clip_relocation(uint32_t kind, struct sgx535_task_packet_reloc *a)
{
    if (kind == SGX535_PACKET_STATE_PDS && !a->section_byte_offset &&
        a->resource_byte_offset == 624 && a->span_bytes == 44)
        a->resource_byte_offset = 576u + SGX535_WINDOW_MESH_CLIP_MASK_MTE;
    else if (kind == SGX535_PACKET_STATE_PDS && a->section_byte_offset == 64 &&
             a->resource_byte_offset == 576 && a->span_bytes == 40)
        a->span_bytes = 64;
    else if (kind == SGX535_PACKET_EVENT_PDS && a->section_byte_offset == 8 &&
             a->resource_byte_offset == 672 && a->span_bytes == 16)
        a->resource_byte_offset = CLIP_PBE;
    else
        return false;
    return true;
}
static enum sgx535_task_status construct(const struct sgx535_window_mesh_request *r,
                                         struct sgx535_window_mesh_workspace *w, uint8_t *geometry,
                                         enum window_convention convention, bool raw_mask,
                                         enum sgx535_window_fragment_io io)
{
    struct sgx535_window_mesh_task *p = &w->result.foreground;
    const struct sgx535_task_packet *seed;
    struct sgx535_pbe_surface surface;
    struct sgx535_pbe_state pbe;
    struct sgx535_window_mesh_fragment_layout fragment_layout;
    enum sgx535_nir_status compiled;
    enum sgx535_task_status status;
    uint32_t i, j, instances, pixels, allocation, changed = 0, moved = 0;
    const bool no_input = r->interpolation == SGX535_WINDOW_MESH_NO_INPUT;
    const bool clip = clip_space(r);
    const uint32_t seed_vertex_bytes = no_input ? 48u : 96u;
    if (r->uniform_word_count >= SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!r->uniform_word_count != !!r->uniforms) ||
        (r->uniform_word_count && !span(r->uniforms, (unsigned long)r->uniform_word_count * 4u)) ||
        (!no_input && (r->uniforms || r->uniform_word_count)) ||
        (no_input && (io != SGX535_WINDOW_PRIMARY_COLOR || raw_mask)) ||
        (io != SGX535_WINDOW_PRIMARY_COLOR && io != SGX535_WINDOW_GENERIC0_F32 &&
         io != SGX535_WINDOW_GENERIC0_F32_SECONDARY) ||
        (io != SGX535_WINDOW_PRIMARY_COLOR &&
         (raw_mask || convention != WINDOW_WIN7_GL_SINGLE_SAMPLE)) ||
        !request_version(r) || (clip && convention != WINDOW_WIN7_GL_SINGLE_SAMPLE) ||
        r->profile !=
            (raw_mask ? SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1 : SGX535_WINDOW_MESH_PROFILE) ||
        r->core_id != SGX535_ENGINE121_CORE_ID || r->revision != SGX535_ENGINE121_REVISION ||
        !r->width || !r->height || r->width >= 2048 || r->height >= 2048 ||
        !source_convention(r, convention) || r->reserved[0] || r->reserved[1] || r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 3; i++)
        if (!finite(r->window.viewport_scale_f32[i]) ||
            !finite(r->window.viewport_translate_f32[i]))
            return SGX535_TASK_UNSUPPORTED;
    memset(&w->result, 0, sizeof(w->result));
    if (sgx535_window_mesh_input_storage_plan(r, &p->geometry) != SGX535_TASK_BLOCKED)
        return SGX535_TASK_UNSUPPORTED;
    status = geometry_build(r, &p->geometry, geometry);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    memcpy(w->fragment, r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment));
    if (no_input) {
        struct sgx535_normalized_pa0_request literal = {
            .policy = SGX535_NORMALIZED_PA0_RESERVED_SA0_V1,
            .core_id = r->core_id,
            .revision = r->revision,
            .clip_position = {{0xbf400000u, 0x3f400000u, 0, 0x3f800000u},
                              {0x3f400000u, 0x3f400000u, 0, 0x3f800000u},
                              {0x3f200000u, 0xbf000000u, 0, 0x3f800000u}},
            .fragment = w->fragment,
            .fragment_count = r->fragment_count,
            .uniforms = r->uniform_word_count ? w->uniforms : NULL,
            .uniform_word_count = r->uniform_word_count,
        };
        memset(w->uniforms, 0, sizeof(w->uniforms));
        w->uniform_word_count = r->uniform_word_count;
        if (r->uniform_word_count)
            memcpy(w->uniforms, r->uniforms, (unsigned long)r->uniform_word_count * 4u);
        status = sgx535_window_mesh_no_input_source(r);
        if (status != SGX535_TASK_BLOCKED)
            return status;
        status = sgx535_normalized_pa0_task_build(&literal, &w->pa0_task, &w->pa0_seed);
        if (status != SGX535_TASK_BLOCKED)
            return status;
        p->fragment = w->pa0_task.task.fragment;
        memcpy(w->secondary.logical_uniform_words_used, w->pa0_task.logical_uniform_words_used,
               sizeof(w->secondary.logical_uniform_words_used));
        w->secondary.logical_uniform_extent = w->pa0_task.logical_uniform_extent;
        w->secondary.physical_sa_words = w->pa0_task.physical_sa_words;
        w->secondary.reserved_sa_words = w->pa0_task.reserved_sa_words;
        w->secondary.uniform_sa_words = w->pa0_task.uniform_sa_words;
        w->secondary.literal_sa_words = w->pa0_task.literal_sa_words;
        w->secondary.sampler_state_sa_words = w->pa0_task.sampler_state_sa_words;
        status = sgx535_normalized_no_input_mask_mesh_template(&w->pa0_seed.common.seed,
                                                               &w->seed.seed, &w->seed);
    } else
        status = sgx535_normalized_color0_mask_mesh_template(r->core_id, r->revision, &w->seed.seed,
                                                             &w->seed);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    seed = &w->seed.seed;
    compiled =
        no_input ? SGX535_NIR_OK
        : io == SGX535_WINDOW_GENERIC0_F32_SECONDARY
            ? sgx535_fragment_compile_generic_f32_secondary_candidate(
                  w->fragment, r->fragment_count, &window_fragment_linkage, &w->secondary)
            : window_fragment_compile(w->fragment, r->fragment_count, io, raw_mask, &p->fragment);
    if (compiled != SGX535_NIR_OK)
        return compiled == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    if (io == SGX535_WINDOW_GENERIC0_F32_SECONDARY)
        p->fragment = w->secondary.program;
    if (p->fragment.primary_register_count != (no_input ? 0u : 4u) ||
        (!no_input && io != SGX535_WINDOW_GENERIC0_F32_SECONDARY &&
         p->fragment.secondary_register_count) ||
        !p->fragment.encoding.temp_register_count ||
        p->fragment.encoding.temp_register_count > SGX535_NIR_TEMP_COUNT ||
        !p->fragment.encoding.word_count ||
        p->fragment.encoding.word_count > SGX535_NIR_MAX_WORDS ||
        (p->fragment.encoding.word_count & 1u))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 4; i++)
        if (!no_input && io != SGX535_WINDOW_GENERIC0_F32_SECONDARY &&
            p->fragment.uniform_words_used[i])
            return SGX535_TASK_UNSUPPORTED;
    if (sgx535_window_mesh_fragment_layout(p->fragment.encoding.word_count, &fragment_layout) !=
        SGX535_TASK_BLOCKED)
        return SGX535_TASK_UNSUPPORTED;
    p->fragment_code_bytes = fragment_layout.code_bytes;
    p->fragment_temp_words = p->fragment.encoding.temp_register_count;
    allocation = (p->fragment_temp_words * 4u + 15u) & ~15u;
    p->dms_temp_allocation_bytes = allocation;
    p->event_use_offset_bytes = fragment_layout.event_offset_bytes;
    p->fragment_use_extent_bytes = fragment_layout.foreground_extent_bytes;
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
    p->version = r->version;
    p->profile = r->profile;
    p->core_id = r->core_id;
    p->revision = r->revision;
    p->width = r->width;
    p->height = r->height;
    p->stride_bytes = (r->width * 4u + 127u) & ~127u;
    p->order = r->order;
    p->interpolation = r->interpolation;
    p->vertex_count = r->vertex_count;
    p->index_count = r->index_count;
    p->window = r->window;
    p->missing_packet_contracts = SGX535_VARYING_MISSING_ALL;
    p->missing_mesh_contracts = SGX535_WINDOW_MESH_MISSING_ALL;
    if (sgx535_engine121_scene_build(r->width, r->height, &p->scene) != SGX535_ENGINE121_OK)
        return SGX535_TASK_UNSUPPORTED;
    memcpy(p->requirements, seed->requirements, sizeof(p->requirements));
    p->requirements[SGX535_TASK_VERTEX_DATA].minimum_bytes = p->geometry.allocation_bytes;
    p->requirements[SGX535_TASK_SCENE].minimum_bytes = page_bytes(p->scene.allocation_bytes);
    p->requirements[SGX535_TASK_TARGET].minimum_bytes = page_bytes(p->stride_bytes * r->height);
    if (p->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes < p->fragment_use_extent_bytes)
        p->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes =
            page_bytes(p->fragment_use_extent_bytes);
    surface = (struct sgx535_pbe_surface){
        r->width, r->height, p->stride_bytes, 0, 0, (uint64_t)p->stride_bytes * r->height};
    if (sgx535_pbe_state_project(&surface, &pbe))
        return SGX535_TASK_UNSUPPORTED;
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
        if (clip && !clip_placement(d))
            return SGX535_TASK_UNSUPPORTED;
        dst = section_bytes(p, d->source, d->byte_offset, d->byte_count);
        if (!dst)
            return SGX535_TASK_UNSUPPORTED;
        if (s->kind == SGX535_PACKET_FRAGMENT_USE) {
            for (j = 0; j < p->fragment.encoding.word_count; j++)
                put(dst + j * 4u, p->fragment.encoding.words[j]);
        } else
            memcpy(dst, seed->bytes + s->byte_offset, s->byte_count);
        if (s->kind == SGX535_PACKET_FRAGMENT_PDS) {
            if (d->byte_count != (no_input ? 56u : 60u))
                return SGX535_TASK_UNSUPPORTED;
            put(dst + 4, p->fragment_temp_words << 27);
            put(dst + 32, (p->fragment_temp_words >> 5) | 0x20u);
            if (io != SGX535_WINDOW_PRIMARY_COLOR) {
                if (word(dst + 36) != 0x0fd0a00fu)
                    return SGX535_TASK_UNSUPPORTED;
                put(dst + 36, 0x0fd0000fu);
            }
        } else if (s->kind == SGX535_PACKET_STATE_PDS &&
                   (clip || io != SGX535_WINDOW_PRIMARY_COLOR)) {
            struct sgx535_vertex_dma_plan dma;
            if (d->byte_count != 124 ||
                sgx535_vertex_dma_build(clip ? 16u : 11u, &dma) != SGX535_VERTEX_DMA_OK ||
                dma.transfer_count != 1)
                return SGX535_TASK_UNSUPPORTED;
            /* PSB387ff derives both DMA and state USE output count from the
             * serialized DWORD extent. Eleven words occupy44 of the existing
             *48 primary bytes; the mask already uses this checked program. */
            put(dst + 68, dma.transfers[0].descriptor_word);
        } else if (clip && s->kind == SGX535_PACKET_STATE_USE) {
            /* The clip-space record's own sixteen-output program replaces the
             * window-space primary record's ten-output one at +16. */
            if (d->byte_count != 48 || word(dst + 20) != 0x28a19001u ||
                word(dst + 24) != 0xa0200500u || !state_use(dst + 16, 16))
                return SGX535_TASK_UNSUPPORTED;
        } else if (s->kind == SGX535_PACKET_MTE_STATE) {
            if (d->byte_count != (clip ? SGX535_WINDOW_MESH_CLIP_MTE_BYTES : 92u))
                return SGX535_TASK_UNSUPPORTED;
            if (clip &&
                !clip_mte(dst, seed->bytes + s->byte_offset, r, io != SGX535_WINDOW_PRIMARY_COLOR))
                return SGX535_TASK_UNSUPPORTED;
            put(dst + 12, no_input ? w->pa0_task.dms_word
                                   : 4u | (instances - 1u) << 16 | (pixels & 31u) << 25);
            if (!clip && io != SGX535_WINDOW_PRIMARY_COLOR) {
                if (word(dst) != 0x54c1u || word(dst + 48) != 0x54c5u)
                    return SGX535_TASK_UNSUPPORTED;
                put(dst, 0xd4c1u);
                put(dst + 28, 0x08001000u);
                put(dst + 36, 7);
                put(dst + 40, 0); /* Selected TEXFLOAT follows TEXSIZE. */
            }
        } else if (s->kind == SGX535_PACKET_TERMINATE_STATE) {
            if (d->byte_count != 8)
                return SGX535_TASK_UNSUPPORTED;
            put(dst + 4, ((r->width + 15u) / 16u) << 16 | ((r->height + 15u) / 16u));
        } else if (s->kind == SGX535_PACKET_PBE_STATE) {
            if (d->byte_count != 16)
                return SGX535_TASK_UNSUPPORTED;
            for (j = 0; j < 4; j++)
                put(dst + j * 4u, pbe.words[j]);
        } else if (s->kind == SGX535_PACKET_TA_STREAM) {
            if (d->byte_count != 68)
                return SGX535_TASK_UNSUPPORTED;
            put(dst + 36, 0x81400000u | r->index_count);
            /* The foreground STATE's attribute allocation, in 16-byte units,
             * holds the clip-space record's sixteen DMA words. */
            if (clip) {
                if (word(dst + 32) != 0x0c02a203u)
                    return SGX535_TASK_UNSUPPORTED;
                put(dst + 32, 0x0c02a200u | 16u * 4u / 16u);
            }
        }
    }
    memcpy(p->relocations, seed->relocations, sizeof(seed->relocations));
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *a = &p->relocations[i];
        uint32_t kind = p->sections[a->section].kind;
        if (a->role == SGX535_TASK_VERTEX_DATA) {
            if (kind == SGX535_PACKET_VERTEX_PDS && a->kind == SGX535_PACKET_DRI_OFFSET &&
                !a->section_byte_offset && !a->resource_byte_offset &&
                a->span_bytes == seed_vertex_bytes) {
                a->span_bytes = p->geometry.foreground_vertex_bytes;
                changed++;
            } else if (kind == SGX535_PACKET_VERTEX_PDS && a->kind == SGX535_PACKET_DRI_OFFSET &&
                       a->section_byte_offset == 64 &&
                       a->resource_byte_offset == seed_vertex_bytes && a->span_bytes == 64) {
                a->resource_byte_offset = p->geometry.mask_vertex_offset_bytes;
                changed++;
            } else if (kind == SGX535_PACKET_TA_STREAM && a->kind == SGX535_PACKET_DRI_OFFSET &&
                       a->section_byte_offset == 12 &&
                       a->resource_byte_offset == seed_vertex_bytes + 64u && a->span_bytes == 12) {
                a->resource_byte_offset = p->geometry.index_offset_bytes;
                changed++;
            } else if (kind == SGX535_PACKET_TA_STREAM && a->kind == SGX535_PACKET_DRI_OFFSET &&
                       a->section_byte_offset == 40 &&
                       a->resource_byte_offset == seed_vertex_bytes + 76u && a->span_bytes == 6) {
                a->resource_byte_offset = p->geometry.foreground_index_offset_bytes;
                a->span_bytes = p->geometry.foreground_index_bytes;
                changed++;
            } else
                return SGX535_TASK_UNSUPPORTED;
        } else if (!clip && io != SGX535_WINDOW_PRIMARY_COLOR && kind == SGX535_PACKET_STATE_PDS &&
                   a->role == SGX535_TASK_USE_VERTEX && a->section_byte_offset == 72) {
            if (a->resource_byte_offset != 48 || a->span_bytes != 16)
                return SGX535_TASK_UNSUPPORTED;
            a->resource_byte_offset = 32; /* Existing11-output state USE. */
        } else if (clip && a->role == SGX535_TASK_PDS &&
                   (kind == SGX535_PACKET_STATE_PDS || kind == SGX535_PACKET_EVENT_PDS)) {
            if (!clip_relocation(kind, a))
                return SGX535_TASK_UNSUPPORTED;
            moved++;
        } else if (a->role == SGX535_TASK_USE_FRAGMENT) {
            if (kind == SGX535_PACKET_FRAGMENT_PDS && !a->resource_byte_offset)
                a->span_bytes = p->fragment_code_bytes;
            else if (kind == SGX535_PACKET_EVENT_PDS && a->resource_byte_offset >= 32 &&
                     a->resource_byte_offset < 64 && a->span_bytes <= 64u - a->resource_byte_offset)
                a->resource_byte_offset = p->event_use_offset_bytes + a->resource_byte_offset - 32u;
            else
                return SGX535_TASK_UNSUPPORTED;
        } else if (a->role == SGX535_TASK_TARGET) {
            if (kind != SGX535_PACKET_PBE_STATE || a->section_byte_offset != 4 ||
                a->span_bytes != 2048)
                return SGX535_TASK_UNSUPPORTED;
            a->span_bytes = p->stride_bytes * r->height;
        }
    }
    return changed == 4 && moved == (clip ? 3u : 0u) &&
                   (!POULSBACK_SELFCHECK || copied_inputs(r, geometry, &p->geometry, w->fragment))
               ? SGX535_TASK_BLOCKED
               : SGX535_TASK_UNSUPPORTED;
}
static enum sgx535_task_status regenerate_io(const struct sgx535_window_mesh_request *r,
                                             struct sgx535_window_mesh_workspace *w,
                                             uint8_t *scratch, enum window_convention convention,
                                             bool raw_mask, enum sgx535_window_fragment_io io)
{
    struct sgx535_window_mesh_request *frozen = &w->foreground_request;
    enum sgx535_task_status status;
    *frozen = *r;
    status = construct(frozen, w, scratch, convention, raw_mask, io);
    return status == SGX535_TASK_BLOCKED &&
                   (!same_request(r, frozen) ||
                    (frozen->uniform_word_count &&
                     memcmp(frozen->uniforms, w->uniforms,
                            (unsigned long)frozen->uniform_word_count * 4u)))
               ? SGX535_TASK_UNSUPPORTED
               : status;
}
static enum sgx535_task_status regenerate(const struct sgx535_window_mesh_request *r,
                                          struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                                          enum window_convention convention, bool raw_mask)
{
    /* Static SECONDARY owns the complete background/PDS composition below. */
    if (r->uniform_word_count)
        return SGX535_TASK_UNSUPPORTED;
    return regenerate_io(r, w, scratch, convention, raw_mask, SGX535_WINDOW_PRIMARY_COLOR);
}
enum sgx535_task_status sgx535_window_mesh_build(const struct sgx535_window_mesh_request *r,
                                                 struct sgx535_window_mesh_task *out,
                                                 uint8_t *geometry, uint32_t capacity,
                                                 struct sgx535_window_mesh_workspace *w,
                                                 uint8_t *scratch, uint32_t scratch_capacity)
{
    enum sgx535_task_status status;
    if (!arguments(r, out, sizeof(*out), geometry, capacity, w, scratch, scratch_capacity))
        return SGX535_TASK_INVALID;
    status = regenerate(r, w, scratch, WINDOW_LEGACY, false);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result.foreground;
        memcpy(geometry, scratch, out->geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_window_mesh_validate(const struct sgx535_window_mesh_request *r,
                                                    const struct sgx535_window_mesh_task *out,
                                                    const uint8_t *geometry, uint32_t bytes,
                                                    struct sgx535_window_mesh_workspace *w,
                                                    uint8_t *scratch, uint32_t scratch_capacity)
{
    enum sgx535_task_status status;
    if (!arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, scratch_capacity))
        return SGX535_TASK_INVALID;
    status = regenerate(r, w, scratch, WINDOW_LEGACY, false);
    return status == SGX535_TASK_BLOCKED && (bytes != w->result.foreground.geometry.bytes ||
                                             memcmp(out, &w->result.foreground, sizeof(*out)) ||
                                             memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static int binding_valid(const struct sgx535_window_mesh_task *p,
                         const struct sgx535_window_mesh_binding *b)
{
    struct sgx535_heap_layout_input h;
    struct sgx535_heap_layout_projection projection;
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
#define HEAP(role)                                                                                 \
    (struct sgx535_heap_extent){b->roles[role].storage_id, b->roles[role].gpu_va_bytes,            \
                                b->roles[role].allocation_bytes}
    h.pds = HEAP(SGX535_TASK_PDS);
    h.parameters = HEAP(SGX535_TASK_DPM_PARAMETERS);
    h.tables = HEAP(SGX535_TASK_DPM_TABLES);
    h.scene = HEAP(SGX535_TASK_SCENE);
#undef HEAP
    return sgx535_heap_layout_project(&h, &projection) == SGX535_HEAP_LAYOUT_OK;
}
static enum sgx535_launch_prefix_convention
launch_convention(const struct sgx535_window_mesh_task *p, enum window_convention convention)
{
    return p->version == SGX535_WINDOW_MESH_CLIP_VERSION ? SGX535_LAUNCH_PREFIX_WIN7_GL_CLIP
           : convention != WINDOW_LEGACY                 ? SGX535_LAUNCH_PREFIX_WIN7_GL_WINDOW
                                                         : SGX535_LAUNCH_PREFIX_LEGACY;
}
static enum sgx535_task_status bind_image(const struct sgx535_window_mesh_task *p,
                                          const struct sgx535_window_mesh_binding *b,
                                          struct sgx535_window_mesh_bound *out,
                                          enum window_convention convention)
{
    uint32_t i, j;
    if (!binding_valid(p, b))
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
        dst = section_bytes(&out->image, s->source, s->byte_offset + r->section_byte_offset, 4);
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
        put(dst, (word(dst) & ~r->mask) | ((uint32_t)value & r->mask));
        out->resolved_relocations++;
    }
    out->ta_stream_va_bytes = (uint32_t)b->roles[SGX535_TASK_PDS].gpu_va_bytes +
                              p->sections[p->ta_stream_section].resource_byte_offset;
    out->event_pds_va_bytes = (uint32_t)b->roles[SGX535_TASK_PDS].gpu_va_bytes +
                              p->sections[p->event_pds_section].resource_byte_offset;
    if (!sgx535_launch_prefix_build_convention(
            &out->stages[1], &out->stages[2], out->ta_stream_va_bytes, out->event_pds_va_bytes, 1,
            SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, 0, launch_convention(p, convention)))
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
static int bind_arguments(const struct sgx535_window_mesh_request *r, const void *p,
                          unsigned long pbytes, const uint8_t *geometry, uint32_t bytes,
                          const struct sgx535_window_mesh_binding *b, const void *out,
                          unsigned long outbytes, struct sgx535_window_mesh_workspace *w,
                          uint8_t *scratch, uint32_t cap)
{
    return arguments(r, out, outbytes, geometry, bytes, w, scratch, cap) &&
           extra(p, pbytes, r, out, outbytes, geometry, bytes, w, scratch, cap) &&
           extra(b, sizeof(*b), r, out, outbytes, geometry, bytes, w, scratch, cap) &&
           !overlap(p, pbytes, b, sizeof(*b));
}
static enum sgx535_task_status bind_construct(const struct sgx535_window_mesh_request *r,
                                              const struct sgx535_window_mesh_task *p,
                                              const uint8_t *geometry, uint32_t bytes,
                                              const struct sgx535_window_mesh_binding *b,
                                              struct sgx535_window_mesh_workspace *w,
                                              uint8_t *scratch, enum window_convention convention)
{
    struct sgx535_window_mesh_binding frozen = *b;
    enum sgx535_task_status status = regenerate(r, w, scratch, convention, false);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (bytes != w->result.foreground.geometry.bytes ||
        memcmp(p, &w->result.foreground, sizeof(*p)) || memcmp(geometry, scratch, bytes))
        return SGX535_TASK_INVALID;
    status = bind_image(&w->result.foreground, &frozen, &w->bound.foreground, convention);
    return status == SGX535_TASK_BLOCKED && memcmp(b, &frozen, sizeof(frozen)) ? SGX535_TASK_INVALID
                                                                               : status;
}
enum sgx535_task_status sgx535_window_mesh_bind(const struct sgx535_window_mesh_request *r,
                                                const struct sgx535_window_mesh_task *p,
                                                const uint8_t *geometry, uint32_t bytes,
                                                const struct sgx535_window_mesh_binding *b,
                                                struct sgx535_window_mesh_bound *out,
                                                struct sgx535_window_mesh_workspace *w,
                                                uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!bind_arguments(r, p, sizeof(*p), geometry, bytes, b, out, sizeof(*out), w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, p, geometry, bytes, b, w, scratch, WINDOW_LEGACY);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound.foreground;
    return status;
}
enum sgx535_task_status sgx535_window_mesh_bound_validate(
    const struct sgx535_window_mesh_request *r, const struct sgx535_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_window_mesh_binding *b,
    const struct sgx535_window_mesh_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!bind_arguments(r, p, sizeof(*p), geometry, bytes, b, out, sizeof(*out), w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, p, geometry, bytes, b, w, scratch, WINDOW_LEGACY);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound.foreground, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
static struct sgx535_background_request
background_request(const struct sgx535_window_mesh_background_request *r)
{
    struct sgx535_background_request b = {0};
    b.version = SGX535_BACKGROUND_WINDOW_MESH_VERSION;
    b.core_id = r->foreground.core_id;
    b.revision = r->foreground.revision;
    b.width = r->foreground.width;
    b.height = r->foreground.height;
    b.packed_argb = r->packed_background_argb;
    return b;
}
static int background_arguments(const struct sgx535_window_mesh_background_request *r,
                                const void *out, unsigned long outbytes, const uint8_t *geometry,
                                uint32_t bytes, struct sgx535_window_mesh_workspace *w,
                                uint8_t *scratch, uint32_t cap)
{
    return span(r, sizeof(*r)) &&
           arguments(&r->foreground, out, outbytes, geometry, bytes, w, scratch, cap) &&
           !overlap(r, sizeof(*r), r->foreground.vertices,
                    (unsigned long)r->foreground.vertex_count * sizeof(*r->foreground.vertices)) &&
           !overlap(r, sizeof(*r), r->foreground.indices,
                    (unsigned long)r->foreground.index_count * sizeof(*r->foreground.indices)) &&
           !overlap(r, sizeof(*r), r->foreground.fragment,
                    (unsigned long)r->foreground.fragment_count *
                        sizeof(*r->foreground.fragment)) &&
           (!r->foreground.uniform_word_count ||
            !overlap(r, sizeof(*r), r->foreground.uniforms,
                     (unsigned long)r->foreground.uniform_word_count * 4u)) &&
           !overlap(r, sizeof(*r), out, outbytes) && !overlap(r, sizeof(*r), geometry, bytes) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(r, sizeof(*r), scratch, cap);
}
static enum sgx535_task_status
background_construct_io(const struct sgx535_window_mesh_background_request *r,
                        struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                        enum window_convention convention, bool raw_mask,
                        enum sgx535_window_fragment_io io)
{
    struct sgx535_window_mesh_background_request *frozen = &w->background_request;
    struct sgx535_background_request b;
    struct sgx535_window_mesh_fragment_layout layout;
    enum sgx535_task_status status;
    *frozen = *r;
    b = background_request(frozen);
    if (r->version != (convention != WINDOW_LEGACY ? SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION
                                                   : SGX535_WINDOW_MESH_BACKGROUND_VERSION) ||
        r->reserved[0] || r->reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    status = regenerate_io(&frozen->foreground, w, scratch, convention, raw_mask, io);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = sgx535_window_mesh_fragment_layout(w->result.foreground.fragment.encoding.word_count,
                                                &layout);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if ((io == SGX535_WINDOW_GENERIC0_F32_SECONDARY ||
         frozen->foreground.interpolation == SGX535_WINDOW_MESH_NO_INPUT) &&
        w->secondary.physical_sa_words &&
        w->result.foreground.fragment_use_extent_bytes + 8u > layout.background_offset_bytes) {
        layout.background_offset_bytes =
            (w->result.foreground.fragment_use_extent_bytes + 8u + 31u) & ~31u;
        layout.clear_offset_bytes = layout.background_offset_bytes + 32u;
    }
    status = sgx535_background_window_mesh_layout_seed(&b, layout.background_offset_bytes,
                                                       &w->result.background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if ((io == SGX535_WINDOW_GENERIC0_F32_SECONDARY ||
         frozen->foreground.interpolation == SGX535_WINDOW_MESH_NO_INPUT) &&
        window_secondary_compose(w) != SGX535_TASK_BLOCKED)
        return SGX535_TASK_UNSUPPORTED;
    if (w->result.foreground.requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes <
        layout.clear_offset_bytes + 8u)
        w->result.foreground.requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes =
            page_bytes(layout.clear_offset_bytes + 8u);
    w->result.version = frozen->version;
    w->result.missing_background_contracts = SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL;
    return r->version == frozen->version &&
                   r->packed_background_argb == frozen->packed_background_argb &&
                   !memcmp(r->reserved, frozen->reserved, sizeof(frozen->reserved)) &&
                   same_request(&r->foreground, &frozen->foreground)
               ? status
               : SGX535_TASK_UNSUPPORTED;
}
static enum sgx535_task_status
background_construct(const struct sgx535_window_mesh_background_request *r,
                     struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                     enum window_convention convention, bool raw_mask)
{
    return background_construct_io(r, w, scratch, convention, raw_mask,
                                   SGX535_WINDOW_PRIMARY_COLOR);
}
enum sgx535_task_status sgx535_window_mesh_background_build(
    const struct sgx535_window_mesh_background_request *r,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!background_arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = background_construct(r, w, scratch, WINDOW_LEGACY, false);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result;
        memcpy(geometry, scratch, out->foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_window_mesh_background_validate(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *out, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!background_arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = background_construct(r, w, scratch, WINDOW_LEGACY, false);
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->result.foreground.geometry.bytes ||
                    memcmp(out, &w->result, sizeof(*out)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static int background_bind_arguments(const struct sgx535_window_mesh_background_request *r,
                                     const struct sgx535_window_mesh_background_image *p,
                                     const uint8_t *geometry, uint32_t bytes,
                                     const struct sgx535_window_mesh_binding *b,
                                     const struct sgx535_background_binding *bg,
                                     const struct sgx535_window_mesh_background_bound *out,
                                     struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                                     uint32_t cap)
{
    return background_arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap) &&
           !overlap(p, sizeof(*p), r, sizeof(*r)) && !overlap(b, sizeof(*b), r, sizeof(*r)) &&
           !overlap(bg, sizeof(*bg), r, sizeof(*r)) &&
           extra(p, sizeof(*p), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           extra(b, sizeof(*b), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           extra(bg, sizeof(*bg), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           !overlap(p, sizeof(*p), b, sizeof(*b)) && !overlap(p, sizeof(*p), bg, sizeof(*bg)) &&
           !overlap(b, sizeof(*b), bg, sizeof(*bg));
}
static enum sgx535_task_status background_bind_construct_owner_io(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, enum window_convention convention, bool full_owner, bool raw_mask,
    enum sgx535_window_fragment_io io)
{
    struct sgx535_window_mesh_binding *b = &w->bind_binding;
    struct sgx535_background_binding *bg = &w->bind_background;
    struct sgx535_background_request *request = &w->bind_request;
    enum sgx535_task_status status;
    uint32_t i, j;
    *b = *binding;
    *bg = *background;
    *request = background_request(r);
    status = background_construct_io(r, w, scratch, convention, raw_mask, io);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (bytes != w->result.foreground.geometry.bytes || memcmp(p, &w->result, sizeof(*p)) ||
        memcmp(geometry, scratch, bytes))
        return SGX535_TASK_INVALID;
    status = bind_image(&w->result.foreground, b, &w->bound.foreground, convention);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (bg->epoch != b->epoch ||
        bg->parameter_storage_id != b->roles[SGX535_TASK_DPM_PARAMETERS].storage_id ||
        bg->parameter_va != b->roles[SGX535_TASK_DPM_PARAMETERS].gpu_va_bytes ||
        bg->parameter_bytes != b->roles[SGX535_TASK_DPM_PARAMETERS].allocation_bytes ||
        bg->fragment_bank_index != b->bank_index[1])
        return SGX535_TASK_INVALID;
    for (i = 0; i < 2; i++) {
        uint32_t role = i ? SGX535_TASK_PDS : SGX535_TASK_USE_FRAGMENT;
        if (bg->resources[i].storage_id != b->roles[role].storage_id ||
            bg->resources[i].gpu_va_bytes != b->roles[role].gpu_va_bytes ||
            bg->resources[i].allocation_bytes != b->roles[role].allocation_bytes ||
            bg->resources[i].access != b->roles[role].access)
            return SGX535_TASK_INVALID;
    }
    for (i = 0; i < 8; i++)
        if (bg->resources[2].storage_id == b->roles[i].storage_id ||
            (bg->resources[2].gpu_va_bytes <
                 b->roles[i].gpu_va_bytes + b->roles[i].allocation_bytes &&
             b->roles[i].gpu_va_bytes <
                 bg->resources[2].gpu_va_bytes + bg->resources[2].allocation_bytes))
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
    status = sgx535_background_window_mesh_bind_layout_seed(
        request, &w->result.background, bg, w->result.background.sections[0].resource_offset,
        full_owner, &w->bound.background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    memset(&w->bound.foreground.stages[1], 0, sizeof(w->bound.foreground.stages[1]));
    memset(&w->bound.foreground.stages[2], 0, sizeof(w->bound.foreground.stages[2]));
    if (!sgx535_launch_prefix_build_convention(
            &w->bound.foreground.stages[1], &w->bound.foreground.stages[2],
            w->bound.foreground.ta_stream_va_bytes, w->bound.foreground.event_pds_va_bytes, 1,
            SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, &w->bound.background,
            launch_convention(&w->result.foreground, convention)))
        return SGX535_TASK_UNSUPPORTED;
    w->bound.missing_background_contracts = SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL;
    return memcmp(binding, b, sizeof(*b)) || memcmp(background, bg, sizeof(*bg))
               ? SGX535_TASK_INVALID
               : status;
}
static enum sgx535_task_status background_bind_construct_owner(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, enum window_convention convention, bool full_owner)
{
    return background_bind_construct_owner_io(r, p, geometry, bytes, binding, background, w,
                                              scratch, convention, full_owner, false,
                                              SGX535_WINDOW_PRIMARY_COLOR);
}
static enum sgx535_task_status background_bind_construct(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, enum window_convention convention)
{
    return background_bind_construct_owner(r, p, geometry, bytes, binding, background, w, scratch,
                                           convention, false);
}
enum sgx535_task_status sgx535_window_mesh_background_bind(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!background_bind_arguments(r, p, geometry, bytes, b, bg, out, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = background_bind_construct(r, p, geometry, bytes, b, bg, w, scratch, WINDOW_LEGACY);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_window_mesh_background_bound_validate(
    const struct sgx535_window_mesh_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    const struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!background_bind_arguments(r, p, geometry, bytes, b, bg, out, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = background_bind_construct(r, p, geometry, bytes, b, bg, w, scratch, WINDOW_LEGACY);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}

#include "sgx535_window_mesh_gl_window.inc"

#include "sgx535_window_mesh_single_sample.inc"
