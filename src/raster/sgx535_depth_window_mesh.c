// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_window_mesh.h"
#include "sgx535_depth_window_mesh_gl_window.h"
#include "sgx535_depth_window_mesh_single_sample.h"
#include "sgx535_window_mesh_internal.h"
#include "sgx535_single_sample_internal.h"
#include "sgx535_depth_window_mesh_state.h"
#include "sgx535_depth_window_mesh_preserve.h"
#include "sgx535_pds_use_link.h"
#include "poulsback_selfcheck.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#endif

/* ISP_BGOBJ as EMGD renders (sgx535_zls.c): stencil 0 in 7:0, MASK (8), the
 * ISP's draw mask set over the target at each render's start, for the
 * scissor's viewport objects to edit, and ENABLEBGTAG (9). It does not write
 * the stencil (docs/native-3d.md#pixel-pds-and-isp-words). */
#define BGOBJ_DRAW_MASK 0x300u

enum section_id {
    V,
    I,
    VU,
    FU,
    SU,
    EU,
    VP,
    FP,
    SECOND,
    STATE,
    TERM,
    EVENT,
    MTE,
    TERM_STATE,
    PBE,
    TA,
    CLEAR_USE,
    CLEAR_FP,
    CLEAR_STATE,
    CLEAR_MTE,
    STATIC_SECOND
};
enum placement {
    CLEAR_FP_OFFSET = 896,
    CLEAR_STATE_OFFSET = 960,
    CLEAR_MTE_OFFSET = 1024,
    TA_OFFSET = 1088
};
struct cpu_span {
    const void *p;
    unsigned long bytes;
};
struct depth_policy {
    uint32_t version, compare, write_enable, content, clear_prefix, gl_window;
    const struct sgx535_single_sample_source *single_sample;
    bool raw_mask;
    enum sgx535_window_fragment_io io;
};
static const struct depth_policy fixed_depth = {
    SGX535_DEPTH_WINDOW_MESH_VERSION, SGX535_DEPTH_LESS, 1, SGX535_DEPTH_FRESH, 1, 0, 0, false,
    SGX535_WINDOW_PRIMARY_COLOR};
static const struct depth_policy preserved_depth = {SGX535_DEPTH_WINDOW_MESH_PRESERVE_VERSION,
                                                    SGX535_DEPTH_LESS,
                                                    1,
                                                    SGX535_DEPTH_PREVIOUS_SCENE,
                                                    0,
                                                    0,
                                                    0,
                                                    false,
                                                    SGX535_WINDOW_PRIMARY_COLOR};
static const struct depth_policy gl_depth = {SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION,
                                             SGX535_DEPTH_LESS,
                                             1,
                                             SGX535_DEPTH_FRESH,
                                             1,
                                             1,
                                             0,
                                             false,
                                             SGX535_WINDOW_PRIMARY_COLOR};
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
static void put(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}
static int arguments(const void *root, unsigned long root_bytes,
                     const struct sgx535_depth_window_mesh_request *r, const void *image,
                     unsigned long image_bytes, const void *binding, unsigned long binding_bytes,
                     const void *out, unsigned long out_bytes, const uint8_t *geometry,
                     uint32_t capacity, struct sgx535_depth_window_mesh_workspace *w,
                     uint8_t *scratch, uint32_t scratch_capacity)
{
    struct sgx535_window_mesh_storage p;
    struct cpu_span ranges[11];
    const struct sgx535_window_mesh_request *m;
    uint32_t count = 8, i, j;
    if (!span(root, root_bytes) || !span(r, sizeof(*r)))
        return 0;
    m = &r->scene.foreground;
    if (m->fragment_count < 5 || m->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
        sgx535_window_mesh_input_storage_plan(m, &p) != SGX535_TASK_BLOCKED || capacity < p.bytes ||
        scratch_capacity < p.bytes)
        return 0;
    ranges[0] = (struct cpu_span){root, root_bytes};
    ranges[1] =
        (struct cpu_span){m->vertices, (unsigned long)m->vertex_count * sizeof(*m->vertices)};
    ranges[2] = (struct cpu_span){m->indices, (unsigned long)m->index_count * sizeof(*m->indices)};
    ranges[3] =
        (struct cpu_span){m->fragment, (unsigned long)m->fragment_count * sizeof(*m->fragment)};
    ranges[4] = (struct cpu_span){out, out_bytes};
    ranges[5] = (struct cpu_span){geometry, capacity};
    ranges[6] = (struct cpu_span){w, sizeof(*w)};
    ranges[7] = (struct cpu_span){scratch, scratch_capacity};
    if (m->uniform_word_count >= SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!m->uniform_word_count != !!m->uniforms))
        return 0;
    if (m->uniform_word_count)
        ranges[count++] = (struct cpu_span){m->uniforms, (unsigned long)m->uniform_word_count * 4u};
    if (image)
        ranges[count++] = (struct cpu_span){image, image_bytes};
    if (binding)
        ranges[count++] = (struct cpu_span){binding, binding_bytes};
    for (i = 0; i < count; i++) {
        if (!span(ranges[i].p, ranges[i].bytes))
            return 0;
        for (j = 0; j < i; j++)
            if (overlap(ranges[i].p, ranges[i].bytes, ranges[j].p, ranges[j].bytes))
                return 0;
    }
    return 1;
}
static int same_request(const struct sgx535_depth_window_mesh_request *a,
                        const struct sgx535_depth_window_mesh_request *b)
{
    const struct sgx535_window_mesh_request *x = &a->scene.foreground, *y = &b->scene.foreground;
    return a->version == b->version && a->format == b->format && a->no_depth == b->no_depth &&
           a->reserved == b->reserved && a->scene.version == b->scene.version &&
           a->scene.packed_background_argb == b->scene.packed_background_argb &&
           !memcmp(a->scene.reserved, b->scene.reserved, sizeof(a->scene.reserved)) &&
           x->version == y->version && x->profile == y->profile && x->core_id == y->core_id &&
           x->revision == y->revision && x->width == y->width && x->height == y->height &&
           x->order == y->order && x->interpolation == y->interpolation &&
           !memcmp(&x->window, &y->window, sizeof(x->window)) && x->vertices == y->vertices &&
           x->indices == y->indices && x->vertex_count == y->vertex_count &&
           x->index_count == y->index_count && x->fragment == y->fragment &&
           x->fragment_count == y->fragment_count && x->uniforms == y->uniforms &&
           x->uniform_word_count == y->uniform_word_count && x->cull == y->cull &&
           !memcmp(x->reserved, y->reserved, sizeof(x->reserved));
}
static int copied_inputs(const struct sgx535_depth_window_mesh_request *r, const uint8_t *geometry,
                         const struct sgx535_window_mesh_storage *p,
                         const struct sgx535_fragment_instruction *ir)
{
    const struct sgx535_window_mesh_request *m = &r->scene.foreground;
    uint32_t i, j, stride = p->foreground_vertex_bytes / m->vertex_count;
    for (i = 0; i < m->vertex_count; i++)
        for (j = 0; j < 4; j++)
            if (word(geometry + i * stride + j * 4u) != m->vertices[i].window_xyz_reciprocal_w[j] ||
                (stride == 32u
                     ? word(geometry + i * stride + 16u + j * 4u) != m->vertices[i].rgba[j]
                     : m->vertices[i].rgba[j] != 0))
                return 0;
    for (i = 0; i < m->index_count; i++) {
        const uint8_t *v = geometry + p->foreground_index_offset_bytes + i * 2u;
        if (((uint32_t)v[0] | (uint32_t)v[1] << 8) != m->indices[i])
            return 0;
    }
    return !memcmp(m->fragment, ir, (unsigned long)m->fragment_count * sizeof(*ir));
}
static uint8_t *payload(struct sgx535_depth_window_mesh_task *p, uint32_t source, uint32_t offset,
                        uint32_t bytes)
{
    uint8_t *base;
    uint32_t capacity;
    if (source == SGX535_DEPTH_WINDOW_MESH_PDS) {
        base = p->pds;
        capacity = sizeof(p->pds);
    } else if (source == SGX535_DEPTH_WINDOW_MESH_VERTEX_USE) {
        base = p->seed.foreground.vertex_use;
        capacity = sizeof(p->seed.foreground.vertex_use);
    } else if (source == SGX535_DEPTH_WINDOW_MESH_FRAGMENT_USE) {
        base = p->seed.foreground.fragment_use;
        capacity = sizeof(p->seed.foreground.fragment_use);
    } else if (source == SGX535_DEPTH_WINDOW_MESH_CLEAR_USE) {
        base = p->clear_use;
        capacity = sizeof(p->clear_use);
    } else if (source == SGX535_DEPTH_WINDOW_MESH_SECONDARY_UPLOAD) {
        base = p->seed.foreground.secondary_upload;
        capacity = sizeof(p->seed.foreground.secondary_upload);
    } else
        return 0;
    return offset <= capacity && bytes <= capacity - offset ? base + offset : 0;
}
static int add(struct sgx535_depth_window_mesh_task *p, uint32_t id, uint32_t kind, uint32_t source,
               uint32_t offset, uint32_t bytes, uint32_t role, uint32_t target, uint32_t alignment,
               const uint8_t *data)
{
    uint8_t *dst = payload(p, source, offset, bytes);
    if (!dst || id >= SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u)
        return 0;
    p->sections[id] =
        (struct sgx535_window_mesh_section){kind, source, offset, bytes, role, target, alignment};
    memcpy(dst, data, bytes);
    return 1;
}
static void address(struct sgx535_depth_window_mesh_task *p, uint32_t sid, uint32_t offset,
                    uint32_t role, uint32_t target, uint32_t bytes, uint32_t alignment,
                    uint32_t mask, uint32_t right)
{
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        SGX535_PACKET_DRI_OFFSET, sid, offset, role, target, bytes, alignment, mask, right, 0};
}
static void use_link(struct sgx535_depth_window_mesh_task *p, uint32_t sid, uint32_t offset,
                     uint32_t target, uint32_t bytes)
{
    p->relocations[p->relocation_count++] =
        (struct sgx535_task_packet_reloc){SGX535_PACKET_USE_BANK,
                                          sid,
                                          offset,
                                          SGX535_TASK_USE_FRAGMENT,
                                          target,
                                          bytes,
                                          16,
                                          0xf,
                                          0,
                                          0};
    p->relocations[p->relocation_count++] =
        (struct sgx535_task_packet_reloc){SGX535_PACKET_USE_ENTRY,
                                          sid,
                                          offset,
                                          SGX535_TASK_USE_FRAGMENT,
                                          target,
                                          bytes,
                                          16,
                                          0xf0,
                                          15,
                                          4};
    p->relocations[p->relocation_count++] =
        (struct sgx535_task_packet_reloc){SGX535_PACKET_USE_ENTRY,
                                          sid,
                                          offset,
                                          SGX535_TASK_USE_FRAGMENT,
                                          target,
                                          bytes,
                                          16,
                                          0x7ff00,
                                          4,
                                          8};
}
static int extents(struct sgx535_depth_window_mesh_task *p)
{
    const struct sgx535_window_mesh_task *seed = &p->seed.foreground;
    uint32_t i, j;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &p->sections[i];
        if (s->role >= SGX535_TASK_IMAGE_ROLES || !s->alignment ||
            (s->alignment & (s->alignment - 1u)) ||
            (s->resource_byte_offset & (s->alignment - 1u)) ||
            s->resource_byte_offset > seed->requirements[s->role].minimum_bytes ||
            s->byte_count > seed->requirements[s->role].minimum_bytes - s->resource_byte_offset)
            return 0;
        if (s->source == SGX535_DEPTH_WINDOW_MESH_GEOMETRY) {
            if (s->byte_offset > seed->geometry.bytes ||
                s->byte_count > seed->geometry.bytes - s->byte_offset)
                return 0;
        } else if (!payload(p, s->source, s->byte_offset, s->byte_count))
            return 0;
        for (j = 0; j < i; j++) {
            const struct sgx535_window_mesh_section *t = &p->sections[j];
            if (s->role == t->role &&
                s->resource_byte_offset < t->resource_byte_offset + t->byte_count &&
                t->resource_byte_offset < s->resource_byte_offset + s->byte_count)
                return 0;
        }
        for (j = 0; j < p->seed.background.section_count; j++) {
            const struct sgx535_background_section *b = &p->seed.background.sections[j];
            uint32_t role =
                b->role == SGX535_BACKGROUND_USE ? SGX535_TASK_USE_FRAGMENT : SGX535_TASK_PDS;
            if (b->role != SGX535_BACKGROUND_RASTGEOM && role == s->role &&
                s->resource_byte_offset < b->resource_offset + b->byte_count &&
                b->resource_offset < s->resource_byte_offset + s->byte_count)
                return 0;
        }
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        const struct sgx535_window_mesh_section *s;
        if (r->section >= p->section_count || r->role >= SGX535_TASK_IMAGE_ROLES)
            return 0;
        s = &p->sections[r->section];
        if (!r->alignment || (r->alignment & (r->alignment - 1u)) ||
            r->section_byte_offset > s->byte_count || 4u > s->byte_count - r->section_byte_offset ||
            r->resource_byte_offset > seed->requirements[r->role].minimum_bytes ||
            r->span_bytes > seed->requirements[r->role].minimum_bytes - r->resource_byte_offset)
            return 0;
    }
    return 1;
}
static enum sgx535_task_status construct(const struct sgx535_depth_window_mesh_request *r,
                                         const struct depth_policy *policy,
                                         struct sgx535_depth_window_mesh_workspace *w)
{
    struct sgx535_depth_window_mesh_task *p = &w->image;
    const struct sgx535_window_mesh_request *m = &r->scene.foreground;
    struct sgx535_depth_request d = {SGX535_DEPTH_VERSION, 0, 0,  0, SGX535_DEPTH_FRESH, 1,
                                     SGX535_DEPTH_LESS,    1, {0}};
    struct sgx535_window_mesh_fragment_layout layout;
    const bool clip = m->version == SGX535_WINDOW_MESH_CLIP_VERSION;
    const bool generic = policy->io == SGX535_WINDOW_GENERIC0_F32;
    uint32_t i, extra;
    if (r->version != (policy->gl_window ? SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION
                                         : SGX535_DEPTH_WINDOW_MESH_VERSION) ||
        r->reserved || r->no_depth > 1 ||
        (r->no_depth && (policy->compare != SGX535_DEPTH_ALWAYS || policy->write_enable)) ||
        (policy->gl_window && r->format != SGX535_DEPTH_D24S8))
        return SGX535_TASK_UNSUPPORTED;
    d.width = m->width;
    d.height = m->height;
    d.format = r->format;
    d.compare = policy->compare;
    d.write_enable = policy->write_enable;
    d.content = policy->content;
    if (policy->clear_prefix) {
        if (sgx535_depth_clear_build(&d, &w->clear, &w->clear_work) != SGX535_DEPTH_BLOCKED)
            return SGX535_TASK_UNSUPPORTED;
        p->depth = w->clear.depth;
    } else {
        if (r->format != SGX535_DEPTH_D24S8 ||
            (!policy->single_sample && (m->width != 64 || m->height != 64)) ||
            sgx535_depth_prepare(&d, &p->depth) != SGX535_DEPTH_BLOCKED)
            return SGX535_TASK_UNSUPPORTED;
    }
    extra = p->seed.foreground.secondary.physical_sa_words
                ? p->seed.foreground.relocation_count - 40u
                : 0u;
    if (p->seed.foreground.section_count != 16 ||
        p->seed.foreground.relocation_count != 40u + extra || extra > 5u ||
        p->seed.foreground.sections[MTE].resource_byte_offset != 576 ||
        p->seed.foreground.sections[MTE].byte_count !=
            (clip ? SGX535_WINDOW_MESH_CLIP_MTE_BYTES : 92u) ||
        p->seed.foreground.sections[TA].byte_count != 68 ||
        sgx535_window_mesh_fragment_layout(p->seed.foreground.fragment.encoding.word_count,
                                           &layout) != SGX535_TASK_BLOCKED ||
        (!extra &&
         p->seed.background.sections[0].resource_offset != layout.background_offset_bytes))
        return SGX535_TASK_UNSUPPORTED;
    if (extra) {
        layout.background_offset_bytes = p->seed.background.sections[0].resource_offset;
        layout.clear_offset_bytes = layout.background_offset_bytes + 32u;
    }
    if (policy->clear_prefix &&
        p->seed.foreground.requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes <
            layout.clear_offset_bytes + 8u)
        return SGX535_TASK_UNSUPPORTED;
    p->version = policy->version;
    if (extra && !policy->clear_prefix)
        return SGX535_TASK_UNSUPPORTED;
    p->section_count = policy->clear_prefix ? SGX535_DEPTH_WINDOW_MESH_SECTIONS + !!extra : 16;
    p->relocation_count = 40u + extra;
    p->ta_stream_section = TA;
    p->event_pds_section = EVENT;
    p->missing_depth_contracts = SGX535_DEPTH_MISSING_ALL;
    for (i = 0; i < 16; i++) {
        const struct sgx535_window_mesh_section *s = &p->seed.foreground.sections[i];
        p->sections[i] = *s;
        if (s->source == SGX535_WINDOW_MESH_SECONDARY_UPLOAD) {
            p->sections[STATIC_SECOND] = *s;
            p->sections[STATIC_SECOND].source = SGX535_DEPTH_WINDOW_MESH_SECONDARY_UPLOAD;
            p->sections[STATIC_SECOND].resource_byte_offset =
                (SGX535_DEPTH_WINDOW_MESH_PDS_BYTES + 31u) & ~31u;
            p->sections[i] = (struct sgx535_window_mesh_section){SGX535_PACKET_SECONDARY_PDS,
                                                                 SGX535_DEPTH_WINDOW_MESH_PDS,
                                                                 192,
                                                                 4,
                                                                 SGX535_TASK_PDS,
                                                                 192,
                                                                 32};
            /* Retain the seed's actual empty DM1 HALT, not a zero word. */
            memcpy(p->pds + 192, p->seed.foreground.pds + 192, 4);
        }
        if (s->source == SGX535_WINDOW_MESH_PDS && i != TA) {
            if (!payload(p, s->source, s->byte_offset, s->byte_count))
                return SGX535_TASK_UNSUPPORTED;
            memcpy(p->pds + s->byte_offset, p->seed.foreground.pds + s->byte_offset, s->byte_count);
        }
    }
    if (word(p->pds + 576) != (clip      ? generic ? SGX535_WINDOW_MESH_CLIP_F32_MTE_HEADER
                                                   : SGX535_WINDOW_MESH_CLIP_MTE_HEADER
                               : generic ? 0xd4c1u
                                         : 0x54c1u) ||
        word(p->pds + 580) != 0x01d00000 ||
        word(p->pds + 576 + (clip ? SGX535_WINDOW_MESH_CLIP_MASK_MTE : 48u)) != 0x54c5)
        return SGX535_TASK_UNSUPPORTED;
    put(p->pds + 580, (word(p->pds + 580) & ~p->depth.isp_clear_mask) | p->depth.isp_set_bits);
    if (!policy->clear_prefix) {
        const struct sgx535_window_mesh_section *ta = &p->seed.foreground.sections[TA];
        if (ta->byte_count != 68)
            return SGX535_TASK_UNSUPPORTED;
        p->sections[TA] = (struct sgx535_window_mesh_section){SGX535_PACKET_TA_STREAM,
                                                              SGX535_DEPTH_WINDOW_MESH_PDS,
                                                              TA_OFFSET,
                                                              68,
                                                              SGX535_TASK_PDS,
                                                              TA_OFFSET,
                                                              16};
        memcpy(p->pds + TA_OFFSET, p->seed.foreground.pds + ta->byte_offset, 68);
        memcpy(p->relocations, p->seed.foreground.relocations,
               sizeof(p->seed.foreground.relocations));
        return extents(p) ? SGX535_TASK_BLOCKED : SGX535_TASK_UNSUPPORTED;
    }
    if (!add(p, CLEAR_USE, SGX535_PACKET_FRAGMENT_USE, SGX535_DEPTH_WINDOW_MESH_CLEAR_USE, 0, 8,
             SGX535_TASK_USE_FRAGMENT, layout.clear_offset_bytes, 32,
             w->clear.bytes + w->clear.sections[3].byte_offset) ||
        !add(p, CLEAR_FP, SGX535_PACKET_FRAGMENT_PDS, SGX535_DEPTH_WINDOW_MESH_PDS, CLEAR_FP_OFFSET,
             56, SGX535_TASK_PDS, CLEAR_FP_OFFSET, 32,
             w->clear.bytes + w->clear.sections[6].byte_offset) ||
        !add(p, CLEAR_STATE, SGX535_PACKET_STATE_PDS, SGX535_DEPTH_WINDOW_MESH_PDS,
             CLEAR_STATE_OFFSET, 60, SGX535_TASK_PDS, CLEAR_STATE_OFFSET, 32,
             w->clear.bytes + w->clear.sections[8].byte_offset) ||
        !add(p, CLEAR_MTE, SGX535_PACKET_MTE_STATE, SGX535_DEPTH_WINDOW_MESH_PDS, CLEAR_MTE_OFFSET,
             44, SGX535_TASK_PDS, CLEAR_MTE_OFFSET, 16,
             w->clear.bytes + w->clear.sections[9].byte_offset))
        return SGX535_TASK_UNSUPPORTED;
    p->sections[TA] = (struct sgx535_window_mesh_section){SGX535_PACKET_TA_STREAM,
                                                          SGX535_DEPTH_WINDOW_MESH_PDS,
                                                          TA_OFFSET,
                                                          96,
                                                          SGX535_TASK_PDS,
                                                          TA_OFFSET,
                                                          16};
    {
        const uint8_t *old = p->seed.foreground.pds + p->seed.foreground.sections[TA].byte_offset;
        uint8_t *dst = p->pds + TA_OFFSET;
        memcpy(dst, old, 28);
        memcpy(dst + 28, w->clear.bytes + w->clear.sections[10].byte_offset, 28);
        memcpy(dst + 56, old + 28, 40);
    }
    memcpy(p->relocations, p->seed.foreground.relocations, sizeof(p->seed.foreground.relocations));
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *a = &p->relocations[i];
        if (extra && a->role == SGX535_TASK_PDS &&
            a->resource_byte_offset >= p->seed.foreground.sections[SECOND].resource_byte_offset &&
            a->resource_byte_offset - p->seed.foreground.sections[SECOND].resource_byte_offset <
                p->seed.foreground.sections[SECOND].byte_count)
            a->resource_byte_offset = p->sections[STATIC_SECOND].resource_byte_offset +
                                      a->resource_byte_offset -
                                      p->seed.foreground.sections[SECOND].resource_byte_offset;
        if (extra && a->section == SECOND)
            a->section = STATIC_SECOND;
        if (a->section == TA && a->section_byte_offset >= 28)
            a->section_byte_offset += 28;
    }
    use_link(p, CLEAR_FP, 0, layout.clear_offset_bytes, 8);
    address(p, CLEAR_STATE, 0, SGX535_TASK_PDS, CLEAR_MTE_OFFSET, 44, 16, 0xffffffffu, 0);
    /* Same11-output state USE as the mask, but the clear's own MTE record. */
    for (i = 0; i < 3; i++) {
        struct sgx535_task_packet_reloc a = p->seed.foreground.relocations[12u + i];
        if (a.section != STATE || a.section_byte_offset != 8 || a.role != SGX535_TASK_USE_VERTEX ||
            a.resource_byte_offset != 32 || a.span_bytes != 16)
            return SGX535_TASK_UNSUPPORTED;
        a.section = CLEAR_STATE;
        p->relocations[p->relocation_count++] = a;
    }
    address(p, CLEAR_MTE, 12, SGX535_TASK_PDS, 192, 4, 32, 0xffffffu, 4);
    address(p, CLEAR_MTE, 20, SGX535_TASK_PDS, CLEAR_FP_OFFSET, 56, 32, 0xffffffu, 4);
    address(p, TA, 28, SGX535_TASK_PDS, CLEAR_STATE_OFFSET, 60, 32, 0xfffffffu, 4);
    address(p, TA, 40, SGX535_TASK_VERTEX_DATA, p->seed.foreground.geometry.index_offset_bytes, 12,
            2, 0xffffffffu, 0);
    address(p, TA, 48, SGX535_TASK_PDS, 64, 64, 32, 0xfffffffu, 4);
    return p->relocation_count == SGX535_DEPTH_WINDOW_MESH_RELOCS + extra && extents(p)
               ? SGX535_TASK_BLOCKED
               : SGX535_TASK_UNSUPPORTED;
}

/* Two disjoint geometry temporaries are required by task14's transactional
 * builder. The public scratch is split into exact count-derived halves. */
static enum sgx535_task_status regenerate(const struct sgx535_depth_window_mesh_request *r,
                                          const struct depth_policy *policy,
                                          struct sgx535_depth_window_mesh_workspace *w,
                                          uint8_t *scratch, uint32_t capacity)
{
    struct sgx535_depth_window_mesh_request *frozen = &w->regenerate_request;
    struct sgx535_window_mesh_storage p;
    enum sgx535_task_status status;
    *frozen = *r;
    if (sgx535_window_mesh_input_storage_plan(&r->scene.foreground, &p) != SGX535_TASK_BLOCKED ||
        p.bytes > capacity / 2u)
        return SGX535_TASK_INVALID;
    /* The complete source build uses the second half as its private scratch.
     * construct consumes the already-regenerated source without heap effects. */
    memset(&w->image, 0, sizeof(w->image));
    if (policy->gl_window) {
        struct sgx535_window_mesh_gl_window_background_request source = {
            .version = frozen->scene.version,
            .packed_background_argb = frozen->scene.packed_background_argb,
            .reserved = {frozen->scene.reserved[0], frozen->scene.reserved[1]},
            .foreground = frozen->scene.foreground};
        status = policy->io == SGX535_WINDOW_GENERIC0_F32
                     ? sgx535_window_mesh_single_sample_background_f32_build(
                           &source, policy->single_sample, &w->image.seed, scratch, p.bytes,
                           &w->mesh, scratch + p.bytes, p.bytes)
                 : policy->single_sample && policy->raw_mask
                     ? sgx535_window_mesh_single_sample_background_raw_mask_build_v1(
                           &source, policy->single_sample, &w->image.seed, scratch, p.bytes,
                           &w->mesh, scratch + p.bytes, p.bytes)
                 : policy->single_sample
                     ? sgx535_window_mesh_single_sample_background_build(
                           &source, policy->single_sample, &w->image.seed, scratch, p.bytes,
                           &w->mesh, scratch + p.bytes, p.bytes)
                     : sgx535_window_mesh_gl_window_background_build(&source, &w->image.seed,
                                                                     scratch, p.bytes, &w->mesh,
                                                                     scratch + p.bytes, p.bytes);
    } else
        status = sgx535_window_mesh_background_build(&frozen->scene, &w->image.seed, scratch,
                                                     p.bytes, &w->mesh, scratch + p.bytes, p.bytes);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = construct(frozen, policy, w);
    if (POULSBACK_SELFCHECK && status == SGX535_TASK_BLOCKED &&
        (!same_request(r, frozen) ||
         !copied_inputs(r, scratch, &w->image.seed.foreground.geometry, w->mesh.fragment)))
        return SGX535_TASK_UNSUPPORTED;
    return status;
}
enum sgx535_task_status
sgx535_depth_window_mesh_build(const struct sgx535_depth_window_mesh_request *r,
                               struct sgx535_depth_window_mesh_task *out, uint8_t *geometry,
                               uint32_t capacity, struct sgx535_depth_window_mesh_workspace *w,
                               uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, sizeof(*r), r, 0, 0, 0, 0, out, sizeof(*out), geometry, capacity, w, scratch,
                   cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, &fixed_depth, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->image;
        memcpy(geometry, scratch, out->seed.foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_validate(
    const struct sgx535_depth_window_mesh_request *r,
    const struct sgx535_depth_window_mesh_task *out, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, sizeof(*r), r, 0, 0, 0, 0, out, sizeof(*out), geometry, bytes, w, scratch,
                   cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, &fixed_depth, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->image.seed.foreground.geometry.bytes ||
                    memcmp(out, &w->image, sizeof(*out)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static int owner_graph(const struct sgx535_depth_window_mesh_task *p,
                       const struct sgx535_depth_window_mesh_binding *b)
{
    const struct sgx535_depth_window_mesh_owner *d = &b->depth;
    uint64_t bytes = (p->depth.allocation_bytes + 4095u) & ~UINT64_C(4095);
    uint32_t i;
    if (!d->storage_id || d->reserved ||
        !sgx535_task_extent_in(SGX535_TASK_FULL_ADDRESS_FIRST, UINT64_C(0x100000000),
                               d->gpu_va_bytes, d->allocation_bytes) ||
        (d->gpu_va_bytes & 4095u) || d->allocation_bytes < bytes || (d->allocation_bytes & 4095u) ||
        !bytes || bytes > 0x10000000u || d->access != 3 ||
        /* ZLS takes the top four address bits once, the rest per base. */
        d->gpu_va_bytes >> 28 != (d->gpu_va_bytes + d->allocation_bytes - 1u) >> 28)
        return 0;
    for (i = 0; i < 9; i++) {
        uint64_t id, va, size;
        if (i < 8) {
            id = b->scene.roles[i].storage_id;
            va = b->scene.roles[i].gpu_va_bytes;
            size = b->scene.roles[i].allocation_bytes;
        } else {
            id = b->background.resources[2].storage_id;
            va = b->background.resources[2].gpu_va_bytes;
            size = b->background.resources[2].allocation_bytes;
        }
        if (id == d->storage_id ||
            (va < d->gpu_va_bytes + d->allocation_bytes && d->gpu_va_bytes < va + size))
            return 0;
    }
    return 1;
}
static int bind_payload(struct sgx535_depth_window_mesh_bound *out)
{
    uint32_t i;
    for (i = 0; i < out->image.relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &out->image.relocations[i];
        const struct sgx535_window_mesh_section *s = &out->image.sections[r->section];
        const struct sgx535_task_launch_role *owner = &out->binding.scene.roles[r->role];
        uint64_t va = owner->gpu_va_bytes + r->resource_byte_offset, value;
        uint8_t *dst;
        if (r->resource_byte_offset > owner->allocation_bytes ||
            r->span_bytes > owner->allocation_bytes - r->resource_byte_offset || va > 0xffffffffu ||
            (va & (r->alignment - 1u)))
            return 0;
        dst = payload(&out->image, s->source, s->byte_offset + r->section_byte_offset, 4);
        if (!dst)
            return 0;
        if (r->kind == SGX535_PACKET_DRI_OFFSET)
            value = (va >> r->right_shift) << r->left_shift;
        else {
            struct sgx535_use_code_bank_input code;
            struct sgx535_pds_use_link_projection link;
            uint32_t index;
            if (r->role != SGX535_TASK_USE_VERTEX && r->role != SGX535_TASK_USE_FRAGMENT)
                return 0;
            index = r->role - SGX535_TASK_USE_VERTEX;
            code = (struct sgx535_use_code_bank_input){
                (uint32_t)owner->gpu_va_bytes, (uint32_t)owner->allocation_bytes, (uint32_t)va,
                out->binding.scene.bank_index[index], index};
            if (sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
                return 0;
            if (r->kind == SGX535_PACKET_USE_BANK)
                value = link.bank_selector_fields;
            else if (r->kind == SGX535_PACKET_USE_ENTRY)
                value = link.address_fields;
            else
                return 0;
        }
        put(dst, (word(dst) & ~r->mask) | ((uint32_t)value & r->mask));
        out->resolved_relocations++;
    }
    return 1;
}
static enum sgx535_task_status bind_construct_owner(
    const struct sgx535_depth_window_mesh_request *r, const struct depth_policy *policy,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *b, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap, bool full_owner)
{
    struct sgx535_depth_window_mesh_request *frozen = &w->bind_request;
    struct sgx535_depth_window_mesh_binding *binding = &w->bind_binding;
    struct sgx535_depth_window_mesh_bound *out = &w->bound;
    struct sgx535_window_mesh_storage storage;
    enum sgx535_task_status status;
    uint32_t i, j;
    *frozen = *r;
    *binding = *b;
    status = regenerate(frozen, policy, w, scratch, cap);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    storage = w->image.seed.foreground.geometry;
    if (bytes != storage.bytes || memcmp(p, &w->image, sizeof(*p)) ||
        memcmp(geometry, scratch, bytes))
        return SGX535_TASK_INVALID;
    if (policy->gl_window) {
        struct sgx535_window_mesh_gl_window_background_request source = {
            .version = frozen->scene.version,
            .packed_background_argb = frozen->scene.packed_background_argb,
            .reserved = {frozen->scene.reserved[0], frozen->scene.reserved[1]},
            .foreground = frozen->scene.foreground};
        status =
            policy->io == SGX535_WINDOW_GENERIC0_F32 && policy->single_sample && full_owner
                ? sgx535_window_mesh_single_sample_background_f32_bind_full_owner(
                      &source, policy->single_sample, &w->image.seed, geometry, bytes,
                      &binding->scene, &binding->background, &w->seed_bound, &w->mesh,
                      scratch + bytes, cap - bytes)
            : policy->single_sample && full_owner
                ? sgx535_window_mesh_single_sample_background_bind_full_owner(
                      &source, policy->single_sample, &w->image.seed, geometry, bytes,
                      &binding->scene, &binding->background, &w->seed_bound, &w->mesh,
                      scratch + bytes, cap - bytes, policy->raw_mask)
            : policy->single_sample
                ? sgx535_window_mesh_single_sample_background_bind(
                      &source, policy->single_sample, &w->image.seed, geometry, bytes,
                      &binding->scene, &binding->background, &w->seed_bound, &w->mesh,
                      scratch + bytes, cap - bytes)
                : sgx535_window_mesh_gl_window_background_bind(
                      &source, &w->image.seed, geometry, bytes, &binding->scene,
                      &binding->background, &w->seed_bound, &w->mesh, scratch + bytes, cap - bytes);
    } else
        status = sgx535_window_mesh_background_bind(
            &frozen->scene, &w->image.seed, geometry, bytes, &binding->scene, &binding->background,
            &w->seed_bound, &w->mesh, scratch + bytes, cap - bytes);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (!owner_graph(&w->image, binding))
        return SGX535_TASK_INVALID;
    memset(out, 0, sizeof(*out));
    out->version = policy->version;
    out->binding = *binding;
    out->image = w->image;
    out->background = w->seed_bound.background;
    out->missing_launch_contracts = w->seed_bound.foreground.missing_launch_contracts;
    out->missing_depth_contracts = SGX535_DEPTH_MISSING_ALL;
    memcpy(out->bank_base, w->seed_bound.foreground.bank_base, sizeof(out->bank_base));
    out->ta_stream_va_bytes =
        (uint32_t)binding->scene.roles[SGX535_TASK_PDS].gpu_va_bytes + TA_OFFSET;
    out->event_pds_va_bytes = w->seed_bound.foreground.event_pds_va_bytes;
    out->reset_actions = 2;
    out->required_isp_reset[0] =
        (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, 0x80, 0x20, 0};
    out->required_isp_reset[1] =
        (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, 0x80, 0, 0};
    for (i = 0; i < SGX535_DEPTH_ADDRESS_PAIRS; i++)
        out->depth_base_words[i] = (uint32_t)binding->depth.gpu_va_bytes &
                                   out->image.depth.address_pairs[i].source_relocation_mask;
    for (i = 0; i < SGX535_TASK_LAUNCH_STAGE_COUNT; i++) {
        out->stages[i] = w->seed_bound.foreground.stages[i];
        for (j = 0; j < out->stages[i].count; j++) {
            struct sgx535_engine121_action *a = &out->stages[i].actions[j];
            uint32_t k;
            if (i == SGX535_TASK_LAUNCH_TA && a->offset == 0x238)
                a->value = out->ta_stream_va_bytes;
            if (i != SGX535_TASK_LAUNCH_RASTER)
                continue;
            if (a->offset == 0x480)
                a->value = out->image.depth.zls_control;
            if (a->offset == 0x4bc)
                a->value = BGOBJ_DRAW_MASK;
            for (k = 0; k < SGX535_DEPTH_ADDRESS_PAIRS; k++)
                if (a->offset == out->image.depth.address_pairs[k].register_offset)
                    a->value = out->depth_base_words[k];
        }
    }
    if (!bind_payload(out))
        return SGX535_TASK_INVALID;
    return same_request(r, frozen) && !memcmp(b, binding, sizeof(*binding)) &&
                   !memcmp(p, &w->image, sizeof(*p)) && !memcmp(geometry, scratch + bytes, bytes) &&
                   copied_inputs(r, geometry, &storage, w->mesh.fragment)
               ? SGX535_TASK_BLOCKED
               : SGX535_TASK_UNSUPPORTED;
}
static enum sgx535_task_status
bind_construct(const struct sgx535_depth_window_mesh_request *r, const struct depth_policy *policy,
               const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry,
               uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
               struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return bind_construct_owner(r, policy, p, geometry, bytes, b, w, scratch, cap, false);
}
enum sgx535_task_status sgx535_depth_window_mesh_bind(
    const struct sgx535_depth_window_mesh_request *r, const struct sgx535_depth_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
    struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!p || !b ||
        !arguments(r, sizeof(*r), r, p, sizeof(*p), b, sizeof(*b), out, sizeof(*out), geometry,
                   bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, &fixed_depth, p, geometry, bytes, b, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_bound_validate(
    const struct sgx535_depth_window_mesh_request *r, const struct sgx535_depth_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
    const struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!p || !b ||
        !arguments(r, sizeof(*r), r, p, sizeof(*p), b, sizeof(*b), out, sizeof(*out), geometry,
                   bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, &fixed_depth, p, geometry, bytes, b, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status sgx535_depth_window_mesh_preserve_build(
    const struct sgx535_depth_window_mesh_request *r, struct sgx535_depth_window_mesh_task *out,
    uint8_t *geometry, uint32_t capacity, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, sizeof(*r), r, 0, 0, 0, 0, out, sizeof(*out), geometry, capacity, w, scratch,
                   cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, &preserved_depth, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->image;
        memcpy(geometry, scratch, out->seed.foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_preserve_validate(
    const struct sgx535_depth_window_mesh_request *r,
    const struct sgx535_depth_window_mesh_task *out, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, sizeof(*r), r, 0, 0, 0, 0, out, sizeof(*out), geometry, bytes, w, scratch,
                   cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, &preserved_depth, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->image.seed.foreground.geometry.bytes ||
                    memcmp(out, &w->image, sizeof(*out)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status sgx535_depth_window_mesh_preserve_bind(
    const struct sgx535_depth_window_mesh_request *r, const struct sgx535_depth_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
    struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!p || !b ||
        !arguments(r, sizeof(*r), r, p, sizeof(*p), b, sizeof(*b), out, sizeof(*out), geometry,
                   bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, &preserved_depth, p, geometry, bytes, b, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_preserve_bound_validate(
    const struct sgx535_depth_window_mesh_request *r, const struct sgx535_depth_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
    const struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!p || !b ||
        !arguments(r, sizeof(*r), r, p, sizeof(*p), b, sizeof(*b), out, sizeof(*out), geometry,
                   bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, &preserved_depth, p, geometry, bytes, b, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status
sgx535_depth_window_mesh_native_admission(const struct sgx535_depth_window_mesh_bound *p)
{
    (void)p;
    return SGX535_TASK_UNSUPPORTED;
}

static int state_policy(const struct sgx535_depth_window_mesh_state_request *r,
                        struct depth_policy *policy)
{
    if (!r || r->version != SGX535_DEPTH_WINDOW_MESH_STATE_VERSION ||
        r->compare > SGX535_DEPTH_ALWAYS || r->write_enable > 1 || r->reserved)
        return 0;
    *policy = (struct depth_policy){SGX535_DEPTH_WINDOW_MESH_STATE_VERSION,
                                    r->compare,
                                    r->write_enable,
                                    SGX535_DEPTH_FRESH,
                                    1,
                                    0,
                                    0,
                                    false,
                                    SGX535_WINDOW_PRIMARY_COLOR};
    return 1;
}
enum sgx535_task_status
sgx535_depth_window_mesh_state_build(const struct sgx535_depth_window_mesh_state_request *r,
                                     struct sgx535_depth_window_mesh_task *out, uint8_t *geometry,
                                     uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
                                     uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_state_request frozen;
    struct depth_policy policy;
    enum sgx535_task_status status;
    if (!r || !arguments(r, sizeof(*r), &r->mesh, 0, 0, 0, 0, out, sizeof(*out), geometry, bytes, w,
                         scratch, cap))
        return SGX535_TASK_INVALID;
    frozen = *r;
    if (!state_policy(&frozen, &policy))
        return SGX535_TASK_UNSUPPORTED;
    status = regenerate(&r->mesh, &policy, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED && memcmp(r, &frozen, sizeof(frozen)))
        status = SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->image;
        memcpy(geometry, scratch, out->seed.foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_state_validate(
    const struct sgx535_depth_window_mesh_state_request *r,
    const struct sgx535_depth_window_mesh_task *out, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_state_request frozen;
    struct depth_policy policy;
    enum sgx535_task_status status;
    if (!r || !arguments(r, sizeof(*r), &r->mesh, 0, 0, 0, 0, out, sizeof(*out), geometry, bytes, w,
                         scratch, cap))
        return SGX535_TASK_INVALID;
    frozen = *r;
    if (!state_policy(&frozen, &policy))
        return SGX535_TASK_UNSUPPORTED;
    status = regenerate(&r->mesh, &policy, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED && memcmp(r, &frozen, sizeof(frozen)))
        return SGX535_TASK_UNSUPPORTED;
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->image.seed.foreground.geometry.bytes ||
                    memcmp(out, &w->image, sizeof(*out)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static enum sgx535_task_status
state_bind_construct(const struct sgx535_depth_window_mesh_state_request *r,
                     const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry,
                     uint32_t bytes, const struct sgx535_depth_window_mesh_binding *b,
                     const void *out, unsigned long out_bytes,
                     struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_state_request frozen;
    struct depth_policy policy;
    enum sgx535_task_status status;
    if (!r || !p || !b ||
        !arguments(r, sizeof(*r), &r->mesh, p, sizeof(*p), b, sizeof(*b), out, out_bytes, geometry,
                   bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    frozen = *r;
    if (!state_policy(&frozen, &policy))
        return SGX535_TASK_UNSUPPORTED;
    status = bind_construct(&r->mesh, &policy, p, geometry, bytes, b, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && memcmp(r, &frozen, sizeof(frozen))
               ? SGX535_TASK_UNSUPPORTED
               : status;
}
enum sgx535_task_status sgx535_depth_window_mesh_state_bind(
    const struct sgx535_depth_window_mesh_state_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *b, struct sgx535_depth_window_mesh_bound *out,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status =
        state_bind_construct(r, p, geometry, bytes, b, out, sizeof(*out), w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_state_bound_validate(
    const struct sgx535_depth_window_mesh_state_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *b,
    const struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status =
        state_bind_construct(r, p, geometry, bytes, b, out, sizeof(*out), w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}

#include "sgx535_depth_window_mesh_gl_window.inc"

#include "sgx535_depth_window_mesh_single_sample.inc"

struct ordered_plan {
    uint32_t groups, vertices, sections, relocations, payload, source, geometry;
    uint32_t workspace, allocation, ta_bytes;
};
/* A TA stream relocation's section until the stream follows the last group. */
#define ORDERED_TA_STREAM UINT32_MAX
struct ordered_work {
    /* Comparisons run only after packet construction has finished. Reuse its
     * existing off-stack storage without changing the caller's workspace. */
    union {
        struct sgx535_depth_window_mesh_workspace packet;
        struct sgx535_depth_window_mesh_ordered_view normalized[2];
        struct sgx535_depth_window_mesh_ordered_bound_view normalized_bound[2];
    };
    struct sgx535_depth_window_mesh_task task;
    struct sgx535_depth_window_mesh_bound bound;
    uint32_t first[SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u];
    uint32_t selected_sections[SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u];
    bool own[SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u]; /* emitted for the current group */
    uint32_t role_end[SGX535_TASK_IMAGE_ROLES];
};
_Static_assert(sizeof(((struct ordered_work *)0)->normalized) <=
                       sizeof(struct sgx535_depth_window_mesh_workspace) &&
                   sizeof(((struct ordered_work *)0)->normalized_bound) <=
                       sizeof(struct sgx535_depth_window_mesh_workspace),
               "ordered comparison storage must fit the existing packet workspace");
_Static_assert(_Alignof(struct sgx535_depth_window_mesh_ordered_view) <=
                       _Alignof(struct sgx535_depth_window_mesh_workspace) &&
                   _Alignof(struct sgx535_depth_window_mesh_ordered_bound_view) <=
                       _Alignof(struct sgx535_depth_window_mesh_workspace),
               "ordered comparison alignment must preserve the existing workspace");
struct sgx535_depth_window_mesh_ordered {
    struct sgx535_depth_window_mesh_ordered_view view, frozen;
    struct ordered_plan plan;
};
static bool ordered_is_f32(const struct sgx535_depth_window_mesh_ordered *o)
{
    return !!((o->view.missing_mesh_contracts | o->frozen.missing_mesh_contracts) &
              SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE);
}
struct sgx535_depth_window_mesh_ordered_bound {
    struct sgx535_depth_window_mesh_ordered_bound_view view, frozen;
    struct sgx535_depth_window_mesh_ordered *source;
    uint32_t allocation;
};
static void *ordered_alloc(uint32_t bytes)
{
#ifdef __KERNEL__
    return kvzalloc(bytes, GFP_KERNEL);
#else
    return calloc(1, bytes);
#endif
}
static void ordered_free(struct sgx535_depth_window_mesh_ordered *o)
{
#ifdef __KERNEL__
    kvfree(o);
#else
    free(o);
#endif
}
void sgx535_depth_window_mesh_ordered_destroy(struct sgx535_depth_window_mesh_ordered *o)
{
    if (!o || !ordered_is_f32(o))
        ordered_free(o);
}
static uint32_t ordered_align(uint32_t n)
{
    return (n + 31u) & ~31u;
}
static int ordered_plan(const struct sgx535_depth_window_mesh_single_sample_request *r,
                        uint32_t count, struct ordered_plan *p)
{
    uint64_t source = 0, payload_bytes = 0, workspace, allocation;
    struct sgx535_window_mesh_storage geometry;
    uint32_t i;
    memset(p, 0, sizeof(*p));
    if (!count || count > SGX535_WINDOW_MESH_MAX_VERTICES / 3u ||
        !span(r, (unsigned long)count * sizeof(*r)))
        return -EINVAL;
    p->groups = count;
    /* A later group adds seven sections, and an eighth when its vertex USE
     * program differs from the first group's (ordered_own_vertex_use). */
    p->sections = 20u + 8u * (count - 1u);
    p->relocations = 52u * count;
    p->ta_bytes = 68u + 28u * count;
    for (i = 0; i < count; i++) {
        const struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
        if (m->fragment_count < 5 || m->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
            m->uniform_word_count >= SGX535_FRAGMENT_UNIFORM_WORDS ||
            (!!m->uniform_word_count != !!m->uniforms) ||
            (m->uniform_word_count &&
             (m->interpolation != SGX535_WINDOW_MESH_NO_INPUT ||
              !span(m->uniforms, (unsigned long)m->uniform_word_count * 4u))) ||
            sgx535_window_mesh_input_storage_plan(m, &geometry) != SGX535_TASK_BLOCKED ||
            m->order != SGX535_WINDOW_MESH_DEINDEXED ||
            m->vertex_count > SGX535_WINDOW_MESH_MAX_VERTICES - p->vertices)
            return -E2BIG;
        if (!span(m->vertices, (unsigned long)m->vertex_count * sizeof(*m->vertices)) ||
            !span(m->indices, (unsigned long)m->index_count * sizeof(*m->indices)) ||
            !span(m->fragment, (unsigned long)m->fragment_count * sizeof(*m->fragment)))
            return -EINVAL;
        p->vertices += m->vertex_count;
        if (geometry.bytes > p->geometry)
            p->geometry = geometry.bytes;
        source += ordered_align(m->vertex_count * sizeof(*m->vertices)) +
                  ordered_align(m->index_count * sizeof(*m->indices)) +
                  ordered_align(m->fragment_count * sizeof(*m->fragment));
        source += ordered_align(m->uniform_word_count * 4u);
        if (m->uniform_word_count)
            p->sections += i ? 2u : 1u;
        if (m->uniform_word_count)
            p->relocations += 5u;
        payload_bytes += geometry.bytes + SGX535_DEPTH_WINDOW_MESH_PDS_BYTES +
                         SGX535_WINDOW_MESH_VERTEX_USE_BYTES +
                         SGX535_WINDOW_MESH_FRAGMENT_STORAGE_BYTES + 8u +
                         (m->uniform_word_count ? 96u + (m->uniform_word_count + 1u) * 4u : 0u);
    }
    payload_bytes += p->ta_bytes + 32u * p->sections;
    workspace = ordered_align(sizeof(struct ordered_work)) +
                2u * (uint64_t)ordered_align(count * sizeof(*r)) + source +
                3u * (uint64_t)ordered_align(p->geometry) + ordered_align(p->ta_bytes);
    allocation = ordered_align(sizeof(struct sgx535_depth_window_mesh_ordered)) +
                 ordered_align(count * sizeof(struct sgx535_depth_window_mesh_ordered_group)) +
                 ordered_align(p->sections * sizeof(struct sgx535_window_mesh_section)) +
                 ordered_align(p->relocations * sizeof(struct sgx535_task_packet_reloc)) +
                 payload_bytes;
    if (source > 0xffffffffu || payload_bytes > 0xffffffffu || workspace > 0xffffffffu ||
        allocation > 0xffffffffu)
        return -E2BIG;
    p->source = (uint32_t)source;
    p->payload = (uint32_t)payload_bytes;
    p->workspace = (uint32_t)workspace;
    p->allocation = (uint32_t)allocation;
    return 0;
}
int sgx535_depth_window_mesh_ordered_workspace(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n, uint32_t *bytes)
{
    struct ordered_plan p;
    int ret;
    if (!bytes)
        return -EINVAL;
    ret = ordered_plan(r, n, &p);
    if (!ret)
        *bytes = p.workspace;
    return ret;
}
static bool ordered_span(const void *p, unsigned long n, const void *scratch, uint32_t cap,
                         const void *out, unsigned long out_bytes)
{
    return span(p, n) && !overlap(p, n, scratch, cap) && !overlap(p, n, out, out_bytes);
}
static bool
ordered_payload_arguments(const struct sgx535_depth_window_mesh_single_sample_request *r,
                          const struct ordered_plan *p, const void *out, unsigned long out_bytes,
                          const void *scratch, uint32_t cap)
{
    for (uint32_t i = 0; i < p->groups; i++) {
        const struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
        if (!ordered_span(m->vertices, (unsigned long)m->vertex_count * sizeof(*m->vertices),
                          scratch, cap, out, out_bytes) ||
            !ordered_span(m->indices, (unsigned long)m->index_count * sizeof(*m->indices), scratch,
                          cap, out, out_bytes) ||
            !ordered_span(m->fragment, (unsigned long)m->fragment_count * sizeof(*m->fragment),
                          scratch, cap, out, out_bytes) ||
            (m->uniform_word_count &&
             !ordered_span(m->uniforms, (unsigned long)m->uniform_word_count * 4u, scratch, cap,
                           out, out_bytes)))
            return false;
    }
    return true;
}
static bool ordered_arguments(const struct sgx535_depth_window_mesh_single_sample_request *r,
                              const struct ordered_plan *p, const void *out,
                              unsigned long out_bytes, void *scratch, uint32_t cap)
{
    return span(scratch, cap) && !((unsigned long)scratch & (_Alignof(struct ordered_work) - 1u)) &&
           cap >= p->workspace && span(out, out_bytes) && !overlap(scratch, cap, out, out_bytes) &&
           ordered_span(r, (unsigned long)p->groups * sizeof(*r), scratch, cap, out, out_bytes) &&
           ordered_payload_arguments(r, p, out, out_bytes, scratch, cap);
}
static bool ordered_same(const struct sgx535_depth_window_mesh_single_sample_request *r,
                         const struct sgx535_depth_window_mesh_single_sample_request *original,
                         const struct sgx535_depth_window_mesh_single_sample_request *copy,
                         uint32_t count)
{
    /* Check every retained pointer/count header before following any payload. */
    if (memcmp(r, original, (unsigned long)count * sizeof(*r)))
        return false;
    for (uint32_t i = 0; i < count; i++) {
        const struct sgx535_window_mesh_request *a = &original[i].mesh.scene.foreground;
        const struct sgx535_window_mesh_request *b = &copy[i].mesh.scene.foreground;
        if (memcmp(a->vertices, b->vertices,
                   (unsigned long)a->vertex_count * sizeof(*a->vertices)) ||
            memcmp(a->indices, b->indices, (unsigned long)a->index_count * sizeof(*a->indices)) ||
            memcmp(a->fragment, b->fragment,
                   (unsigned long)a->fragment_count * sizeof(*a->fragment)) ||
            (a->uniform_word_count &&
             memcmp(a->uniforms, b->uniforms, (unsigned long)a->uniform_word_count * 4u)))
            return false;
    }
    return true;
}
static bool ordered_selected(uint32_t group, uint32_t section, bool secondary)
{
    return section != TA && (!group || section == V || section == I || section == FU ||
                             section == VP || section == FP || section == STATE || section == MTE ||
                             ((section == STATIC_SECOND || section == EU) && secondary));
}
bool sgx535_depth_window_mesh_ordered_draw_section(
    const struct sgx535_depth_window_mesh_ordered_view *v, uint32_t i)
{
    /* One group: every section but the TA stream (last) in section_id order. */
    bool secondary = v->section_count == SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u;
    if (v->group_count != 1 || i + 1u >= v->section_count || i == v->ta_stream_section)
        return false;
    return ordered_selected(1, i < TA ? i : i + 1u, secondary);
}
/* A group without fragment inputs has no varyings for the generic F32 linkage
 * to carry: it is built as the primary-colour family in any frame, and its
 * window W must be 1. Frame-level state (background, clear, event, launch
 * registers) is the same in both families; only the group's own sections
 * differ. */
static enum sgx535_window_fragment_io ordered_group_io(enum sgx535_window_fragment_io io,
                                                       const struct sgx535_window_mesh_request *m)
{
    return m->interpolation == SGX535_WINDOW_MESH_NO_INPUT ? SGX535_WINDOW_PRIMARY_COLOR : io;
}
static const uint8_t *ordered_payload(struct sgx535_depth_window_mesh_task *p,
                                      const struct sgx535_window_mesh_section *s,
                                      const uint8_t *geometry)
{
    return s->source == SGX535_DEPTH_WINDOW_MESH_GEOMETRY
               ? geometry + s->byte_offset
               : payload(p, s->source, s->byte_offset, s->byte_count);
}
/* The vertex USE program is read only through its group's vertex PDS. A later
 * group whose program differs from the first group's (a uniform colour, so
 * position only, against vertex colours) gets its own copy; equal programs
 * stay shared. */
static bool ordered_own_vertex_use(const struct sgx535_depth_window_mesh_ordered *o,
                                   struct ordered_work *w, const uint8_t *geometry, uint32_t i)
{
    const struct sgx535_window_mesh_section *s = &w->task.sections[i];
    const struct sgx535_window_mesh_section *first = &o->view.sections[w->first[i]];
    const uint8_t *source;
    if (i != VU)
        return false;
    source = ordered_payload(&w->task, s, geometry);
    return !source || first->byte_count != s->byte_count ||
           memcmp(o->view.bytes + first->byte_offset, source, s->byte_count);
}
static bool ordered_compact_role(uint32_t role)
{
    return role == SGX535_TASK_PDS || role == SGX535_TASK_USE_FRAGMENT ||
           role == SGX535_TASK_VERTEX_DATA;
}
static int ordered_append_cursors(struct ordered_work *w)
{
    const struct sgx535_depth_window_mesh_task *p = &w->task;
    const struct sgx535_window_mesh_task *first = &p->seed.foreground;
    const struct sgx535_background_image *background = &p->seed.background;
    uint32_t i;
    if (p->section_count !=
            SGX535_DEPTH_WINDOW_MESH_SECTIONS + !!first->secondary.physical_sa_words ||
        background->section_count != SGX535_BACKGROUND_SECTIONS)
        return -EOPNOTSUPP;
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++)
        w->role_end[i] = ordered_compact_role(i) ? 0 : first->requirements[i].minimum_bytes;
    /* Allocation floors reserve whole owners, not additional packet positions.
     * Keep every emitted span, including the old TA envelope, and all shared
     * background programs. Scene/DPM reservations are never compacted. */
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &p->sections[i];
        uint32_t limit;
        if (s->role >= SGX535_TASK_IMAGE_ROLES || !s->alignment ||
            (s->alignment & (s->alignment - 1u)) || (s->resource_byte_offset & (s->alignment - 1u)))
            return -EOPNOTSUPP;
        limit = first->requirements[s->role].minimum_bytes;
        if (s->resource_byte_offset > limit || s->byte_count > limit - s->resource_byte_offset)
            return -E2BIG;
        if (ordered_compact_role(s->role) &&
            s->resource_byte_offset + s->byte_count > w->role_end[s->role])
            w->role_end[s->role] = s->resource_byte_offset + s->byte_count;
    }
    for (i = 0; i < background->section_count; i++) {
        const struct sgx535_background_section *s = &background->sections[i];
        uint32_t role, limit;
        if (s->role == SGX535_BACKGROUND_RASTGEOM)
            continue;
        if (s->role != SGX535_BACKGROUND_USE && s->role != SGX535_BACKGROUND_PDS)
            return -EOPNOTSUPP;
        role = s->role == SGX535_BACKGROUND_USE ? SGX535_TASK_USE_FRAGMENT : SGX535_TASK_PDS;
        limit = first->requirements[role].minimum_bytes;
        if (!s->alignment || (s->alignment & (s->alignment - 1u)) ||
            (s->resource_offset & (s->alignment - 1u)) || s->resource_offset > limit ||
            s->byte_count > limit - s->resource_offset)
            return -E2BIG;
        if (s->resource_offset + s->byte_count > w->role_end[role])
            w->role_end[role] = s->resource_offset + s->byte_count;
    }
    return 0;
}
static int ordered_sections(struct sgx535_depth_window_mesh_ordered *o, struct ordered_work *w,
                            const uint8_t *geometry, uint32_t group)
{
    struct sgx535_depth_window_mesh_ordered_view *v = &o->view;
    struct sgx535_window_mesh_section *sections = (void *)v->sections;
    uint8_t *bytes = (void *)v->bytes;
    uint32_t i;
    for (i = 0; i < w->task.section_count; i++) {
        const struct sgx535_window_mesh_section *s = &w->task.sections[i];
        struct sgx535_window_mesh_section *d;
        const uint8_t *source;
        w->own[i] =
            ordered_selected(group, i, w->task.seed.foreground.secondary.physical_sa_words) ||
            ordered_own_vertex_use(o, w, geometry, i);
        if (!w->own[i]) {
            w->selected_sections[i] = w->first[i];
            continue;
        }
        if (v->section_count >= o->plan.sections - 1u)
            return -E2BIG;
        w->selected_sections[i] = v->section_count++;
        if (!group)
            w->first[i] = w->selected_sections[i];
        d = &sections[w->selected_sections[i]];
        *d = *s;
        d->source = 0;
        d->byte_offset = ordered_align(v->byte_count);
        if (group)
            d->resource_byte_offset =
                (w->role_end[s->role] + s->alignment - 1u) & ~(s->alignment - 1u);
        if (d->byte_offset > o->plan.payload || s->byte_count > o->plan.payload - d->byte_offset ||
            d->resource_byte_offset > 0xffffffffu - s->byte_count)
            return -E2BIG;
        source = ordered_payload(&w->task, s, geometry);
        if (!source)
            return -EOPNOTSUPP;
        memcpy(bytes + d->byte_offset, source, s->byte_count);
        v->byte_count = d->byte_offset + s->byte_count;
        if (d->resource_byte_offset + d->byte_count > w->role_end[d->role])
            w->role_end[d->role] = d->resource_byte_offset + d->byte_count;
    }
    return 0;
}
static int ordered_target(const struct sgx535_depth_window_mesh_ordered *o, struct ordered_work *w,
                          struct sgx535_task_packet_reloc *r)
{
    for (uint32_t i = 0; i < w->task.section_count; i++) {
        const struct sgx535_window_mesh_section *s = &w->task.sections[i];
        const struct sgx535_window_mesh_section *d;
        uint32_t offset;
        if (s->role != r->role || r->resource_byte_offset < s->resource_byte_offset)
            continue;
        offset = r->resource_byte_offset - s->resource_byte_offset;
        if (offset > s->byte_count || r->span_bytes > s->byte_count - offset)
            continue;
        if (i == TA)
            return -EOPNOTSUPP;
        d = &o->view.sections[w->selected_sections[i]];
        /* Shared state/vertex USE and secondary PDS must be identical, not
         * silently inherited from the first program's different grammar. */
        if (d->byte_count != s->byte_count || d->role != s->role ||
            (!ordered_selected(1, i, w->task.seed.foreground.secondary.physical_sa_words) &&
             memcmp(o->view.bytes + d->byte_offset, ordered_payload(&w->task, s, NULL),
                    s->byte_count)))
            return -EOPNOTSUPP;
        r->resource_byte_offset = d->resource_byte_offset + offset;
        return 0;
    }
    return r->role == SGX535_TASK_TARGET ? 0 : -EOPNOTSUPP;
}
static int ordered_relocations(struct sgx535_depth_window_mesh_ordered *o, struct ordered_work *w,
                               uint32_t group)
{
    struct sgx535_task_packet_reloc *dst = (void *)o->view.relocations;
    for (uint32_t i = 0; i < w->task.relocation_count; i++) {
        struct sgx535_task_packet_reloc r = w->task.relocations[i];
        int ret;
        if (r.section == TA) {
            if (group && (r.section_byte_offset < 56 || r.section_byte_offset >= 84))
                continue;
            if (r.section_byte_offset >= 84)
                r.section_byte_offset += 28u * (o->plan.groups - 1u);
            else if (r.section_byte_offset >= 56)
                r.section_byte_offset += group * 28u;
            r.section = ORDERED_TA_STREAM; /* its index is known after the last group */
        } else {
            if (!w->own[r.section])
                continue;
            r.section = w->selected_sections[r.section];
        }
        ret = ordered_target(o, w, &r);
        if (ret)
            return ret;
        if (o->view.relocation_count >= o->plan.relocations)
            return -E2BIG;
        dst[o->view.relocation_count++] = r;
    }
    return 0;
}
static int ordered_extents(struct sgx535_depth_window_mesh_ordered *o)
{
    struct sgx535_depth_window_mesh_ordered_view *v = &o->view;
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        if (s->source || s->role >= 8 || !s->alignment || (s->alignment & (s->alignment - 1u)) ||
            (s->resource_byte_offset & (s->alignment - 1u)) || s->byte_offset > v->byte_count ||
            s->byte_count > v->byte_count - s->byte_offset ||
            s->resource_byte_offset > v->requirements[s->role].minimum_bytes ||
            s->byte_count > v->requirements[s->role].minimum_bytes - s->resource_byte_offset)
            return -EOPNOTSUPP;
        for (uint32_t j = 0; j < i; j++) {
            const struct sgx535_window_mesh_section *t = &v->sections[j];
            if (s->role == t->role &&
                s->resource_byte_offset < t->resource_byte_offset + t->byte_count &&
                t->resource_byte_offset < s->resource_byte_offset + s->byte_count)
                return -EOPNOTSUPP;
        }
    }
    for (uint32_t i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &v->relocations[i];
        const struct sgx535_window_mesh_section *s;
        if (r->section >= v->section_count || r->role >= 8)
            return -EOPNOTSUPP;
        s = &v->sections[r->section];
        if (!r->alignment || (r->alignment & (r->alignment - 1u)) ||
            (r->resource_byte_offset & (r->alignment - 1u)) ||
            r->section_byte_offset > s->byte_count || 4u > s->byte_count - r->section_byte_offset ||
            r->resource_byte_offset > v->requirements[r->role].minimum_bytes ||
            r->span_bytes > v->requirements[r->role].minimum_bytes - r->resource_byte_offset)
            return -EOPNOTSUPP;
    }
    for (uint32_t i = 0; i < 2; i++) {
        struct sgx535_use_code_bank_input code = {
            0, v->requirements[SGX535_TASK_USE_VERTEX + i].minimum_bytes, 0, 0, i};
        struct sgx535_use_code_bank_projection projected;
        if (sgx535_use_code_bank_project(&code, &projected) != SGX535_USE_CODE_BANK_OK)
            return -E2BIG;
    }
    return 0;
}
/* Stage R in SCRATCH: a zeroed ordered_work, the original and copied request
 * headers, then every copied payload, the copies' pointers moved onto them.
 * *GEOMETRY receives the first byte after the payloads. */
static int ordered_stage(const struct sgx535_depth_window_mesh_single_sample_request *r,
                         uint32_t count, const struct ordered_plan *p, void *out_span,
                         unsigned long out_bytes, void *scratch, uint32_t cap,
                         enum sgx535_window_fragment_io io,
                         struct sgx535_depth_window_mesh_single_sample_request **original_out,
                         struct sgx535_depth_window_mesh_single_sample_request **copy_out,
                         uint8_t **geometry)
{
    struct ordered_plan captured;
    struct ordered_work *w = scratch;
    struct sgx535_depth_window_mesh_single_sample_request *original, *copy;
    uint8_t *data;
    if ((io != SGX535_WINDOW_PRIMARY_COLOR && io != SGX535_WINDOW_GENERIC0_F32) ||
        !ordered_arguments(r, p, out_span, out_bytes, scratch, cap))
        return -EINVAL;
    /* The builders initialise the workspace, task and bound parts they read
     * (zeroing all 225 KB cost 7 % of a frame); only the section bookkeeping
     * starts at zero. */
    memset(w->first, 0, sizeof(w->first));
    memset(w->selected_sections, 0, sizeof(w->selected_sections));
    memset(w->own, 0, sizeof(w->own));
    memset(w->role_end, 0, sizeof(w->role_end));
    data = (uint8_t *)scratch + ordered_align(sizeof(*w));
    original = (void *)data;
    data += ordered_align(count * sizeof(*r));
    copy = (void *)data;
    data += ordered_align(count * sizeof(*r));
    memcpy(original, r, count * sizeof(*r));
    /* Planning does not authorize later copied counts/pointers. Freeze the
     * complete headers, re-plan their extents, then reject scratch/output
     * aliases before following even the first captured payload pointer. */
    if (POULSBACK_SELFCHECK &&
        (ordered_plan(original, count, &captured) || memcmp(p, &captured, sizeof(*p)) ||
         !ordered_payload_arguments(original, &captured, out_span, out_bytes, scratch, cap)))
        return -ESTALE;
    memcpy(copy, original, count * sizeof(*r));
    for (uint32_t i = 0; i < count; i++) {
        struct sgx535_window_mesh_request *m = &copy[i].mesh.scene.foreground;
        uint32_t n = m->vertex_count * sizeof(*m->vertices);
        memcpy(data, m->vertices, n);
        m->vertices = (void *)data;
        data += ordered_align(n);
        n = m->index_count * sizeof(*m->indices);
        memcpy(data, m->indices, n);
        m->indices = (void *)data;
        data += ordered_align(n);
        n = m->fragment_count * sizeof(*m->fragment);
        memcpy(data, m->fragment, n);
        m->fragment = (void *)data;
        data += ordered_align(n);
        n = m->uniform_word_count * 4u;
        if (n) {
            memcpy(data, m->uniforms, n);
            m->uniforms = (void *)data;
            data += ordered_align(n);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct sgx535_window_mesh_request *m = &copy[i].mesh.scene.foreground;
        /* Window-space only: a clip-space W is the MTE's to divide by. */
        for (uint32_t j = 0; j < m->vertex_count; j++)
            if (ordered_group_io(io, m) == SGX535_WINDOW_PRIMARY_COLOR &&
                m->version != SGX535_WINDOW_MESH_CLIP_VERSION &&
                m->vertices[j].window_xyz_reciprocal_w[3] != 0x3f800000u)
                return -EOPNOTSUPP;
    }
    *original_out = original;
    *copy_out = copy;
    *geometry = data;
    return 0;
}
static int ordered_create_io(const struct sgx535_depth_window_mesh_single_sample_request *r,
                             uint32_t count, struct sgx535_depth_window_mesh_ordered **out,
                             void *out_span, void *scratch, uint32_t cap,
                             enum sgx535_window_fragment_io io)
{
    struct ordered_plan p;
    struct ordered_work *w = scratch;
    struct sgx535_depth_window_mesh_single_sample_request *original, *copy;
    struct sgx535_depth_window_mesh_ordered *o = NULL;
    struct sgx535_depth_window_mesh_ordered_group *groups;
    uint8_t *data, *geometry, *temporary, *ta;
    uint32_t first = 0;
    int ret = ordered_plan(r, count, &p);
    if (ret)
        return ret;
    ret = ordered_stage(r, count, &p, out_span, sizeof(*out), scratch, cap, io, &original, &copy,
                        &geometry);
    if (ret)
        return ret;
    temporary = geometry + ordered_align(p.geometry);
    ta = temporary + 2u * ordered_align(p.geometry);
    memset(ta, 0, p.ta_bytes);
    o = ordered_alloc(p.allocation);
    if (!o)
        return -ENOMEM;
    o->plan = p;
    data = (uint8_t *)o + ordered_align(sizeof(*o));
    o->view.groups = groups = (void *)data;
    data += ordered_align(count * sizeof(*groups));
    o->view.sections = (void *)data;
    data += ordered_align(p.sections * sizeof(*o->view.sections));
    o->view.relocations = (void *)data;
    data += ordered_align(p.relocations * sizeof(*o->view.relocations));
    o->view.bytes = data;
    o->view.group_count = count;
    o->view.vertex_count = p.vertices;
    o->view.missing_packet_contracts = SGX535_PACKET_MISSING_ALL;
    o->view.missing_mesh_contracts = SGX535_WINDOW_MESH_MISSING_ALL;
    if (io == SGX535_WINDOW_GENERIC0_F32)
        o->view.missing_mesh_contracts |= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    o->view.missing_depth_contracts = SGX535_DEPTH_MISSING_ALL;
    for (uint32_t i = 0; i < count; i++) {
        const struct sgx535_window_mesh_request *m = &copy[i].mesh.scene.foreground;
        struct sgx535_single_sample_lowering lowering;
        enum sgx535_task_status status;
        if (i && (copy[i].mesh.format != copy[0].mesh.format ||
                  copy[i].mesh.no_depth != copy[0].mesh.no_depth ||
                  copy[i].mesh.scene.packed_background_argb !=
                      copy[0].mesh.scene.packed_background_argb ||
                  m->width != copy[0].mesh.scene.foreground.width ||
                  m->height != copy[0].mesh.scene.foreground.height)) {
            ret = -EOPNOTSUPP;
            goto done;
        }
        status = ordered_group_io(io, m) == SGX535_WINDOW_GENERIC0_F32
                     ? depth_single_build_io(&copy[i], &w->task, &lowering, geometry, p.geometry,
                                             &w->packet, temporary, 2u * p.geometry, false, io)
                 : m->profile == SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1
                     ? sgx535_depth_window_mesh_single_sample_raw_mask_build_v1(
                           &copy[i], &w->task, &lowering, geometry, p.geometry, &w->packet,
                           temporary, 2u * p.geometry)
                     : sgx535_depth_window_mesh_single_sample_build(
                           &copy[i], &w->task, &lowering, geometry, p.geometry, &w->packet,
                           temporary, 2u * p.geometry);
        if (status != SGX535_TASK_BLOCKED) {
            ret = status == SGX535_TASK_NOMEM ? -ENOMEM : -EOPNOTSUPP;
            goto done;
        }
        if (!i) {
            o->view.scene = w->task.seed.foreground.scene;
            o->view.depth = w->task.depth;
            o->view.background = w->task.seed.background;
            memcpy(o->view.requirements, w->task.seed.foreground.requirements,
                   sizeof(o->view.requirements));
            ret = ordered_append_cursors(w);
            if (ret)
                goto done;
            memcpy(ta, w->task.pds + TA_OFFSET, 56);
            memcpy(ta + 56u + 28u * count, w->task.pds + TA_OFFSET + 84u, 12);
        }
        memcpy(ta + 56u + 28u * i, w->task.pds + TA_OFFSET + 56u, 28);
        ret = ordered_sections(o, w, geometry, i);
        if (ret)
            goto done;
        groups[i] = (struct sgx535_depth_window_mesh_ordered_group){
            .first_vertex = first,
            .vertex_count = m->vertex_count,
            .interpolation = m->interpolation,
            .profile = m->profile,
            .fragment_section = w->selected_sections[FU],
            .vertex_pds_section = w->selected_sections[VP],
            .state_section = w->selected_sections[STATE],
            .mte_section = w->selected_sections[MTE],
            .ta_byte_offset = 56u + 28u * i,
            .window = m->window,
            .samples = copy[i].samples,
            .lowering = lowering};
        first += m->vertex_count;
        ret = ordered_relocations(o, w, i);
        if (ret)
            goto done;
    }
    {
        struct sgx535_window_mesh_section *s = (void *)(o->view.sections + o->view.section_count);
        uint32_t offset = ordered_align(o->view.byte_count);
        uint32_t target = ordered_align(w->role_end[SGX535_TASK_PDS]);
        struct sgx535_task_packet_reloc *relocations = (void *)o->view.relocations;
        if (o->view.section_count > p.sections - 1u || offset > p.payload ||
            p.ta_bytes > p.payload - offset) {
            ret = -E2BIG;
            goto done;
        }
        *s = (struct sgx535_window_mesh_section){
            SGX535_PACKET_TA_STREAM, 0, offset, p.ta_bytes, SGX535_TASK_PDS, target, 16};
        memcpy((uint8_t *)o->view.bytes + offset, ta, p.ta_bytes);
        o->view.ta_stream_section = o->view.section_count++;
        for (uint32_t i = 0; i < o->view.relocation_count; i++)
            if (relocations[i].section == ORDERED_TA_STREAM)
                relocations[i].section = o->view.ta_stream_section;
        o->view.event_pds_section = w->first[EVENT];
        o->view.byte_count = offset + p.ta_bytes;
        w->role_end[SGX535_TASK_PDS] = target + p.ta_bytes;
        for (uint32_t i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
            uint32_t end = w->role_end[i];
            if (end < o->view.requirements[i].minimum_bytes)
                end = o->view.requirements[i].minimum_bytes;
            if (end > UINT32_MAX - 4095u) {
                ret = -E2BIG;
                goto done;
            }
            o->view.requirements[i].minimum_bytes = (end + 4095u) & ~4095u;
        }
    }
    ret = ordered_extents(o);
    if (POULSBACK_SELFCHECK && !ret && !ordered_same(r, original, copy, count))
        ret = -ESTALE;
    if (!ret) {
        o->frozen = o->view;
        *out = o;
        o = NULL;
    }
done:
    ordered_free(o);
    return ret;
}
int sgx535_depth_window_mesh_ordered_create(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    struct sgx535_depth_window_mesh_ordered **out, void *scratch, uint32_t cap)
{
    return ordered_create_io(r, count, out, out, scratch, cap, SGX535_WINDOW_PRIMARY_COLOR);
}
int sgx535_depth_window_mesh_f32_ordered_create(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    struct sgx535_depth_window_mesh_f32_ordered **out, void *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_ordered *fresh = NULL;
    int ret = ordered_create_io(r, count, &fresh, out, scratch, cap, SGX535_WINDOW_GENERIC0_F32);
    if (!ret)
        *out = (struct sgx535_depth_window_mesh_f32_ordered *)fresh;
    return ret;
}
const struct sgx535_depth_window_mesh_ordered_view *
sgx535_depth_window_mesh_ordered_view(const struct sgx535_depth_window_mesh_ordered *o)
{
    return o && !ordered_is_f32(o) ? &o->view : NULL;
}
static int ordered_validate_io(const struct sgx535_depth_window_mesh_single_sample_request *r,
                               uint32_t count, const struct sgx535_depth_window_mesh_ordered *o,
                               void *scratch, uint32_t cap, enum sgx535_window_fragment_io io)
{
    struct sgx535_depth_window_mesh_ordered *fresh = NULL;
    int ret;
    if (!o || ordered_is_f32(o) != (io == SGX535_WINDOW_GENERIC0_F32) ||
        memcmp(&o->view, &o->frozen, sizeof(o->view)) ||
        overlap(o, o->plan.allocation, scratch, cap))
        return -ESTALE;
    ret = ordered_create_io(r, count, &fresh, &fresh, scratch, cap, io);
    if (!ret) {
        struct ordered_work *w = scratch;
        struct sgx535_depth_window_mesh_ordered_view *a = &w->normalized[0], *b = &w->normalized[1];
        *a = o->view;
        *b = fresh->view;
        a->groups = b->groups = NULL;
        a->sections = b->sections = NULL;
        a->relocations = b->relocations = NULL;
        a->bytes = b->bytes = NULL;
        if (memcmp(a, b, sizeof(*a)) ||
            memcmp(o->view.groups, fresh->view.groups, count * sizeof(*a->groups)) ||
            memcmp(o->view.sections, fresh->view.sections,
                   a->section_count * sizeof(*a->sections)) ||
            memcmp(o->view.relocations, fresh->view.relocations,
                   a->relocation_count * sizeof(*a->relocations)) ||
            memcmp(o->view.bytes, fresh->view.bytes, a->byte_count))
            ret = -ESTALE;
    }
    ordered_free(fresh);
    return ret;
}
int sgx535_depth_window_mesh_ordered_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_ordered *o, void *scratch, uint32_t cap)
{
    return ordered_validate_io(r, count, o, scratch, cap, SGX535_WINDOW_PRIMARY_COLOR);
}
int sgx535_depth_window_mesh_f32_ordered_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_f32_ordered *o, void *scratch, uint32_t cap)
{
    return ordered_validate_io(r, count, (const struct sgx535_depth_window_mesh_ordered *)o,
                               scratch, cap, SGX535_WINDOW_GENERIC0_F32);
}
const struct sgx535_depth_window_mesh_ordered_view *
sgx535_depth_window_mesh_f32_ordered_view(const struct sgx535_depth_window_mesh_f32_ordered *p)
{
    const struct sgx535_depth_window_mesh_ordered *o = (const void *)p;
    return o && ordered_is_f32(o) && !memcmp(&o->view, &o->frozen, sizeof(o->view)) ? &o->view
                                                                                    : NULL;
}
void sgx535_depth_window_mesh_f32_ordered_destroy(struct sgx535_depth_window_mesh_f32_ordered *p)
{
    struct sgx535_depth_window_mesh_ordered *o = (void *)p;
    if (!o || (o->frozen.missing_mesh_contracts & SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE))
        ordered_free(o);
}
/* A copy of S at its own address: the view's spans move with it. */
static struct sgx535_depth_window_mesh_ordered *
ordered_clone(const struct sgx535_depth_window_mesh_ordered *s)
{
    struct sgx535_depth_window_mesh_ordered *o = ordered_alloc(s->plan.allocation);
    const uint8_t *from = (const uint8_t *)s;
    uint8_t *to;
    if (!o)
        return NULL;
    to = (uint8_t *)o;
    memcpy(o, s, s->plan.allocation);
    o->view.groups = (const void *)(to + ((const uint8_t *)s->view.groups - from));
    o->view.sections = (const void *)(to + ((const uint8_t *)s->view.sections - from));
    o->view.relocations = (const void *)(to + ((const uint8_t *)s->view.relocations - from));
    o->view.bytes = to + (s->view.bytes - from);
    o->frozen = o->view;
    return o;
}
static bool ordered_equal(const struct sgx535_depth_window_mesh_ordered *a,
                          const struct sgx535_depth_window_mesh_ordered *b, struct ordered_work *w)
{
    struct sgx535_depth_window_mesh_ordered_view *x = &w->normalized[0], *y = &w->normalized[1];
    *x = a->view;
    *y = b->view;
    if (memcmp(&a->view, &a->frozen, sizeof(a->view)) ||
        memcmp(&a->plan, &b->plan, sizeof(a->plan)))
        return false;
    x->groups = y->groups = NULL;
    x->sections = y->sections = NULL;
    x->relocations = y->relocations = NULL;
    x->bytes = y->bytes = NULL;
    return !memcmp(x, y, sizeof(*x)) &&
           !memcmp(a->view.groups, b->view.groups, x->group_count * sizeof(*x->groups)) &&
           !memcmp(a->view.sections, b->view.sections, x->section_count * sizeof(*x->sections)) &&
           !memcmp(a->view.relocations, b->view.relocations,
                   x->relocation_count * sizeof(*x->relocations)) &&
           !memcmp(a->view.bytes, b->view.bytes, x->byte_count);
}
static int ordered_bound_sizes(const struct ordered_plan *p,
                               struct sgx535_depth_window_mesh_ordered_bind_sizes *s)
{
    uint64_t workspace =
        (uint64_t)p->workspace + ordered_align(sizeof(struct sgx535_depth_window_mesh_binding));
    uint64_t bound = ordered_align(sizeof(struct sgx535_depth_window_mesh_ordered_bound)) +
                     (uint64_t)p->payload + p->allocation;
    if (workspace > UINT32_MAX || bound > UINT32_MAX)
        return -E2BIG;
    *s = (struct sgx535_depth_window_mesh_ordered_bind_sizes){(uint32_t)workspace, p->allocation,
                                                              (uint32_t)bound};
    return 0;
}
int sgx535_depth_window_mesh_ordered_bind_sizes(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    struct sgx535_depth_window_mesh_ordered_bind_sizes *out)
{
    struct ordered_plan p;
    struct sgx535_depth_window_mesh_ordered_bind_sizes s;
    int ret = ordered_plan(r, count, &p);
    if (ret)
        return ret;
    if (!span(out, sizeof(*out)) || overlap(r, count * sizeof(*r), out, sizeof(*out)) ||
        !ordered_payload_arguments(r, &p, out, sizeof(*out), NULL, 0))
        return -EINVAL;
    ret = ordered_bound_sizes(&p, &s);
    if (!ret)
        *out = s;
    return ret;
}
static void ordered_bound_free(struct sgx535_depth_window_mesh_ordered_bound *o)
{
    if (o)
        ordered_free(o->source);
#ifdef __KERNEL__
    kvfree(o);
#else
    free(o);
#endif
}
void sgx535_depth_window_mesh_ordered_bound_destroy(
    struct sgx535_depth_window_mesh_ordered_bound *o)
{
    if (!o || (o->source && !ordered_is_f32(o->source)))
        ordered_bound_free(o);
}
static bool ordered_binding_extents(const struct sgx535_depth_window_mesh_ordered_view *v,
                                    const struct sgx535_depth_window_mesh_binding *b)
{
    for (uint32_t i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
        const struct sgx535_task_launch_role *r = &b->scene.roles[i];
        const struct sgx535_task_requirement *q = &v->requirements[i];
        if (r->allocation_bytes < q->minimum_bytes || r->access != q->access ||
            (r->gpu_va_bytes & (q->alignment - 1u)))
            return false;
    }
    /* The existing first-group binder validates complete canonical owners,
     * heaps, background roles, banks and depth. The counted sections add new
     * placements within those same owners and must remain disjoint from BG. */
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        const struct sgx535_task_launch_role *r = &b->scene.roles[s->role];
        if (s->resource_byte_offset > r->allocation_bytes ||
            s->byte_count > r->allocation_bytes - s->resource_byte_offset ||
            ((r->gpu_va_bytes + s->resource_byte_offset) & (s->alignment - 1u)))
            return false;
        for (uint32_t j = 0; j < v->background.section_count; j++) {
            const struct sgx535_background_section *t = &v->background.sections[j];
            uint32_t role = t->role == SGX535_BACKGROUND_USE   ? SGX535_TASK_USE_FRAGMENT
                            : t->role == SGX535_BACKGROUND_PDS ? SGX535_TASK_PDS
                                                               : UINT32_MAX;
            if (s->role == role && s->resource_byte_offset < t->resource_offset + t->byte_count &&
                t->resource_offset < s->resource_byte_offset + s->byte_count)
                return false;
        }
    }
    return true;
}
static bool ordered_bind_payload(struct sgx535_depth_window_mesh_ordered_bound_view *v)
{
    for (uint32_t i = 0; i < v->source->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &v->source->relocations[i];
        const struct sgx535_window_mesh_section *s = &v->source->sections[r->section];
        const struct sgx535_task_launch_role *owner = &v->binding.scene.roles[r->role];
        uint64_t va = owner->gpu_va_bytes + r->resource_byte_offset, value;
        uint8_t *dst = (uint8_t *)v->bytes + s->byte_offset + r->section_byte_offset;
        if (r->resource_byte_offset > owner->allocation_bytes ||
            r->span_bytes > owner->allocation_bytes - r->resource_byte_offset || va > UINT32_MAX ||
            (va & (r->alignment - 1u)))
            return false;
        if (r->kind == SGX535_PACKET_DRI_OFFSET)
            value = (va >> r->right_shift) << r->left_shift;
        else {
            struct sgx535_use_code_bank_input code;
            struct sgx535_pds_use_link_projection link;
            uint32_t index;
            if (r->role != SGX535_TASK_USE_VERTEX && r->role != SGX535_TASK_USE_FRAGMENT)
                return false;
            index = r->role - SGX535_TASK_USE_VERTEX;
            code = (struct sgx535_use_code_bank_input){
                (uint32_t)owner->gpu_va_bytes, (uint32_t)owner->allocation_bytes, (uint32_t)va,
                v->binding.scene.bank_index[index], index};
            if (sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
                return false;
            if (r->kind == SGX535_PACKET_USE_BANK)
                value = link.bank_selector_fields;
            else if (r->kind == SGX535_PACKET_USE_ENTRY)
                value = link.address_fields;
            else
                return false;
        }
        put(dst, (word(dst) & ~r->mask) | ((uint32_t)value & r->mask));
        v->resolved_relocations++;
    }
    return true;
}
static int ordered_bind_io(const struct sgx535_depth_window_mesh_single_sample_request *r,
                           uint32_t count, const struct sgx535_depth_window_mesh_ordered *source,
                           const struct sgx535_depth_window_mesh_binding *binding,
                           struct sgx535_depth_window_mesh_ordered_bound **out, void *out_span,
                           void *scratch, uint32_t cap, enum sgx535_window_fragment_io io)
{
    struct ordered_plan p;
    struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
    struct sgx535_depth_window_mesh_binding *frozen = scratch;
    struct sgx535_depth_window_mesh_ordered *clone = NULL;
    struct sgx535_depth_window_mesh_ordered_bound *o = NULL;
    struct ordered_work *w;
    struct sgx535_depth_window_mesh_single_sample_request *original, *copy;
    struct sgx535_depth_window_mesh_ordered_bound_view *v;
    uint8_t *work, *geometry, *temporary;
    uint32_t prefix = ordered_align(sizeof(*binding));
    struct sgx535_single_sample_lowering lowering;
    enum sgx535_task_status status;
    int ret = ordered_plan(r, count, &p);
    if (ret)
        return ret;
    ret = ordered_bound_sizes(&p, &sizes);
    if (ret)
        return ret;
    if (!source || ordered_is_f32(source) != (io == SGX535_WINDOW_GENERIC0_F32) || !binding ||
        cap < sizes.workspace_bytes ||
        !ordered_arguments(r, &p, out_span, sizeof(*out), scratch, cap) ||
        overlap(binding, sizeof(*binding), scratch, cap) ||
        overlap(binding, sizeof(*binding), out_span, sizeof(*out)) ||
        overlap(source, p.allocation, scratch, cap) ||
        overlap(source, p.allocation, out_span, sizeof(*out)) ||
        overlap(source, p.allocation, binding, sizeof(*binding)))
        return -EINVAL;
    memcpy(frozen, binding, sizeof(*binding));
    work = (uint8_t *)scratch + prefix;
    w = (void *)work;
    /* SOURCE is ordered_create_io's build of R, intact and planned alike: the
     * bound owns a copy of it, and only the first group is rebuilt here (its
     * constructor is relocated below). Bound validation rebuilds all of SOURCE
     * from R; binding every frame does not repeat that. */
    ret = ordered_stage(r, count, &p, &clone, sizeof(clone), work, cap - prefix, io, &original,
                        &copy, &geometry);
    if (ret)
        goto done;
    if (memcmp(&source->view, &source->frozen, sizeof(source->view)) ||
        memcmp(&p, &source->plan, sizeof(p))) {
        ret = -ESTALE;
        goto done;
    }
    clone = ordered_clone(source);
    if (!clone) {
        ret = -ENOMEM;
        goto done;
    }
    temporary = geometry + ordered_align(p.geometry);
    /* Same immutable first-group grammar used by ordered_create_io. Numeric
     * binding regenerates that constructor before relocating counted payload. */
    bool raw_mask = copy[0].mesh.scene.foreground.profile == SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1;
    enum sgx535_window_fragment_io first_io = ordered_group_io(io, &copy[0].mesh.scene.foreground);
    {
        status = depth_single_build_io(&copy[0], &w->task, &lowering, geometry, p.geometry,
                                       &w->packet, temporary, 2u * p.geometry, raw_mask, first_io);
    }
    if (status != SGX535_TASK_BLOCKED) {
        ret = status == SGX535_TASK_NOMEM ? -ENOMEM : -EOPNOTSUPP;
        goto done;
    }
    {
        struct depth_policy policy;
        struct sgx535_depth_window_mesh_request first = depth_gl_source(&copy[0].mesh);
        if (!depth_single_policy(&copy[0], &policy, &lowering, raw_mask)) {
            ret = -EOPNOTSUPP;
            goto done;
        }
        policy.io = first_io;
        status = bind_construct_owner(&first, &policy, &w->task, geometry,
                                      w->task.seed.foreground.geometry.bytes, frozen, &w->packet,
                                      temporary, 2u * p.geometry, true);
        if (status == SGX535_TASK_BLOCKED)
            w->bound = w->packet.bound;
    }
    if (status != SGX535_TASK_BLOCKED || !ordered_binding_extents(&clone->view, frozen)) {
        ret = status == SGX535_TASK_NOMEM ? -ENOMEM : -EINVAL;
        goto done;
    }
    o = ordered_alloc(sizes.bound_metadata_bytes - p.allocation);
    if (!o) {
        ret = -ENOMEM;
        goto done;
    }
    o->source = clone;
    clone = NULL;
    o->allocation = sizes.bound_metadata_bytes - p.allocation;
    v = &o->view;
    v->source_owner = o->source;
    v->source = &o->source->view;
    v->bytes = (uint8_t *)o + ordered_align(sizeof(*o));
    v->byte_count = v->source->byte_count;
    v->metadata_bytes = sizes.bound_metadata_bytes;
    v->binding = *frozen;
    v->background = w->bound.background;
    v->missing_launch_contracts = w->bound.missing_launch_contracts;
    v->missing_depth_contracts = w->bound.missing_depth_contracts;
    memcpy(v->bank_base, w->bound.bank_base, sizeof(v->bank_base));
    memcpy(v->required_isp_reset, w->bound.required_isp_reset, sizeof(v->required_isp_reset));
    v->reset_actions = w->bound.reset_actions;
    memcpy(v->depth_base_words, w->bound.depth_base_words, sizeof(v->depth_base_words));
    memcpy(v->stages, w->bound.stages, sizeof(v->stages));
    v->ta_stream_va_bytes = (uint32_t)frozen->scene.roles[SGX535_TASK_PDS].gpu_va_bytes +
                            v->source->sections[v->source->ta_stream_section].resource_byte_offset;
    v->event_pds_va_bytes = (uint32_t)frozen->scene.roles[SGX535_TASK_PDS].gpu_va_bytes +
                            v->source->sections[v->source->event_pds_section].resource_byte_offset;
    for (uint32_t i = 0; i < v->stages[SGX535_TASK_LAUNCH_TA].count; i++)
        if (v->stages[SGX535_TASK_LAUNCH_TA].actions[i].offset == 0x238)
            v->stages[SGX535_TASK_LAUNCH_TA].actions[i].value = v->ta_stream_va_bytes;
    memcpy((void *)v->bytes, v->source->bytes, v->byte_count);
    if (!ordered_bind_payload(v) || memcmp(binding, frozen, sizeof(*binding)) ||
        (POULSBACK_SELFCHECK &&
         (!ordered_equal(source, o->source, w) || !ordered_same(r, original, copy, count)))) {
        ret = -ESTALE;
        goto done;
    }
    o->frozen = o->view;
    *out = o;
    o = NULL;
    ret = 0;
done:
    ordered_free(clone);
    ordered_bound_free(o);
    return ret;
}
int sgx535_depth_window_mesh_ordered_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_ordered *source,
    const struct sgx535_depth_window_mesh_binding *binding,
    struct sgx535_depth_window_mesh_ordered_bound **out, void *scratch, uint32_t cap)
{
    return ordered_bind_io(r, count, source, binding, out, out, scratch, cap,
                           SGX535_WINDOW_PRIMARY_COLOR);
}
int sgx535_depth_window_mesh_f32_ordered_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_f32_ordered *source,
    const struct sgx535_depth_window_mesh_binding *binding,
    struct sgx535_depth_window_mesh_f32_ordered_bound **out, void *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_ordered_bound *fresh = NULL;
    int ret = ordered_bind_io(r, count, (const void *)source, binding, &fresh, out, scratch, cap,
                              SGX535_WINDOW_GENERIC0_F32);
    if (!ret)
        *out = (struct sgx535_depth_window_mesh_f32_ordered_bound *)fresh;
    return ret;
}
const struct sgx535_depth_window_mesh_ordered_bound_view *
sgx535_depth_window_mesh_ordered_bound_view(const struct sgx535_depth_window_mesh_ordered_bound *o)
{
    return o && o->source && !ordered_is_f32(o->source) ? &o->view : NULL;
}
static int ordered_bound_validate_io(const struct sgx535_depth_window_mesh_single_sample_request *r,
                                     uint32_t count,
                                     const struct sgx535_depth_window_mesh_ordered *source,
                                     const struct sgx535_depth_window_mesh_binding *binding,
                                     const struct sgx535_depth_window_mesh_ordered_bound *o,
                                     void *scratch, uint32_t cap, enum sgx535_window_fragment_io io)
{
    struct sgx535_depth_window_mesh_ordered_bound *fresh = NULL;
    int ret;
    if (!o || !source || ordered_is_f32(source) != (io == SGX535_WINDOW_GENERIC0_F32) ||
        !o->source || ordered_is_f32(o->source) != (io == SGX535_WINDOW_GENERIC0_F32) ||
        memcmp(&o->view, &o->frozen, sizeof(o->view)) || overlap(o, o->allocation, scratch, cap) ||
        overlap(o->source, o->source->plan.allocation, scratch, cap))
        return -ESTALE;
    /* Binding trusts SOURCE as built from R; validation rebuilds it. */
    ret = ordered_validate_io(r, count, source, scratch, cap, io);
    if (!ret)
        ret = ordered_bind_io(r, count, source, binding, &fresh, &fresh, scratch, cap, io);
    if (!ret) {
        struct ordered_work *w = (void *)((uint8_t *)scratch + ordered_align(sizeof(*binding)));
        struct sgx535_depth_window_mesh_ordered_bound_view *a = &w->normalized_bound[0],
                                                           *b = &w->normalized_bound[1];
        uint32_t byte_count = o->view.byte_count;
        *a = o->view;
        *b = fresh->view;
        a->source_owner = b->source_owner = NULL;
        a->source = b->source = NULL;
        a->bytes = b->bytes = NULL;
        /* ordered_equal reuses this union; no bound-view field remains live. */
        if (memcmp(a, b, sizeof(*a)) || !ordered_equal(o->source, fresh->source, w) ||
            memcmp(o->view.bytes, fresh->view.bytes, byte_count))
            ret = -ESTALE;
    }
    ordered_bound_free(fresh);
    return ret;
}

int sgx535_depth_window_mesh_ordered_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_ordered *source,
    const struct sgx535_depth_window_mesh_binding *binding,
    const struct sgx535_depth_window_mesh_ordered_bound *o, void *scratch, uint32_t cap)
{
    return ordered_bound_validate_io(r, count, source, binding, o, scratch, cap,
                                     SGX535_WINDOW_PRIMARY_COLOR);
}
int sgx535_depth_window_mesh_f32_ordered_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t count,
    const struct sgx535_depth_window_mesh_f32_ordered *source,
    const struct sgx535_depth_window_mesh_binding *binding,
    const struct sgx535_depth_window_mesh_f32_ordered_bound *o, void *scratch, uint32_t cap)
{
    return ordered_bound_validate_io(r, count, (const void *)source, binding, (const void *)o,
                                     scratch, cap, SGX535_WINDOW_GENERIC0_F32);
}
const struct sgx535_depth_window_mesh_ordered_bound_view *
sgx535_depth_window_mesh_f32_ordered_bound_view(
    const struct sgx535_depth_window_mesh_f32_ordered_bound *p)
{
    const struct sgx535_depth_window_mesh_ordered_bound *o = (const void *)p;
    return o && o->source && ordered_is_f32(o->source) &&
                   !memcmp(&o->source->view, &o->source->frozen, sizeof(o->source->view)) &&
                   !memcmp(&o->view, &o->frozen, sizeof(o->view))
               ? &o->view
               : NULL;
}
void sgx535_depth_window_mesh_f32_ordered_bound_destroy(
    struct sgx535_depth_window_mesh_f32_ordered_bound *p)
{
    struct sgx535_depth_window_mesh_ordered_bound *o = (void *)p;
    if (!o || (o->source && (o->source->frozen.missing_mesh_contracts &
                             SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE)))
        ordered_bound_free(o);
}
