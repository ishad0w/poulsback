// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The context's bound state: CSO objects, shaders, buffers, samplers and
 * views, the small setters, and the resource and blit hooks. */
#include "poulsback_context_private.h"

static struct state *find(struct poulsback_context *c, void *p, enum state_kind k)
{
    struct state *s;
    for (s = c->states; s; s = s->next)
        if (s == p && s->kind == k)
            return s;
    return NULL;
}
static struct state *allocate(struct poulsback_context *c, enum state_kind k)
{
    struct state *s = calloc(1, sizeof(*s));
    if (!s) {
        fail(c, -ENOMEM);
        return NULL;
    }
    s->kind = k;
    s->refs = 1;
    s->caller = true;
    s->next = c->states;
    c->states = s;
    return s;
}
void poulsback_context_release_state(struct poulsback_context *c, struct state *s)
{
    struct state **p;
    if (!s || --s->refs)
        return;
    if (s->kind == VS && s->u.vs.draw && s->u.vs.draw == c->draw_vs) {
        draw_bind_vertex_shader(c->draw, NULL);
        c->draw_vs = NULL;
    }
    if (s->kind == VS && s->u.vs.draw)
        draw_delete_vertex_shader(c->draw, s->u.vs.draw);
    if (s->kind == VS)
        poulsback_vertex_shader_destroy(s->u.vs.gpu);
    if (s->kind == FS) {
        if (s->u.fs.draw)
            draw_delete_fragment_shader(c->draw, s->u.fs.draw);
        ralloc_free(s->u.fs.nir);
        free(s->u.fs.ir);
        free(s->u.fs.program);
        free(s->u.fs.scene_program[0]);
        free(s->u.fs.scene_program[1]);
        poulsback_fragment_destroy(s->u.fs.backend);
    }
    for (p = &c->states; *p != s; p = &(*p)->next) {
    }
    *p = s->next;
    free(s);
}
void poulsback_context_bind_state(struct poulsback_context *c, void *p, enum state_kind k)
{
    struct state *s = p ? find(c, p, k) : NULL, *old = c->bound[k];
    if (p && (!s || !s->caller)) {
        fail(c, -EINVAL);
        return;
    }
    if (s)
        ++s->refs;
    if (k == RASTER && s) {
        draw_flush(c->draw);
        c->draw_raster = s->u.raster;
        c->draw_raster.cull_face = PIPE_FACE_NONE;
        draw_set_rasterizer_state(c->draw, &c->draw_raster, s);
    } else if (k == RASTER)
        draw_set_rasterizer_state(c->draw, s ? &s->u.raster : NULL, s);
    if (k == ELEMENTS)
        draw_set_vertex_elements(c->draw, s ? s->u.ve.count : 0, s ? s->u.ve.element : NULL);
    if (k == FS)
        draw_bind_fragment_shader(c->draw, s ? s->u.fs.draw : NULL);
    c->bound[k] = s;
    c->state_serial++;
    poulsback_context_release_state(c, old);
}
static void remove_state(struct pipe_context *p, void *data, enum state_kind k)
{
    struct poulsback_context *c = ctx(p);
    struct state *s = find(c, data, k);
    if (!s || !s->caller) {
        fail(c, -EINVAL);
        return;
    }
    s->caller = false;
    poulsback_context_release_state(c, s);
}
#define RAW_STATE(name, kind, type, member)                                                        \
    static void *create_##name(struct pipe_context *p, const type *v)                              \
    {                                                                                              \
        struct state *s;                                                                           \
        if (!v) {                                                                                  \
            fail(ctx(p), -EINVAL);                                                                 \
            return NULL;                                                                           \
        }                                                                                          \
        s = allocate(ctx(p), kind);                                                                \
        if (s)                                                                                     \
            s->u.member = *v;                                                                      \
        return s;                                                                                  \
    }                                                                                              \
    static void bind_##name(struct pipe_context *p, void *s)                                       \
    {                                                                                              \
        poulsback_context_bind_state(ctx(p), s, kind);                                             \
    }                                                                                              \
    static void delete_##name(struct pipe_context *p, void *s)                                     \
    {                                                                                              \
        remove_state(p, s, kind);                                                                  \
    }
RAW_STATE(raster, RASTER, struct pipe_rasterizer_state, raster)
RAW_STATE(blend, BLEND, struct pipe_blend_state, blend)
RAW_STATE(depth, DEPTH, struct pipe_depth_stencil_alpha_state, depth)
static void *create_elements(struct pipe_context *p, unsigned count,
                             const struct pipe_vertex_element *e)
{
    struct state *s;
    unsigned i;
    if (!count || count > PIPE_MAX_ATTRIBS || !e) {
        fail(ctx(p), -EINVAL);
        return NULL;
    }
    for (i = 0; i < count; i++)
        if (e[i].vertex_buffer_index >= PB_VERTEX_BUFFERS || e[i].instance_divisor ||
            e[i].dual_slot) {
            fail(ctx(p), -EOPNOTSUPP);
            return NULL;
        }
    s = allocate(ctx(p), ELEMENTS);
    if (s) {
        s->u.ve.count = count;
        memcpy(s->u.ve.element, e, count * sizeof(*e));
    }
    return s;
}
static void bind_elements(struct pipe_context *p, void *s)
{
    poulsback_context_bind_state(ctx(p), s, ELEMENTS);
}
static void delete_elements(struct pipe_context *p, void *s)
{
    remove_state(p, s, ELEMENTS);
}
static void *create_vs(struct pipe_context *p, const struct pipe_shader_state *v)
{
    struct poulsback_context *c = ctx(p);
    struct state *s;
    void *nir_owner = NULL;
    if (!v || (v->type != PIPE_SHADER_IR_NIR && v->type != PIPE_SHADER_IR_TGSI) ||
        v->stream_output.num_outputs) {
        if (v && v->type == PIPE_SHADER_IR_NIR)
            ralloc_free(v->ir.nir);
        fail(c, -EOPNOTSUPP);
        return NULL;
    }
    s = allocate(c, VS);
    if (!s) {
        if (v->type == PIPE_SHADER_IR_NIR)
            ralloc_free(v->ir.nir);
        return NULL;
    }
    /* Read the GPU program from NIR before the draw module consumes it;
     * draw keeps the shader for the draws the GPU path does not take. */
    if (v->type == PIPE_SHADER_IR_NIR) {
        const char *refusal;
        s->u.vs.gpu = poulsback_vertex_shader_create(v->ir.nir, &refusal);
        if (!s->u.vs.gpu && poulsback_trace_on()) {
            POULSBACK_TRACE("vs: on the CPU: %s\n", refusal);
            nir_print_shader(v->ir.nir, stderr);
        }
    }
    if (v->type == PIPE_SHADER_IR_NIR) {
        nir_owner = ralloc_context(NULL);
        if (!nir_owner) {
            ralloc_free(v->ir.nir);
            poulsback_context_release_state(c, s);
            fail(c, -ENOMEM);
            return NULL;
        }
        ralloc_steal(nir_owner, v->ir.nir);
    }
    /* The pinned noLLVM converter frees NIR after conversion, but draw's
   * initial exec-VS allocation can fail before consuming it. Keep a
   * temporary parent: a consumed child is already gone, an early-failure
   * child is freed exactly once here. TGSI remains duplicated by draw. */
    s->u.vs.draw = draw_create_vertex_shader(c->draw, v);
    ralloc_free(nir_owner);
    if (!s->u.vs.draw) {
        poulsback_context_release_state(c, s);
        fail(c, -EINVAL);
        return NULL;
    }
    return s;
}
static void bind_vs(struct pipe_context *p, void *s)
{
    poulsback_context_bind_state(ctx(p), s, VS);
}
static void delete_vs(struct pipe_context *p, void *s)
{
    remove_state(p, s, VS);
}
static bool fragment_inputs(struct draw_fragment_shader *draw,
                            const struct sgx535_fragment_linkage *link)
{
    struct tgsi_shader_info *info = &draw->info;
    unsigned i;
    /* Mesa26's legacy NIR->TGSI scanner derives fragment input semantics and
   * usage from declarations/load_deref. This validated profile has neither:
   * its canonical linkage is the complete normalized input interface. Keep
   * the ordinary draw object; supply only its owned input metadata here. */
    if (link->count > POULSBACK_DRAW_SLOTS || link->count > UINT8_MAX)
        return false;
    for (i = 0; i < link->count; i++) {
        const struct sgx535_fragment_varying *v = &link->varying[i];
        if (!v->components || v->components > 4 || v->index > UINT8_MAX ||
            (v->semantic != SGX535_FRAGMENT_COLOR && v->semantic != SGX535_FRAGMENT_GENERIC) ||
            (v->interpolation != SGX535_FRAGMENT_SMOOTH &&
             v->interpolation != SGX535_FRAGMENT_FLAT &&
             v->interpolation != SGX535_FRAGMENT_COLOR_INTERPOLATION))
            return false;
    }
    memset(info->input_semantic_name, 0, sizeof(info->input_semantic_name));
    memset(info->input_semantic_index, 0, sizeof(info->input_semantic_index));
    memset(info->input_usage_mask, 0, sizeof(info->input_usage_mask));
    memset(info->input_interpolate, 0, sizeof(info->input_interpolate));
    memset(info->input_interpolate_loc, 0, sizeof(info->input_interpolate_loc));
    info->num_inputs = (uint8_t)link->count;
    info->file_max[TGSI_FILE_INPUT] = link->count ? (int)link->count - 1 : -1;
    info->file_mask[TGSI_FILE_INPUT] = (1u << link->count) - 1u;
    for (i = 0; i < link->count; i++) {
        const struct sgx535_fragment_varying *v = &link->varying[i];
        info->input_semantic_name[i] =
            v->semantic == SGX535_FRAGMENT_COLOR ? TGSI_SEMANTIC_COLOR : TGSI_SEMANTIC_GENERIC;
        info->input_semantic_index[i] = (uint8_t)v->index;
        info->input_usage_mask[i] = (uint8_t)((1u << v->components) - 1u);
        info->input_interpolate[i] =
            v->interpolation == SGX535_FRAGMENT_FLAT     ? TGSI_INTERPOLATE_CONSTANT
            : v->interpolation == SGX535_FRAGMENT_SMOOTH ? TGSI_INTERPOLATE_PERSPECTIVE
                                                         : TGSI_INTERPOLATE_COLOR;
        info->input_interpolate_loc[i] = TGSI_INTERPOLATE_LOC_CENTER;
    }
    return true;
}
static int scene_fragment_profile(const struct sgx535_fragment_instruction *ir, size_t count,
                                  const struct sgx535_fragment_linkage *linkage, uint32_t *profile)
{
    struct sgx535_fragment_linkage canonical = *linkage;
    struct sgx535_fragment_normalized_candidate *checked;
    enum sgx535_nir_status status;
    /* The scene handoff requires COLOR0. Other interfaces retain the existing
   * later capture refusal; they cannot gain admission by choosing a profile. */
    *profile = POULSBACK_RENDER_PROFILE_COLOR0_RAW_SIGN_NORMALIZED;
    if (canonical.count != 1 || canonical.varying[0].semantic != SGX535_FRAGMENT_COLOR ||
        canonical.varying[0].index || canonical.varying[0].components != 4)
        return 0;
    canonical.varying[0].driver_base = 0;
    canonical.varying[0].interpolation = SGX535_FRAGMENT_SMOOTH;
    checked = calloc(1, sizeof(*checked));
    if (!checked)
        return -ENOMEM;
    /* These are the complete freshly lowered operations, including dead sign
   * operations. Select the normal grammar only after it validates them;
   * profile5 is independently checked, never relabelled as profile4. Capture
   * subsequently validates the retained NIR and actual interpolation again. */
    status =
        sgx535_fragment_compile_dri_color0_normalized_candidate(ir, count, &canonical, checked);
    if (status == SGX535_NIR_OK)
        *profile = POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED;
    else if (status != SGX535_NIR_ERR_NOMEM)
        status = sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
            ir, count, &canonical, checked);
    free(checked);
    return status == SGX535_NIR_OK ? 0 : status == SGX535_NIR_ERR_NOMEM ? -ENOMEM : -EOPNOTSUPP;
}
static enum sgx535_nir_status solid_scene_shader(struct state *s,
                                                 enum sgx535_fragment_uniform_layout layout)
{
    struct poulsback_nir_solid_fragment *work = calloc(1, sizeof(*work));
    if (!work)
        return SGX535_NIR_ERR_NOMEM;
    enum sgx535_nir_status status =
        poulsback_nir_solid_fragment_source_ir(s->u.fs.nir, layout, work);
    if (status == SGX535_NIR_OK) {
        memcpy(s->u.fs.ir, work->instruction, work->instruction_count * sizeof(*work->instruction));
        s->u.fs.count = work->instruction_count;
        memset(&s->u.fs.linkage, 0, sizeof(s->u.fs.linkage));
        s->u.fs.scene_profile = POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED;
        s->u.fs.solid = true;
    }
    free(work);
    return status;
}
static void *create_fs(struct pipe_context *p, const struct pipe_shader_state *v)
{
    struct poulsback_context *c = ctx(p);
    struct state *s;
    enum sgx535_nir_status status;
    if (!v || v->type != PIPE_SHADER_IR_NIR || !v->ir.nir || v->stream_output.num_outputs) {
        if (v && v->type == PIPE_SHADER_IR_NIR)
            ralloc_free(v->ir.nir);
        fail(c, -EOPNOTSUPP);
        return NULL;
    }
    s = allocate(c, FS);
    if (!s) {
        ralloc_free(v->ir.nir);
        return NULL;
    }
    s->u.fs.nir = v->ir.nir;
    /* Gallium transfers this variant to the driver. The screen callback may
   * already have finalized it; repeat the structural pass idempotently for
   * direct context callers, then retain/free this same owner exactly once. */
    poulsback_nir_finalize_fragment(s->u.fs.nir);
    s->u.fs.ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*s->u.fs.ir));
    if (!s->u.fs.ir) {
        fail(c, -ENOMEM);
        goto error;
    }
    /* The state tracker selects its standard layout from this exact cap.
   * Never infer address units from shader values or silently change caps. */
    {
        enum sgx535_fragment_uniform_layout layout = p->screen->caps.packed_uniforms
                                                         ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                         : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS;
        status = solid_scene_shader(s, layout);
        if (status == SGX535_NIR_ERR_UNSUPPORTED)
            status = sgx535_nir_fragment_raw_sign_inf_ir(
                v->ir.nir,
                p->screen->caps.packed_uniforms ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                s->u.fs.ir, SGX535_NIR_MAX_INSTRUCTIONS, &s->u.fs.count, &s->u.fs.linkage);
        if (status != SGX535_NIR_OK) {
            /* a program that only the backend takes draws on the GPU path
             * alone: solid unless it reads the colour */
            const char *refusal;
            s->u.fs.backend =
                poulsback_fragment_create(s->u.fs.nir, p->screen->caps.packed_uniforms, &refusal);
            /* none: a refused CSO, whose draws are skipped */
            if (!s->u.fs.backend) {
                s->u.fs.refused = true;
                snprintf(s->u.fs.refusal, sizeof(s->u.fs.refusal), "%s", refusal);
                fprintf(stderr, "poulsback: fragment program refused: %s\n", refusal);
            }
            s->u.fs.backend_only = true;
            s->u.fs.solid = !s->u.fs.backend || !poulsback_fragment_reads_color(s->u.fs.backend);
            s->u.fs.count = 0;
            memset(&s->u.fs.linkage, 0, sizeof(s->u.fs.linkage));
            s->u.fs.scene_profile = POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED;
        }
        /* Lowering a private projection must not hide an original dead load
         * from the later complete constant-buffer binding check. */
        nir_foreach_function_impl(impl, s->u.fs.nir) nir_foreach_block(block, impl)
            nir_foreach_instr(instr, block) if (instr->type == nir_instr_type_intrinsic &&
                                                nir_instr_as_intrinsic(instr)->intrinsic ==
                                                    nir_intrinsic_load_uniform)
                s->u.fs.has_uniform = true;
        if (!s->u.fs.solid && !s->u.fs.backend_only) {
            int e = scene_fragment_profile(s->u.fs.ir, s->u.fs.count, &s->u.fs.linkage,
                                           &s->u.fs.scene_profile);
            if (e) {
                fail(c, e);
                goto error;
            }
        }
    }
    /* Only metadata is installed; draw never executes fragment instructions. */
    s->u.fs.draw = draw_create_fragment_shader(c->draw, v);
    if (!s->u.fs.draw) {
        fail(c, -ENOMEM);
        goto error;
    }
    if (!fragment_inputs(s->u.fs.draw, &s->u.fs.linkage)) {
        fail(c, -EOPNOTSUPP);
        goto error;
    }
    /* The backend's program for GPU-shaded draws beside the
     * bounded COLOR0 program used by CPU-shaded draws. */
    if (!s->u.fs.backend && !s->u.fs.refused) {
        const char *refusal;
        s->u.fs.backend =
            poulsback_fragment_create(s->u.fs.nir, p->screen->caps.packed_uniforms, &refusal);
        if (!s->u.fs.backend)
            POULSBACK_TRACE("fragment program on the legacy path: %s\n", refusal);
    }
    return s;
error:
    poulsback_fragment_destroy(s->u.fs.backend);
    s->u.fs.backend = NULL;
    if (s->u.fs.draw)
        draw_delete_fragment_shader(c->draw, s->u.fs.draw);
    ralloc_free(s->u.fs.nir);
    s->u.fs.nir = NULL;
    free(s->u.fs.ir);
    s->u.fs.ir = NULL;
    free(s->u.fs.program);
    s->u.fs.program = NULL;
    {
        struct state **link;
        for (link = &c->states; *link != s; link = &(*link)->next) {
        }
        *link = s->next;
    }
    free(s);
    return NULL;
}
static void bind_fs(struct pipe_context *p, void *s)
{
    poulsback_context_bind_state(ctx(p), s, FS);
}
static void delete_fs(struct pipe_context *p, void *s)
{
    remove_state(p, s, FS);
}
int poulsback_context_snapshot_fragment(struct poulsback_context *c)
{
    const struct state *fs = c->bound[FS];
    struct sgx535_fragment_instruction *ir;
    struct sgx535_fragment_linkage linkage;
    nir_shader *copy = NULL;
    size_t count = 0;
    uint32_t profile = 0;
    enum sgx535_nir_status status;
    enum sgx535_fragment_uniform_layout layout = c->base.screen->caps.packed_uniforms
                                                     ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                     : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS;
    /* Solid CSOs retain original UNIFORM/MAD operations. Their capture copies
     * this draw's already-owned CB0 prefix; other interfaces keep their
     * existing per-binding specialization policy. */
    if (!fs->u.fs.has_uniform || fs->u.fs.solid) {
        c->scene_fragment = fs->u.fs.nir;
        c->scene_fragment_profile = fs->u.fs.scene_profile;
        c->scene_fragment_owned = false;
        return 0;
    }
    int e = poulsback_nir_specialize_fragment(fs->u.fs.nir, layout, c->constant[1].data,
                                              c->constant[1].bytes, &copy);
    if (e)
        return e;
    ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    if (!ir) {
        ralloc_free(copy);
        return -ENOMEM;
    }
    /* Specialization grants no shader/profile capability. Validate the full
   * new source again, including unused/nonfinite constants and original
   * FP controls, before the normal collector can capture any primitive. */
    if (fs->u.fs.solid) {
        struct poulsback_nir_solid_fragment *work = calloc(1, sizeof(*work));
        if (!work) {
            free(ir);
            ralloc_free(copy);
            return -ENOMEM;
        }
        status = poulsback_nir_solid_fragment_ir(copy, layout, work);
        free(work);
        profile = POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED;
    } else
        status = sgx535_nir_fragment_raw_sign_inf_ir(copy, layout, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                                     &count, &linkage);
    e = status == SGX535_NIR_OK
            ? (fs->u.fs.solid ? 0 : scene_fragment_profile(ir, count, &linkage, &profile))
        : status == SGX535_NIR_ERR_NOMEM ? -ENOMEM
                                         : -EOPNOTSUPP;
    free(ir);
    if (e) {
        ralloc_free(copy);
        return e;
    }
    c->scene_fragment = copy;
    c->scene_fragment_profile = profile;
    c->scene_fragment_owned = true;
    return 0;
}
static void vertex_buffers(struct pipe_context *p, unsigned n, const struct pipe_vertex_buffer *v)
{
    struct poulsback_context *c = ctx(p);
    unsigned i;
    struct pipe_vertex_buffer next[PB_VERTEX_BUFFERS] = {0};
    if (n > PB_VERTEX_BUFFERS || (n && !v)) {
        fail(c, -EINVAL);
        return;
    }
    /* A caller can move a borrowed final bound reference to another slot.
   * Hold the complete incoming graph before dropping any old reference;
   * copying descriptors first also handles an aliased input array. */
    for (i = 0; i < n; i++) {
        struct pipe_resource *new_ref = NULL;
        next[i] = v[i];
        if (!next[i].is_user_buffer) {
            pipe_resource_reference(&new_ref, next[i].buffer.resource);
            next[i].buffer.resource = new_ref;
        }
    }
    for (i = 0; i < PB_VERTEX_BUFFERS; i++) {
        if (!c->vb[i].is_user_buffer)
            pipe_resource_reference(&c->vb[i].buffer.resource, NULL);
        c->vb[i] = next[i];
    }
    c->vb_count = n;
}
static int constant_snapshot(struct pipe_resource *resource, unsigned offset, unsigned bytes,
                             uint8_t **out)
{
    void *copy = NULL;
    int e = poulsback_resource_buffer_snapshot_range(resource, offset, bytes, &copy);
    if (!e)
        *out = copy;
    return e;
}
static void constants(struct pipe_context *p, mesa_shader_stage stage, uint index,
                      const struct pipe_constant_buffer *b)
{
    struct poulsback_context *c = ctx(p);
    uint8_t *copy = NULL;
    int e;
    unsigned k = stage == MESA_SHADER_VERTEX ? 0 : 1, bytes = b ? b->buffer_size : 0;
    if (!b && index) /* unbinding a slot this context never binds */
        return;
    if ((stage != MESA_SHADER_VERTEX && stage != MESA_SHADER_FRAGMENT) || index ||
        bytes > (k ? POULSBACK_RENDER_UNIFORM_WORDS * 4u : PB_CONSTANT_BYTES) || (bytes & 3u) ||
        (b && b->buffer && b->user_buffer)) {
        fail(c, -EOPNOTSUPP);
        return;
    }
    if (bytes) {
        if (!b->buffer && !b->user_buffer) {
            fail(c, -EINVAL);
            return;
        }
        if (b->user_buffer &&
            ((uintptr_t)b->user_buffer > UINTPTR_MAX - b->buffer_offset ||
             (uintptr_t)b->user_buffer + b->buffer_offset > UINTPTR_MAX - (bytes - 1u))) {
            fail(c, -EINVAL);
            return;
        }
        if (b->buffer) {
            e = constant_snapshot(b->buffer, b->buffer_offset, bytes, &copy);
            if (e) {
                fail(c, e);
                return;
            }
        } else {
            copy = malloc(bytes);
            if (!copy) {
                fail(c, -ENOMEM);
                return;
            }
            memcpy(copy, (const void *)((uintptr_t)b->user_buffer + b->buffer_offset), bytes);
        }
    }
    if (!k)
        draw_set_mapped_constant_buffer(c->draw, MESA_SHADER_VERTEX, 0, copy, bytes);
    free(c->constant[k].data);
    c->constant[k].data = copy;
    c->constant[k].bytes = bytes;
    pipe_resource_reference(&c->constant[k].resource, bytes ? b->buffer : NULL);
    c->constant[k].offset = bytes && b->buffer ? b->buffer_offset : 0;
}
void poulsback_context_cpu_draw_begin(struct poulsback_context *c,
                                      const struct pipe_vertex_buffer *vb,
                                      const void *const *buffers, const size_t *sizes,
                                      const void *indices, unsigned index_size,
                                      unsigned index_bytes)
{
    unsigned i;
    if (c->draw_vs != c->bound[VS]->u.vs.draw) {
        draw_bind_vertex_shader(c->draw, c->bound[VS]->u.vs.draw);
        c->draw_vs = c->bound[VS]->u.vs.draw;
    }
    draw_set_vertex_buffers(c->draw, c->vb_count, vb);
    for (i = 0; i < c->vb_count; i++)
        draw_set_mapped_vertex_buffer(c->draw, i, buffers[i], sizes[i]);
    draw_set_indexes(c->draw, indices, index_size, index_bytes);
    c->draw_inputs = true;
}
void poulsback_context_cpu_draw_end(struct poulsback_context *c)
{
    unsigned i;
    if (!c->draw_inputs)
        return;
    draw_flush(c->draw);
    draw_set_vertex_buffers(c->draw, 0, NULL);
    draw_set_indexes(c->draw, NULL, 0, 0);
    for (i = 0; i < PB_VERTEX_BUFFERS; i++)
        draw_set_mapped_vertex_buffer(c->draw, i, NULL, 0);
    c->draw_inputs = false;
}
int poulsback_context_refresh_constants(struct poulsback_context *c)
{
    uint8_t *next[2] = {0};
    unsigned k;
    int e = 0;
    /* Resource bindings remain live across subdata/map writes. Refresh both
   * stages before executing vertices or publishing a fragment program;
   * user-buffer bindings retain their owned bind-time copy. */
    for (k = 0; k < 2; k++)
        if (c->constant[k].resource) {
            e = constant_snapshot(c->constant[k].resource, c->constant[k].offset,
                                  c->constant[k].bytes, &next[k]);
            if (e)
                goto done;
        }
    for (k = 0; k < 2; k++)
        if (c->constant[k].resource) {
            if (!k)
                draw_set_mapped_constant_buffer(c->draw, MESA_SHADER_VERTEX, 0, next[k],
                                                c->constant[k].bytes);
            free(c->constant[k].data);
            c->constant[k].data = next[k];
            next[k] = NULL;
        }
done:
    for (k = 0; k < 2; k++)
        free(next[k]);
    return e;
}
static void framebuffer(struct pipe_context *p, const struct pipe_framebuffer_state *fb)
{
    struct poulsback_context *c = ctx(p);
    int e = poulsback_context_flush_frame(c);
    if (!e)
        e = poulsback_surface_framebuffer_set(p->screen, &c->fb, fb);
    c->state_serial++;
    POULSBACK_TRACE("framebuffer %ux%u cbufs %u (%s) zs %s -> %d\n", fb->width, fb->height,
                    fb->nr_cbufs, fb->nr_cbufs ? util_format_name(fb->cbufs[0].format) : "-",
                    fb->zsbuf.texture ? util_format_name(fb->zsbuf.format) : "-", e);
    if (e)
        fail(ctx(p), e);
}
static void *create_sampler(struct pipe_context *p, const struct pipe_sampler_state *s)
{
    void *out = NULL;
    int e = poulsback_sampler_state_create(ctx(p)->samplers, s, &out);
    if (e)
        fail(ctx(p), e);
    return out;
}
static void bind_samplers(struct pipe_context *p, mesa_shader_stage stage, unsigned start,
                          unsigned count, void **states)
{
    int e = poulsback_sampler_states_bind(ctx(p)->samplers, stage, start, count, states);
    if (e)
        fail(ctx(p), e);
}
static void delete_sampler(struct pipe_context *p, void *state)
{
    int e = poulsback_sampler_state_delete(ctx(p)->samplers, state);
    if (e)
        fail(ctx(p), e);
}
static struct pipe_sampler_view *create_sampler_view(struct pipe_context *p,
                                                     struct pipe_resource *r,
                                                     const struct pipe_sampler_view *t)
{
    struct pipe_sampler_view *out = NULL;
    int e = poulsback_sampler_view_create(ctx(p)->samplers, r, t, &out);
    if (e)
        fail(ctx(p), e);
    return out;
}
static void set_sampler_views(struct pipe_context *p, mesa_shader_stage stage, unsigned start,
                              unsigned count, unsigned trailing, struct pipe_sampler_view **views)
{
    int e = poulsback_sampler_views_bind(ctx(p)->samplers, stage, start, count, trailing, views);
    if (e)
        fail(ctx(p), e);
}
static void destroy_sampler_view(struct pipe_context *p, struct pipe_sampler_view *view)
{
    int e = poulsback_sampler_view_destroy(p, view);
    if (e)
        fail(ctx(p), e);
}
static void viewport(struct pipe_context *p, unsigned start, unsigned n,
                     const struct pipe_viewport_state *v)
{
    struct poulsback_context *c = ctx(p);
    if (start || n != 1 || !v) {
        fail(c, -EOPNOTSUPP);
        return;
    }
    c->viewport = *v;
    c->have_viewport = true;
    c->state_serial++;
    {
        /* The MTE applies the viewport: the draw module transforms nothing. */
        static const struct pipe_viewport_state identity = {
            .scale = {1, 1, 1},
            .swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X,
            .swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y,
            .swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z,
            .swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W};
        draw_set_viewport_states(c->draw, 0, 1, &identity);
    }
}
static void scissor(struct pipe_context *p, unsigned start, unsigned n,
                    const struct pipe_scissor_state *s)
{
    if (start || n != 1 || !s) {
        fail(ctx(p), -EOPNOTSUPP);
        return;
    }
    ctx(p)->scissor = *s;
    ctx(p)->have_scissor = true;
}
static void blend_color(struct pipe_context *p, const struct pipe_blend_color *color)
{
    /* Constant blend color is inert while blending is disabled. Keep the
   * caller's exact bits, without imposing floating-point restrictions. */
    if (!color) {
        fail(ctx(p), -EINVAL);
        return;
    }
    memcpy(&ctx(p)->blend_color, color, sizeof(*color));
    ctx(p)->state_serial++;
}
/* Called by st/mesa on every validation. The rasterizer CSO refuses
 * poly_stipple_enable, so the pattern is never used. */
static void polygon_stipple(struct pipe_context *p, const struct pipe_poly_stipple *s)
{
    (void)p;
    (void)s;
}
/* One in-order GPU queue and CPU-coherent buffers: barriers, resource flushes
 * and invalidation have nothing to do. */
static void barrier(struct pipe_context *p, unsigned flags)
{
    (void)p;
    (void)flags;
}
static void resource_noop(struct pipe_context *p, struct pipe_resource *r)
{
    (void)p;
    (void)r;
}
/* CPU access to the open frame's target sees that frame: the frame is
 * submitted first and the resource layer waits for its job. Without this a
 * read before the swap returned the buffer's previous frame. */
static void scene_access(struct pipe_context *p, const struct pipe_resource *r)
{
    struct poulsback_context *c = ctx(p);
    if (c->frame && r &&
        (r == c->scene_target || r == c->scene_depth_target || poulsback_context_samples(c, r)))
        p->flush(p, NULL, 0);
}
static void *map_texture(struct pipe_context *p, struct pipe_resource *r, unsigned level,
                         unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
    scene_access(p, r);
    return ctx(p)->resource_map(p, r, level, usage, box, out);
}
static void texture_subdata(struct pipe_context *p, struct pipe_resource *r, unsigned level,
                            unsigned usage, const struct pipe_box *box, const void *data,
                            unsigned stride, uintptr_t layer_stride)
{
    scene_access(p, r);
    ctx(p)->resource_subdata(p, r, level, usage, box, data, stride, layer_stride);
}
/* A non-empty 2D box inside level 0 of r. */
static bool box_inside(const struct pipe_box *b, const struct pipe_resource *r)
{
    return !b->z && b->depth == 1 && b->x >= 0 && b->y >= 0 && b->width > 0 && b->height > 0 &&
           (int64_t)b->x + b->width <= r->width0 && (int64_t)b->y + b->height <= r->height0;
}
/* A copy the 2D core does: whole pixels of one format between two colour
 * images (or within one), unscaled and unclipped, inside both. */
static bool image_copy(const struct pipe_blit_info *b)
{
    const struct pipe_resource *d = b->dst.resource, *s = b->src.resource;
    const struct util_format_description *f;
    if (!d || !s || b->dst.level || b->src.level || b->dst.format != d->format ||
        b->src.format != s->format || d->format != s->format || d->nr_samples > 1 ||
        s->nr_samples > 1 || !box_inside(&b->dst.box, d) || !box_inside(&b->src.box, s) ||
        b->src.box.width != b->dst.box.width || b->src.box.height != b->dst.box.height ||
        b->dst_sample || b->scissor_enable || b->swizzle_enable || b->window_rectangle_include ||
        b->num_window_rectangles || b->alpha_blend)
        return false;
    f = util_format_description(d->format);
    return f && f->colorspace == UTIL_FORMAT_COLORSPACE_RGB && !(b->mask & ~PIPE_MASK_RGBA) &&
           util_format_colormask_full(f, b->mask);
}
/* What the DRI loader asks of blit: a back buffer's contents kept across its
 * reallocation (Present stopped flipping) or a swap, and the fake front's
 * copy of the back. The copy is a 2D-core job on the render's engine
 * context, after this context's draws into either image and its earlier
 * jobs, and ordered by the images' reservations against other clients; the
 * CPU does not wait. Any other blit stops the context (no software fallback). */
static void blit(struct pipe_context *p, const struct pipe_blit_info *info)
{
    struct poulsback_context *c = ctx(p);
    struct poulsback_ws_image *dst = NULL, *src = NULL;
    struct pipe_fence_handle *fence = NULL;
    int e, fd = -1;
    if (c->error)
        return;
    if (!info || !image_copy(info)) {
        c->refusal = "blit";
        fail(c, -EOPNOTSUPP);
        return;
    }
    draw_flush(c->draw);
    scene_access(p, info->src.resource);
    scene_access(p, info->dst.resource);
    if (c->error)
        return;
    e = poulsback_resource_image_acquire(info->dst.resource, &dst);
    if (!e)
        e = poulsback_resource_image_acquire(info->src.resource, &src);
    if (!e)
        e = poulsback_ws_render_copy(
            c->render, dst, src,
            &(struct poulsback_engine_copy){info->dst.box.x, info->dst.box.y, info->src.box.x,
                                            info->src.box.y, (uint32_t)info->dst.box.width,
                                            (uint32_t)info->dst.box.height},
            &fd);
    if (!e)
        e = poulsback_fence_adopt(fd, &fence);
    for (unsigned i = 0; i < 2; i++) {
        struct poulsback_ws_image *image = i ? src : dst;
        int cleanup = image ? poulsback_ws_image_unref(image) : 0;
        if (!e)
            e = cleanup;
    }
    if (e) {
        if (fence)
            p->screen->fence_reference(p->screen, &fence, NULL);
        fail(c, e);
        return;
    }
    p->screen->fence_reference(p->screen, &c->last_fence, fence);
    p->screen->fence_reference(p->screen, &fence, NULL);
}
/* Another context's fence: wait on the CPU (there is no GPU-side wait). */
static void fence_server_sync(struct pipe_context *p, struct pipe_fence_handle *f, uint64_t value)
{
    (void)value;
    p->screen->fence_finish(p->screen, NULL, f, OS_TIMEOUT_INFINITE);
}
static void stencil_ref(struct pipe_context *p, const struct pipe_stencil_ref ref)
{
    ctx(p)->stencil_ref = ref;
    ctx(p)->state_serial++;
}
static void sample_mask(struct pipe_context *p, unsigned mask)
{
    ctx(p)->sample_mask = mask;
    ctx(p)->state_serial++;
}
/* 0 and 1 both shade each pixel once (Mesa's clear quad sets 1); the
 * window's single-sample form takes 0. */
static void min_samples(struct pipe_context *p, unsigned count)
{
    ctx(p)->min_samples = count > 1 ? count : 0;
    ctx(p)->state_serial++;
}
/* The bound state's fragment state for a backend draw's colour (FLAT), its
 * constants, textures and viewport left to the draw: built again only after
 * a bind or a setter. NULL: a blend factor without a form. */
const struct poulsback_fragment_state *poulsback_context_fragment_state(struct poulsback_context *c,
                                                                        bool flat)
{
    struct poulsback_packed_state *k = &c->packed_state;
    if (!k->valid || k->serial != c->state_serial || k->flat != flat) {
        k->valid = true, k->serial = c->state_serial, k->flat = flat;
        k->ok = poulsback_fragment_state(&c->bound[RASTER]->u.raster, &c->bound[DEPTH]->u.depth,
                                         &c->bound[BLEND]->u.blend, &c->blend_color,
                                         &c->stencil_ref, &c->fb, flat, &k->state);
    }
    return k->ok ? &k->state : NULL;
}
/* A backend draw's frame shape: the group's template (the colour passed
 * through, or a constant; SOLID) and its interpolation (FLAT), built again
 * only after a bind, a setter, or another clear or depth target. */
int poulsback_context_draw_shape(struct poulsback_context *c, bool solid, bool flat,
                                 const poulsback_frame_shape **shape)
{
    struct poulsback_packed_shape *k = &c->packed_shape;
    const bool depth = c->scene_depth_target != NULL;
    if (!k->valid || k->serial != c->state_serial || k->solid != solid || k->flat != flat ||
        k->clear != c->frame_clear || k->depth != depth || k->fragment != c->scene_fragment) {
        k->valid = true, k->serial = c->state_serial, k->solid = solid, k->flat = flat;
        k->clear = c->frame_clear, k->depth = depth, k->fragment = c->scene_fragment;
        k->status = poulsback_context_frame_shape(
            c, &poulsback_template_program[!solid], solid, flat,
            solid ? poulsback_template_uniforms : NULL, solid ? 4u : 0,
            POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED, true, false, &k->shape);
    }
    *shape = &k->shape;
    return k->status;
}
static void clip(struct pipe_context *p, const struct pipe_clip_state *s)
{
    if (!s) {
        fail(ctx(p), -EINVAL);
        return;
    }
    draw_set_clip_state(ctx(p)->draw, s);
}
/* The GPU's loss, reported once per context (poulsback_lost.h). */
static enum pipe_reset_status reset_status(struct pipe_context *p)
{
    return poulsback_lost_status(&ctx(p)->lost);
}
static void reset_callback(struct pipe_context *p, const struct pipe_device_reset_callback *cb)
{
    poulsback_lost_set_callback(&ctx(p)->lost, cb);
}
/* Installs the state, shader, sampler, resource, blit and reset callbacks. */
void poulsback_context_state_init(struct poulsback_context *c)
{
    c->base.get_device_reset_status = reset_status;
    c->base.set_device_reset_callback = reset_callback;
    c->base.create_rasterizer_state = create_raster;
    c->base.bind_rasterizer_state = bind_raster;
    c->base.delete_rasterizer_state = delete_raster;
    c->base.create_blend_state = create_blend;
    c->base.bind_blend_state = bind_blend;
    c->base.delete_blend_state = delete_blend;
    c->base.create_depth_stencil_alpha_state = create_depth;
    c->base.bind_depth_stencil_alpha_state = bind_depth;
    c->base.delete_depth_stencil_alpha_state = delete_depth;
    c->base.create_vertex_elements_state = create_elements;
    c->base.bind_vertex_elements_state = bind_elements;
    c->base.delete_vertex_elements_state = delete_elements;
    c->base.create_vs_state = create_vs;
    c->base.bind_vs_state = bind_vs;
    c->base.delete_vs_state = delete_vs;
    c->base.create_fs_state = create_fs;
    c->base.bind_fs_state = bind_fs;
    c->base.delete_fs_state = delete_fs;
    c->base.set_vertex_buffers = vertex_buffers;
    c->base.set_constant_buffer = constants;
    c->base.set_framebuffer_state = framebuffer;
    c->base.set_viewport_states = viewport;
    c->base.set_scissor_states = scissor;
    c->base.set_clip_state = clip;
    c->base.set_blend_color = blend_color;
    c->base.set_stencil_ref = stencil_ref;
    c->base.set_polygon_stipple = polygon_stipple;
    c->base.texture_barrier = barrier;
    c->base.memory_barrier = barrier;
    c->base.blit = blit;
    c->base.flush_resource = resource_noop;
    c->base.invalidate_resource = resource_noop;
    c->base.fence_server_sync = fence_server_sync;
    c->base.set_sample_mask = sample_mask;
    c->base.set_min_samples = min_samples;
    c->base.create_sampler_state = create_sampler;
    c->base.bind_sampler_states = bind_samplers;
    c->base.delete_sampler_state = delete_sampler;
    c->base.create_sampler_view = create_sampler_view;
    c->base.set_sampler_views = set_sampler_views;
    c->base.sampler_view_destroy = destroy_sampler_view;
    c->base.sampler_view_release = u_default_sampler_view_release;
    poulsback_resource_context_init(&c->base);
    c->resource_map = c->base.texture_map;
    c->resource_subdata = c->base.texture_subdata;
    c->base.texture_map = map_texture;
    c->base.texture_subdata = texture_subdata;
}
