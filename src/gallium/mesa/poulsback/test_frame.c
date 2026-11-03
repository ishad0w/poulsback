// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The frame writer against the ordered constructor (docs/graphics.md#frame-writer).
 *
 * Each supplied file contains six native uint32_t header words: magic
 * 0x47524250 ('PBRG' on little-endian hosts), version 1, struct bytes, depth,
 * target stride and height, followed by the native poulsback_scene_depth_groups
 * layout declared below. This format requires matching structure layouts; it is
 * not a portable interchange format. The frame is built twice into fake
 * buffers: by the ordered constructor and bind, and by the writer. The fixture then walks what the
 * GPU reads,
 * from the register tables: every word must be equal, except relocated
 * fields, whose two targets must again be equal over the relocation's span,
 * recursively (the constructor's relocations name every address). Files come
 * from POULSBACK_FRAME_SCENES (a directory); every file runs twice, so the
 * second pass reuses the writer's state and templates, and a frame whose draws
 * are all flat also runs with every W set to 1 (the legacy primary-colour
 * family, on states the F32 frames built). The first frame is also compared
 * after flipping a vertex word and the first word of each of the writer's code
 * arena chunks (32 KiB; a draw template's first program) in turn: each must
 * be found.
 *
 * usage: POULSBACK_FRAME_SCENES=DIR poulsback-frame-writer */
#include "poulsback_frame.h"
#include "poulsback_bo_uapi.h"
#include "poulsback_color0_task.h"
#include "poulsback_refuse.h"
#include "poulsback_resource.h"
#include "poulsback_scene_color.h"
#include "poulsback_scene_samples.h"
#include "sgx535_depth_window_mesh_single_sample.h"
#include "util/u_math.h"
#include "sgx535_depth_window_mesh.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixture file's native depth-group layout and its conversion into the
 * ordered constructor's requests (the oracle). The header's byte count must
 * equal sizeof(struct poulsback_scene_depth_groups). */
struct poulsback_scene_depth_draw {
    uint32_t depth_test, depth_write, depth_function;
};
#define POULSBACK_SCENE_DEPTH_GROUPS_VERSION 1u
#define POULSBACK_SCENE_DEPTH_GROUPS_MISSING_CONTINUATION 1u
#define POULSBACK_SCENE_DEPTH_GROUPS_MISSING_QUANTIZATION 2u
struct poulsback_scene_depth_group {
    uint32_t ordinal, first_vertex, vertex_count, program_profile;
    struct poulsback_color0_source source;
    struct poulsback_draw_window_raster raster;
    uint32_t has_residual;
    struct poulsback_draw_residual_raster residual;
    struct sgx535_fragment_dri_nir_ir symbolic;
    uint32_t uniform_word_count, uniform_words_used[4];
    uint32_t uniforms[SGX535_FRAGMENT_UNIFORM_WORDS];
    struct poulsback_scene_sample_state samples;
    struct poulsback_scene_color_state color;
    struct poulsback_scene_depth_draw depth;
    struct sgx535_single_sample_lowering lowering;
    uint32_t instruction_count;
    struct poulsback_render_instruction instruction[POULSBACK_RENDER_MAX_INSTRUCTIONS];
};
struct poulsback_scene_depth_groups {
    uint32_t version, width, height, draw_count, vertex_count, missing_contracts;
    struct poulsback_render_scene_load load;
    struct poulsback_render_scene_depth depth;
    struct poulsback_depth_capture_description depth_capture;
    struct poulsback_scene_depth_group draw[POULSBACK_RENDER_SCENE_MAX_DRAWS];
    uint32_t clip_position[POULSBACK_RENDER_MAX_VERTICES][4];
    struct sgx535_window_mesh_vertex vertices[POULSBACK_RENDER_MAX_VERTICES];
};

static bool packet_legacy_supported(const struct poulsback_scene_depth_groups *g)
{
    /* Packed COLOR0 remains the old flat W1 class. Smooth W1 and copied
     * positive unequal-W use the coupled generic-F32 physical linkage. */
    for (uint32_t i = 0; i < g->draw_count; i++)
        if (g->draw[i].symbolic.source_linkage.count &&
            g->draw[i].symbolic.resolved_interpolation != SGX535_FRAGMENT_FLAT)
            return false;
    for (uint32_t i = 0; i < g->vertex_count; i++)
        if (g->clip_position[i][3] != 0x3f800000u ||
            g->vertices[i].window_xyz_reciprocal_w[3] != 0x3f800000u)
            return false;
    return true;
}
static bool group_supported(const struct poulsback_scene_depth_group *d)
{
    /* Dither (GL's default) is not part of the request: it has no effect on
     * the 8-bit-per-channel target. */
    if ((d->source.rasterizer.cull_face || d->source.rasterizer.light_twoside) && !d->has_residual)
        return false;
    /* Original cull/two-sided state stays in the source; the collector's
     * checked residual authenticates the already-selected triangles. */
    return !d->has_residual ||
           !(d->residual.version != POULSBACK_DRAW_RESIDUAL_RASTER_VERSION ||
             d->residual.producer != POULSBACK_DRAW_RESIDUAL_FINAL_COLLECTOR ||
             d->residual.residual_cull_face || d->residual.residual_light_twoside ||
             d->residual.triangle_count != d->vertex_count / 3u ||
             d->residual.source_cull_face != d->source.rasterizer.cull_face ||
             d->residual.source_light_twoside != d->source.rasterizer.light_twoside);
}
static bool group_request(const struct poulsback_scene_depth_groups *g,
                          const struct poulsback_scene_depth_group *first,
                          const struct sgx535_window_mesh_vertex *vertices, const uint32_t *indices,
                          uint32_t count, struct sgx535_depth_window_mesh_single_sample_request *r)
{
    uint32_t packed = 0;
    static const unsigned shifts[4] = {16, 8, 0, 24};
    if (!group_supported(first))
        return false;
    /* The clear colour as the 8-bit target stores it (Mesa's own rounding);
     * the scene admits only components within [0, 1]. */
    for (unsigned i = 0; i < 4; i++) {
        float component;
        memcpy(&component, &g->load.clear_rgba_f32[i], sizeof(component));
        packed |= (uint32_t)float_to_ubyte(component) << shifts[i];
    }
    memset(r, 0, sizeof(*r));
    r->mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
    /* This is the existing packet container, not the logical attachment type.
     * groups.depth_capture retains D24S8 versus depth24/stencil0 D24X8. No
     * stencil access, native storage or upper-byte policy is admitted here. */
    r->mesh.format = SGX535_DEPTH_D24S8;
    r->mesh.no_depth = !g->depth_capture.version;
    r->mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
    r->mesh.scene.packed_background_argb = packed;
    r->mesh.scene.foreground = (struct sgx535_window_mesh_request){
        .version = SGX535_WINDOW_MESH_VERSION,
        .profile = first->program_profile == POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1
                       ? SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1
                       : SGX535_WINDOW_MESH_PROFILE,
        .core_id = SGX535_ENGINE121_CORE_ID,
        .revision = SGX535_ENGINE121_REVISION,
        .width = g->width,
        .height = g->height,
        .order = SGX535_WINDOW_MESH_DEINDEXED,
        .interpolation = !first->symbolic.source_linkage.count ? SGX535_WINDOW_MESH_NO_INPUT
                         : first->symbolic.resolved_interpolation == SGX535_FRAGMENT_FLAT
                             ? SGX535_VARYING_FLAT_REPLICATED
                             : SGX535_VARYING_SMOOTH,
        .window = {.origin = first->raster.origin,
                   .half_pixel_center = first->raster.half_pixel_center,
                   .bottom_edge_rule = first->raster.bottom_edge_rule,
                   .sample_mask = first->raster.sample_mask,
                   .min_samples = first->raster.min_samples},
        .vertices = vertices,
        .indices = indices,
        .vertex_count = count,
        .index_count = count,
        .fragment = first->symbolic.instruction,
        .fragment_count = first->symbolic.instruction_count,
        .uniforms = first->uniform_word_count ? first->uniforms : NULL,
        .uniform_word_count = first->uniform_word_count};
    memcpy(r->mesh.scene.foreground.window.viewport_scale_f32, first->raster.viewport_scale, 12);
    memcpy(r->mesh.scene.foreground.window.viewport_translate_f32, first->raster.viewport_translate,
           12);
    r->samples = first->samples.samples;
    return true;
}
static bool groups_ordered_requests(const struct poulsback_scene_depth_groups *g,
                                    const uint32_t *indices,
                                    struct sgx535_depth_window_mesh_single_sample_request *r,
                                    bool f32)
{
    if (!g->draw_count || g->draw_count > POULSBACK_RENDER_SCENE_MAX_DRAWS)
        return false;
    /* Reuse the exact homogeneous conversion for each retained group; the
     * ordered constructor is responsible for sharing frame initialization. */
    for (uint32_t i = 0; i < g->draw_count; i++)
        if ((f32 && g->draw[i].program_profile != POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED) ||
            !group_request(g, &g->draw[i], g->vertices + g->draw[i].first_vertex, indices,
                           g->draw[i].vertex_count, &r[i])) {
            POULSBACK_TRACE("ordered requests: draw %u (profile %u, f32 %d) refused\n", i,
                            g->draw[i].program_profile, f32);
            return false;
        }
    return true;
}
static int ordered_requests(const struct poulsback_scene_depth_groups *g, const uint32_t *indices,
                            struct sgx535_depth_window_mesh_single_sample_request *r, bool *f32)
{
    *f32 = !packet_legacy_supported(g);
    return groups_ordered_requests(g, indices, r, *f32) ? 0 : POULSBACK_REFUSED(-EOPNOTSUPP);
}

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define CODE_VA 0x40000000u
#define PARAMETER_VA 0x30000000u

/* ---- fake buffers: a VA range per class, size-aligned like the kernel's ---- */

struct fake {
    uint32_t handle, bo_class;
    uint64_t va, size;
    uint8_t *bytes;
    bool live, writer;
};
#define FAKES 512
static struct fake fakes[FAKES];
static uint32_t fake_count;
static uint64_t next_va[6] = {0x60000000u, 0x20000000u, 0x40000000u,
                              0x50000000u, 0x70000000u, 0x34000000u};
static int fake_alloc(void *data, uint32_t bo_class, uint64_t bytes, struct poulsback_frame_bo *out)
{
    uint64_t size = 4096;
    struct fake *f;
    (void)data;
    while (size < bytes)
        size <<= 1;
    CHECK(fake_count < FAKES && bo_class < 6);
    f = &fakes[fake_count];
    f->va = (next_va[bo_class] + size - 1u) & ~(size - 1u);
    if (bo_class == POULSBACK_BO_CLASS_CODE && f->va / 0x80000u != (f->va + size - 1u) / 0x80000u)
        f->va = (f->va + 0x7ffffu) & ~UINT64_C(0x7ffff);
    next_va[bo_class] = f->va + size;
    f->handle = 1000u + fake_count++;
    f->bo_class = bo_class;
    f->size = size;
    f->bytes = calloc(1, size);
    f->live = true;
    f->writer = data != NULL;
    CHECK(f->bytes);
    *out = (struct poulsback_frame_bo){f->handle, f->va, f->size, f->bytes};
    return 0;
}
static void fake_free(void *data, struct poulsback_frame_bo *bo)
{
    (void)data;
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].handle == bo->handle)
            fakes[i].live = false;
    memset(bo, 0, sizeof(*bo));
}
static int writer_alloc(void *data, uint32_t bo_class, uint64_t bytes,
                        struct poulsback_frame_bo *out)
{
    (void)data;
    return fake_alloc(fakes, bo_class, bytes, out);
}
static int fake_idle(void *data)
{
    (void)data;
    return 0;
}
static const struct fake *fake_handle(uint32_t handle)
{
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].handle == handle && fakes[i].live)
            return &fakes[i];
    return NULL;
}
static const uint8_t *new_bytes(uint64_t va, uint32_t len)
{
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].live && va >= fakes[i].va && va + len <= fakes[i].va + fakes[i].size)
            return fakes[i].bytes + (va - fakes[i].va);
    return NULL;
}

/* ---- the ordered constructor's frame, as VA-placed bytes and relocations ---- */

struct oracle_reloc {
    uint64_t word_va, target_va;
    uint32_t span, mask, right_shift, left_shift, kind, data_master;
};
enum { OFFSET, USE_BANK, USE_ENTRY };
struct region {
    uint64_t va;
    uint32_t len;
    const uint8_t *bytes;
};
static struct {
    struct region region[256];
    uint32_t regions, relocs;
    struct oracle_reloc reloc[1024];
    uint64_t target_va, target_size, depth_va;
} old;
static const uint8_t *old_bytes(uint64_t va, uint32_t len)
{
    for (uint32_t i = 0; i < old.regions; i++)
        if (va >= old.region[i].va && va + len <= old.region[i].va + old.region[i].len)
            return old.region[i].bytes + (va - old.region[i].va);
    return NULL;
}
static uint32_t word(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* NEW_WORD's target VA for relocation R (and the word's other fields). A
 * field without the address's top bits names a buffer of the old target's
 * class (its VA range). */
static bool decode(const struct oracle_reloc *r, uint32_t new_word, uint64_t *va)
{
    if (r->kind == OFFSET) {
        uint64_t bits = ((uint64_t)(new_word & r->mask) >> r->left_shift) << r->right_shift;
        uint64_t covered = ((uint64_t)r->mask >> r->left_shift) << r->right_shift;
        for (uint32_t i = 0; i < fake_count; i++) {
            uint64_t v = (fakes[i].va & ~covered) | bits;
            if (fakes[i].live && v >= fakes[i].va && v < fakes[i].va + fakes[i].size &&
                v >> 28 == r->target_va >> 28) {
                *va = v;
                return true;
            }
        }
        return false;
    }
    /* sgx535_pds_use_link_project's fields: bank, then 16-byte units. */
    uint32_t bank = new_word & 0xfu,
             units = ((new_word >> 4) & 0xfu) * 2048u + ((new_word >> 8) & 0x7ffu);
    if (bank < 3u + r->data_master || (bank - 3u - r->data_master) % 2u)
        return false;
    *va = CODE_VA + (uint64_t)(bank - 3u - r->data_master) / 2u * 0x80000u + units * 16u;
    return true;
}

static uint32_t walked, compared;
struct pair {
    uint64_t old_va, new_va;
    uint32_t len;
};
static struct pair seen[4096];
static uint32_t seen_count;
static bool walk(uint64_t old_va, uint64_t new_va, uint32_t len, const char *what);
static bool walk(uint64_t old_va, uint64_t new_va, uint32_t len, const char *what)
{
    const uint8_t *a = old_bytes(old_va, len), *b = new_bytes(new_va, len);
    for (uint32_t i = 0; i < seen_count; i++)
        if (seen[i].old_va == old_va && seen[i].new_va == new_va && seen[i].len >= len)
            return true;
    if (seen_count < 4096)
        seen[seen_count++] = (struct pair){old_va, new_va, len};
    if (!a || !b) {
        fprintf(stderr, "%s: no bytes: old 0x%llx (%p) new 0x%llx (%p) len %u\n", what,
                (unsigned long long)old_va, (const void *)a, (unsigned long long)new_va,
                (const void *)b, len);
        return false;
    }
    walked++;
    for (uint32_t at = 0; at < len; at += 4) {
        uint32_t mask = len - at < 4 ? ~0u << 8u * (len - at) : 0;
        uint32_t ow = 0, nw = 0;
        memcpy(&ow, a + at, len - at < 4 ? len - at : 4);
        memcpy(&nw, b + at, len - at < 4 ? len - at : 4);
        for (uint32_t i = 0; i < old.relocs; i++)
            if (old.reloc[i].word_va == old_va + at)
                mask |= old.reloc[i].mask;
        compared++;
        if ((ow ^ nw) & ~mask) {
            fprintf(stderr, "%s: word +%u (old 0x%llx new 0x%llx): 0x%08x != 0x%08x (mask %08x)\n",
                    what, at, (unsigned long long)(old_va + at), (unsigned long long)(new_va + at),
                    ow, nw, mask);
            return false;
        }
        for (uint32_t i = 0; i < old.relocs; i++) {
            const struct oracle_reloc *r = &old.reloc[i];
            uint64_t target;
            if (r->word_va != old_va + at || r->kind == USE_BANK)
                continue;
            if (!decode(r, nw, &target)) {
                fprintf(stderr, "%s: word +%u: 0x%08x decodes to no buffer\n", what, at, nw);
                return false;
            }
            if (r->target_va >= old.target_va && r->target_va < old.target_va + old.target_size) {
                /* The target: the same buffer in both builds. */
                if (target != r->target_va) {
                    fprintf(stderr, "%s: word +%u: target 0x%llx != 0x%llx\n", what, at,
                            (unsigned long long)target, (unsigned long long)r->target_va);
                    return false;
                }
                continue;
            }
            if (!walk(r->target_va, target, r->span, what))
                return false;
        }
    }
    return true;
}

static void old_reloc(uint64_t word_va, uint64_t target_va, uint32_t span, uint32_t mask,
                      uint32_t rs, uint32_t ls, uint32_t kind, uint32_t dm)
{
    CHECK(old.relocs < 1024);
    old.reloc[old.relocs++] =
        (struct oracle_reloc){word_va, target_va, span, mask, rs, ls, kind, dm};
}
static uint32_t packet_kind(uint32_t k)
{
    return k == SGX535_PACKET_DRI_OFFSET ? OFFSET
           : k == SGX535_PACKET_USE_BANK ? USE_BANK
                                         : USE_ENTRY;
}

/* The engine's bind() for the ordered frame, into fake slot buffers. */
static void old_frame(const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n,
                      bool f32, const struct poulsback_frame_buffer *target,
                      const struct poulsback_frame_buffer *depth,
                      const struct sgx535_depth_window_mesh_ordered_bound_view **out_view,
                      void **owners)
{
    struct sgx535_depth_window_mesh_ordered *legacy = NULL;
    struct sgx535_depth_window_mesh_f32_ordered *f = NULL;
    struct sgx535_depth_window_mesh_ordered_bound *lb = NULL;
    struct sgx535_depth_window_mesh_f32_ordered_bound *fb = NULL;
    const struct sgx535_depth_window_mesh_ordered_view *v;
    const struct sgx535_depth_window_mesh_ordered_bound_view *b;
    struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
    struct sgx535_depth_window_mesh_binding bind;
    struct poulsback_frame_bo bo[5];
    uint32_t bytes;
    void *scratch;
    CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, n, &bytes));
    scratch = malloc(bytes);
    CHECK(scratch);
    CHECK(!(f32 ? sgx535_depth_window_mesh_f32_ordered_create(r, n, &f, scratch, bytes)
                : sgx535_depth_window_mesh_ordered_create(r, n, &legacy, scratch, bytes)));
    free(scratch);
    v = f32 ? sgx535_depth_window_mesh_f32_ordered_view(f)
            : sgx535_depth_window_mesh_ordered_view(legacy);
    static const uint32_t classes[5] = {POULSBACK_BO_CLASS_PDS, POULSBACK_BO_CLASS_CODE,
                                        POULSBACK_BO_CLASS_CODE, POULSBACK_BO_CLASS_GENERAL,
                                        POULSBACK_BO_CLASS_PARAMETER};
    for (unsigned i = 0; i < 5; i++) {
        uint64_t need = i == 4 ? 4096 : v->requirements[i].minimum_bytes;
        CHECK(!fake_alloc(NULL, classes[i], need < 4096 ? 4096 : need, &bo[i]));
    }
    memset(&bind, 0, sizeof(bind));
    bind.scene.epoch = 1;
    for (unsigned i = 0; i < 4; i++)
        bind.scene.roles[i] = (struct sgx535_task_launch_role){
            bo[i].handle, bo[i].gpu_va, bo[i].size, v->requirements[i].access, 0};
    bind.scene.roles[SGX535_TASK_SCENE] = (struct sgx535_task_launch_role){
        (UINT64_C(1) << 32) | 4, 0x4c000000u,
        (v->requirements[SGX535_TASK_SCENE].minimum_bytes + 4095u) & ~4095u,
        v->requirements[SGX535_TASK_SCENE].access, 0};
    bind.scene.roles[SGX535_TASK_TARGET] =
        (struct sgx535_task_launch_role){target->handle, target->gpu_va, target->size,
                                         v->requirements[SGX535_TASK_TARGET].access, 0};
    bind.scene.roles[SGX535_TASK_DPM_TABLES] = (struct sgx535_task_launch_role){
        (UINT64_C(1) << 32) | 6, 0x48000000u,
        (v->requirements[SGX535_TASK_DPM_TABLES].minimum_bytes + 4095u) & ~4095u,
        v->requirements[SGX535_TASK_DPM_TABLES].access, 0};
    bind.scene.roles[SGX535_TASK_DPM_PARAMETERS] =
        (struct sgx535_task_launch_role){(UINT64_C(1) << 32) | 7, PARAMETER_VA, 0x02000000u,
                                         v->requirements[SGX535_TASK_DPM_PARAMETERS].access, 0};
    bind.scene.bank_index[0] = 3u + 2u * (uint32_t)((bo[1].gpu_va - CODE_VA) / 0x80000u);
    bind.scene.bank_index[1] = 4u + 2u * (uint32_t)((bo[2].gpu_va - CODE_VA) / 0x80000u);
    bind.background = (struct sgx535_background_binding){
        .epoch = 1,
        .parameter_storage_id = (UINT64_C(1) << 32) | 7,
        .fragment_bank_index = bind.scene.bank_index[1],
        .pds_exec_base = 0x20000000u,
        .bif_3d_req_base = PARAMETER_VA,
        .parameter_va = PARAMETER_VA,
        .parameter_bytes = 0x02000000u,
        .resources = {{bo[2].handle, bo[2].gpu_va, bo[2].size, SGX535_BACKGROUND_USE, 1},
                      {bo[0].handle, bo[0].gpu_va, bo[0].size, SGX535_BACKGROUND_PDS, 1},
                      {bo[4].handle, bo[4].gpu_va, bo[4].size, SGX535_BACKGROUND_RASTGEOM, 1}}};
    bind.depth =
        (struct sgx535_depth_window_mesh_owner){depth->handle, depth->gpu_va, depth->size, 3, 0};
    CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(r, n, &sizes));
    scratch = malloc(sizes.workspace_bytes);
    CHECK(scratch);
    int ret = f32 ? sgx535_depth_window_mesh_f32_ordered_bind(r, n, f, &bind, &fb, scratch,
                                                              sizes.workspace_bytes)
                  : sgx535_depth_window_mesh_ordered_bind(r, n, legacy, &bind, &lb, scratch,
                                                          sizes.workspace_bytes);
    if (ret)
        fprintf(stderr, "ordered bind: %d\n", ret);
    CHECK(!ret);
    b = f32 ? sgx535_depth_window_mesh_f32_ordered_bound_view(fb)
            : sgx535_depth_window_mesh_ordered_bound_view(lb);
    free(scratch);
    CHECK(b);
    /* Bytes and relocations in VA space. */
    memset(&old, 0, sizeof(old));
    old.target_va = target->gpu_va;
    old.target_size = target->size;
    old.depth_va = depth->gpu_va;
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        CHECK(old.regions < 256 && s->role < 4);
        old.region[old.regions++] = (struct region){bo[s->role].gpu_va + s->resource_byte_offset,
                                                    s->byte_count, b->bytes + s->byte_offset};
    }
    for (uint32_t i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *x = &v->relocations[i];
        const struct sgx535_window_mesh_section *s = &v->sections[x->section];
        old_reloc(bo[s->role].gpu_va + s->resource_byte_offset + x->section_byte_offset,
                  bind.scene.roles[x->role].gpu_va_bytes + x->resource_byte_offset, x->span_bytes,
                  x->mask, x->right_shift, x->left_shift, packet_kind(x->kind),
                  x->role == SGX535_TASK_USE_FRAGMENT);
    }
    const struct sgx535_background_image *bg = &b->background.image;
    for (uint32_t i = 0; i < bg->section_count; i++) {
        const struct sgx535_background_section *s = &bg->sections[i];
        CHECK(old.regions < 256);
        old.region[old.regions++] = (struct region){
            bind.background.resources[s->role - 1].gpu_va_bytes + s->resource_offset, s->byte_count,
            bg->bytes + s->byte_offset};
    }
    for (uint32_t i = 0; i < bg->relocation_count; i++) {
        const struct sgx535_background_reloc *x = &bg->relocations[i];
        const struct sgx535_background_section *s = &bg->sections[x->section];
        old_reloc(bind.background.resources[s->role - 1].gpu_va_bytes + s->resource_offset +
                      x->section_offset,
                  bind.background.resources[x->role - 1].gpu_va_bytes + x->resource_offset,
                  x->span_bytes, x->mask, x->kind == SGX535_BACKGROUND_OFFSET ? x->right_shift : 0,
                  0,
                  x->kind == SGX535_BACKGROUND_OFFSET     ? OFFSET
                  : x->kind == SGX535_BACKGROUND_USE_BANK ? USE_BANK
                                                          : USE_ENTRY,
                  1);
    }
    *out_view = b;
    owners[0] = legacy;
    owners[1] = f;
    owners[2] = lb;
    owners[3] = fb;
}

/* The two register tables: equal plain values, equal reachable words. */
static bool compare_regs(const struct sgx535_task_launch_stage_plan *p,
                         const struct poulsback_submit_reg *regs, uint32_t count,
                         const struct poulsback_frame_job *job,
                         const struct sgx535_depth_window_mesh_ordered_bound_view *b,
                         const char *what)
{
    const struct sgx535_depth_window_mesh_ordered_view *v = b->source;
    if (p->count != count) {
        fprintf(stderr, "%s: %u registers, the writer %u\n", what, p->count, count);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct sgx535_engine121_action *a = &p->actions[i];
        const struct poulsback_submit_reg *r = &regs[i];
        const struct fake *f;
        uint64_t va;
        if (a->offset != r->offset) {
            fprintf(stderr, "%s: register %u: 0x%x != 0x%x\n", what, i, a->offset, r->offset);
            return false;
        }
        if (!r->bo) {
            if (a->value != r->value) {
                fprintf(stderr, "%s: 0x%x = 0x%08x, the writer 0x%08x\n", what, a->offset, a->value,
                        r->value);
                return false;
            }
            continue;
        }
        CHECK(r->bo <= job->bo_count && (f = fake_handle(job->bos[r->bo - 1u].handle)));
        va = f->va + r->bo_offset;
        if (a->offset == 0x238) {
            if (!walk(a->value, va, v->sections[v->ta_stream_section].byte_count, "TA stream"))
                return false;
        } else if (a->offset == 0xa5c) {
            if (!walk(a->value, va, v->sections[v->event_pds_section].byte_count, "pixel event"))
                return false;
        } else if (a->offset == 0x4c4) {
            uint64_t old_va = PARAMETER_VA + ((uint64_t)(a->value & 0xffffffu) << 4);
            if ((a->value & 0x3f000000u) != r->value || !walk(old_va, va, 68, "background"))
                return false;
        } else if (f->va != old.depth_va || r->bo_offset) {
            fprintf(stderr, "%s: 0x%x names 0x%llx, not the depth buffer\n", what, a->offset,
                    (unsigned long long)va);
            return false;
        }
    }
    return true;
}

struct header {
    uint32_t magic, version, bytes, depth, stride, height;
};

static bool equal(const struct sgx535_depth_window_mesh_ordered_bound_view *b,
                  const struct poulsback_frame_job *job)
{
    seen_count = 0;
    return compare_regs(&b->stages[SGX535_TASK_LAUNCH_TA], job->ta, job->ta_count, job, b, "TA") &&
           compare_regs(&b->stages[SGX535_TASK_LAUNCH_RASTER], job->raster, job->raster_count, job,
                        b, "raster");
}
static void frame(const char *path, const char *note,
                  const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n,
                  bool f32, uint32_t stride, struct poulsback_frame_writer *w,
                  const struct poulsback_frame_bo *pds, const struct poulsback_frame_bo *vertices,
                  bool mutate);
/* Runs PATH's frame; LEGACY sets every W to 1 first (false when a draw is not
 * flat). */
static bool run(const char *path, struct poulsback_frame_writer *w,
                const struct poulsback_frame_bo *pds, const struct poulsback_frame_bo *vertices,
                bool legacy, bool mutate)
{
    static struct poulsback_scene_depth_groups g;
    static struct sgx535_depth_window_mesh_single_sample_request
        r[POULSBACK_RENDER_SCENE_MAX_DRAWS];
    static uint32_t indices[POULSBACK_RENDER_MAX_VERTICES];
    struct header h;
    bool f32;
    FILE *f = fopen(path, "rb");
    CHECK(f && fread(&h, sizeof(h), 1, f) == 1 && h.magic == 0x47524250u && h.version == 1);
    CHECK(h.bytes == sizeof(g) && fread(&g, sizeof(g), 1, f) == 1);
    fclose(f);
    if (legacy) {
        for (uint32_t i = 0; i < g.draw_count; i++)
            if (g.draw[i].symbolic.source_linkage.count &&
                g.draw[i].symbolic.resolved_interpolation != SGX535_FRAGMENT_FLAT)
                return false;
        for (uint32_t i = 0; i < g.vertex_count; i++)
            g.clip_position[i][3] = g.vertices[i].window_xyz_reciprocal_w[3] = 0x3f800000u;
    }
    for (uint32_t i = 0; i < POULSBACK_RENDER_MAX_VERTICES; i++)
        indices[i] = i;
    CHECK(!ordered_requests(&g, indices, r, &f32));
    frame(path, legacy ? " (W 1)" : "", r, g.draw_count, f32, h.stride, w, pds, vertices, mutate);
    return true;
}
/* The ordered constructor's and the writer's frame of R; the writer reads each
 * draw's vertices from its request (clip W as window 1/W: the same family). */
static void frame(const char *path, const char *note,
                  const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n,
                  bool f32, uint32_t stride, struct poulsback_frame_writer *w,
                  const struct poulsback_frame_bo *pds, const struct poulsback_frame_bo *vertices,
                  bool mutate)
{
    static struct poulsback_frame_job job;
    const struct sgx535_depth_window_mesh_ordered_bound_view *b;
    const struct sgx535_window_mesh_request *m0 = &r[0].mesh.scene.foreground;
    struct poulsback_frame_buffer target, depth;
    struct poulsback_frame_bo t, d;
    uint32_t count = 0;
    void *owners[4];
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, (uint64_t)stride * m0->height, &t));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_DEPTH,
                      poulsback_frame_depth_bytes(m0->width, m0->height), &d));
    target = (struct poulsback_frame_buffer){t.handle, stride, t.gpu_va, t.size};
    depth = (struct poulsback_frame_buffer){d.handle, 0, d.gpu_va, d.size};
    old_frame(r, n, f32, &target, &depth, &b, owners);
    /* The writer, from the same draws. */
    poulsback_frame_begin(w, pds, vertices);
    for (uint32_t i = 0; i < n; i++) {
        const struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
        struct poulsback_frame_vertices in = {m->vertex_count,
                                              m->vertices[0].window_xyz_reciprocal_w,
                                              &m->vertices[0].window_xyz_reciprocal_w[3],
                                              m->vertices[0].rgba,
                                              sizeof(m->vertices[0]) / 4u,
                                              sizeof(m->vertices[0]) / 4u,
                                              sizeof(m->vertices[0]) / 4u};
        CHECK(!poulsback_frame_draw(w, &r[i], &in));
        count += m->vertex_count;
    }
    CHECK(!poulsback_frame_end(w, &target, &depth, &job));
    CHECK(job.width == m0->width && job.height == m0->height);
    CHECK(equal(b, &job));
    printf("%s%s: %u draws, %u vertices, %s: equal (%u regions, %u words)\n", path, note, n, count,
           f32 ? "F32" : "legacy", walked, compared);
    if (mutate) {
        /* Every flip must be found; each is undone before the next. */
        uint32_t found = 0;
        uint8_t *at[1 + FAKES] = {vertices->map + 4};
        uint32_t n = 1;
        for (uint32_t i = 0; i < fake_count; i++)
            if (fakes[i].live && fakes[i].writer && fakes[i].bo_class == POULSBACK_BO_CLASS_CODE &&
                fakes[i].size == 32768 && word(fakes[i].bytes))
                at[n++] = fakes[i].bytes;
        for (uint32_t i = 0; i < n; i++) {
            *at[i] ^= 1u;
            fprintf(stderr, "(mutation %u: expect one mismatch line)\n", i);
            found += !equal(b, &job);
            *at[i] ^= 1u;
        }
        CHECK(found == n && equal(b, &job));
        printf("%s: %u mutations found\n", path, found);
    }
    walked = compared = 0;
    sgx535_depth_window_mesh_f32_ordered_bound_destroy(owners[3]);
    sgx535_depth_window_mesh_ordered_bound_destroy(owners[2]);
    sgx535_depth_window_mesh_f32_ordered_destroy(owners[1]);
    sgx535_depth_window_mesh_ordered_destroy(owners[0]);
}
/* A frame the recordings lack: a flat colour draw, a uniform colour draw (no
 * inputs: its own secondary program and event USE, its own vertex USE) and
 * the colour draw again, 64x64 with depth; F32 gives the colour draws W 1/2.
 * CLIP makes them clip-space draws: positions past the window and
 * behind the near plane, W 2 in F32, the last draw culled clockwise. */
static void synthetic(bool f32, bool clip, struct poulsback_frame_writer *w,
                      const struct poulsback_frame_bo *pds,
                      const struct poulsback_frame_bo *vertices)
{
    static struct sgx535_depth_window_mesh_single_sample_request r[3];
    static struct sgx535_window_mesh_vertex v[3][6];
    static struct sgx535_fragment_instruction ir[3][SGX535_NIR_MAX_INSTRUCTIONS];
    static uint32_t ix[6] = {0, 1, 2, 3, 4, 5},
                    uniforms[4] = {0x3f800000u, 0, 0x3f000000u, 0x3f800000u};
    static const uint32_t corner[6][2] = {
        {0, 0},           {0x42800000u, 0},           {0, 0x42800000u},
        {0x42800000u, 0}, {0x42800000u, 0x42800000u}, {0, 0x42800000u}};
    memset(r, 0, sizeof(r));
    memset(v, 0, sizeof(v));
    memset(ir, 0, sizeof(ir));
    for (unsigned i = 0; i < 3; i++) {
        struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
        bool uniform = i == 1;
        unsigned n = uniform ? 3 : 6;
        for (unsigned c = 0; c < 4; c++) {
            ir[i][c] = (struct sgx535_fragment_instruction){
                .opcode = uniform ? SGX535_FRAGMENT_UNIFORM : SGX535_FRAGMENT_INPUT,
                .index = uniform ? c : 0,
                .component = uniform ? 0 : c};
            ir[i][c + 4] = (struct sgx535_fragment_instruction){
                .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {c}};
        }
        for (unsigned j = 0; j < n; j++) {
            static const uint32_t clip_xyz[6][3] = {
                {0xbfc00000u, 0xbf000000u, 0x3f000000u}, {0x3f400000u, 0xbf400000u, 0xbfe00000u},
                {0xbf000000u, 0x3f800000u, 0x3e800000u}, {0x40100000u, 0xbe800000u, 0},
                {0x3f000000u, 0x3fc00000u, 0x3f400000u}, {0xbf800000u, 0x3f000000u, 0xbe000000u}};
            v[i][j].window_xyz_reciprocal_w[0] = clip ? clip_xyz[j][0] : corner[j][0];
            v[i][j].window_xyz_reciprocal_w[1] = clip ? clip_xyz[j][1] : corner[j][1];
            v[i][j].window_xyz_reciprocal_w[2] =
                clip ? clip_xyz[j][2] : 0x3f000000u - i * 0x00100000u;
            v[i][j].window_xyz_reciprocal_w[3] =
                f32 && !uniform ? (clip ? 0x40000000u : 0x3f000000u) : 0x3f800000u;
            if (!uniform) {
                v[i][j].rgba[i == 0 ? 0 : 1] = 0x3f800000u;
                v[i][j].rgba[3] = 0x3f800000u;
            }
        }
        *m = (struct sgx535_window_mesh_request){
            .version = SGX535_WINDOW_MESH_VERSION,
            .profile = SGX535_WINDOW_MESH_PROFILE,
            .core_id = SGX535_ENGINE121_CORE_ID,
            .revision = SGX535_ENGINE121_REVISION,
            .width = 64,
            .height = 64,
            .order = SGX535_WINDOW_MESH_DEINDEXED,
            .interpolation = uniform ? SGX535_WINDOW_MESH_NO_INPUT : SGX535_VARYING_FLAT_REPLICATED,
            .vertices = v[i],
            .indices = ix,
            .vertex_count = n,
            .index_count = n,
            .fragment = ir[i],
            .fragment_count = 8,
            .uniforms = uniform ? uniforms : NULL,
            .uniform_word_count = uniform ? 4 : 0};
        if (clip) {
            m->version = SGX535_WINDOW_MESH_CLIP_VERSION;
            m->cull = i == 2 ? SGX535_WINDOW_MESH_CULL_CW : SGX535_WINDOW_MESH_CULL_NONE;
        }
        m->window.origin = 1;
        m->window.sample_mask = 0xffffffffu;
        m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
        m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
        m->window.viewport_scale_f32[1] = 0xc2000000u;
        m->window.viewport_translate_f32[1] = 0x42000000u;
        m->window.viewport_scale_f32[2] = m->window.viewport_translate_f32[2] = 0x3f000000u;
        r[i].mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
        r[i].mesh.format = SGX535_DEPTH_D24S8;
        r[i].mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
        r[i].mesh.scene.packed_background_argb = 0xff102030u;
        r[i].samples = (struct sgx535_single_sample_source){
            .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
    }
    frame(clip ? "synthetic clip-space" : "synthetic", f32 ? " (F32)" : " (legacy)", r, 3, f32, 256,
          w, pds, vertices, false);
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}
int main(void)
{
    const struct poulsback_frame_bo_ops ops = {NULL, writer_alloc, fake_free, fake_idle};
    const char *dir = getenv("POULSBACK_FRAME_SCENES");
    struct poulsback_frame_writer *w;
    struct poulsback_frame_bo pds[2], vertices[2];
    struct dirent **names;
    int n, runs = 0;
    CHECK(dir);
    CHECK(!poulsback_frame_writer_create(&ops, &w));
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_PDS, POULSBACK_FRAME_PDS_BYTES, &pds[i]));
        CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, POULSBACK_FRAME_VERTEX_BYTES,
                          &vertices[i]));
    }
    for (unsigned f32 = 0; f32 < 4; f32++) {
        synthetic(f32 & 1, f32 >> 1, w, &pds[runs % 2], &vertices[runs % 2]);
        runs++;
    }
    n = scandir(dir, &names, NULL, by_name);
    CHECK(n > 0);
    for (unsigned pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            char path[1024];
            if (!strstr(names[i]->d_name, ".groups"))
                continue;
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]->d_name);
            run(path, w, &pds[runs % 2], &vertices[runs % 2], false, !runs);
            runs++;
            if (run(path, w, &pds[runs % 2], &vertices[runs % 2], true, false))
                runs++;
        }
    poulsback_frame_writer_destroy(w);
    printf("poulsback-frame-writer: %d frames equal\n", runs);
    return runs ? 0 : 1;
}
