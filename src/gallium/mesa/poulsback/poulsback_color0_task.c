// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_color0_task.h"
#include "poulsback_selfcheck.h"
#include "poulsback_nir.h"
#include "draw/draw_context.h"
#include "draw/draw_fs.h"
#include "draw/draw_pipe.h"
#include "nir.h"
#include "sgx535_clip_w1.h"
#include "sgx535_fragment_internal.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "tgsi/tgsi_scan.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool span(const void *p, size_t bytes)
{
    return p && bytes <= UINTPTR_MAX - (uintptr_t)p;
}
/* The draw module's producer: CPU clipping and viewport (the window path), or
 * for a scene the clip-space path, where nothing is clipped, culled or
 * transformed on the CPU (no clip producer, an identity viewport, cull NONE,
 * no two-sided lighting; poulsback_draw_stage_begin_clip_space). */
static bool clip_space(const struct draw_context *d)
{
    return !d->clip_xy && !d->clip_z && !d->clip_user && d->bypass_viewport &&
           d->rasterizer->cull_face == PIPE_FACE_NONE && !d->rasterizer->light_twoside;
}
static bool state_supported(const struct draw_context *d, bool scene)
{
    const struct pipe_rasterizer_state *r = d->rasterizer;
    return r && !d->llvm && !d->ms.mesh_shader &&
           ((!d->bypass_viewport && (d->clip_xy || d->clip_z || d->clip_user)) ||
            (scene && clip_space(d))) &&
           d->vs.vertex_shader && d->fs.fragment_shader && !d->gs.geometry_shader &&
           !d->tes.tess_eval_shader && !d->tcs.tess_ctrl_shader &&
           r->fill_front == PIPE_POLYGON_MODE_FILL && r->fill_back == PIPE_POLYGON_MODE_FILL &&
           !r->offset_tri && !r->offset_line && !r->offset_point && (scene || !r->light_twoside) &&
           !r->rasterizer_discard && !r->poly_smooth && !r->poly_stipple_enable &&
           !r->multisample && !r->force_persample_interp && !r->depth_clamp && !r->clip_halfz &&
           r->depth_clip_near && r->depth_clip_far && !r->rasterization_stream &&
           r->conservative_raster_mode == PIPE_CONSERVATIVE_RASTER_OFF &&
           (scene || (!r->half_pixel_center && !r->bottom_edge_rule)) && !r->clamp_fragment_color;
}
static bool source_policy(const struct draw_context *d, const struct poulsback_draw_batch *b,
                          struct poulsback_color0_source *out, bool scene)
{
    const struct tgsi_shader_info *vs, *fs;
    const struct poulsback_draw_attribute *a = &b->layout.slot[0];
    unsigned i, position, interpolation;
    if (!state_supported(d, scene) || !b->triangle_count ||
        b->triangle_count > (scene ? POULSBACK_DRAW_MAX_TRIANGLES : 1u) || b->stride_words != 8 ||
        !b->words || (!b->window_position && !clip_space(d)) || b->layout.provoking_first > 1 ||
        b->layout.provoking_first != d->rasterizer->flatshade_first || a->components != 4 ||
        (a->interpolation != POULSBACK_DRAW_SMOOTH && a->interpolation != POULSBACK_DRAW_FLAT))
        return false;
    for (i = 1; i < POULSBACK_DRAW_SLOTS; i++) {
        const struct poulsback_draw_attribute *unused = &b->layout.slot[i];
        if (unused->source || unused->fragment_input || unused->components ||
            unused->interpolation != POULSBACK_DRAW_UNUSED)
            return false;
    }
    vs = draw_get_shader_info(d);
    fs = &d->fs.fragment_shader->info;
    position = draw_current_shader_position_output(d);
    if (vs->num_outputs > PIPE_MAX_SHADER_OUTPUTS || position >= vs->num_outputs ||
        position != b->position_output || a->source >= vs->num_outputs || a->source == position ||
        vs->properties[TGSI_PROPERTY_VS_WINDOW_SPACE_POSITION] ||
        vs->output_semantic_name[position] != TGSI_SEMANTIC_POSITION ||
        vs->output_semantic_index[position] || vs->output_usagemask[position] != 15 ||
        vs->output_semantic_name[a->source] != TGSI_SEMANTIC_COLOR ||
        vs->output_semantic_index[a->source] || vs->output_usagemask[a->source] != 15 ||
        fs->num_inputs > POULSBACK_DRAW_SLOTS || a->fragment_input >= fs->num_inputs)
        return false;
    for (i = 0; i < fs->num_inputs; i++)
        if (fs->input_usage_mask[i] != (i == a->fragment_input ? 15u : 0u))
            return false;
    if (fs->input_semantic_name[a->fragment_input] != TGSI_SEMANTIC_COLOR ||
        fs->input_semantic_index[a->fragment_input] ||
        fs->input_interpolate_loc[a->fragment_input] != TGSI_INTERPOLATE_LOC_CENTER)
        return false;
    interpolation = fs->input_interpolate[a->fragment_input];
    if (interpolation == TGSI_INTERPOLATE_COLOR)
        interpolation =
            d->rasterizer->flatshade ? TGSI_INTERPOLATE_CONSTANT : TGSI_INTERPOLATE_PERSPECTIVE;
    if ((a->interpolation == POULSBACK_DRAW_FLAT && interpolation != TGSI_INTERPOLATE_CONSTANT) ||
        (a->interpolation == POULSBACK_DRAW_SMOOTH &&
         interpolation != TGSI_INTERPOLATE_PERSPECTIVE))
        return false;
    memset(out, 0, sizeof(*out));
    out->layout = b->layout;
    out->rasterizer = *d->rasterizer;
    memcpy(out->viewport, d->viewports, sizeof(out->viewport));
    out->position_output = position;
    out->color_output = a->source;
    out->fragment_input = a->fragment_input;
    out->clip_xy = d->clip_xy;
    out->clip_z = d->clip_z;
    out->clip_user = d->clip_user;
    return true;
}
static bool capture_source(const struct draw_context *d, const struct poulsback_draw_batch *b,
                           struct poulsback_color0_source *out, bool solid)
{
    if (!solid)
        return source_policy(d, b, out, true);
    if (!d || !b || !out || !state_supported(d, true) || !b->triangle_count ||
        b->triangle_count > POULSBACK_DRAW_MAX_TRIANGLES || b->stride_words != 4 || !b->words ||
        (!b->window_position && !clip_space(d)) || b->layout.provoking_first > 1 ||
        b->layout.provoking_first != d->rasterizer->flatshade_first ||
        d->fs.fragment_shader->info.num_inputs)
        return false;
    for (unsigned i = 0; i < POULSBACK_DRAW_SLOTS; i++) {
        const struct poulsback_draw_attribute *a = &b->layout.slot[i];
        if (a->source || a->fragment_input || a->components ||
            a->interpolation != POULSBACK_DRAW_UNUSED)
            return false;
    }
    const struct tgsi_shader_info *vs = draw_get_shader_info(d);
    unsigned position = draw_current_shader_position_output(d);
    if (!vs || vs->num_outputs > PIPE_MAX_SHADER_OUTPUTS || position >= vs->num_outputs ||
        position != b->position_output || vs->properties[TGSI_PROPERTY_VS_WINDOW_SPACE_POSITION] ||
        vs->output_semantic_name[position] != TGSI_SEMANTIC_POSITION ||
        vs->output_semantic_index[position] || vs->output_usagemask[position] != 15)
        return false;
    memset(out, 0, sizeof(*out));
    out->layout = b->layout;
    out->rasterizer = *d->rasterizer;
    memcpy(out->viewport, d->viewports, sizeof(out->viewport));
    out->position_output = position;
    out->color_output = out->fragment_input = UINT32_MAX;
    out->clip_xy = d->clip_xy;
    out->clip_z = d->clip_z;
    out->clip_user = d->clip_user;
    return true;
}
static bool linked_fields(const struct draw_context *d,
                          const struct poulsback_color0_source *captured_source,
                          const struct sgx535_fragment_dri_nir_ir *symbolic)
{
    const struct sgx535_fragment_varying *v = &symbolic->source_linkage.varying[0];
    const struct tgsi_shader_info *fs = &d->fs.fragment_shader->info;
    if (captured_source->color_output == UINT32_MAX ||
        captured_source->fragment_input == UINT32_MAX)
        return captured_source->color_output == UINT32_MAX &&
               captured_source->fragment_input == UINT32_MAX && !fs->num_inputs &&
               !symbolic->source_linkage.count;
    unsigned interpolation = fs->input_interpolate[captured_source->fragment_input];
    enum sgx535_fragment_interpolation resolved =
        captured_source->layout.slot[0].interpolation == POULSBACK_DRAW_FLAT
            ? SGX535_FRAGMENT_FLAT
            : SGX535_FRAGMENT_SMOOTH;
    if (symbolic->source_linkage.count != 1 || v->semantic != SGX535_FRAGMENT_COLOR || v->index ||
        v->components != 4 || symbolic->resolved_interpolation != resolved)
        return false;
    /* Source driver_base is retained independently. Semantic identity, not
   * that original NIR number, chooses the actual draw FS/VS locations. */
    return (v->interpolation == SGX535_FRAGMENT_COLOR_INTERPOLATION &&
            interpolation == TGSI_INTERPOLATE_COLOR) ||
           (v->interpolation == SGX535_FRAGMENT_FLAT &&
            interpolation == TGSI_INTERPOLATE_CONSTANT) ||
           (v->interpolation == SGX535_FRAGMENT_SMOOTH &&
            interpolation == TGSI_INTERPOLATE_PERSPECTIVE);
}

struct capture_normalized_work {
    struct sgx535_fragment_dri_nir_ir source;
    struct sgx535_fragment_normalized_candidate checked;
    bool live[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t remap[SGX535_NIR_MAX_INSTRUCTIONS];
};
static enum sgx535_nir_status
capture_normalized_ir(const struct nir_shader *nir, enum sgx535_fragment_uniform_layout layout,
                      enum sgx535_fragment_interpolation interpolation,
                      struct sgx535_fragment_dri_nir_ir *out)
{
    struct capture_normalized_work *w = calloc(1, sizeof(*w));
    struct sgx535_fragment_linkage canonical;
    enum sgx535_nir_status status;
    size_t count = 0;
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    /* Extract the original operations and linkage together. Raw extraction
   * preserves FNEG/FABS as explicit sign operations, so profile4 can refuse
   * them instead of selecting the older MAD/MAX word projections. */
    status = sgx535_nir_fragment_raw_sign_inf_ir(nir, layout, w->source.instruction,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                 &w->source.source_linkage);
    if (status != SGX535_NIR_OK)
        goto done;
    canonical = w->source.source_linkage;
    if (canonical.count != 1 ||
        (canonical.varying[0].interpolation != SGX535_FRAGMENT_COLOR_INTERPOLATION &&
         canonical.varying[0].interpolation != interpolation)) {
        status = SGX535_NIR_ERR_UNSUPPORTED;
        goto done;
    }
    canonical.varying[0].driver_base = 0;
    canonical.varying[0].interpolation = SGX535_FRAGMENT_SMOOTH;
    /* Validate the WHOLE source, including dead literals/sign operations,
   * under the normal profile before removing unreachable scalar nodes. */
    status = sgx535_fragment_compile_dri_color0_normalized_candidate(w->source.instruction, count,
                                                                     &canonical, &w->checked);
    if (status != SGX535_NIR_OK)
        goto done;
    status = sgx535_fragment_dri_live_ir(w->source.instruction, &count, w->live, w->remap, false);
    if (status != SGX535_NIR_OK)
        goto done;
    status = sgx535_fragment_compile_dri_color0_normalized_candidate(w->source.instruction, count,
                                                                     &canonical, &w->checked);
    if (status == SGX535_NIR_OK) {
        memset(w->source.instruction + count, 0,
               (SGX535_NIR_MAX_INSTRUCTIONS - count) * sizeof(*w->source.instruction));
        w->source.instruction_count = (uint32_t)count;
        w->source.resolved_interpolation = interpolation;
        *out = w->source;
    }
done:
    free(w);
    return status;
}
static enum sgx535_nir_status capture_solid_ir(const struct nir_shader *nir,
                                               enum sgx535_fragment_uniform_layout layout,
                                               struct sgx535_fragment_dri_nir_ir *out)
{
    struct poulsback_nir_solid_fragment *work = calloc(1, sizeof(*work));
    if (!work)
        return SGX535_NIR_ERR_NOMEM;
    enum sgx535_nir_status status = poulsback_nir_solid_fragment_source_ir(nir, layout, work);
    if (status == SGX535_NIR_OK) {
        memset(out, 0, sizeof(*out));
        out->instruction_count = work->instruction_count;
        memcpy(out->instruction, work->instruction,
               work->instruction_count * sizeof(*work->instruction));
    }
    free(work);
    return status;
}
/* A refused capture names its line on stderr (draw status numbers are in
 * poulsback_draw.h). */
static enum poulsback_draw_status capture_refused(enum poulsback_draw_status status,
                                                  const char *function, int line)
{
    fprintf(stderr, "poulsback: capture refused (status %d) in %s:%d\n", status, function, line);
    return status;
}
#define CAPTURE_REFUSED(s) capture_refused((s), __func__, __LINE__)

enum sgx535_nir_status poulsback_color0_program(const struct nir_shader *nir,
                                                enum sgx535_fragment_uniform_layout layout,
                                                uint32_t profile,
                                                enum sgx535_fragment_interpolation interpolation,
                                                bool solid, struct sgx535_fragment_dri_nir_ir *out)
{
    if (!nir || !out)
        return SGX535_NIR_ERR_ARGUMENT;
    if (solid)
        return profile == POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED
                   ? capture_solid_ir(nir, layout, out)
                   : SGX535_NIR_ERR_UNSUPPORTED;
    switch (profile) {
    case POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED:
        return capture_normalized_ir(nir, layout, interpolation, out);
    case POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1:
        return sgx535_nir_dri_color0_raw_mask_normalized_v1_ir(nir, layout, interpolation, out);
    case POULSBACK_RENDER_PROFILE_COLOR0_RAW_SIGN_NORMALIZED:
        return sgx535_nir_dri_color0_raw_sign_normalized_ir(nir, layout, interpolation, out);
    default:
        return SGX535_NIR_ERR_UNSUPPORTED;
    }
}
/* Exactly one of nir (compiled here) and program (compiled by the caller with
 * poulsback_color0_program) is given. linked_fields below checks a given
 * program against this draw's linkage and interpolation. */
/* What a capture checks of a draw besides its vertices: the producer state,
 * raster request, linkage and uniform words of SYMBOLIC. */
static enum poulsback_draw_status
draw_check(const struct draw_context *d, const struct poulsback_draw_batch *b,
           const struct sgx535_fragment_dri_nir_ir *symbolic, uint32_t program_profile,
           uint32_t width, uint32_t height, const struct poulsback_draw_window_raster *raster,
           bool solid, const void *uniforms, size_t uniform_bytes,
           struct poulsback_color0_draw *out)
{
    uint32_t snapshot[SGX535_FRAGMENT_UNIFORM_WORDS] = {0};
    if (uniform_bytes > sizeof(snapshot) || (uniform_bytes & 3u) ||
        (!!uniform_bytes != !!uniforms) || (uniform_bytes && !span(uniforms, uniform_bytes)) ||
        (!solid && uniform_bytes))
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_ARGUMENT);
    if (uniform_bytes)
        memcpy(snapshot, uniforms, uniform_bytes);
    if (program_profile != POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED &&
        program_profile != POULSBACK_RENDER_PROFILE_COLOR0_RAW_SIGN_NORMALIZED &&
        program_profile != POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1)
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
    if ((solid && program_profile != POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED) || !width ||
        !height || width > 4096 || height > 4096 || !capture_source(d, b, &out->source, solid) ||
        d->rasterizer->scissor)
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
    memset(&out->residual, 0, sizeof(out->residual));
    out->has_residual = poulsback_draw_batch_residual_raster(d, b, &out->residual);
    /* Only the live canonical twoside producer can certify that copied
   * COLOR already selects the original face's complete BCOLOR. */
    if (out->source.rasterizer.light_twoside && !out->has_residual)
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
    /* The clip-space path gives the draw module an identity viewport; the
     * request's viewport is the MTE's, checked by the caller. */
    if (raster->origin != POULSBACK_DRAW_WINDOW_ORIGIN_UPPER_LEFT ||
        raster->half_pixel_center != out->source.rasterizer.half_pixel_center ||
        raster->bottom_edge_rule != out->source.rasterizer.bottom_edge_rule ||
        (!clip_space(d) && (memcmp(raster->viewport_scale, out->source.viewport[0].scale,
                                   sizeof(raster->viewport_scale)) ||
                            memcmp(raster->viewport_translate, out->source.viewport[0].translate,
                                   sizeof(raster->viewport_translate)))))
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
    out->uniform_word_count = 0;
    memset(out->uniform_words_used, 0, sizeof(out->uniform_words_used));
    for (uint32_t i = 0; i < symbolic->instruction_count; i++) {
        const struct sgx535_fragment_instruction *in = &symbolic->instruction[i];
        if (in->opcode != SGX535_FRAGMENT_UNIFORM)
            continue;
        if (!solid || in->index >= SGX535_FRAGMENT_UNIFORM_WORDS - 1u || in->component)
            return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
        out->uniform_words_used[in->index / 32u] |= 1u << (in->index % 32u);
        if (out->uniform_word_count <= in->index)
            out->uniform_word_count = in->index + 1u;
    }
    if (out->uniform_word_count > uniform_bytes / 4u)
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
    for (uint32_t i = 0; i < out->uniform_word_count; i++) {
        if ((snapshot[i] & 0x7f800000u) == 0x7f800000u)
            return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
        out->uniforms[i] = snapshot[i];
    }
    return linked_fields(d, &out->source, symbolic) ? POULSBACK_DRAW_OK
                                                    : CAPTURE_REFUSED(POULSBACK_DRAW_ERR_STATE);
}
enum poulsback_draw_status
poulsback_color0_draw_check(const struct draw_context *d, const struct poulsback_draw_batch *b,
                            const struct sgx535_fragment_dri_nir_ir *program, uint32_t profile,
                            bool solid, const void *uniforms, size_t uniform_bytes, uint32_t width,
                            uint32_t height, const struct poulsback_draw_window_raster *raster,
                            struct poulsback_color0_draw *out)
{
    if (!d || !b || !program || !raster || !out)
        return CAPTURE_REFUSED(POULSBACK_DRAW_ERR_ARGUMENT);
    return draw_check(d, b, program, profile, width, height, raster, solid, uniforms, uniform_bytes,
                      out);
}
