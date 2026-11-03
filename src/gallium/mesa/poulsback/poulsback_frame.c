// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_frame.h"
#include "poulsback_bo_uapi.h"
#include "poulsback_refuse.h"
#include "sgx535_depth_window_mesh.h"
#include "sgx535_mte_state.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_pixel_pds.h"
#include "sgx535_use_code_bank.h"
#include "sgx535_vertex.h"
#include "sgx535_zls.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "poulsback_frame_private.h"

static const uint32_t fs_class[FS_BOS] = {POULSBACK_BO_CLASS_PDS, POULSBACK_BO_CLASS_CODE,
                                          POULSBACK_BO_CLASS_CODE, POULSBACK_BO_CLASS_GENERAL,
                                          POULSBACK_BO_CLASS_PARAMETER};
static const uint32_t arena_class[ARENAS] = {POULSBACK_BO_CLASS_PDS, POULSBACK_BO_CLASS_CODE,
                                             POULSBACK_BO_CLASS_CODE};
static const uint32_t arena_chunk[ARENAS] = {65536u, 32768u, 32768u};

static uint32_t get(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static void put(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}
static uint64_t page_bytes(uint64_t n)
{
    return (n + 4095u) & ~UINT64_C(4095);
}
/* VALUE for relocation R to VA in OWNER (bind_payload's rule). */
static bool reloc_value(const struct sgx535_task_packet_reloc *r, uint64_t va,
                        const struct poulsback_frame_bo *owner, uint32_t *value)
{
    struct sgx535_use_code_bank_input code;
    struct sgx535_pds_use_link_projection link;
    uint32_t dm;
    if (va > UINT32_MAX || (va & (r->alignment - 1u)) || va < owner->gpu_va ||
        va - owner->gpu_va + r->span_bytes > owner->size)
        return false;
    if (r->kind == SGX535_PACKET_DRI_OFFSET) {
        *value = (uint32_t)((va >> r->right_shift) << r->left_shift);
        return true;
    }
    if (r->role != SGX535_TASK_USE_VERTEX && r->role != SGX535_TASK_USE_FRAGMENT)
        return false;
    dm = r->role - SGX535_TASK_USE_VERTEX;
    code = (struct sgx535_use_code_bank_input){(uint32_t)owner->gpu_va, (uint32_t)owner->size,
                                               (uint32_t)va, code_bank(owner->gpu_va, dm), dm};
    if (sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
        return false;
    if (r->kind == SGX535_PACKET_USE_BANK)
        *value = link.bank_selector_fields;
    else if (r->kind == SGX535_PACKET_USE_ENTRY)
        *value = link.address_fields;
    else
        return false;
    return true;
}
static bool relocate(uint8_t *bytes, uint32_t at, const struct sgx535_task_packet_reloc *r,
                     uint64_t va, const struct poulsback_frame_bo *owner)
{
    uint32_t value;
    if (!reloc_value(r, va, owner, &value))
        return false;
    put(bytes + at, (get(bytes + at) & ~r->mask) | (value & r->mask));
    return true;
}

/* ---- shapes and groups ---- */

/* Every scalar of a shape: its identity with the fragment and uniform words. */
static uint32_t shape_key(const poulsback_frame_shape *s, bool gpu_flat, uint32_t key[64])
{
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    uint32_t n = 0;
#define K(x) key[n++] = (x)
    K(gpu_flat);
    K(s->mesh.version), K(s->mesh.format), K(s->mesh.no_depth), K(s->mesh.reserved);
    K(s->mesh.scene.version), K(s->mesh.scene.packed_background_argb);
    K(s->mesh.scene.reserved[0]), K(s->mesh.scene.reserved[1]);
    K(m->version), K(m->profile), K(m->core_id), K(m->revision), K(m->width), K(m->height);
    K(m->order), K(m->interpolation), K(m->fragment_count), K(m->uniform_word_count);
    K(m->reserved[0]), K(m->reserved[1]), K(m->reserved[2]), K(m->cull);
    memcpy(key + n, &m->window, sizeof(m->window));
    n += sizeof(m->window) / 4u;
    memcpy(key + n, &s->samples, sizeof(s->samples));
    n += sizeof(s->samples) / 4u;
#undef K
    return n;
}
_Static_assert(23u + sizeof(struct sgx535_window_mesh_window) / 4u +
                       sizeof(struct sgx535_single_sample_source) / 4u <=
                   64u,
               "shape key");
static bool group_is(const struct group *g, const poulsback_frame_shape *s, const uint32_t *key,
                     uint32_t words)
{
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    return g->key_words == words && !memcmp(g->key, key, words * 4u) &&
           !memcmp(g->fragment, m->fragment, m->fragment_count * sizeof(*m->fragment)) &&
           (!m->uniform_word_count ||
            !memcmp(g->uniforms, m->uniforms, m->uniform_word_count * 4u));
}

static void arena_free(struct poulsback_frame_writer *w, struct arena *a)
{
    for (uint32_t i = 0; i < a->count; i++)
        w->ops.free(w->ops.data, &a->bo[i]);
    memset(a, 0, sizeof(*a));
}
static void state_free(struct poulsback_frame_writer *w, struct state *fs)
{
    if (!fs)
        return;
    for (uint32_t i = 0; i < fs->group_count; i++)
        free(fs->groups[i]);
    for (unsigned i = 0; i < FS_BOS; i++)
        if (fs->bo[i].handle)
            w->ops.free(w->ops.data, &fs->bo[i]);
    for (unsigned i = 0; i < ARENAS; i++)
        arena_free(w, &fs->arena[i]);
    sgx535_depth_window_mesh_ordered_destroy(fs->legacy);
    sgx535_depth_window_mesh_f32_ordered_destroy(fs->f32);
    free(fs);
}
/* Drops every template of FS and refills its arenas from their first chunk. */
static int state_reset_templates(struct poulsback_frame_writer *w, struct state *fs)
{
    int ret = w->ops.idle(w->ops.data);
    if (ret)
        return ret;
    for (uint32_t i = 0; i < fs->group_count; i++)
        fs->groups[i]->t[0].built = fs->groups[i]->t[1].built = false;
    for (unsigned i = 0; i < ARENAS; i++)
        fs->arena[i].current = fs->arena[i].used = 0;
    return 0;
}

struct state *poulsback_frame_state_get(struct poulsback_frame_writer *w,
                                        const poulsback_frame_shape *s)
{
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    struct state *fs, **slot = NULL;
    for (unsigned i = 0; i < STATES; i++) {
        fs = w->states[i];
        if (fs && fs->version == m->version && fs->width == m->width && fs->height == m->height &&
            fs->clear == s->mesh.scene.packed_background_argb && fs->no_depth == s->mesh.no_depth)
            return fs;
        if (!fs)
            slot = slot ? slot : &w->states[i];
    }
    if (!slot) { /* the least recently used, never the open frame's */
        for (unsigned i = 0; i < STATES; i++)
            if (w->states[i] != w->fs && (!slot || w->states[i]->last_use < (*slot)->last_use))
                slot = &w->states[i];
        if (w->ops.idle(w->ops.data))
            return NULL;
        state_free(w, *slot);
        *slot = NULL;
    }
    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return NULL;
    fs->version = m->version;
    fs->width = m->width;
    fs->height = m->height;
    fs->clear = s->mesh.scene.packed_background_argb;
    fs->no_depth = s->mesh.no_depth;
    fs->stride = (m->width * 4u + 127u) & ~127u;
    *slot = fs;
    return fs;
}

struct group *poulsback_frame_group_get(struct poulsback_frame_writer *w, struct state *fs,
                                        const poulsback_frame_shape *s, bool gpu_flat)
{
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    uint32_t key[64], words = shape_key(s, gpu_flat, key);
    struct group *g;
    for (uint32_t i = 0; i < fs->group_count; i++)
        if (group_is(fs->groups[i], s, key, words))
            return fs->groups[i];
    if (m->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
        m->uniform_word_count > SGX535_FRAGMENT_UNIFORM_WORDS ||
        (m->fragment_count && !m->fragment) || (m->uniform_word_count && !m->uniforms))
        return NULL;
    if (fs->group_count == GROUPS) {
        /* Only at a frame's first draw: no open draw names a group. */
        if (w->draw_count || state_reset_templates(w, fs))
            return NULL;
        for (uint32_t i = 0; i < fs->group_count; i++)
            free(fs->groups[i]);
        fs->group_count = 0;
    }
    g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->shape = *s;
    g->key_words = words;
    memcpy(g->key, key, words * 4u);
    memcpy(g->fragment, m->fragment, m->fragment_count * sizeof(*m->fragment));
    if (m->uniform_word_count)
        memcpy(g->uniforms, m->uniforms, m->uniform_word_count * 4u);
    g->shape.mesh.scene.foreground.vertices = NULL;
    g->shape.mesh.scene.foreground.indices = NULL;
    g->shape.mesh.scene.foreground.vertex_count = g->shape.mesh.scene.foreground.index_count = 0;
    g->shape.mesh.scene.foreground.fragment = g->fragment;
    g->shape.mesh.scene.foreground.uniforms = m->uniform_word_count ? g->uniforms : NULL;
    g->inputs = m->interpolation != SGX535_WINDOW_MESH_NO_INPUT;
    g->flat = m->interpolation == SGX535_VARYING_FLAT_REPLICATED;
    g->stride = g->inputs ? 32u : 16u;
    g->normalized = m->profile == SGX535_WINDOW_MESH_PROFILE;
    g->clip = m->version == SGX535_WINDOW_MESH_CLIP_VERSION;
    g->gpu_flat = gpu_flat;
    fs->groups[fs->group_count++] = g;
    return g;
}

/* ---- one-draw ordered views ---- */

struct one {
    struct sgx535_depth_window_mesh_ordered *legacy;
    struct sgx535_depth_window_mesh_f32_ordered *f32;
    const struct sgx535_depth_window_mesh_ordered_view *view;
    poulsback_frame_shape request;
    struct sgx535_window_mesh_vertex vertices[3];
    uint32_t indices[3];
};
/* G's shape as a frame of one triangle, built by the ordered constructor. */
static int one_create(const struct group *g, bool f32, struct one *o)
{
    struct sgx535_window_mesh_request *m = &o->request.mesh.scene.foreground;
    uint32_t bytes;
    void *scratch;
    int ret;
    memset(o, 0, sizeof(*o));
    o->request = g->shape;
    for (unsigned i = 0; i < 3; i++) {
        o->vertices[i].window_xyz_reciprocal_w[0] = i == 1 ? 0x3f800000u : 0;
        o->vertices[i].window_xyz_reciprocal_w[1] = i == 2 ? 0x3f800000u : 0;
        o->vertices[i].window_xyz_reciprocal_w[3] = 0x3f800000u;
        o->indices[i] = i;
    }
    m->vertices = o->vertices;
    m->indices = o->indices;
    m->vertex_count = m->index_count = 3;
    ret = sgx535_depth_window_mesh_ordered_workspace(&o->request, 1, &bytes);
    if (ret)
        return POULSBACK_REFUSED(ret);
    scratch = malloc(bytes);
    if (!scratch)
        return -ENOMEM;
    ret = f32 ? sgx535_depth_window_mesh_f32_ordered_create(&o->request, 1, &o->f32, scratch, bytes)
              : sgx535_depth_window_mesh_ordered_create(&o->request, 1, &o->legacy, scratch, bytes);
    free(scratch);
    if (ret)
        return POULSBACK_REFUSED(ret);
    o->view = f32 ? sgx535_depth_window_mesh_f32_ordered_view(o->f32)
                  : sgx535_depth_window_mesh_ordered_view(o->legacy);
    return o->view ? 0 : POULSBACK_REFUSED(-EINVAL);
}
static void one_destroy(struct one *o)
{
    sgx535_depth_window_mesh_f32_ordered_destroy(o->f32);
    sgx535_depth_window_mesh_ordered_destroy(o->legacy);
}
/* The section of V holding R's target, or UINT32_MAX. */
static uint32_t target_section(const struct sgx535_depth_window_mesh_ordered_view *v,
                               const struct sgx535_task_packet_reloc *r)
{
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        if (s->role == r->role && r->resource_byte_offset >= s->resource_byte_offset &&
            r->resource_byte_offset - s->resource_byte_offset <= s->byte_count &&
            r->span_bytes <= s->byte_count - (r->resource_byte_offset - s->resource_byte_offset))
            return i;
    }
    return UINT32_MAX;
}

/* ---- frame state ---- */

static int state_buffers(struct poulsback_frame_writer *w, struct state *fs,
                         const struct sgx535_depth_window_mesh_ordered_view *v)
{
    for (unsigned i = 0; i < FS_BOS; i++) {
        uint64_t need = i == FS_BACKGROUND ? 4096u : page_bytes(v->requirements[i].minimum_bytes);
        int ret;
        for (uint32_t b = 0; i != FS_VERTICES && b < v->background.section_count; b++) {
            const struct sgx535_background_section *s = &v->background.sections[b];
            unsigned role = s->role == SGX535_BACKGROUND_USE   ? FS_USE_FRAGMENT
                            : s->role == SGX535_BACKGROUND_PDS ? FS_PDS
                                                               : FS_BACKGROUND;
            if (role == i && need < page_bytes((uint64_t)s->resource_offset + s->byte_count))
                need = page_bytes((uint64_t)s->resource_offset + s->byte_count);
        }
        ret = w->ops.alloc(w->ops.data, fs_class[i], need, &fs->bo[i]);
        if (ret)
            return ret;
    }
    return 0;
}
static void state_binding(const struct state *fs,
                          const struct sgx535_depth_window_mesh_ordered_view *v,
                          struct sgx535_depth_window_mesh_binding *b)
{
    const struct sgx535_task_requirement *q = v->requirements;
    struct sgx535_window_mesh_binding *scene = &b->scene;
    memset(b, 0, sizeof(*b));
    scene->epoch = 1;
    for (unsigned i = 0; i < 4; i++) /* PDS, USE vertex, USE fragment, vertex data */
        scene->roles[i] = (struct sgx535_task_launch_role){fs->bo[i].handle, fs->bo[i].gpu_va,
                                                           fs->bo[i].size, q[i].access, 0};
    scene->roles[SGX535_TASK_SCENE] = (struct sgx535_task_launch_role){
        KERNEL_STORAGE | SGX535_TASK_SCENE, SCENE_VA,
        page_bytes(q[SGX535_TASK_SCENE].minimum_bytes), q[SGX535_TASK_SCENE].access, 0};
    scene->roles[SGX535_TASK_TARGET] = (struct sgx535_task_launch_role){
        KERNEL_STORAGE | SGX535_TASK_TARGET, TARGET_STAND_IN,
        page_bytes(q[SGX535_TASK_TARGET].minimum_bytes), q[SGX535_TASK_TARGET].access, 0};
    scene->roles[SGX535_TASK_DPM_TABLES] = (struct sgx535_task_launch_role){
        KERNEL_STORAGE | SGX535_TASK_DPM_TABLES, TABLES_VA,
        page_bytes(q[SGX535_TASK_DPM_TABLES].minimum_bytes), q[SGX535_TASK_DPM_TABLES].access, 0};
    scene->roles[SGX535_TASK_DPM_PARAMETERS] =
        (struct sgx535_task_launch_role){KERNEL_STORAGE | SGX535_TASK_DPM_PARAMETERS, PARAMETER_VA,
                                         PARAMETER_BYTES, q[SGX535_TASK_DPM_PARAMETERS].access, 0};
    scene->bank_index[0] = code_bank(fs->bo[FS_USE_VERTEX].gpu_va, 0);
    scene->bank_index[1] = code_bank(fs->bo[FS_USE_FRAGMENT].gpu_va, 1);
    b->background = (struct sgx535_background_binding){
        .epoch = 1,
        .parameter_storage_id = KERNEL_STORAGE | SGX535_TASK_DPM_PARAMETERS,
        .fragment_bank_index = scene->bank_index[1],
        .pds_exec_base = PDS_EXEC_BASE,
        .bif_3d_req_base = PARAMETER_VA,
        .parameter_va = PARAMETER_VA,
        .parameter_bytes = PARAMETER_BYTES,
        .resources = {
            {fs->bo[FS_USE_FRAGMENT].handle, fs->bo[FS_USE_FRAGMENT].gpu_va,
             fs->bo[FS_USE_FRAGMENT].size, SGX535_BACKGROUND_USE, SGX535_TASK_ACCESS_READ},
            {fs->bo[FS_PDS].handle, fs->bo[FS_PDS].gpu_va, fs->bo[FS_PDS].size,
             SGX535_BACKGROUND_PDS, SGX535_TASK_ACCESS_READ},
            {fs->bo[FS_BACKGROUND].handle, fs->bo[FS_BACKGROUND].gpu_va, fs->bo[FS_BACKGROUND].size,
             SGX535_BACKGROUND_RASTGEOM, SGX535_TASK_ACCESS_READ}}};
    b->depth = (struct sgx535_depth_window_mesh_owner){
        KERNEL_STORAGE | 8u, DEPTH_STAND_IN, page_bytes(v->depth.allocation_bytes),
        SGX535_TASK_ACCESS_READ | SGX535_TASK_ACCESS_WRITE, 0};
}
static int state_copy(struct state *fs, const struct sgx535_depth_window_mesh_ordered_bound_view *b)
{
    const struct sgx535_depth_window_mesh_ordered_view *v = b->source;
    const struct sgx535_background_image *bg = &b->background.image;
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        if (s->source || s->role > SGX535_TASK_VERTEX_DATA || s->byte_offset > b->byte_count ||
            s->byte_count > b->byte_count - s->byte_offset ||
            s->resource_byte_offset + (uint64_t)s->byte_count > fs->bo[s->role].size)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        memcpy(fs->bo[s->role].map + s->resource_byte_offset, b->bytes + s->byte_offset,
               s->byte_count);
    }
    for (uint32_t i = 0; i < bg->section_count; i++) {
        const struct sgx535_background_section *s = &bg->sections[i];
        unsigned role = s->role == SGX535_BACKGROUND_USE   ? FS_USE_FRAGMENT
                        : s->role == SGX535_BACKGROUND_PDS ? FS_PDS
                                                           : FS_BACKGROUND;
        if (s->byte_offset > sizeof(bg->bytes) ||
            s->byte_count > sizeof(bg->bytes) - s->byte_offset ||
            s->resource_offset + (uint64_t)s->byte_count > fs->bo[role].size)
            return POULSBACK_REFUSED(-EINVAL);
        memcpy(fs->bo[role].map + s->resource_offset, bg->bytes + s->byte_offset, s->byte_count);
    }
    return 0;
}
/* The code stage equals what engine bring-up programmed: PDS_EXEC_BASE and
 * each window's vertex and pixel code bank. */
static int check_code_stage(const struct sgx535_task_launch_stage_plan *p,
                            const struct sgx535_window_mesh_binding *b)
{
    for (uint32_t i = 0; i < p->count; i++) {
        const struct sgx535_engine121_action *a = &p->actions[i];
        bool ok = a->kind == SGX535_ENGINE121_WRITE && !p->unknown_value_masks[i] &&
                  a->offset == 0xab8 && a->value == PDS_EXEC_BASE;
        for (uint32_t dm = 0; !ok && dm < 2; dm++) {
            uint32_t win = (b->bank_index[dm] - 3u - dm) / 2u, base = CODE_VA + win * 0x80000u;
            struct sgx535_use_code_bank_input in = {base, 8, base, b->bank_index[dm], dm};
            struct sgx535_use_code_bank_projection bank;
            ok = a->kind == SGX535_ENGINE121_WRITE && !p->unknown_value_masks[i] &&
                 sgx535_use_code_bank_project(&in, &bank) == SGX535_USE_CODE_BANK_OK &&
                 a->offset == bank.register_offset_bytes && a->value == bank.register_word;
        }
        if (!ok)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
    }
    return 0;
}
/* A launch stage as SUBMIT registers: the TA stream and the pixel event are
 * the slot's (bo set per frame), the depth bases the frame's depth buffer and
 * BGOBJTAG's vertex pointer (16-byte units from the 3D request base) the
 * state's background; everything else a plain value the kernel checks. */
static int state_regs(struct state *fs, const struct sgx535_task_launch_stage_plan *p, bool raster,
                      struct poulsback_submit_reg *out, uint32_t *count)
{
    uint32_t *late = raster ? &fs->event_reg : &fs->ta_stream_reg;
    if (p->count > POULSBACK_SUBMIT_MAX_REGS)
        return POULSBACK_REFUSED(-E2BIG);
    *late = UINT32_MAX;
    for (uint32_t i = 0; i < p->count; i++) {
        const struct sgx535_engine121_action *a = &p->actions[i];
        struct poulsback_submit_reg r = {a->offset, a->value, 0, 0};
        if (a->kind != SGX535_ENGINE121_WRITE || p->unknown_value_masks[i])
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        if (!raster && a->offset == 0x238) {
            fs->ta_stream_reg = i;
            r.value = 0;
        } else if (raster && a->offset == 0xa5c) {
            fs->event_reg = i;
            r.value = 0;
        } else if (raster && (a->offset == 0xcb0 || (a->offset >= 0x484 && a->offset <= 0x490))) {
            uint32_t mask = a->offset == 0xcb0 ? 0xf0000000u : 0x0ffffff0u;
            if (a->value != (DEPTH_STAND_IN & mask))
                return POULSBACK_REFUSED(-EINVAL);
            fs->depth_regs |= 1u << i;
            r.value = 0;
        } else if (raster && a->offset == 0x4c4 && a->value) {
            uint64_t va = PARAMETER_VA + ((uint64_t)(a->value & 0xffffffu) << 4);
            const struct poulsback_frame_bo *bg = &fs->bo[FS_BACKGROUND];
            if (va < bg->gpu_va || va - bg->gpu_va >= bg->size)
                return POULSBACK_REFUSED(-EINVAL);
            r = (struct poulsback_submit_reg){a->offset, a->value & 0x3f000000u,
                                              1u + JOB_FS + FS_BACKGROUND,
                                              (uint32_t)(va - bg->gpu_va)};
        }
        out[i] = r;
    }
    *count = p->count;
    return *late == UINT32_MAX ? POULSBACK_REFUSED(-EOPNOTSUPP) : 0;
}
/* The pixel event and PBE state, copied per frame: the PBE word names the
 * target, the event program the PBE copy. Nothing else may refer to either. */
static int state_event(struct state *fs,
                       const struct sgx535_depth_window_mesh_ordered_bound_view *b)
{
    const struct sgx535_depth_window_mesh_ordered_view *v = b->source;
    const struct sgx535_window_mesh_section *event = &v->sections[v->event_pds_section],
                                            *pbe = NULL;
    uint32_t pbe_section = UINT32_MAX, targets = 0;
    for (uint32_t i = 0; i < v->section_count; i++)
        if (v->sections[i].kind == SGX535_PACKET_PBE_STATE)
            pbe = &v->sections[pbe_section = i];
    if (!pbe || pbe->byte_count != PBE_BYTES || event->byte_count > EVENT_BYTES)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    memcpy(fs->event, b->bytes + event->byte_offset, event->byte_count);
    memcpy(fs->pbe, b->bytes + pbe->byte_offset, PBE_BYTES);
    fs->event_bytes = event->byte_count;
    for (uint32_t i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &v->relocations[i];
        uint32_t t = r->role == SGX535_TASK_TARGET ? UINT32_MAX : target_section(v, r);
        if (r->role == SGX535_TASK_TARGET) {
            if (r->section != pbe_section || targets++)
                return POULSBACK_REFUSED(-EOPNOTSUPP);
            fs->pbe_target = *r;
            fs->pbe_target_at = r->section_byte_offset;
        } else if (t == pbe_section || t == v->event_pds_section) {
            if (r->section != v->event_pds_section || t != pbe_section || fs->event_pbes == LATES)
                return POULSBACK_REFUSED(-EOPNOTSUPP);
            fs->event_pbe[fs->event_pbes++] = (struct late){
                *r, r->section_byte_offset, r->resource_byte_offset - pbe->resource_byte_offset, 0};
        }
    }
    return targets == 1 ? 0 : POULSBACK_REFUSED(-EOPNOTSUPP);
}
/* FS's words, from G's shape as a bound frame of one draw. */
static int state_build(struct poulsback_frame_writer *w, struct state *fs, const struct group *g,
                       bool f32)
{
    static const struct sgx535_engine121_action engine_isp_reset[2] = {
        {SGX535_ENGINE121_WRITE, 0x80, 0x20, 0}, {SGX535_ENGINE121_WRITE, 0x80, 0, 0}};
    struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
    struct sgx535_depth_window_mesh_binding binding;
    struct sgx535_depth_window_mesh_ordered_bound *legacy_bound = NULL;
    struct sgx535_depth_window_mesh_f32_ordered_bound *f32_bound = NULL;
    const struct sgx535_depth_window_mesh_ordered_bound_view *b = NULL;
    const struct sgx535_window_mesh_section *ta;
    struct one *o = calloc(1, sizeof(*o));
    void *scratch = NULL;
    int ret = o ? one_create(g, f32, o) : -ENOMEM;
    if (!ret)
        ret = state_buffers(w, fs, o->view);
    if (!ret) {
        state_binding(fs, o->view, &binding);
        ret = sgx535_depth_window_mesh_ordered_bind_sizes(&o->request, 1, &sizes);
        if (ret)
            ret = POULSBACK_REFUSED(ret);
    }
    if (!ret && !(scratch = malloc(sizes.workspace_bytes)))
        ret = -ENOMEM;
    if (!ret) {
        ret = f32 ? sgx535_depth_window_mesh_f32_ordered_bind(&o->request, 1, o->f32, &binding,
                                                              &f32_bound, scratch,
                                                              sizes.workspace_bytes)
                  : sgx535_depth_window_mesh_ordered_bind(&o->request, 1, o->legacy, &binding,
                                                          &legacy_bound, scratch,
                                                          sizes.workspace_bytes);
        if (ret)
            ret = POULSBACK_REFUSED(ret);
        else
            b = f32 ? sgx535_depth_window_mesh_f32_ordered_bound_view(f32_bound)
                    : sgx535_depth_window_mesh_ordered_bound_view(legacy_bound);
    }
    free(scratch);
    if (!ret && (!b || b->reset_actions != 2 ||
                 memcmp(b->required_isp_reset, engine_isp_reset, sizeof(engine_isp_reset))))
        ret = POULSBACK_REFUSED(-EOPNOTSUPP);
    if (!ret)
        ret = check_code_stage(&b->stages[SGX535_TASK_LAUNCH_CODE], &b->binding.scene);
    if (!ret)
        ret = state_copy(fs, b);
    if (!ret)
        ret = state_event(fs, b);
    if (!ret)
        ret = state_regs(fs, &b->stages[SGX535_TASK_LAUNCH_TA], false, fs->ta, &fs->ta_count);
    if (!ret)
        ret = state_regs(fs, &b->stages[SGX535_TASK_LAUNCH_RASTER], true, fs->raster,
                         &fs->raster_count);
    if (!ret) {
        ta = &b->source->sections[b->source->ta_stream_section];
        if (ta->byte_count != TA_HEADER + TA_ENTRY + TA_TRAILER)
            ret = POULSBACK_REFUSED(-EOPNOTSUPP);
        else {
            memcpy(fs->ta_header, b->bytes + ta->byte_offset, TA_HEADER);
            memcpy(fs->ta_trailer, b->bytes + ta->byte_offset + TA_HEADER + TA_ENTRY, TA_TRAILER);
        }
    }
    if (!ret) {
        /* The source view stays: draw templates check shared sections against it. */
        fs->legacy = o->legacy;
        fs->f32 = o->f32;
        fs->view = o->view;
        fs->depth_bytes = page_bytes(o->view->depth.allocation_bytes);
        fs->built = true;
    } else if (o)
        one_destroy(o);
    sgx535_depth_window_mesh_f32_ordered_bound_destroy(f32_bound);
    sgx535_depth_window_mesh_ordered_bound_destroy(legacy_bound);
    free(o);
    return ret;
}

/* ---- draw templates ---- */

static int arena_place(struct poulsback_frame_writer *w, struct state *fs, unsigned which,
                       uint32_t bytes, uint32_t alignment, const struct poulsback_frame_bo **bo,
                       uint32_t *offset)
{
    struct arena *a = &fs->arena[which];
    uint32_t at = align(a->used, alignment);
    if (bytes > arena_chunk[which])
        return POULSBACK_REFUSED(-E2BIG);
    if (!a->count || at + bytes > arena_chunk[which]) {
        uint32_t next = a->count ? a->current + 1u : 0;
        if (next == ARENA_CHUNKS)
            return -ENOSPC;
        if (next == a->count) {
            int ret =
                w->ops.alloc(w->ops.data, arena_class[which], arena_chunk[which], &a->bo[next]);
            if (ret)
                return ret;
            a->count++;
        }
        a->current = next;
        at = 0;
    }
    *bo = &a->bo[a->current];
    *offset = at;
    a->used = at + bytes;
    return 0;
}
struct placement {
    const struct poulsback_frame_bo *bo;
    uint32_t offset;
    bool placed;
};
/* The VA of R's target in TEMPLATE (whose sections are placed as P), or a
 * late kind: the draw's vertices or its vertex PDS. */
static int template_target(struct state *fs, const struct sgx535_depth_window_mesh_ordered_view *v,
                           const struct placement *p, struct poulsback_frame_writer *w,
                           const struct sgx535_task_packet_reloc *r, uint64_t *va,
                           const struct poulsback_frame_bo **owner, int *late, uint32_t *delta)
{
    uint32_t t = target_section(v, r);
    const struct sgx535_window_mesh_section *s;
    *late = -1;
    if (t == UINT32_MAX)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    s = &v->sections[t];
    *delta = r->resource_byte_offset - s->resource_byte_offset;
    if (s->kind == SGX535_PACKET_VERTICES) {
        if (*delta)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        *late = LATE_VERTICES;
    } else if (s->kind == SGX535_PACKET_INDICES) {
        if (*delta != FOREGROUND_INDICES)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        *owner = &w->indices;
        *va = w->indices.gpu_va;
    } else if (s->kind == SGX535_PACKET_VERTEX_PDS) {
        if (*delta)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        *late = LATE_VERTEX_PDS;
    } else if (p[t].placed) {
        *owner = p[t].bo;
        *va = p[t].bo->gpu_va + p[t].offset + *delta;
    } else {
        /* A frame section: the state's own copy, which must be the same. */
        const struct sgx535_depth_window_mesh_ordered_view *f = fs->view;
        const struct sgx535_window_mesh_section *c = t < f->section_count ? &f->sections[t] : NULL;
        if (!c || t == f->ta_stream_section || c->kind != s->kind || c->role != s->role ||
            c->byte_count != s->byte_count ||
            memcmp(f->bytes + c->byte_offset, v->bytes + s->byte_offset, s->byte_count))
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        *owner = &fs->bo[c->role];
        *va = fs->bo[c->role].gpu_va + c->resource_byte_offset + *delta;
    }
    return 0;
}
static int template_build(struct poulsback_frame_writer *w, struct state *fs, struct group *g,
                          bool f32)
{
    struct template *t = &g->t[f32];
    struct one *o = calloc(1, sizeof(*o));
    struct placement p[SGX535_DEPTH_WINDOW_MESH_SECTIONS + 2u] = {{0}};
    const struct sgx535_depth_window_mesh_ordered_view *v;
    uint32_t vertex_pds = UINT32_MAX;
    uint8_t buffer[4096];
    int ret = o ? one_create(g, f32, o) : -ENOMEM;
    if (ret)
        goto done;
    v = o->view;
    if (v->section_count > SGX535_DEPTH_WINDOW_MESH_SECTIONS + 2u ||
        v->sections[v->ta_stream_section].byte_count != TA_HEADER + TA_ENTRY + TA_TRAILER) {
        ret = POULSBACK_REFUSED(-EOPNOTSUPP);
        goto done;
    }
    memset(t, 0, sizeof(*t));
    /* Place the draw's programs; a vertex USE program equal to the state's
     * stays shared (ordered_own_vertex_use). */
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        const struct sgx535_window_mesh_section *c = &fs->view->sections[i];
        bool own = sgx535_depth_window_mesh_ordered_draw_section(v, i);
        unsigned which;
        if (s->kind == SGX535_PACKET_VERTEX_USE && i < fs->view->section_count &&
            i != fs->view->ta_stream_section)
            own =
                c->kind != s->kind || c->byte_count != s->byte_count ||
                memcmp(fs->view->bytes + c->byte_offset, v->bytes + s->byte_offset, s->byte_count);
        if (s->kind == SGX535_PACKET_VERTEX_PDS)
            vertex_pds = i;
        if (!own || s->kind == SGX535_PACKET_VERTICES || s->kind == SGX535_PACKET_INDICES ||
            s->kind == SGX535_PACKET_VERTEX_PDS)
            continue;
        which = s->role == SGX535_TASK_PDS            ? ARENA_PDS
                : s->role == SGX535_TASK_USE_VERTEX   ? ARENA_USE_VERTEX
                : s->role == SGX535_TASK_USE_FRAGMENT ? ARENA_USE_FRAGMENT
                                                      : ARENAS;
        if (which == ARENAS || s->byte_count > sizeof(buffer)) {
            ret = POULSBACK_REFUSED(-EOPNOTSUPP);
            goto done;
        }
        ret = arena_place(w, fs, which, s->byte_count, s->alignment, &p[i].bo, &p[i].offset);
        if (ret)
            goto done;
        p[i].placed = true;
    }
    if (vertex_pds == UINT32_MAX || v->sections[vertex_pds].byte_count < VERTEX_PDS_DRAW) {
        ret = POULSBACK_REFUSED(-EOPNOTSUPP);
        goto done;
    }
    /* Each placed program with its relocations, then the vertex PDS's draw
     * half and the TA entry with theirs. */
    for (uint32_t i = 0; i <= v->section_count; i++) {
        bool entry = i == v->section_count, pds = i == vertex_pds;
        const struct sgx535_window_mesh_section *s = &v->sections[entry ? v->ta_stream_section : i];
        uint32_t base = entry ? TA_HEADER : 0, bytes = entry ? TA_ENTRY
                                                       : pds ? VERTEX_PDS_DRAW
                                                             : s->byte_count;
        uint8_t *out = entry ? t->entry : pds ? t->vertex_pds : buffer;
        if (!entry && !pds && !p[i].placed)
            continue;
        memcpy(out, v->bytes + s->byte_offset + base, bytes);
        for (uint32_t k = 0; k < v->relocation_count; k++) {
            const struct sgx535_task_packet_reloc *r = &v->relocations[k];
            const struct poulsback_frame_bo *owner = NULL;
            uint64_t va = 0;
            uint32_t delta, at;
            int late;
            if (r->section != (entry ? v->ta_stream_section : i) || r->section_byte_offset < base ||
                r->section_byte_offset - base >= bytes)
                continue;
            at = r->section_byte_offset - base;
            ret = template_target(fs, v, p, w, r, &va, &owner, &late, &delta);
            if (ret)
                goto done;
            if (late >= 0) {
                uint32_t *n = entry ? &t->entry_lates : &t->vertex_pds_lates;
                struct late *l = entry ? t->entry_late : t->vertex_pds_late;
                if (*n == LATES || (!entry && !pds) || (entry && late != LATE_VERTEX_PDS) ||
                    (pds && late != LATE_VERTICES)) {
                    ret = POULSBACK_REFUSED(-EOPNOTSUPP);
                    goto done;
                }
                l[(*n)++] = (struct late){*r, at, delta, (uint32_t)late};
            } else if (!relocate(out, at, r, va, owner)) {
                ret = POULSBACK_REFUSED(-EINVAL);
                goto done;
            }
        }
        /* A GPU draw's flat colour: the fragment PDS's iteration (DOUTI
         * FLATSHADE 27:26) takes vertex 2, as INDEXLIST and the MTE do. */
        if (!entry && !pds && g->gpu_flat && s->kind == SGX535_PACKET_FRAGMENT_PDS) {
            uint32_t iterate = bytes >= 40u ? get(buffer + 36) : 0;
            if (iterate != 0x0fd0a00fu && iterate != 0x0fd0000fu) {
                ret = POULSBACK_REFUSED(-EOPNOTSUPP);
                goto done;
            }
            put(buffer + 36, (iterate & ~0x0c000000u) | 0x08000000u);
        }
        if (!entry && !pds)
            memcpy(p[i].bo->map + p[i].offset, buffer, bytes);
    }
    /* INDEXLIST's count: the one triangle's 3 indices here. */
    t->count_base = get(t->entry + TA_COUNT) - 3u;
    if ((get(t->entry + TA_COUNT) & 0x3fffu) != 3u || t->entry_lates != 1 ||
        t->vertex_pds_lates != 1) {
        ret = POULSBACK_REFUSED(-EOPNOTSUPP);
        goto done;
    }
    t->built = true;
done:
    if (o)
        one_destroy(o);
    free(o);
    return ret;
}

/* ---- writer ---- */

int poulsback_frame_writer_create(const struct poulsback_frame_bo_ops *ops,
                                  struct poulsback_frame_writer **out)
{
    struct poulsback_frame_writer *w = calloc(1, sizeof(*w));
    uint16_t *sequence = malloc(POULSBACK_FRAME_SEQUENCE * 2u);
    int ret = w && sequence ? 0 : -ENOMEM;
    /* glDrawArrays' indices, 0..n-1, shared by every draw. */
    if (!ret) {
        w->ops = *ops;
        ret = w->ops.alloc(w->ops.data, POULSBACK_BO_CLASS_GENERAL, POULSBACK_FRAME_SEQUENCE * 2u,
                           &w->indices);
    }
    if (ret) {
        free(sequence);
        free(w);
        return ret;
    }
    for (uint32_t i = 0; i < POULSBACK_FRAME_SEQUENCE; i++)
        sequence[i] = (uint16_t)i;
    memcpy(w->indices.map, sequence, POULSBACK_FRAME_SEQUENCE * 2u);
    free(sequence);
    *out = w;
    return 0;
}
void poulsback_frame_writer_destroy(struct poulsback_frame_writer *w)
{
    if (!w)
        return;
    for (unsigned i = 0; i < STATES; i++)
        state_free(w, w->states[i]);
    for (unsigned i = 0; i < w->program_chunks; i++)
        w->ops.free(w->ops.data, &w->program[i]);
    w->ops.free(w->ops.data, &w->indices);
    free(w);
}
void poulsback_frame_begin(struct poulsback_frame_writer *w, const struct poulsback_frame_bo *pds,
                           const struct poulsback_frame_bo *vertices)
{
    w->open = true;
    w->f32 = w->load = false;
    w->fs = NULL;
    w->pds = *pds;
    w->vertices = *vertices;
    w->draw_count = w->vertex_bytes = w->gpu_pds_bytes = w->extra_count = 0;
    w->memos = w->memo_next = 0; /* a memo names this frame's state tasks */
    w->last_fragment = NULL;
}
bool poulsback_frame_open(const struct poulsback_frame_writer *w)
{
    return w->open;
}
void poulsback_frame_discard(struct poulsback_frame_writer *w)
{
    w->f32 = w->load = false;
    w->fs = NULL;
    w->draw_count = w->vertex_bytes = w->gpu_pds_bytes = w->extra_count = 0;
    w->memos = w->memo_next = 0; /* a memo names this frame's state tasks */
    w->last_fragment = NULL;
}
void poulsback_frame_load(struct poulsback_frame_writer *w)
{
    w->load = w->open;
}
uint32_t poulsback_frame_draws(const struct poulsback_frame_writer *w)
{
    return w->open ? w->draw_count : 0;
}
void poulsback_frame_abort(struct poulsback_frame_writer *w)
{
    w->open = false;
}
uint64_t poulsback_frame_depth_bytes(uint32_t width, uint32_t height)
{
    return (uint64_t)((width + 31u) & ~31u) * 4u * ((height + 31u) & ~31u);
}
int poulsback_frame_extent(const struct poulsback_frame_writer *w, uint32_t *width,
                           uint32_t *height, uint64_t *depth_bytes)
{
    if (!w->open || !w->fs)
        return -EINVAL;
    *width = w->fs->width;
    *height = w->fs->height;
    *depth_bytes = page_bytes(poulsback_frame_depth_bytes(w->fs->width, w->fs->height));
    return 0;
}

static uint32_t f32_bits(uint32_t value)
{
    uint32_t exponent = 0, leading = value;
    if (!value)
        return 0;
    while (leading >>= 1)
        exponent++;
    return ((exponent + 127u) << 23) | ((value << (23u - exponent)) & 0x7fffffu);
}
static bool finite(uint32_t v)
{
    return (v & 0x7f800000u) != 0x7f800000u;
}
/* geometry_build's domain: 0 <= v <= max, -0 allowed. */
static bool bounded(uint32_t v, uint32_t max)
{
    uint32_t magnitude = v & 0x7fffffffu;
    return magnitude <= max && (!(v >> 31) || !magnitude);
}
int poulsback_frame_draw(struct poulsback_frame_writer *w, const poulsback_frame_shape *s,
                         const struct poulsback_frame_vertices *in)
{
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    struct state *fs;
    struct group *g;
    struct draw x = {0};
    uint32_t width, height, bytes, words[8];
    uint8_t *dst;
    bool f32 = w->f32;
    int ret;
    if (!w->open || !in->count || in->count % 3u || !in->window || !in->clip_w)
        return POULSBACK_REFUSED(-EINVAL);
    if (w->draw_count == POULSBACK_FRAME_MAX_DRAWS || m->order != SGX535_WINDOW_MESH_DEINDEXED ||
        !m->width || !m->height || m->width >= 2048 || m->height >= 2048)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    fs = poulsback_frame_state_get(w, s);
    if (!fs || (w->fs && fs != w->fs))
        return POULSBACK_REFUSED(fs ? -EOPNOTSUPP : -ENOMEM);
    g = poulsback_frame_group_get(w, fs, s, false);
    if (!g)
        return POULSBACK_REFUSED(-ENOMEM);
    /* its own ISP words first: their record is stream data, before the
     * vertices (a refusal after it leaves that space used) */
    if (in->isp_groups && (ret = poulsback_frame_cpu_state(w, in, &x)))
        return ret;
    bytes = in->count * g->stride;
    w->vertex_bytes = align(w->vertex_bytes, 16); /* after GPU draws' stream data */
    if (bytes > POULSBACK_FRAME_VERTEX_BYTES - w->vertex_bytes || (g->inputs && !in->rgba))
        return POULSBACK_REFUSED(-E2BIG);
    /* One pass: check each vertex (geometry_build's domain: a window position
     * inside the target and, for a primary-colour draw, W of 1; any finite
     * clip-space position) and write it, whole words, into the vertex buffer. */
    width = f32_bits(m->width);
    height = f32_bits(m->height);
    dst = w->vertices.map + w->vertex_bytes;
    f32 |= g->inputs && !g->flat;
    for (uint32_t i = 0; i < in->count; i++) {
        const uint32_t *p = in->window + (size_t)i * in->window_stride;
        uint32_t clip_w = in->clip_w[(size_t)i * in->clip_stride];
        if (g->clip ? !finite(p[0]) || !finite(p[1]) || !finite(p[2]) || !finite(p[3])
                    : !bounded(p[0], width) || !bounded(p[1], height) ||
                          !bounded(p[2], 0x3f800000u) || !p[3] || p[3] >= 0x7f800000u)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        memcpy(words, p, 16);
        if (g->inputs) {
            const uint32_t *c = in->rgba + (size_t)i * in->rgba_stride;
            for (unsigned k = 0; k < 4; k++)
                if (c[k] > 0x3f800000u)
                    return POULSBACK_REFUSED(-EOPNOTSUPP);
            if (g->flat && i % 3u &&
                memcmp(c, in->rgba + (size_t)(i - i % 3u) * in->rgba_stride, 16))
                return POULSBACK_REFUSED(-EOPNOTSUPP);
            memcpy(words + 4, c, 16);
            f32 |= p[3] != 0x3f800000u || clip_w != 0x3f800000u;
        } else if (!g->clip && (p[3] != 0x3f800000u || clip_w != 0x3f800000u))
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        memcpy(dst + (size_t)i * g->stride, words, g->stride);
    }
    w->fs = fs;
    w->f32 = f32;
    x.g = g, x.offset = w->vertex_bytes, x.count = in->count, x.pixel = in->isp_groups != 0;
    w->draws[w->draw_count++] = x;
    w->vertex_bytes += bytes;
    return 0;
}

/* ---- GPU-shaded draws ---- */

/* HANDLE in the open frame's SUBMIT, once. */
/* ZLSCTL of a render that loads its depth and stencil: the template's, which
 * stores them, with the load (sgx535_zls_words); its pitch and format from
 * the template's word. */
static int zls_load(struct poulsback_submit_reg *r)
{
    const uint32_t pitch = ((r->value >> 8 & 0xffu) + 1u) * 16u;
    const uint8_t format = (r->value >> 16 & 3u) == 2u ? SGX535_ZLS_D16 : SGX535_ZLS_D24S8;
    struct sgx535_zls_request q = {format, pitch, 0, 1, 1.0f, 0};
    struct sgx535_zls_words store, load;
    if (!sgx535_zls_words(&q, &store) || store.zlsctl != r->value)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    q.load = 1;
    if (!sgx535_zls_words(&q, &load))
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    r->value = load.zlsctl;
    return 0;
}
int poulsback_frame_end(struct poulsback_frame_writer *w,
                        const struct poulsback_frame_buffer *target,
                        const struct poulsback_frame_buffer *depth, struct poulsback_frame_job *job)
{
    struct state *fs = w->fs;
    uint8_t ta[TA_HEADER + (TA_GPU_ENTRY + TA_STATE) * POULSBACK_FRAME_MAX_DRAWS + TA_TRAILER];
    uint8_t vertex_pds[VERTEX_PDS_DRAW], event[EVENT_BYTES], pbe[PBE_BYTES], cpu_entry[TA_ENTRY];
    uint32_t event_at, pbe_at, ta_at, ta_bytes, header, n = 0;
    bool reset = false;
    int ret = 0;
    w->open = false;
    if (!fs || !w->draw_count)
        return POULSBACK_REFUSED(-EINVAL);
    if (!fs->built && (ret = state_build(w, fs, w->draws[0].g, w->f32)))
        return ret;
    /* PBE writes rows of the builders' stride. */
    if (target->stride != fs->stride || (uint64_t)target->stride * fs->height > target->size ||
        depth->size < fs->depth_bytes)
        return POULSBACK_REFUSED(-EINVAL);
    for (uint32_t i = 0; i < w->draw_count; i++) {
        struct group *g = w->draws[i].g;
        if (w->f32 && !g->normalized)
            return POULSBACK_REFUSED(-EOPNOTSUPP);
        if (g->t[w->f32].built)
            continue;
        ret = template_build(w, fs, g, w->f32);
        if (ret == -ENOSPC && !reset) {
            /* Arenas full: once, wait for the GPU and rebuild from empty. */
            reset = true;
            ret = state_reset_templates(w, fs);
            i = UINT32_MAX;
        }
        if (ret)
            return ret;
    }
    /* The slot's PDS buffer: vertex PDS programs, pixel event, PBE, TA stream
     * (the GPU draws' programs are above POULSBACK_FRAME_GPU_PDS). */
    event_at = w->draw_count * VERTEX_PDS_DRAW;
    pbe_at = align(event_at + fs->event_bytes, 16);
    ta_at = align(pbe_at + PBE_BYTES, 16);
    /* The template's stream starts with the mask entry, then the depth clear
     * (fullscreen at depth one, sgx535_depth_window_mesh); a render that loads
     * its depth leaves the clear out, which would overwrite what ZLS loaded. */
    header = w->load ? TA_HEADER - TA_DEPTH_CLEAR : TA_HEADER;
    ta_bytes = header + TA_TRAILER;
    for (uint32_t i = 0; i < w->draw_count; i++)
        ta_bytes +=
            (w->draws[i].gpu ? TA_GPU_ENTRY : TA_ENTRY) + (w->draws[i].pixel ? TA_STATE : 0);
    if (ta_at + ta_bytes > POULSBACK_FRAME_GPU_PDS || POULSBACK_FRAME_GPU_PDS > w->pds.size)
        return POULSBACK_REFUSED(-E2BIG);
    memcpy(ta, fs->ta_header, header);
    ta_bytes = header;
    for (uint32_t i = 0; i < w->draw_count; i++) {
        const struct draw *d = &w->draws[i];
        const struct template *t = &d->g->t[w->f32];
        uint8_t *entry = ta + ta_bytes;
        uint64_t pds_va = w->pds.gpu_va + VERTEX_PDS_DRAW * i;
        if (d->gpu) {
            /* the group's STATE, a backend program's own after it, the tail */
            memcpy(entry, t->entry, TA_STATE);
            if (d->pixel) {
                memcpy(entry + TA_STATE, d->state, TA_STATE);
                entry += TA_STATE;
                ta_bytes += TA_STATE;
            }
            memcpy(entry + TA_STATE, d->tail, sizeof(d->tail));
            ta_bytes += TA_GPU_ENTRY;
            continue;
        }
        ta_bytes += TA_ENTRY;
        memcpy(vertex_pds, t->vertex_pds, VERTEX_PDS_DRAW);
        if (!relocate(vertex_pds, t->vertex_pds_late[0].at, &t->vertex_pds_late[0].r,
                      w->vertices.gpu_va + d->offset, &w->vertices))
            return POULSBACK_REFUSED(-EINVAL);
        memcpy(w->pds.map + VERTEX_PDS_DRAW * i, vertex_pds, VERTEX_PDS_DRAW);
        memcpy(cpu_entry, t->entry, TA_ENTRY);
        put(cpu_entry + TA_COUNT, t->count_base + d->count);
        if (!relocate(cpu_entry, t->entry_late[0].at, &t->entry_late[0].r, pds_va, &w->pds))
            return POULSBACK_REFUSED(-EINVAL);
        /* the group's STATE, the draw's own after it, the rest of the entry */
        memcpy(entry, cpu_entry, TA_STATE);
        if (d->pixel) {
            memcpy(entry + TA_STATE, d->state, TA_STATE);
            entry += TA_STATE;
            ta_bytes += TA_STATE;
        }
        memcpy(entry + TA_STATE, cpu_entry + TA_STATE, TA_ENTRY - TA_STATE);
    }
    memcpy(ta + ta_bytes, fs->ta_trailer, TA_TRAILER);
    ta_bytes += TA_TRAILER;
    memcpy(w->pds.map + ta_at, ta, ta_bytes);
    memcpy(event, fs->event, fs->event_bytes);
    for (uint32_t i = 0; i < fs->event_pbes; i++)
        if (!relocate(event, fs->event_pbe[i].at, &fs->event_pbe[i].r,
                      w->pds.gpu_va + pbe_at + fs->event_pbe[i].delta, &w->pds))
            return POULSBACK_REFUSED(-EINVAL);
    memcpy(pbe, fs->pbe, PBE_BYTES);
    {
        struct poulsback_frame_bo t = {target->handle, target->gpu_va, target->size, NULL};
        if (!relocate(pbe, fs->pbe_target_at, &fs->pbe_target, target->gpu_va, &t))
            return POULSBACK_REFUSED(-EINVAL);
    }
    memcpy(w->pds.map + event_at, event, fs->event_bytes);
    memcpy(w->pds.map + pbe_at, pbe, PBE_BYTES);
    /* SUBMIT: the BO list and both register tables. */
    job->width = fs->width;
    job->height = fs->height;
    job->bos[n++] = (struct poulsback_submit_bo){w->pds.handle, 0};
    job->bos[n++] = (struct poulsback_submit_bo){w->vertices.handle, 0};
    for (unsigned i = 0; i < FS_BOS; i++)
        job->bos[n++] = (struct poulsback_submit_bo){fs->bo[i].handle, 0};
    job->bos[n++] = (struct poulsback_submit_bo){w->indices.handle, 0};
    for (unsigned i = 0; i < ARENAS; i++)
        for (uint32_t c = 0; c < fs->arena[i].count; c++)
            job->bos[n++] = (struct poulsback_submit_bo){fs->arena[i].bo[c].handle, 0};
    for (uint32_t c = 0; c < w->program_chunks; c++)
        job->bos[n++] = (struct poulsback_submit_bo){w->program[c].handle, 0};
    /* a draw's BOs, but the target and depth once, as written: the load
     * draw samples the target (the kernel refuses a BO listed twice) */
    for (uint32_t c = 0; c < w->extra_count; c++)
        if (w->extra[c] != depth->handle && w->extra[c] != target->handle)
            job->bos[n++] = (struct poulsback_submit_bo){w->extra[c], 0};
    job->bos[n++] = (struct poulsback_submit_bo){depth->handle, POULSBACK_SUBMIT_BO_WRITE};
    job->bos[n++] = (struct poulsback_submit_bo){target->handle, POULSBACK_SUBMIT_BO_WRITE};
    job->bo_count = n;
    memcpy(job->ta, fs->ta, sizeof(job->ta));
    memcpy(job->raster, fs->raster, sizeof(job->raster));
    job->ta_count = fs->ta_count;
    job->raster_count = fs->raster_count;
    job->ta[fs->ta_stream_reg].bo = 1u + JOB_SLOT_PDS;
    job->ta[fs->ta_stream_reg].bo_offset = ta_at;
    job->raster[fs->event_reg].bo = 1u + JOB_SLOT_PDS;
    job->raster[fs->event_reg].bo_offset = event_at;
    for (uint32_t i = 0; i < fs->raster_count; i++) {
        if (fs->depth_regs & (1u << i))
            job->raster[i].bo = n - 1u; /* depth: the second to last */
        if (w->load && job->raster[i].offset == ZLSCTL && (ret = zls_load(&job->raster[i])))
            return ret;
    }
    fs->last_use = ++w->frames;
    return 0;
}
