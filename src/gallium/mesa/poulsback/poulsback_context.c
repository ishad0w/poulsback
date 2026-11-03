// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The Gallium context: draws into the frame writer's open frame
 * (poulsback_frame.h) and flushes as SUBMIT jobs (poulsback_engine.h); its
 * bound state is poulsback_context_state.c's. */
#include "poulsback_context_private.h"

/* The first error stops the context; the message names where it was raised.
 * After the GPU's loss a robust context's reset callback fires once. */
void poulsback_context_fail_at(struct poulsback_context *c, int e, const char *function, int line)
{
    if (!c->error) {
        c->error = e;
        fprintf(stderr, "poulsback: context stopped (%d) in %s:%d%s%s; no software fallback\n", e,
                function, line, c->refusal ? ", refused: " : "", c->refusal ? c->refusal : "");
    }
    poulsback_lost_fire(&c->lost);
}
static void free_context(struct poulsback_context *c)
{
    c->base.screen->fence_reference(c->base.screen, &c->failed_fence, NULL);
    free(c->scene_program);
    free(c);
}
/* The attachment has a stencil plane (Z24S8; the hardware keeps one either way). */
static bool stencil_target(const struct poulsback_context *c)
{
    return c->fb.zsbuf.texture && c->fb.zsbuf.format == PIPE_FORMAT_Z24_UNORM_S8_UINT;
}
static bool scene_target_supported(const struct poulsback_context *c)
{
    const struct pipe_surface *s = &c->fb.cbufs[0];
    struct poulsback_depth_capture_description depth;
    const struct pipe_surface *zs = &c->fb.zsbuf;
    if (zs->texture &&
        (poulsback_resource_depth_capture_description(zs->texture, &depth) ||
         depth.width != c->fb.width || depth.height != c->fb.height ||
         zs->format != zs->texture->format || zs->level || zs->first_layer || zs->last_layer ||
         zs->nr_samples > 1 || (zs->nr_samples && zs->nr_samples != depth.samples) ||
         zs->texture == s->texture))
        return false;
    return c->fb.width && c->fb.height && c->fb.width <= 4096 && c->fb.height <= 4096 &&
           c->fb.nr_cbufs == 1 && c->fb.samples <= 1 && !c->fb.viewmask && !c->fb.pls_enabled &&
           !c->fb.resolve && s->texture && s->format == s->texture->format && !s->level &&
           !s->first_layer && !s->last_layer && s->nr_samples <= 1 &&
           c->fb.width == s->texture->width0 && c->fb.height == s->texture->height0;
}
/* The frame: one target and depth until it is submitted (a framebuffer
 * change submits it, so an open frame's target is still supported). */
static int ensure_frame(struct poulsback_context *c)
{
    if (c->error)
        return c->error;
    if (c->frame)
        return c->scene_target == c->fb.cbufs[0].texture &&
                       c->scene_depth_target == c->fb.zsbuf.texture
                   ? 0
                   : -EINVAL;
    if (!scene_target_supported(c))
        return -EOPNOTSUPP;
    c->frame = true;
    c->frame_cleared = false;
    c->stencil_ready = c->mask_cut = false;
    pipe_resource_reference(&c->scene_target, c->fb.cbufs[0].texture);
    pipe_resource_reference(&c->scene_depth_target, c->fb.zsbuf.texture);
    return 0;
}
/* The open frame for a draw or partial clear: one no full clear began starts
 * with the target's contents (poulsback_context_load). Nothing without a
 * window target. */
static int frame_start(struct poulsback_context *c)
{
    int e;
    /* an open frame began (a framebuffer change submits it first) */
    if (c->frame && c->frame_cleared)
        return 0;
    e = scene_target_supported(c) ? ensure_frame(c) : 0;
    return e ? e : c->frame ? poulsback_context_load(c) : 0;
}
/* A refusal names what has no form, in the skipped line or the
 * context-stopped message. */
static bool refused(struct poulsback_context *c, const char *what)
{
    c->refusal = what;
    return false;
}
/* A draw or clear no path takes: skipped, one named line per fragment CSO
 * and reason; the context stops for device errors alone. */
static void skip(struct poulsback_context *c, const char *what)
{
    struct state *fs = c->bound[FS];
    const char *why = c->refusal ? c->refusal : "refused";
    if (fs && fs->u.fs.skipped == why)
        return;
    if (fs)
        fs->u.fs.skipped = why;
    fprintf(stderr, "poulsback: %s skipped: %s\n", what, why);
}
/* A full colour (and depth) clear starts the frame over: components within
 * [0, 1], depth 1.0 into a D24X8 or D24S8 attachment of the frame's size, as
 * the ordered frames had them. STENCIL is the stencil clear's value, or -1
 * without one: then no draw may have written the stencil since its last
 * clear, whose value the frame's stencil object writes (stencil_object). */
static int frame_clear(struct poulsback_context *c, const union pipe_color_union *value,
                       double depth, int stencil)
{
    static const unsigned shifts[4] = {16, 8, 0, 24};
    uint32_t packed = 0;
    int e = ensure_frame(c);
    if (e)
        return e;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t m = value->ui[i] & 0x7fffffffu;
        if (m > 0x3f800000u || ((value->ui[i] >> 31) && m))
            return -EOPNOTSUPP;
        packed |= (uint32_t)float_to_ubyte(value->f[i]) << shifts[i];
    }
    if (c->fb.zsbuf.texture) {
        struct poulsback_depth_capture_description d;
        if (depth != 1.0 || poulsback_resource_depth_capture_description(c->fb.zsbuf.texture, &d) ||
            (d.format != POULSBACK_DEPTH_CAPTURE_D24X8 &&
             d.format != POULSBACK_DEPTH_CAPTURE_D24S8) ||
            d.samples || d.storage_samples || d.width != c->fb.width || d.height != c->fb.height ||
            (stencil < 0 && c->stencil_dirty))
            return -EOPNOTSUPP;
        if (stencil >= 0)
            c->stencil_value = (uint8_t)stencil, c->stencil_dirty = false;
        c->stencil_ready = false;
        c->scene_depth_ready = true;
    }
    poulsback_ws_render_discard(c->render);
    c->mask_cut = false; /* a render's start: the whole mask */
    c->frame_clear = packed;
    c->frame_cleared = true;
    return 0;
}
static void clear(struct pipe_context *p, unsigned buffers, uint32_t colors,
                  uint8_t stencil_mask, const struct pipe_scissor_state *s,
                  const union pipe_color_union *value, double depth, unsigned stencil)
{
    struct poulsback_context *c = ctx(p);
    int e;
    if (c->error || !c->fb.width || !c->fb.height) /* 0x0: nothing to clear */
        return;
    if (!buffers)
        return;
    /* Mesa26 color_clear_mask has4 component bits per target. A complete
   * target0 color clear, with the depth, and the whole stencil of a Z24S8
   * attachment or not, starts the frame over (frame_clear); any other is a
   * quad in the frame. Inert unused target masks and
   * unselected depth values have no effect. */
    if ((c->fb.zsbuf.texture ? (buffers & ~(stencil_target(c) ? PIPE_CLEAR_STENCIL : 0u)) !=
                                   (PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH)
                             : buffers != PIPE_CLEAR_COLOR0) ||
        (colors & PIPE_MASK_RGBA) != PIPE_MASK_RGBA || !value ||
        ((buffers & PIPE_CLEAR_STENCIL) && stencil_mask != 0xff) ||
        (s && (s->minx || s->miny || s->maxx != c->fb.width || s->maxy != c->fb.height))) {
        e = scene_target_supported(c) ? frame_start(c) : -EOPNOTSUPP;
        if (!e)
            e = poulsback_context_clear_object(c, buffers, colors, stencil_mask, s, value, depth,
                                               stencil);
        if (e == -EOPNOTSUPP)
            skip(c, "clear");
        else if (e)
            fail(c, e);
        return;
    }
    /* a depth other than 1.0, which the frame state cannot hold: the frame
     * starts over at 1.0, then a quad writes the depth */
    e = frame_clear(c, value, depth != 1.0 ? 1.0 : depth,
                    buffers & PIPE_CLEAR_STENCIL ? (int)(stencil & 0xffu) : -1);
    if (!e && depth != 1.0 && c->fb.zsbuf.texture)
        e = poulsback_context_clear_object(c, PIPE_CLEAR_DEPTH, 0, 0, NULL, value, depth, 0);
    /* anything else it cannot hold: a quad over the loaded target */
    if (e == -EOPNOTSUPP && scene_target_supported(c) && !(e = frame_start(c)))
        e = poulsback_context_clear_object(c, buffers, colors, stencil_mask, s, value, depth,
                                           stencil);
    if (e == -EOPNOTSUPP)
        skip(c, "clear");
    else if (e)
        fail(c, e);
}
/* The bound state a draw may have: BACKEND, a GPU draw of a backend program,
 * whose ISP words are the draw's, may test depth with any
 * function and write it or not, test and write a Z24S8 attachment's stencil
 * and offset its depth, blend, mask and logic-op its colour and test its
 * alpha. */
static bool state_supported(struct poulsback_context *c, bool backend)
{
    const struct pipe_rasterizer_state *r;
    const struct pipe_depth_stencil_alpha_state *z;
    const struct pipe_blend_state *b;
    const struct pipe_surface *s = &c->fb.cbufs[0];
    float hw = (float)c->fb.width * .5f, hh = (float)c->fb.height * .5f;
    /* Only sample zero exists in the admitted storage profile. Higher mask
   * bits do not affect it; suppressed coverage and sample shading above
   * one invocation have no native implementation and must not disappear. */
    if (!(c->sample_mask & 1u) || c->min_samples > 1)
        return refused(c, "sample mask");
    /* a backend draw samples its program's units alone */
    if (!backend && poulsback_sampler_draw_admit(c->samplers))
        return refused(c, "samplers");
    if (!c->bound[RASTER] || !c->bound[BLEND] || !c->bound[DEPTH] || !c->bound[ELEMENTS] ||
        !c->bound[VS] || !c->bound[FS] || !c->have_viewport)
        return refused(c, "bound state");
    if (c->bound[FS]->u.fs.refused)
        return refused(c, c->bound[FS]->u.fs.refusal);
    if (!c->fb.width || !c->fb.height || c->fb.width > 4096 || c->fb.height > 4096 ||
        c->fb.nr_cbufs != 1 || c->fb.samples > 1 || c->fb.viewmask || c->fb.pls_enabled ||
        c->fb.resolve) {
        fprintf(stderr,
                "poulsback: framebuffer %ux%u cbufs %u samples %u viewmask %u pls %d "
                "resolve %d\n",
                c->fb.width, c->fb.height, c->fb.nr_cbufs, c->fb.samples, c->fb.viewmask,
                (int)c->fb.pls_enabled, c->fb.resolve != NULL);
        return refused(c, "framebuffer");
    }
    if (!s->texture || s->format != s->texture->format || s->level || s->first_layer ||
        s->last_layer || s->nr_samples > 1 || c->fb.width > s->texture->width0 ||
        c->fb.height > s->texture->height0) {
        fprintf(stderr, "poulsback: colour surface %s on %s %ux%u, fb %ux%u cbufs %u\n",
                util_format_name(s->format),
                s->texture ? util_format_name(s->texture->format) : "(none)",
                s->texture ? s->texture->width0 : 0, s->texture ? s->texture->height0 : 0,
                c->fb.width, c->fb.height, c->fb.nr_cbufs);
        return refused(c, "colour surface");
    }
    r = &c->bound[RASTER]->u.raster;
    z = &c->bound[DEPTH]->u.depth;
    b = &c->bound[BLEND]->u.blend;
    if (c->fb.zsbuf.texture) {
        if (!c->scene_depth_ready || (!c->frame && !scene_target_supported(c)) ||
            (!backend &&
             (!z->depth_enabled || !z->depth_writemask || z->depth_func != PIPE_FUNC_LESS)))
            return refused(c, "depth");
    } else if (z->depth_enabled || z->depth_writemask)
        return refused(c, "depth without a depth buffer");
    if ((!backend || !stencil_target(c)) && (z->stencil[0].enabled || z->stencil[1].enabled))
        return refused(c, "stencil");
    if ((!backend && z->alpha_enabled) || z->depth_bounds_test || b->independent_blend_enable ||
        b->alpha_to_coverage || b->alpha_to_coverage_dither || b->alpha_to_one ||
        b->advanced_blend_func ||
        (!backend && (b->logicop_enable || b->rt[0].blend_enable ||
                      b->rt[0].colormask != PIPE_MASK_RGBA)) ||
        r->clip_plane_enable)
        return refused(c, "blend or stencil");
    /* The context uses draw's ordinary primitive pipeline (no vbuf renderer).
   * Its cull stage filters window-space faces before this driver's final
   * collector. Keep native raster culling disabled to avoid a second cull. */
    /* Point and line state is not checked: draw() admits only triangle
     * primitives and FILL polygons, so it never applies (GLES2 always enables
     * point sprites). */
    /* Only filled single-sample triangles reach the native profile. These
   * raster effects are not encoded by v2; never silently use ordinary fill.
   * flatshade/first are handled by real draw's linked interpolation.
   * noLLVM draw also clamps COLOR/BCOLOR vertex outputs when requested;
   * fragment-color clamping remains a separate unsupported operation. */
    if (!poulsback_fragment_offset_supported(r, backend))
        return refused(c, "polygon offset");
    if (r->fill_front != PIPE_POLYGON_MODE_FILL || r->fill_back != PIPE_POLYGON_MODE_FILL ||
        r->offset_line || r->offset_point || r->offset_units_unscaled ||
        r->clamp_fragment_color || r->rasterizer_discard || r->poly_smooth ||
        r->poly_stipple_enable || r->multisample || r->no_ms_sample_mask_out ||
        r->force_persample_interp || r->depth_clamp || r->clip_halfz || !r->depth_clip_near ||
        !r->depth_clip_far || r->rasterization_stream ||
        r->conservative_raster_mode != PIPE_CONSERVATIVE_RASTER_OFF || r->subpixel_precision_x ||
        r->subpixel_precision_y || r->tile_raster_order_fixed ||
        r->tile_raster_order_increasing_x || r->tile_raster_order_increasing_y ||
        r->unclamped_fragment_depth_values || r->representative_fragment_test ||
        r->conservative_raster_dilate != 0)
        return refused(c, "rasterizer");
    /* Exact v2 W1 candidate viewport only; it has no general viewport field.
   * Even this mathematical origin still needs physical provider qualification.
   */
    /* The window's orientation (scale -h/2): the frame's groups and objects
     * take clip and window positions under that transform. A backend draw
     * sends its own viewport (the MTE's words): any depth range, and an FBO's
     * (+h/2; st's front_ccw and its gl_FragCoord transform follow Gallium's
     * top-down surfaces, so the faces and the fragment's y need nothing
     * more). Dither is ignored above: it has no effect on the
     * 8-bit-per-channel targets this context renders. */
    return (c->viewport.scale[0] == hw && c->viewport.translate[0] == hw &&
            c->viewport.translate[1] == hh &&
            (c->viewport.scale[1] == -hh || (backend && c->viewport.scale[1] == hh)) &&
            (backend || (c->viewport.scale[2] == .5f && c->viewport.translate[2] == .5f))) ||
           refused(c, "viewport");
}
static int scene_sample_snapshot(const struct poulsback_context *c,
                                 struct poulsback_scene_sample_input *out)
{
    const struct pipe_surface *color = &c->fb.cbufs[0], *depth = &c->fb.zsbuf;
    const struct pipe_rasterizer_state *raster;
    struct poulsback_scene_sample_input r;
    static const struct pipe_rasterizer_state unbound; /* before a draw: no multisampling */
    if (c->fb.nr_cbufs != 1 || !color->texture)
        return -EINVAL;
    raster = c->bound[RASTER] ? &c->bound[RASTER]->u.raster : &unbound;
    r = (struct poulsback_scene_sample_input){
        .version = POULSBACK_SCENE_SAMPLE_INPUT_VERSION,
        .framebuffer_samples = c->fb.samples,
        .effective_framebuffer_samples = util_framebuffer_get_num_samples(&c->fb),
        .min_samples = c->min_samples,
        .sample_mask = c->sample_mask,
        .color_resource_samples = color->texture->nr_samples,
        .color_storage_samples = color->texture->nr_storage_samples,
        .color_surface_samples = color->nr_samples,
        /* Without a depth attachment its sample counts are zero. */
        .depth_resource_samples = depth->texture ? depth->texture->nr_samples : 0,
        .depth_storage_samples = depth->texture ? depth->texture->nr_storage_samples : 0,
        .depth_surface_samples = depth->texture ? depth->nr_samples : 0,
        .multisample = raster->multisample,
        .force_persample_interp = raster->force_persample_interp,
        .no_ms_sample_mask_out = raster->no_ms_sample_mask_out};
    memcpy(out, &r, sizeof(r));
    return 0;
}
static int scene_color_snapshot(const struct poulsback_context *c,
                                struct poulsback_scene_color_state *out)
{
    if (!c->bound[BLEND] || !c->fb.cbufs[0].texture ||
        c->fb.cbufs[0].format != c->fb.cbufs[0].texture->format)
        return -EOPNOTSUPP;
    *out = (struct poulsback_scene_color_state){POULSBACK_SCENE_COLOR_VERSION,
                                                poulsback_scene_color_format(c->fb.cbufs[0].format),
                                                c->bound[BLEND]->u.blend.dither, 0};
    return 0;
}
/* The scene fragment compiled for this batch's interpolation. A CSO's own NIR
 * compiles once per interpolation and stays with the CSO; NIR specialized for
 * this draw's constants compiles into the context's scratch every draw. */
static int scene_program(struct poulsback_context *c, bool flat,
                         enum sgx535_fragment_uniform_layout layout,
                         const struct sgx535_fragment_dri_nir_ir **out)
{
    struct state *fs = c->bound[FS];
    struct sgx535_fragment_dri_nir_ir **slot =
        c->scene_fragment_owned ? &c->scene_program : &fs->u.fs.scene_program[flat];
    enum sgx535_nir_status status;
    if (*slot && !c->scene_fragment_owned) {
        *out = *slot;
        return 0;
    }
    if (!*slot && !(*slot = malloc(sizeof(**slot))))
        return -ENOMEM;
    memset(*slot, 0, sizeof(**slot));
    status = poulsback_color0_program(c->scene_fragment, layout, c->scene_fragment_profile,
                                      flat ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_SMOOTH,
                                      fs->u.fs.solid, *slot);
    if (status != SGX535_NIR_OK) {
        if (!c->scene_fragment_owned) {
            free(*slot);
            *slot = NULL;
        }
        return status == SGX535_NIR_ERR_NOMEM ? -ENOMEM : -EOPNOTSUPP;
    }
    *out = *slot;
    return 0;
}
/* The MTE's cull mode for the bound rasterizer, in the window's orientation
 * (viewport y scale negative: a CCW front face is CW there). */
static uint32_t mte_cull(const struct pipe_rasterizer_state *r)
{
    if (r->cull_face != PIPE_FACE_BACK && r->cull_face != PIPE_FACE_FRONT)
        return SGX535_WINDOW_MESH_CULL_NONE;
    return (r->cull_face == PIPE_FACE_BACK) == !!r->front_ccw ? SGX535_WINDOW_MESH_CULL_CW
                                                              : SGX535_WINDOW_MESH_CULL_CCW;
}
/* Engine mode: a draw's state as the frame writer's shape (the fragment
 * PROGRAM, its uniform words and the colour's interpolation), for the frame
 * the depth groups made of it: COLOR0 programs only, the exact GL window of the
 * target, LESS with writes into a depth attachment or no depth at all. Clip
 * space: the MTE transforms, clips and culls. */
int poulsback_context_frame_shape(struct poulsback_context *c,
                                  const struct sgx535_fragment_dri_nir_ir *program, bool solid,
                                  bool flat, const uint32_t *uniforms, uint32_t uniform_count,
                                  uint32_t profile, bool backend, bool object,
                                  poulsback_frame_shape *shape)
{
    /* OBJECT: a frame object's, drawn outside a draw too, so
     * none of the bound state: the window's rasterizer rules, viewport and
     * sample state, the target's colour undithered, its own ISP words */
    static const struct pipe_rasterizer_state window = {.half_pixel_center = 1,
                                                        .bottom_edge_rule = 1};
    const struct pipe_rasterizer_state *rs = object ? &window : &c->bound[RASTER]->u.raster;
    const struct pipe_depth_stencil_alpha_state *z = object ? NULL : &c->bound[DEPTH]->u.depth;
    const float scale[3] = {(float)c->fb.width * .5f, (float)c->fb.height * -.5f, .5f};
    const float translate[3] = {(float)c->fb.width * .5f, (float)c->fb.height * .5f, .5f};
    struct sgx535_window_mesh_request *m = &shape->mesh.scene.foreground;
    struct poulsback_scene_sample_input samples = c->scene_samples;
    struct poulsback_scene_color_state colour = c->scene_color;
    memset(shape, 0, sizeof(*shape));
    if (object) {
        if (scene_sample_snapshot(c, &samples))
            return -EOPNOTSUPP;
        colour = (struct poulsback_scene_color_state){
            POULSBACK_SCENE_COLOR_VERSION, poulsback_scene_color_format(c->fb.cbufs[0].format),
            0, 0};
    }
    if (poulsback_scene_sample_source(&samples, object ? NULL : c->scene_fragment,
                                      program->instruction_count, &shape->samples))
        return -EINVAL;
    if ((profile != POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED &&
         profile != POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1) ||
        colour.version != POULSBACK_SCENE_COLOR_VERSION ||
        colour.format != PIPE_FORMAT_B8G8R8A8_UNORM || colour.dither > 1 || colour.reserved ||
        (!object &&
         (c->scene_depth_target ? !backend && (!z->depth_enabled || !z->depth_writemask ||
                                               z->depth_func != PIPE_FUNC_LESS)
                                : z->depth_enabled || z->depth_writemask)) ||
        solid != !program->source_linkage.count || rs->half_pixel_center != 1 ||
        rs->bottom_edge_rule != (object || c->viewport.scale[1] < 0) || rs->light_twoside ||
        (!object &&
         (backend ? c->viewport.scale[0] != scale[0] || fabsf(c->viewport.scale[1]) != -scale[1] ||
                        memcmp(c->viewport.translate, translate, 8)
                  : memcmp(c->viewport.scale, scale, sizeof(scale)) ||
                        memcmp(c->viewport.translate, translate, sizeof(translate)))))
        return -EOPNOTSUPP;
    /* As the ordered constructor's request (test_frame.c's oracle). */
    shape->mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
    shape->mesh.format = SGX535_DEPTH_D24S8;
    shape->mesh.no_depth = !c->scene_depth_target;
    shape->mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
    shape->mesh.scene.packed_background_argb = c->frame_clear;
    m->version = SGX535_WINDOW_MESH_CLIP_VERSION;
    m->cull = mte_cull(rs);
    m->profile = profile == POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1
                     ? SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1
                     : SGX535_WINDOW_MESH_PROFILE;
    m->core_id = SGX535_ENGINE121_CORE_ID;
    m->revision = SGX535_ENGINE121_REVISION;
    m->width = c->fb.width;
    m->height = c->fb.height;
    m->order = SGX535_WINDOW_MESH_DEINDEXED;
    m->interpolation = solid  ? SGX535_WINDOW_MESH_NO_INPUT
                       : flat ? SGX535_VARYING_FLAT_REPLICATED
                              : SGX535_VARYING_SMOOTH;
    m->window =
        (struct sgx535_window_mesh_window){.origin = POULSBACK_DRAW_WINDOW_ORIGIN_UPPER_LEFT,
                                           .half_pixel_center = rs->half_pixel_center,
                                           /* the hardware's own rule: st asks the
                                            * bottom edge's of a window, the top
                                            * edge's of an FBO (GL takes either
                                            * consistent rule) */
                                           .bottom_edge_rule = 1,
                                           .sample_mask = c->sample_mask,
                                           .min_samples = c->min_samples};
    /* the window's: a backend draw's depth range is its own */
    memcpy(m->window.viewport_scale_f32, scale, 12);
    memcpy(m->window.viewport_translate_f32, translate, 12);
    m->fragment = program->instruction;
    m->fragment_count = program->instruction_count;
    m->uniforms = uniform_count ? uniforms : NULL;
    m->uniform_word_count = uniform_count;
    return 0;
}
/* Engine mode, CPU vertex shading: a draw's shape after the checks the
 * scene's capture made of it (poulsback_color0_draw_check), and its clip-space
 * vertices written once into the open frame. */
static int frame_sink(struct poulsback_context *c, const struct poulsback_draw_batch *batch)
{
    const struct pipe_rasterizer_state *rs = &c->bound[RASTER]->u.raster;
    const struct poulsback_color0_draw *d = &c->frame_draw;
    const struct sgx535_fragment_dri_nir_ir *program;
    struct poulsback_draw_window_raster raster = {.origin = POULSBACK_DRAW_WINDOW_ORIGIN_UPPER_LEFT,
                                                  .half_pixel_center = rs->half_pixel_center,
                                                  .bottom_edge_rule = rs->bottom_edge_rule,
                                                  .sample_mask = c->sample_mask,
                                                  .min_samples = c->min_samples};
    enum sgx535_fragment_uniform_layout layout = c->base.screen->caps.packed_uniforms
                                                     ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                     : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS;
    bool solid = c->bound[FS]->u.fs.solid;
    const void *uniforms = solid && c->bound[FS]->u.fs.has_uniform ? c->constant[1].data : NULL;
    size_t uniform_bytes = uniforms ? c->constant[1].bytes : 0;
    poulsback_frame_shape shape;
    struct poulsback_frame_vertices in;
    int e = c->scene_fragment ? ensure_frame(c) : -EINVAL;
    if (!e && (!c->frame_cleared || !c->scene_samples_ready))
        e = -EOPNOTSUPP;
    if (e)
        return e;
    /* Both faces culled: nothing of the draw reaches the target. */
    if (rs->cull_face == PIPE_FACE_FRONT_AND_BACK)
        return 0;
    memcpy(raster.viewport_scale, c->viewport.scale, sizeof(raster.viewport_scale));
    memcpy(raster.viewport_translate, c->viewport.translate, sizeof(raster.viewport_translate));
    e = scene_program(c, batch->layout.slot[0].interpolation == POULSBACK_DRAW_FLAT, layout,
                      &program);
    if (e)
        return e;
    if (poulsback_color0_draw_check(c->draw, batch, program, c->scene_fragment_profile, solid,
                                    uniforms, uniform_bytes, c->fb.width, c->fb.height, &raster,
                                    &c->frame_draw) != POULSBACK_DRAW_OK)
        return -EOPNOTSUPP;
    e = poulsback_context_frame_shape(c, program, solid,
                                      program->resolved_interpolation == SGX535_FRAGMENT_FLAT,
                                      d->uniforms, d->uniform_word_count,
                                      c->scene_fragment_profile, false, false, &shape);
    if (e)
        return e;
    /* CPU culling already applied (the collector's residual). */
    if (batch->stride_words != (solid ? 4u : 8u))
        return -EOPNOTSUPP;
    in = (struct poulsback_frame_vertices){
        batch->triangle_count * 3u, batch->words,
        batch->words + 3,           solid ? NULL : batch->words + 4,
        batch->stride_words,        batch->stride_words,
        batch->stride_words};
    e = poulsback_context_draw_mask(c, rs->scissor);
    if (e)
        return e;
    c->t_vertices += in.count;
    return poulsback_ws_render_draw(c->render, &shape, &in);
}

/* ---- GPU vertex shading (docs/graphics.md#gpu-vertex-shading) ---- */

/* POULSBACK_VS=cpu keeps every draw on the CPU path (A/B runs). */
static bool cpu_vs_forced(void)
{
    static int forced = -1;
    if (forced < 0) {
        const char *v = getenv("POULSBACK_VS");
        forced = v && !strcmp(v, "cpu");
    }
    return forced;
}
/* Why the last draw took the CPU path, traced when it changes. */
static int cpu_path(struct poulsback_context *c, const char *why)
{
    static const char *last;
    if (why != last)
        POULSBACK_TRACE("vs: draw on the CPU: %s\n", why);
    last = why;
    /* a program only the backend takes has no CPU-path form */
    return c->bound[FS] && c->bound[FS]->u.fs.backend_only ? -EOPNOTSUPP : 0;
}
/* A UNORM8 RGBA or BGRA element: its layout flags (one dword, unpacked by the
 * program), or 0. */
static uint8_t packed(unsigned format)
{
    return format == PIPE_FORMAT_R8G8B8A8_UNORM   ? 4u | SGX535_VERTEX_PACKED
           : format == PIPE_FORMAT_B8G8R8A8_UNORM ? 4u | SGX535_VERTEX_PACKED | SGX535_VERTEX_BGRA
                                                  : 0;
}
static unsigned f32_dwords(unsigned format)
{
    switch (format) {
    case PIPE_FORMAT_R32_FLOAT:
        return 1;
    case PIPE_FORMAT_R32G32_FLOAT:
        return 2;
    case PIPE_FORMAT_R32G32B32_FLOAT:
        return 3;
    case PIPE_FORMAT_R32G32B32A32_FLOAT:
        return 4;
    default:
        return 0;
    }
}
/* The lowest and highest of COUNT indices of SIZE bytes at INDICES. */
static void index_bounds(const void *indices, unsigned size, unsigned count, uint32_t *low,
                         uint32_t *high)
{
    uint32_t lo = UINT32_MAX, hi = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t v = size == 1   ? ((const uint8_t *)indices)[i]
                     : size == 2 ? ((const uint16_t *)indices)[i]
                                 : ((const uint32_t *)indices)[i];
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
    }
    *low = lo;
    *high = hi;
}

/* One draw's indices as a triangle list the GPU reads: the u16 sequence for a
 * plain glDrawArrays, an index buffer in place, else converted (u_indices)
 * into the frame's stream. LOW/HIGH bound the vertex indices. 1: nothing to
 * draw. */
static int gpu_indices(struct poulsback_context *c, const struct pipe_draw_info *info,
                       const struct pipe_draw_start_count_bias *d, uint64_t sequence,
                       struct poulsback_frame_gpu_draw *g, uint32_t *low, uint32_t *high)
{
    const unsigned hw = 1u << MESA_PRIM_TRIANGLES;
    enum mesa_prim prim;
    unsigned size, out_count;
    void *out;
    int e;
    if (!info->index_size) {
        u_generate_func generate;
        if (u_index_generator(hw, info->mode, d->start, d->count, PV_LAST, PV_LAST, &prim, &size,
                              &out_count, &generate) == U_TRANSLATE_ERROR ||
            prim != MESA_PRIM_TRIANGLES)
            return -EOPNOTSUPP;
        *low = d->start;
        *high = d->start + d->count - 1u;
        if (out_count < 3)
            return 1;
        if (info->mode == MESA_PRIM_TRIANGLES && size == 2 &&
            d->start + d->count <= POULSBACK_FRAME_SEQUENCE) {
            poulsback_ws_render_sequence(c->render, &g->index_address, &g->index_handle);
            g->index_address += 2u * d->start;
        } else {
            e = poulsback_ws_render_stream(c->render, out_count * size, 4, &out, &g->index_address,
                                           &g->index_handle);
            if (e)
                return e;
            generate(d->start, out_count, out);
        }
    } else {
        struct poulsback_resource_gpu buffer = {0};
        const uint8_t *indices;
        size_t bytes;
        u_translate_func translate;
        enum indices_mode mode;
        if (info->has_user_indices) {
            indices = info->index.user;
            bytes = ((size_t)d->start + d->count) * info->index_size;
        } else {
            e = poulsback_resource_buffer_gpu(info->index.resource, sequence, &buffer);
            if (e)
                return e;
            indices = buffer.cpu;
            bytes = buffer.bytes;
        }
        if (!indices || ((uint64_t)d->start + d->count) * info->index_size > bytes)
            return -EINVAL;
        /* Every index is read here: a bound the GPU's reads stay inside. */
        index_bounds(indices + (size_t)d->start * info->index_size, info->index_size, d->count, low,
                     high);
        mode = u_index_translator(hw, info->mode, info->index_size, d->count, PV_LAST, PV_LAST, 0,
                                  &prim, &size, &out_count, &translate);
        if (mode == U_TRANSLATE_ERROR || prim != MESA_PRIM_TRIANGLES)
            return -EOPNOTSUPP;
        if (out_count < 3)
            return 1;
        if (mode == U_TRANSLATE_MEMCPY && !info->has_user_indices && info->index_size != 1) {
            g->index_handle = buffer.handle;
            g->index_address = buffer.gpu_va + (uint64_t)d->start * info->index_size;
        } else {
            e = poulsback_ws_render_stream(c->render, out_count * size, 4, &out, &g->index_address,
                                           &g->index_handle);
            if (e)
                return e;
            translate(indices, d->start, d->count, out_count, info->restart_index, out);
        }
    }
    g->count = out_count - out_count % 3u;
    g->index32 = size == 4;
    return g->count ? 0 : 1;
}

/* One draw's vertex elements: buffers in place (marked as read by the open
 * frame's job), client arrays copied into the frame's stream. Every vertex the
 * indices LOW..HIGH name lies inside its buffer. */
static int gpu_elements(struct poulsback_context *c, int64_t bias, uint32_t low, uint32_t high,
                        uint64_t sequence, const uint8_t *components,
                        struct poulsback_frame_gpu_draw *g)
{
    const struct state *ve = c->bound[ELEMENTS];
    struct {
        uint64_t address;
        uint32_t handle;
        int64_t low;
        bool done;
    } copy[PB_VERTEX_BUFFERS] = {{0}};
    int64_t lo[POULSBACK_FRAME_GPU_ELEMENTS], hi[POULSBACK_FRAME_GPU_ELEMENTS];
    g->element_count = ve->u.ve.count;
    for (uint32_t i = 0; i < ve->u.ve.count; i++) {
        const struct pipe_vertex_element *a = &ve->u.ve.element[i];
        int64_t rel = (int64_t)c->vb[a->vertex_buffer_index].buffer_offset + a->src_offset +
                      bias * a->src_stride;
        unsigned dwords = components[i] & SGX535_VERTEX_PACKED ? 1u : components[i];
        lo[i] = rel + (int64_t)(a->src_stride ? low : 0) * a->src_stride;
        hi[i] = rel + (int64_t)(a->src_stride ? high : 0) * a->src_stride + 4 * dwords;
        if (lo[i] < 0)
            return -EINVAL;
        g->element[i].dwords = dwords;
        g->element[i].stride = a->src_stride;
    }
    for (uint32_t i = 0; i < ve->u.ve.count; i++) {
        const struct pipe_vertex_element *a = &ve->u.ve.element[i];
        const struct pipe_vertex_buffer *vb = &c->vb[a->vertex_buffer_index];
        int64_t rel = (int64_t)vb->buffer_offset + a->src_offset + bias * a->src_stride;
        if (vb->is_user_buffer) {
            /* The buffer's span every element of it reads, copied once. */
            unsigned b = a->vertex_buffer_index;
            if (!copy[b].done) {
                int64_t from = INT64_MAX, to = 0;
                void *cpu;
                int e;
                for (uint32_t k = 0; k < ve->u.ve.count; k++)
                    if (ve->u.ve.element[k].vertex_buffer_index == b) {
                        from = lo[k] < from ? lo[k] : from;
                        to = hi[k] > to ? hi[k] : to;
                    }
                if (!vb->buffer.user || to - from > POULSBACK_FRAME_VERTEX_BYTES)
                    return -EINVAL;
                e = poulsback_ws_render_stream(c->render, (uint32_t)(to - from), 16, &cpu,
                                               &copy[b].address, &copy[b].handle);
                if (e)
                    return e;
                memcpy(cpu, (const uint8_t *)vb->buffer.user + from, (size_t)(to - from));
                copy[b].low = from;
                copy[b].done = true;
            }
            g->element[i].handle = copy[b].handle;
            g->element[i].address = (uint64_t)((int64_t)copy[b].address - copy[b].low + rel);
        } else {
            struct poulsback_resource_gpu buffer;
            int e = poulsback_resource_buffer_gpu(vb->buffer.resource, sequence, &buffer);
            if (e)
                return e;
            if (hi[i] > (int64_t)buffer.bytes)
                return -EINVAL;
            g->element[i].handle = buffer.handle;
            g->element[i].address = (uint64_t)((int64_t)buffer.gpu_va + rel);
        }
    }
    return 0;
}

/* The viewport as the MTE's words (Tx, Sx, Ty, Sy, Tz, Sz) in WORDS when it
 * is not the window's: a bottom-up target's, a depth range other than [0, 1];
 * else NULL. */
static const uint32_t *draw_viewport(const struct poulsback_context *c, uint32_t words[6])
{
    if (c->viewport.scale[1] < 0 && c->viewport.scale[2] == .5f && c->viewport.translate[2] == .5f)
        return NULL;
    for (unsigned i = 0; i < 3; i++) {
        memcpy(&words[2 * i], &c->viewport.translate[i], 4);
        memcpy(&words[2 * i + 1], &c->viewport.scale[i], 4);
    }
    return words;
}
/* Engine mode with the bound VS on the GPU: 1 when the draws are in the
 * frame, 0 when the CPU path must draw them (nothing done yet), else an
 * error. */
static int gpu_draw(struct poulsback_context *c, const struct pipe_draw_info *info,
                    const struct pipe_draw_start_count_bias *draws, unsigned n,
                    const struct poulsback_draw_layout *l)
{
    const struct state *vs = c->bound[VS], *fs = c->bound[FS], *ve = c->bound[ELEMENTS];
    const struct pipe_rasterizer_state *rs = &c->bound[RASTER]->u.raster;
    const struct poulsback_frame_fragment *fragment = NULL;
    /* the colour's interpolation: a backend-only program has no linkage */
    const bool color_flat =
        fs->u.fs.backend_only ? rs->flatshade : l->slot[0].interpolation == POULSBACK_DRAW_FLAT;
    bool solid = fs->u.fs.solid, flat, stencil = false;
    const struct poulsback_frame_gpu_program *vertex;
    uint8_t components[SGX535_VERTEX_INPUTS];
    uint32_t constants[SGX535_VERTEX_CONSTANTS], constant_count, tc_count = 0;
    struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS];
    const poulsback_frame_shape *shape;
    uint64_t sequence;
    int e;
    if (!vs->u.vs.gpu || cpu_vs_forced())
        return cpu_path(c, "no GPU program");
    /* The backend's program, its uniforms this draw's data; a draw it does
     * not take is the CPU path's (skipped for a program only it takes). */
    if (!fs->u.fs.backend)
        return cpu_path(c, "no backend fragment program");
    /* both faces culled: the triangles draw nothing (no ISP form either) */
    if (rs->cull_face == PIPE_FACE_FRONT_AND_BACK)
        return 1;
    {
        const char *refusal = "a blend factor without a form";
        bool color = poulsback_fragment_reads_color(fs->u.fs.backend);
        const uint32_t samplers = poulsback_fragment_samplers(fs->u.fs.backend);
        struct poulsback_fragment_texture textures[SGX535_FS_MAX_SAMPLERS];
        const struct poulsback_fragment_state *packed =
            poulsback_context_fragment_state(c, color && color_flat);
        struct poulsback_fragment_state st;
        uint32_t viewport[6];
        if (packed && !poulsback_context_textures(c, samplers, textures, &refusal)) {
            st = *packed;
            st.cb0 = c->constant[1].data;
            st.bytes = (uint32_t)c->constant[1].bytes;
            for (uint32_t u = 0; u < SGX535_FS_MAX_SAMPLERS; u++)
                st.texture[u] = samplers >> u & 1u ? &textures[u] : NULL;
            st.viewport = draw_viewport(c, viewport);
            fragment = poulsback_fragment_draw(fs->u.fs.backend, &st, &refusal);
        }
        if (!fragment) {
            refused(c, refusal);
            return cpu_path(c, refusal);
        }
        solid = !color;
        tc_count = poulsback_fragment_tcs(fs->u.fs.backend, tc);
        stencil = st.isp.stencil_test;
        c->stencil_dirty |= poulsback_fragment_writes_stencil(&st.isp);
    }
    flat = !solid && color_flat;
    if (info->index_size && info->primitive_restart)
        return cpu_path(c, "primitive restart");
    if (rs->light_twoside || (flat && rs->flatshade_first) || (!solid && !rs->clamp_vertex_color))
        return cpu_path(c, "rasterizer state");
    if (ve->u.ve.count > POULSBACK_FRAME_GPU_ELEMENTS ||
        ve->u.ve.count < poulsback_vertex_shader_inputs(vs->u.vs.gpu))
        return cpu_path(c, "element count");
    memset(components, 4, sizeof(components));
    for (uint32_t i = 0; i < ve->u.ve.count; i++) {
        const struct pipe_vertex_element *a = &ve->u.ve.element[i];
        components[i] =
            f32_dwords(a->src_format) ? (uint8_t)f32_dwords(a->src_format) : packed(a->src_format);
        if (!components[i] || a->src_stride > 0xffffu || a->vertex_buffer_index >= c->vb_count ||
            (a->src_offset & 3u) || (a->src_stride & 3u) ||
            (c->vb[a->vertex_buffer_index].buffer_offset & 3u))
            return cpu_path(c, "element format");
    }
    /* the legacy program's colour: TC0 */
    if (!fragment && !solid) {
        tc[0] = (struct sgx535_vertex_tc){VARYING_SLOT_COL0, 4};
        tc_count = 1;
    }
    vertex = poulsback_vertex_shader_program(vs->u.vs.gpu, components, tc, tc_count);
    if (!vertex)
        return cpu_path(c, "program for the elements");
    constant_count = poulsback_vertex_shader_constants(vs->u.vs.gpu, c->constant[0].data,
                                                       c->constant[0].bytes, constants);
    if (!constant_count)
        return cpu_path(c, "constants");
    /* From here the draws are the GPU's. */
    e = c->scene_fragment ? ensure_frame(c) : -EINVAL;
    if (!e && (!c->frame_cleared || !c->scene_samples_ready))
        e = -EOPNOTSUPP;
    if (!e)
        e = poulsback_context_draw_shape(c, solid, flat, &shape);
    if (!e && stencil && !c->stencil_ready)
        e = poulsback_context_stencil_object(c);
    if (!e)
        e = poulsback_context_draw_mask(c, rs->scissor);
    if (e)
        return e;
    c->stencil_ready |= stencil;
    sequence = poulsback_ws_render_frame_sequence(c->render);
    for (unsigned i = 0; i < n; i++) {
        struct poulsback_frame_gpu_draw g = {.program = vertex,
                                             .fragment = fragment,
                                             .constants = constants,
                                             .constant_count = constant_count,
                                             .color = !solid,
                                             .flat = flat};
        uint32_t low, high;
        /* texture coordinates other than the group's (the colour's TC0) */
        if (tc_count != !solid) {
            g.tc_count = tc_count;
            for (uint32_t t = 0; t < tc_count; t++)
                g.tc[t] = tc[t].components;
        }
        e = gpu_indices(c, info, &draws[i], sequence, &g, &low, &high);
        if (e == 1)
            continue;
        /* The PDS multiplies the index's low 16 bits by the stride. */
        if (!e && high > 0xffffu)
            e = -EOPNOTSUPP;
        if (!e)
            e = gpu_elements(c, info->index_size ? draws[i].index_bias : 0, low, high, sequence,
                             components, &g);
        if (!e)
            e = poulsback_ws_render_draw_gpu(c->render, shape, &g);
        if (e)
            return e;
        c->t_vertices += g.count;
    }
    return 1;
}

/* The draw module's triangles of a CPU-vertex draw go to the frame writer. */
static bool sink(void *data, const struct poulsback_draw_batch *batch)
{
    struct poulsback_context *c = data;
    int e = frame_sink(c, batch);
    if (e)
        fail(c, e);
    return !e;
}
static unsigned element_bytes(unsigned format)
{
    /* These scalar/vector formats are converted by Mesa draw's ordinary
   * vertex fetch. The driver checks storage spans only; it never decodes,
   * normalizes or repacks attribute values itself. Keep integer, compressed,
   * depth and unreviewed packed formats outside this bounded interface. */
    switch (format) {
    case PIPE_FORMAT_R32_FLOAT:
    case PIPE_FORMAT_R32G32_FLOAT:
    case PIPE_FORMAT_R32G32B32_FLOAT:
    case PIPE_FORMAT_R32G32B32A32_FLOAT:
    case PIPE_FORMAT_R8_UNORM:
    case PIPE_FORMAT_R8G8_UNORM:
    case PIPE_FORMAT_R8G8B8_UNORM:
    case PIPE_FORMAT_R8G8B8A8_UNORM:
    case PIPE_FORMAT_R8_SNORM:
    case PIPE_FORMAT_R8G8_SNORM:
    case PIPE_FORMAT_R8G8B8_SNORM:
    case PIPE_FORMAT_R8G8B8A8_SNORM:
    case PIPE_FORMAT_R16_UNORM:
    case PIPE_FORMAT_R16G16_UNORM:
    case PIPE_FORMAT_R16G16B16_UNORM:
    case PIPE_FORMAT_R16G16B16A16_UNORM:
    case PIPE_FORMAT_R16_SNORM:
    case PIPE_FORMAT_R16G16_SNORM:
    case PIPE_FORMAT_R16G16B16_SNORM:
    case PIPE_FORMAT_R16G16B16A16_SNORM:
    case PIPE_FORMAT_B8G8R8A8_UNORM:
        return util_format_get_blocksize((enum pipe_format)format);
    default:
        return 0;
    }
}
static bool linked_layout(struct poulsback_context *c, struct poulsback_draw_layout *l)
{
    struct state *fs = c->bound[FS];
    const struct tgsi_shader_info *f = &fs->u.fs.draw->info;
    const struct tgsi_shader_info *v = &c->bound[VS]->u.vs.draw->info;
    unsigned i, j, n = 0;
    memset(l, 0, sizeof(*l));
    l->provoking_first = c->bound[RASTER]->u.raster.flatshade_first;
    if (f->num_inputs > POULSBACK_DRAW_SLOTS)
        return false;
    for (i = 0; i < f->num_inputs; i++) {
        unsigned components, interp = f->input_interpolate[i], slot;
        if (!f->input_usage_mask[i])
            continue;
        for (slot = 0; slot < fs->u.fs.linkage.count; slot++) {
            const struct sgx535_fragment_varying *varying = &fs->u.fs.linkage.varying[slot];
            unsigned semantic = varying->semantic == SGX535_FRAGMENT_COLOR ? TGSI_SEMANTIC_COLOR
                                                                           : TGSI_SEMANTIC_GENERIC;
            if (semantic == f->input_semantic_name[i] &&
                varying->index == f->input_semantic_index[i]) {
                unsigned expected = varying->interpolation == SGX535_FRAGMENT_FLAT
                                        ? TGSI_INTERPOLATE_CONSTANT
                                    : varying->interpolation == SGX535_FRAGMENT_SMOOTH
                                        ? TGSI_INTERPOLATE_PERSPECTIVE
                                        : TGSI_INTERPOLATE_COLOR;
                if (interp != expected)
                    return false;
                break;
            }
        }
        if (slot == fs->u.fs.linkage.count || l->slot[slot].components)
            return false;
        components = fs->u.fs.linkage.varying[slot].components;
        if (!components || (f->input_usage_mask[i] & ~((1u << components) - 1u)))
            return false;
        for (j = 0; j < v->num_outputs; j++)
            if (v->output_semantic_name[j] == f->input_semantic_name[i] &&
                v->output_semantic_index[j] == f->input_semantic_index[i])
                break;
        if (j == v->num_outputs)
            return false;
        if (interp == TGSI_INTERPOLATE_COLOR)
            interp = c->bound[RASTER]->u.raster.flatshade ? TGSI_INTERPOLATE_CONSTANT
                                                          : TGSI_INTERPOLATE_PERSPECTIVE;
        if (interp != TGSI_INTERPOLATE_CONSTANT && interp != TGSI_INTERPOLATE_PERSPECTIVE)
            return false;
        l->slot[slot] = (struct poulsback_draw_attribute){
            j, i, components,
            interp == TGSI_INTERPOLATE_CONSTANT ? POULSBACK_DRAW_FLAT : POULSBACK_DRAW_SMOOTH};
        n++;
    }
    return n == fs->u.fs.linkage.count;
}
/* The CPU path's indices: borrowed (user or buffer), every one read for the
 * vertex span *MAX; Q then draws from them as user indices. */
static int cpu_indices(struct poulsback_context *c, const struct pipe_draw_info *info,
                       const struct pipe_draw_start_count_bias *draws, unsigned n,
                       struct pipe_draw_info *q, const void **indices, size_t *index_bytes,
                       unsigned *max)
{
    int e;
    if (info->has_user_indices) {
        if (!info->index.user)
            return refuse(c, -EINVAL);
        *index_bytes = (size_t)*max * info->index_size;
        *indices = info->index.user;
    } else {
        e = poulsback_resource_buffer_borrow(info->index.resource, indices, index_bytes);
        if (e)
            return e;
    }
    if ((uint64_t)*max * info->index_size > *index_bytes || *index_bytes > UINT_MAX)
        return refuse(c, -EINVAL);
    *max = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < draws[i].count; j++) {
            uint32_t value = 0;
            int64_t vertex;
            memcpy(&value,
                   (const uint8_t *)*indices + ((size_t)draws[i].start + j) * info->index_size,
                   info->index_size);
            if (q->primitive_restart && value == q->restart_index)
                continue;
            vertex = (int64_t)value + draws[i].index_bias;
            /* Mesa's draw fetches only the vertices the indices name, so
             * the span bounds just the bytes read (checked per buffer). */
            if (vertex < 0 || vertex >= UINT_MAX)
                return refuse(c, -EINVAL);
            if ((unsigned)vertex + 1 > *max)
                *max = (unsigned)vertex + 1;
        }
    q->has_user_indices = true;
    q->index.user = *indices;
    q->index_bounds_valid = false;
    return 0;
}
/* The draw's fragment program snapshot: none for a backend program on the
 * GPU path (its uniforms are the draw's data), else the legacy
 * program's, specialised for the draw's constants where it needs them. */
static int fragment_snapshot(struct poulsback_context *c, bool backend)
{
    /* a program only the backend takes has no other */
    if (!backend && c->bound[FS]->u.fs.backend_only) {
        refused(c, "a program only a backend draw takes");
        return -EOPNOTSUPP;
    }
    if (!backend)
        return poulsback_context_snapshot_fragment(c);
    c->scene_fragment = c->bound[FS]->u.fs.nir;
    c->scene_fragment_profile = POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED;
    c->scene_fragment_owned = false;
    return 0;
}
/* A draw the GPU path leaves to the CPU path: state only a backend draw takes
 * stops it; else the CPU path's program, unless a refused backend
 * draw took it. */
static int fallback(struct poulsback_context *c, bool backend)
{
    if (backend && !state_supported(c, false))
        return -EOPNOTSUPP;
    if (c->bound[FS]->u.fs.backend && !c->scene_fragment_owned)
        return fragment_snapshot(c, false);
    return 0;
}
/* The draw's snapshots at the boundary the context serializes (bound shader,
 * constants and the complete draw): an independent FS variant, as the CPU
 * vertex constants are; the sample and colour state. No mutable CSO, NIR or
 * buffer is borrowed by the sealed scene. */
static int scene_snapshots(struct poulsback_context *c, bool backend)
{
    int e = fragment_snapshot(c, backend);
    if (!e)
        e = scene_sample_snapshot(c, &c->scene_samples);
    if (e)
        return e;
    c->scene_samples_ready = true;
    e = scene_color_snapshot(c, &c->scene_color);
    if (!e)
        c->scene_color_cso = (uintptr_t)c->bound[BLEND];
    return e;
}
static void draw(struct pipe_context *p, const struct pipe_draw_info *info, unsigned drawid,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws, unsigned n)
{
    struct poulsback_context *c = ctx(p);
    struct pipe_draw_info q;
    struct pipe_vertex_buffer vb[PB_VERTEX_BUFFERS] = {0};
    struct pipe_draw_start_count_bias active[PB_DRAWS];
    /* Borrowed for this synchronous draw: user arrays and CPU buffers. */
    const void *buffers[PB_VERTEX_BUFFERS] = {0}, *indices = NULL;
    size_t sizes[PB_VERTEX_BUFFERS] = {0}, index_bytes = 0;
    struct poulsback_draw_layout l;
    unsigned i, j, max = 0, count = 0;
    uint64_t end;
    bool gpu, backend;
    /* A window st/mesa has not sized yet: drawing into 0x0 is a GL no-op. */
    if (!c->fb.width || !c->fb.height)
        return;
    int e = 0;
    if (c->error)
        return;
    if (!info || n > PB_DRAWS || indirect || (n && !draws)) {
        refused(c, "an indirect draw or more than its batch");
        skip(c, "draw");
        return;
    }
    /* Direct empty intervals have no vertex or fragment work. Keep them out
   * of span validation and draw's first-entry index-bias selection; an empty
   * entry must neither poison a later draw nor require bound shader state. */
    if (!info->instance_count || !n)
        return;
    for (i = 0; i < n; i++)
        if (draws[i].count)
            active[count++] = draws[i];
    if (!count)
        return;
    draws = active;
    n = count;
    if (drawid || info->increment_draw_id || info->index_bias_varies || info->start_instance ||
        info->instance_count != 1 ||
        (info->index_size && info->index_size != 1 && info->index_size != 2 &&
         info->index_size != 4) ||
        (info->mode != MESA_PRIM_TRIANGLES && info->mode != MESA_PRIM_TRIANGLE_STRIP &&
         info->mode != MESA_PRIM_TRIANGLE_FAN && info->mode != MESA_PRIM_QUADS &&
         info->mode != MESA_PRIM_QUAD_STRIP)) {
        refused(c, "instancing, or a primitive other than triangles");
        skip(c, "draw");
        return;
    }
    q = *info;
    /* Gallium restart applies only to indexed draws. Preserve the exact
   * marker for Mesa's primitive decomposition; it is never a vertex and
   * must be recognized before applying the signed vertex bias. */
    q.primitive_restart = info->index_size && info->primitive_restart;
    for (i = 0; i < n; i++) {
        end = (uint64_t)draws[i].start + draws[i].count;
        /* index_bias is meaningless without indices; Mesa leaves it unset for
         * glDrawArrays, and only the indexed path below applies it. The count
         * bounds a draw's work; its start only offsets into the buffers (Mesa's
         * display lists share one, so a short list may start far into it). */
        if (draws[i].count > POULSBACK_RENDER_MAX_INDICES || end > UINT_MAX) {
            e = refuse(c, -EINVAL);
            goto done;
        }
        if (end > max)
            max = (unsigned)end;
    }
    /* The GPU path reads its indices itself. */
    gpu = c->bound[VS] && c->bound[VS]->u.vs.gpu && !cpu_vs_forced() && !q.primitive_restart;
    if (info->index_size && !gpu) {
        e = cpu_indices(c, info, draws, n, &q, &indices, &index_bytes, &max);
        if (e)
            goto done;
    }
    /* An all-marker batch has no shader or resource work. In particular,
   * avoid max-1 span arithmetic and do not refresh live constant buffers. */
    if (!max)
        goto done;
    backend = gpu && c->bound[FS] && c->bound[FS]->u.fs.backend;
    e = frame_start(c);
    if (e)
        goto done;
    if (!state_supported(c, backend) || (!linked_layout(c, &l) && !refused(c, "linked layout"))) {
        e = -EOPNOTSUPP;
        goto done;
    }
    e = poulsback_context_refresh_constants(c);
    if (e)
        goto done;
    e = scene_snapshots(c, backend);
    if (e)
        goto done;
    if (gpu) {
        int drawn = gpu_draw(c, info, draws, n, &l);
        if (drawn) {
            if (!c->t_draws++)
                c->t_frame_begin = trace_clock();
            e = drawn < 0 ? drawn : 0;
            goto done;
        }
        e = fallback(c, backend);
        if (e)
            goto done;
        if (info->index_size) {
            e = cpu_indices(c, info, draws, n, &q, &indices, &index_bytes, &max);
            if (e || !max)
                goto done;
        }
    }
    for (i = 0; i < c->vb_count; i++) {
        size_t need = 0;
        const struct state *ve = c->bound[ELEMENTS];
        for (j = 0; j < ve->u.ve.count; j++)
            if (ve->u.ve.element[j].vertex_buffer_index == i) {
                const struct pipe_vertex_element *a = &ve->u.ve.element[j];
                unsigned b = element_bytes(a->src_format);
                end = (uint64_t)c->vb[i].buffer_offset + a->src_offset +
                      (uint64_t)(max - 1) * a->src_stride + b;
                if (!b || end > POULSBACK_IMAGE_MAX_BYTES) {
                    refused(c, "a vertex element");
                    e = -EOPNOTSUPP;
                    goto done;
                }
                if (end > need)
                    need = (size_t)end;
            }
        if (!need)
            continue;
        if (c->vb[i].is_user_buffer) {
            if (!c->vb[i].buffer.user) {
                e = refuse(c, -EINVAL);
                goto done;
            }
            buffers[i] = c->vb[i].buffer.user;
            sizes[i] = need;
        } else {
            e = poulsback_resource_buffer_borrow(c->vb[i].buffer.resource, &buffers[i], &sizes[i]);
            if (e)
                goto done;
        }
        if (need > sizes[i]) {
            e = refuse(c, -EINVAL);
            goto done;
        }
        vb[i] = c->vb[i];
        vb[i].is_user_buffer = true;
        vb[i].buffer.user = buffers[i];
    }
    for (i = 0; i < c->bound[ELEMENTS]->u.ve.count; i++)
        if (c->bound[ELEMENTS]->u.ve.element[i].vertex_buffer_index >= c->vb_count) {
            e = refuse(c, -EINVAL);
            goto done;
        }
    poulsback_context_cpu_draw_begin(c, vb, buffers, sizes, indices, q.index_size,
                                     (unsigned)index_bytes);
    {
        enum poulsback_draw_status status =
            poulsback_draw_stage_begin_clip_space(c->stage, &l, POULSBACK_DRAW_MAX_TRIANGLES);
        if (status != POULSBACK_DRAW_OK) {
            e = status == POULSBACK_DRAW_ERR_NOMEM ? -ENOMEM : -EOPNOTSUPP;
            goto done;
        }
    }
    if (!c->t_draws++)
        c->t_frame_begin = trace_clock();
    draw_vbo(c->draw, &q, 0, NULL, draws, n, 0);
    {
        enum poulsback_draw_status status = poulsback_draw_stage_end(c->stage);
        if (status != POULSBACK_DRAW_OK) {
            POULSBACK_TRACE("draw: stage status %d\n", (int)status);
            e = c->error ? c->error : -EOPNOTSUPP;
        }
    }
done:
    poulsback_context_cpu_draw_end(c);
    if (c->scene_fragment_owned)
        ralloc_free(c->scene_fragment);
    c->scene_fragment = NULL;
    c->scene_fragment_profile = 0;
    c->scene_fragment_owned = false;
    c->scene_samples_ready = false;
    memset(&c->scene_samples, 0, sizeof(c->scene_samples));
    memset(&c->scene_color, 0, sizeof(c->scene_color));
    c->scene_color_cso = 0;
    if (e == -EOPNOTSUPP)
        skip(c, "draw");
    else if (e)
        fail(c, e);
}
/* Submits the open frame into its target and depth; the job
 * keeps both until its fence signals, and the engine's frame slots bound how
 * many frames are in flight. */
int poulsback_context_flush_frame(struct poulsback_context *c)
{
    struct pipe_fence_handle *fence = NULL;
    struct poulsback_ws_image *image = NULL;
    struct poulsback_ws_depth *depth = NULL;
    uint64_t id = 0, t0 = trace_clock();
    int e, fd = -1;
    if (!c->frame)
        return poulsback_context_textures_release(c);
    if (c->error)
        return c->error;
    /* a frame of clears alone: its render needs a draw */
    e = c->frame_cleared && !poulsback_ws_render_draws(c->render)
            ? poulsback_context_inert_object(c)
            : 0;
    if (!e)
        e = c->frame_cleared ? poulsback_resource_image_acquire(c->scene_target, &image)
                             : POULSBACK_REFUSED(-EOPNOTSUPP);
    if (!e && c->scene_depth_target)
        e = poulsback_resource_depth_acquire(c->scene_depth_target, c->render, &depth);
    if (!e)
        e = poulsback_ws_render_submit_engine(c->render, image, depth, &fd, &id);
    if (!e)
        e = poulsback_fence_adopt(fd, &fence);
    if (depth) {
        int cleanup = poulsback_ws_depth_unref(depth);
        if (!e)
            e = cleanup;
    }
    if (image) {
        int cleanup = poulsback_ws_image_unref(image);
        if (!e)
            e = cleanup;
    }
    {
        int cleanup = poulsback_context_textures_release(c);
        if (!e)
            e = cleanup;
    }
    if (e) {
        if (fence)
            c->base.screen->fence_reference(c->base.screen, &fence, NULL);
        return refuse(c, e);
    }
    c->base.screen->fence_reference(c->base.screen, &c->last_fence, fence);
    c->base.screen->fence_reference(c->base.screen, &fence, NULL);
    POULSBACK_TRACE("frame: draws %u, vertices %u, first draw to flush %.1f ms, submit %.1f\n",
                    c->t_draws, c->t_vertices, (double)(t0 - c->t_frame_begin) / 1e6,
                    (double)(trace_clock() - t0) / 1e6);
    c->t_draws = c->t_vertices = 0;
    c->frame = c->frame_cleared = false;
    pipe_resource_reference(&c->scene_target, NULL);
    pipe_resource_reference(&c->scene_depth_target, NULL);
    c->scene_depth_ready = false;
    return 0;
}
static void flush(struct pipe_context *p, struct pipe_fence_handle **f, unsigned flags)
{
    struct poulsback_context *c = ctx(p);
    int e;
    (void)flags;
    draw_flush(c->draw);
    e = poulsback_context_flush_frame(c);
    if (e)
        fail(c, e);
    e = poulsback_ws_render_reap(c->render);
    if (e && e != -EBUSY)
        fail(c, e);
    e = poulsback_ws_render_error(c->render);
    if (e)
        fail(c, e);
    /* Flush replaces an initialized old reference. A failed newer operation
   * cannot be represented by the last successful native fence. The private
   * failure object is preallocated; old fences and HOLD owners stay intact. */
    if (f)
        p->screen->fence_reference(p->screen, f, c->error ? c->failed_fence : c->last_fence);
}
static void destroy(struct pipe_context *p)
{
    struct poulsback_context *c = ctx(p);
    unsigned i;
    int e;
    /* A failed destroy keeps the entire context graph; no implicit reset. */
    e = poulsback_sampler_bank_close(c->samplers);
    if (e) {
        fail(c, e);
        return;
    }
    e = poulsback_ws_render_destroy(c->render);
    if (e) {
        fail(c, e);
        return;
    }
    e = poulsback_sampler_bank_destroy(c->samplers);
    if (e) {
        fail(c, e);
        return;
    }
    c->samplers = NULL;
    draw_flush(c->draw);
    poulsback_context_bind_state(c, NULL, VS);
    poulsback_context_bind_state(c, NULL, FS);
    poulsback_context_bind_state(c, NULL, ELEMENTS);
    /* Raw raster state remains alive until draw teardown finishes. */
    while (c->states && (c->states->kind == VS || c->states->kind == FS)) {
        struct state *s = c->states;
        s->caller = false;
        poulsback_context_release_state(c, s);
    }
    /* Delete remaining unbound shader objects before destroying draw. */
    {
        struct state *s = c->states, *next;
        while (s) {
            next = s->next;
            if (s->kind == VS || s->kind == FS) {
                s->caller = false;
                poulsback_context_release_state(c, s);
            }
            s = next;
        }
    }
    draw_destroy(c->draw);
    c->draw = NULL;
    /* Both frontend roles share this one ordinary CPU uploader. GPU/HOLD
   * teardown above must succeed before releasing any part of the graph. */
    u_upload_destroy(p->stream_uploader);
    p->stream_uploader = p->const_uploader = NULL;
    for (i = 0; i < STATE_KINDS; i++)
        if (c->bound[i]) {
            struct state *s = c->bound[i];
            c->bound[i] = NULL;
            poulsback_context_release_state(c, s);
        }
    while (c->states) {
        struct state *s = c->states;
        s->caller = false;
        poulsback_context_release_state(c, s);
    }
    pipe_resource_reference(&c->scene_target, NULL);
    pipe_resource_reference(&c->scene_depth_target, NULL);
    poulsback_context_textures_release(c);
    poulsback_fragment_destroy(c->load_fragment);
    util_unreference_framebuffer_state(&c->fb);
    for (i = 0; i < c->vb_count; i++)
        if (!c->vb[i].is_user_buffer)
            pipe_resource_reference(&c->vb[i].buffer.resource, NULL);
    for (i = 0; i < 2; i++) {
        free(c->constant[i].data);
        pipe_resource_reference(&c->constant[i].resource, NULL);
    }
    p->screen->fence_reference(p->screen, &c->last_fence, NULL);
    free_context(c);
}
struct pipe_context *poulsback_context_create(struct pipe_screen *s, void *priv, unsigned flags)
{
    struct poulsback_context *c;
    int e;
    /* Robust buffer access is refused: draws are not bounds-checked. Losing
     * the context on a reset is st/mesa's, through the reset hooks
     * (poulsback_context_state.c). */
    if (!s || !s->fence_reference ||
        (flags & ~(unsigned)(PIPE_CONTEXT_DEBUG | PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET)))
        return NULL;
    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->base.screen = s;
    c->base.priv = priv;
    c->failed_fence = poulsback_fence_create_failed(-EIO);
    if (!c->failed_fence) {
        free(c);
        return NULL;
    }
    c->sample_mask = UINT_MAX;
    e = poulsback_fence_context_init(&c->base);
    if (e) {
        free_context(c);
        return NULL;
    }
    c->base.stream_uploader = u_upload_create_default(&c->base);
    if (!c->base.stream_uploader) {
        free_context(c);
        return NULL;
    }
    c->base.const_uploader = c->base.stream_uploader;
    e = poulsback_resource_render_create(s, &c->render);
    if (e) {
        u_upload_destroy(c->base.stream_uploader);
        free_context(c);
        return NULL;
    }
    /* Frames go through the kernel's depth-scene ABI; there is no other path. */
    if (poulsback_ws_render_depth_scene_version(c->render) != POULSBACK_RENDER_DEPTH_SCENE_ABI ||
        !s->fence_finish) {
        poulsback_ws_render_destroy(c->render);
        u_upload_destroy(c->base.stream_uploader);
        free_context(c);
        return NULL;
    }
    u_index_init();
    e = poulsback_sampler_bank_create(&c->base, &c->samplers);
    if (e) {
        poulsback_ws_render_destroy(c->render);
        u_upload_destroy(c->base.stream_uploader);
        free_context(c);
        return NULL;
    }
    c->draw = draw_create_no_llvm(&c->base);
    if (!c->draw) {
        poulsback_sampler_bank_close(c->samplers);
        poulsback_sampler_bank_destroy(c->samplers);
        poulsback_ws_render_destroy(c->render);
        u_upload_destroy(c->base.stream_uploader);
        free_context(c);
        return NULL;
    }
    c->base.draw = c->draw;
    c->stage = poulsback_draw_stage_create(c->draw, sink, c);
    if (!c->stage) {
        draw_destroy(c->draw);
        poulsback_sampler_bank_close(c->samplers);
        poulsback_sampler_bank_destroy(c->samplers);
        poulsback_ws_render_destroy(c->render);
        u_upload_destroy(c->base.stream_uploader);
        free_context(c);
        return NULL;
    }
    draw_set_rasterize_stage(c->draw, c->stage);
    draw_set_driver_clipping(c->draw, true, true, false, true);
    draw_set_constant_buffer_stride(c->draw, 4);
    c->base.destroy = destroy;
    c->base.draw_vbo = draw;
    c->base.flush = flush;
    c->base.clear = clear;
    poulsback_context_state_init(c);
    return &c->base;
}
