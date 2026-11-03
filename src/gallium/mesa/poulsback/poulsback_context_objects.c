// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The context's frame objects: the solid template a
 * backend draw's group is built from, and quads on the CPU path with ISP words
 * of their own, the stencil's clear object, the scissor's draw-mask objects
 * and partial clears. */

#include "poulsback_context_private.h"
#include <errno.h>
#include <string.h>

/* The program a backend draw's group is built from: the legacy
 * constructor builds a group's state from a shape, and a shape names a
 * program, which never runs there (the draw's own pixel state follows). The
 * colour passed through, or a constant from four uniform words (the forms
 * poulsback-frame-gpu builds its groups from). */
#define TEMPLATE_INPUT(c) {.opcode = SGX535_FRAGMENT_INPUT, .index = 0, .component = c}
#define TEMPLATE_UNIFORM(c) {.opcode = SGX535_FRAGMENT_UNIFORM, .index = c}
#define TEMPLATE_OUTPUT(c)                                                                         \
    {                                                                                              \
        .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {c}                                \
    }
#define TEMPLATE(source, inputs)                                                                   \
    {                                                                                              \
        .instruction = {source(0),          source(1),          source(2),                         \
                        source(3),          TEMPLATE_OUTPUT(0), TEMPLATE_OUTPUT(1),                \
                        TEMPLATE_OUTPUT(2), TEMPLATE_OUTPUT(3)},                                   \
        .instruction_count = 8, .source_linkage = {.count = inputs}                                \
    }
const struct sgx535_fragment_dri_nir_ir poulsback_template_program[2] = {
    TEMPLATE(TEMPLATE_UNIFORM, 0), TEMPLATE(TEMPLATE_INPUT, 1)};
const uint32_t poulsback_template_uniforms[4] = {0, 0, 0, 0x3f800000u};
/* A quad over R (target pixels from the top left) at clip-space Z on the
 * CPU path, of the solid template's COLOUR (four F32 words, NULL: its
 * default), with ISP_GROUPS of ISP its own, culling off: the stencil's clear
 * object, the scissor's draw-mask objects and partial clears. */
static int rect_object(struct poulsback_context *c, const struct sgx535_rect *r, float z,
                       const uint32_t *colour, const uint32_t *isp, uint32_t isp_groups)
{
    const float w = (float)c->fb.width, h = (float)c->fb.height;
    const float x0 = 2.0f * (float)r->x0 / w - 1.0f, x1 = 2.0f * (float)r->x1 / w - 1.0f;
    const float top = 1.0f - 2.0f * (float)r->y0 / h, bottom = 1.0f - 2.0f * (float)r->y1 / h;
    const float quad[6][4] = {{x0, bottom, z, 1}, {x1, bottom, z, 1}, {x1, top, z, 1},
                              {x0, bottom, z, 1}, {x1, top, z, 1},    {x0, top, z, 1}};
    uint32_t words[6][4];
    poulsback_frame_shape shape;
    int e = poulsback_context_frame_shape(c, &poulsback_template_program[0], true, false,
                                          colour ? colour : poulsback_template_uniforms, 4,
                                          POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED, true, true,
                                          &shape);
    if (e)
        return e;
    memcpy(words, quad, sizeof(words));
    shape.mesh.scene.foreground.cull = 0; /* whatever faces the draws cull */
    return poulsback_ws_render_draw(c->render, &shape,
                                    &(struct poulsback_frame_vertices){.count = 6,
                                                                       .window = words[0],
                                                                       .clip_w = &words[0][3],
                                                                       .window_stride = 4,
                                                                       .clip_stride = 4,
                                                                       .isp_groups = isp_groups,
                                                                       .isp = isp});
}
/* The ISP's draw mask to the bound scissor, or the whole target: each
 * render starts with it set over the target (BGOBJ's MASK), and
 * EMGD's viewport objects clear the last rectangle and set the next before a
 * scissored draw: the scissor's pixel-exact edges. Gallium's scissor is the
 * window's, from the top left. */
int poulsback_context_draw_mask(struct poulsback_context *c, bool scissor)
{
    struct sgx535_rect to = {
        MIN2(c->scissor.minx, c->fb.width), MIN2(c->scissor.miny, c->fb.height),
        MIN2(c->scissor.maxx, c->fb.width), MIN2(c->scissor.maxy, c->fb.height)};
    struct sgx535_mask_object o[2];
    int n, e = 0;
    if (scissor && (to.x1 < to.x0 || to.y1 < to.y0))
        to.x1 = to.x0, to.y1 = to.y0; /* empty */
    if (c->mask_cut ? scissor && !memcmp(&c->mask, &to, sizeof(to)) : !scissor)
        return 0;
    n = sgx535_draw_mask_objects(c->mask_cut ? &c->mask : NULL, scissor ? &to : NULL, c->fb.width,
                                 c->fb.height, o);
    if (n < 0)
        return -EOPNOTSUPP;
    for (int i = 0; i < n && !e; i++)
        e = rect_object(c, &o[i].rect, 0.0f, NULL,
                        (const uint32_t[3]){o[i].ispa, o[i].ispb, o[i].ispc}, 7);
    if (!e)
        c->mask_cut = scissor, c->mask = to;
    return e;
}
/* An object over the target that writes nothing (no colour, depth and
 * stencil not tested or written): a frame of clears alone renders with it,
 * as EMGD draws its clears as geometry; the background clears the target. */
int poulsback_context_inert_object(struct poulsback_context *c)
{
    const struct sgx535_isp_state state = {.front_ccw = 1, .no_color = 1};
    const struct sgx535_rect all = {0, 0, (int32_t)c->fb.width, (int32_t)c->fb.height};
    struct sgx535_isp_words w;
    if (!sgx535_isp_words(&state, &w))
        return -EINVAL;
    return rect_object(c, &all, 0.0f, NULL, (const uint32_t[3]){w.ispa, w.ispb, w.ispc}, 3);
}
/* The open frame's stencil, before its first draw that tests it: the
 * background leaves each tile the previous tile's stencil
 * (docs/native-3d.md#pixel-pds-and-isp-words), so, as
 * EMGD clears, an object over the target that writes only the stencil,
 * ALWAYS and REPLACE with the stencil's value, under the whole draw mask. */
int poulsback_context_stencil_object(struct poulsback_context *c)
{
    const struct sgx535_isp_stencil s = {PIPE_FUNC_ALWAYS, c->stencil_value, 0xff, 0xff, 0, 0, 2};
    const struct sgx535_isp_state state = {
        .stencil_test = 1, .front = s, .back = s, .front_ccw = 1, .no_color = 1};
    const struct sgx535_rect all = {0, 0, (int32_t)c->fb.width, (int32_t)c->fb.height};
    struct sgx535_isp_words w;
    int e;
    if (!sgx535_isp_words(&state, &w))
        return -EINVAL;
    e = poulsback_context_draw_mask(c, false);
    return e ? e : rect_object(c, &all, 0.0f, NULL, (const uint32_t[3]){w.ispa, w.ispb, w.ispc}, 7);
}

/* A clear that does not start the frame over: a scissored or
 * partial one, as a quad over its rectangle after the frame's draws, its
 * colour from the solid template, its depth ALWAYS and written, its stencil
 * ALWAYS and REPLACE under its write mask, under the whole draw mask. Only in
 * a frame a full clear began (the target is not loaded yet); a colour mask
 * that leaves channels is refused. */
int poulsback_context_clear_object(struct poulsback_context *c, unsigned buffers, uint32_t colors,
                                   uint8_t stencil_mask, const struct pipe_scissor_state *s,
                                   const union pipe_color_union *value, double depth,
                                   unsigned stencil)
{
    const bool colour = buffers & PIPE_CLEAR_COLOR0, zs = c->fb.zsbuf.texture != NULL,
               d = zs && (buffers & PIPE_CLEAR_DEPTH),
               st = zs && c->fb.zsbuf.format == PIPE_FORMAT_Z24_UNORM_S8_UINT &&
                    (buffers & PIPE_CLEAR_STENCIL) && stencil_mask;
    const struct sgx535_isp_stencil face = {
        PIPE_FUNC_ALWAYS, (uint8_t)stencil, 0xff, stencil_mask, 0, 0, 2};
    const struct sgx535_isp_state state = {.depth_test = d,
                                           .depth_func = PIPE_FUNC_ALWAYS,
                                           .depth_write = d,
                                           .stencil_test = st,
                                           .front = face,
                                           .back = face,
                                           .front_ccw = 1,
                                           .colormask = colour ? 15 : 0,
                                           .no_color = !colour};
    struct sgx535_rect r = {0, 0, (int32_t)c->fb.width, (int32_t)c->fb.height};
    uint32_t words[4] = {0};
    struct sgx535_isp_words w;
    int e = 0;
    if (!c->frame || !c->frame_cleared ||
        (colour && (!value || (colors & PIPE_MASK_RGBA) != PIPE_MASK_RGBA)) ||
        (buffers & ~(PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTHSTENCIL)) || depth < 0.0 || depth > 1.0 ||
        !sgx535_isp_words(&state, &w))
        return -EOPNOTSUPP;
    for (unsigned i = 0; colour && i < 4; i++) {
        float v = value->f[i] < 0.0f ? 0.0f : value->f[i] > 1.0f ? 1.0f : value->f[i];
        memcpy(&words[i], &v, 4);
    }
    if (s) {
        r.x0 = MIN2(s->minx, c->fb.width), r.y0 = MIN2(s->miny, c->fb.height);
        r.x1 = MIN2(s->maxx, c->fb.width), r.y1 = MIN2(s->maxy, c->fb.height);
    }
    if (r.x1 <= r.x0 || r.y1 <= r.y0 || (!colour && !d && !st))
        return 0;
    /* the frame's stencil before a part of it changes (stencil_object) */
    if (st && !c->stencil_ready) {
        e = poulsback_context_stencil_object(c);
        c->stencil_ready = !e;
    }
    if (!e)
        e = poulsback_context_draw_mask(c, false);
    if (!e)
        e = rect_object(c, &r, (float)(2.0 * depth - 1.0), words,
                        (const uint32_t[3]){w.ispa, w.ispb, w.ispc}, st ? 7u : 3u);
    if (!e && st) {
        bool whole = !r.x0 && !r.y0 && r.x1 == (int32_t)c->fb.width &&
                     r.y1 == (int32_t)c->fb.height && stencil_mask == 0xff;
        c->stencil_dirty = !whole;
        if (whole)
            c->stencil_value = (uint8_t)stencil;
    }
    return e;
}

/* The load draw's vertex program: the position and the texture coordinate
 * (pa0-3, pa4-5) through to o0-5. */
static int load_program(struct poulsback_context *c)
{
    struct sgx535_vertex_instruction in[6];
    if (c->load_program.id)
        return 0;
    for (unsigned o = 0; o < 6; o++)
        in[o] = (struct sgx535_vertex_instruction){
            SGX535_VERTEX_MOV, (int8_t)o, {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)o}}};
    if (sgx535_vertex_encode(in, 6, 6, &c->load_code) != SGX535_VERTEX_OK)
        return -EINVAL;
    /* an identity of its own, apart from the shaders' (poulsback_vertex.c) */
    c->load_program =
        (struct poulsback_frame_gpu_program){UINT64_C(3) << 62 | (uintptr_t)c, c->load_code.words,
                                             c->load_code.instruction_count, c->load_code.temps};
    return 0;
}

/* The open frame's start when no full clear began it (docs/native-3d.md,
 * loading the target): the target's previous contents, the target sampled as
 * a stride texture over itself, each pixel its own texel, depth ALWAYS and
 * not written, before the frame's draws; the render loads the depth and
 * stencil its attachment stored. */
int poulsback_context_load(struct poulsback_context *c)
{
    /* clip-space x y z w, then s t: the target's first row is the window's top */
    static const float vertices[6][6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0},
                                         {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0},
                                         {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1}};
    static const uint32_t constants[1] = {0};
    const char *refusal = "the load draw";
    struct poulsback_fragment_texture t;
    struct poulsback_fragment_state st = {
        .height = c->fb.height,
        .isp = {.front_ccw = 1, .colormask = 15},
        .key = {.target = c->fb.cbufs[0].format == PIPE_FORMAT_B8G8R8X8_UNORM
                              ? SGX535_FS_TARGET_RGB8
                              : SGX535_FS_TARGET_B8G8R8A8,
                .colormask = 15},
        .texture = {&t}};
    const struct poulsback_frame_fragment *fragment;
    struct poulsback_frame_gpu_draw g = {.program = &c->load_program,
                                         .element_count = 2,
                                         .constants = constants,
                                         .constant_count = 1,
                                         .count = 6,
                                         .tc_count = 1,
                                         .tc = {2}};
    poulsback_frame_shape shape;
    uint64_t address;
    uint32_t handle;
    void *cpu;
    int e;
    if (c->frame_cleared)
        return 0;
    if (!c->frame)
        return -EINVAL;
    if (!c->load_fragment && !(c->load_fragment = poulsback_fragment_create_load()))
        return -ENOMEM;
    e = load_program(c);
    if (!e)
        e = poulsback_context_target_texture(c, &t, &refusal);
    if (e) {
        POULSBACK_TRACE("load: %s\n", refusal);
        return e;
    }
    fragment = poulsback_fragment_draw(c->load_fragment, &st, &refusal);
    if (!fragment) {
        POULSBACK_TRACE("load: %s\n", refusal);
        return -EOPNOTSUPP;
    }
    g.fragment = fragment;
    e = poulsback_context_frame_shape(
        c, &poulsback_template_program[0], true, false, poulsback_template_uniforms, 4,
        POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED, true, true, &shape);
    if (!e)
        e = poulsback_ws_render_stream(c->render, sizeof(vertices), 16, &cpu, &address, &handle);
    if (e)
        return e;
    memcpy(cpu, vertices, sizeof(vertices));
    g.element[0] = (struct poulsback_frame_gpu_element){handle, address, 4, 24};
    g.element[1] = (struct poulsback_frame_gpu_element){handle, address + 16, 2, 24};
    poulsback_ws_render_sequence(c->render, &g.index_address, &g.index_handle);
    e = poulsback_ws_render_draw_gpu(c->render, &shape, &g);
    if (e)
        return e;
    poulsback_ws_render_load(c->render);
    /* the colour, depth and stencil the previous render stored */
    c->frame_cleared = c->scene_depth_ready = c->stencil_ready = true;
    c->mask_cut = false;
    return 0;
}
