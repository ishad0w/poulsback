// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Observe actual Mesa post-VS/clipped window coordinates; no GPU contract. */
#include "draw/draw_context.h"
#include "draw/draw_pipe.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "tgsi/tgsi_ureg.h"
#include "tgsi/tgsi_scan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture {
    struct draw_stage base;
    unsigned triangles, position;
    float w;
};
static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static void tri(struct draw_stage *stage, struct prim_header *p)
{
    struct capture *s = (struct capture *)stage;
    unsigned i;
    const struct tgsi_shader_info *info = draw_get_shader_info(stage->draw);
    assert(s->position == 1 && info->output_semantic_name[1] == TGSI_SEMANTIC_POSITION);
    s->triangles++;
    for (i = 0; i < 3; i++) {
        const struct vertex_header *v = p->v[i];
        const float *a = v->clip_pos, *b = v->data[s->position];
        assert(!v->clipmask && a[3] == s->w && b[3] == 1.0f / s->w);
        /* Exact dyadic fixture values: this checks Mesa output, never SGX. */
        assert(b[0] == a[0] / a[3] * 123.0f + 200.0f);
        assert(b[1] == a[1] / a[3] * -77.0f + 100.0f);
        assert(b[2] == .5f);
        printf(
            "window-observation t%u v%u position-slot%u clip=%08x,%08x,%08x,%08x window=%08x,%08x,%08x,%08x\n",
            s->triangles, i, s->position, bits(a[0]), bits(a[1]), bits(a[2]), bits(a[3]),
            bits(b[0]), bits(b[1]), bits(b[2]), bits(b[3]));
    }
}
static void unsupported(struct draw_stage *s, struct prim_header *p)
{
    (void)s;
    (void)p;
    assert(0);
}
static void flush(struct draw_stage *s, unsigned flags)
{
    (void)s;
    (void)flags;
}
static void reset(struct draw_stage *s)
{
    (void)s;
}
static void destroy(struct draw_stage *s)
{
    free(s);
}
static const struct tgsi_token *tokens(bool vs)
{
    struct ureg_program *u = ureg_create(vs ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT);
    struct ureg_src in;
    const struct tgsi_token *t;
    assert(u);
    if (vs) {
        in = ureg_DECL_vs_input(u, 0);
        ureg_MOV(u, ureg_DECL_output(u, TGSI_SEMANTIC_GENERIC, 0), in);
        ureg_MOV(u, ureg_DECL_output(u, TGSI_SEMANTIC_POSITION, 0), in);
    } else {
        in = ureg_DECL_fs_input(u, TGSI_SEMANTIC_GENERIC, 0, TGSI_INTERPOLATE_PERSPECTIVE);
        ureg_MOV(u, ureg_DECL_output(u, TGSI_SEMANTIC_COLOR, 0), in);
    }
    ureg_END(u);
    t = ureg_get_tokens(u, NULL);
    assert(t);
    ureg_destroy(u);
    return t;
}
int main(void)
{
    struct pipe_screen screen = {0};
    struct pipe_context p = {.screen = &screen};
    struct draw_context *d = draw_create_no_llvm(&p);
    struct capture *s = calloc(1, sizeof(*s));
    struct pipe_rasterizer_state raster = {.fill_front = PIPE_POLYGON_MODE_FILL,
                                           .fill_back = PIPE_POLYGON_MODE_FILL,
                                           .depth_clip_near = true,
                                           .depth_clip_far = true,
                                           .point_size = 1,
                                           .line_width = 1};
    struct pipe_viewport_state vp = {{123, -77, .25f}, {200, 100, .5f}};
    struct pipe_vertex_buffer vb = {.is_user_buffer = true};
    struct pipe_vertex_element e = {.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT, .src_stride = 16};
    struct pipe_draw_info info = {.mode = MESA_PRIM_TRIANGLES, .instance_count = 1};
    struct pipe_draw_start_count_bias range = {.count = 3};
    struct pipe_shader_state state = {.type = PIPE_SHADER_IR_TGSI};
    struct draw_vertex_shader *vs;
    struct draw_fragment_shader *fs;
    const struct tgsi_token *vt = tokens(true), *ft = tokens(false);
    float v[3][4] = {{-1, -1, 0, 2}, {1, -1, 0, 2}, {0, 1, 0, 2}};
    unsigned phase, i;
    assert(d && s);
    s->base.draw = d;
    s->base.name = "CPU-window-observer";
    s->base.tri = tri;
    s->base.point = unsupported;
    s->base.line = unsupported;
    s->base.flush = flush;
    s->base.reset_stipple_counter = reset;
    s->base.destroy = destroy;
    s->position = 1;
    draw_set_rasterize_stage(d, &s->base);
    draw_set_rasterizer_state(d, &raster, &raster);
    draw_set_viewport_states(d, 0, 1, &vp);
    state.tokens = vt;
    vs = draw_create_vertex_shader(d, &state);
    assert(vs);
    draw_bind_vertex_shader(d, vs);
    state.tokens = ft;
    fs = draw_create_fragment_shader(d, &state);
    assert(fs);
    draw_bind_fragment_shader(d, fs);
    vb.buffer.user = v;
    draw_set_vertex_buffers(d, 1, &vb);
    draw_set_vertex_elements(d, 1, &e);
    for (phase = 0; phase < 3; phase++) {
        s->w = phase == 1 ? 1 : 2;
        s->triangles = 0;
        if (phase == 1)
            for (i = 0; i < 3; i++) {
                v[i][0] *= .5f;
                v[i][1] *= .5f;
                v[i][3] = 1;
            }
        if (phase == 2)
            for (i = 0; i < 3; i++) {
                v[i][0] *= 2;
                v[i][1] *= 2;
                v[i][3] = 2;
            }
        if (phase == 2)
            v[2][1] = 3; /* Actual clipping creates extra window vertices. */
        draw_set_mapped_vertex_buffer(d, 0, v, sizeof(v));
        draw_vbo(d, &info, 0, NULL, &range, 1, 0);
        draw_flush(d);
        assert(s->triangles == (phase == 2 ? 2u : 1u));
    }
    draw_bind_vertex_shader(d, NULL);
    draw_bind_fragment_shader(d, NULL);
    draw_delete_vertex_shader(d, vs);
    draw_delete_fragment_shader(d, fs);
    draw_destroy(d);
    ureg_free_tokens(vt);
    ureg_free_tokens(ft);
    puts(
        "PASS actual Mesa window XYZ/reciprocalW at discovered position slot1, W1/W2 and clipping; no SGX interpretation");
    return 0;
}
