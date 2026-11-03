// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_draw.h"
#include "draw/draw_pipe.h"
#include "draw/draw_fs.h"
#include "tgsi/tgsi_scan.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct producer_identity {
    const struct pipe_rasterizer_state *rasterizer;
    const void *render;
    struct draw_stage *rasterize, *clip, *cull, *twoside, *validate, *tail;
    uint32_t clip_xy, clip_z, clip_user;
};
struct poulsback_stage {
    struct draw_stage base;
    struct poulsback_draw_layout layout;
    struct pipe_rasterizer_state rasterizer;
    struct draw_vertex_shader *vs;
    struct draw_fragment_shader *fs;
    poulsback_draw_sink sink;
    void *user;
    /* Interval views into the buffers below; NULL outside an interval, and
     * window stays NULL for a clip-only interval. */
    uint32_t *words;
    struct poulsback_draw_window_position *window;
    uint32_t *word_buffer;
    struct poulsback_draw_window_position *window_buffer;
    struct pipe_viewport_state viewports[PIPE_MAX_VIEWPORTS];
    enum poulsback_draw_status status;
    uint32_t stride, limit, count, position;
    struct producer_identity producer;
    struct draw_stage *created_clip, *created_cull, *created_twoside, *created_validate;
    const struct prim_header *forwarded;
    uint32_t witnessed;
    bool active, canonical;
    /* Clip-space interval: positions straight from the VS, nothing clipped,
     * culled or viewport-transformed on the CPU (the MTE does it). */
    bool clip_space;
};
struct sink_scope {
    const struct poulsback_stage *owner;
    const struct poulsback_draw_batch *batch;
    const struct sink_scope *previous;
};
static _Thread_local const struct sink_scope *live_sink;
_Static_assert(sizeof(struct poulsback_draw_window_position) == 4u * sizeof(uint32_t),
               "Window sidecar holds exactly four unchanged F32 words");
static struct poulsback_stage *owner(struct draw_stage *stage)
{
    return (struct poulsback_stage *)stage;
}
static bool finite_word(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}
static struct producer_identity producer_identity(const struct draw_stage *stage)
{
    const struct draw_context *d = stage->draw;
    return (struct producer_identity){.rasterizer = d->rasterizer,
                                      .render = d->render,
                                      .rasterize = d->pipeline.rasterize,
                                      .clip = d->pipeline.clip,
                                      .cull = d->pipeline.cull,
                                      .twoside = d->pipeline.twoside,
                                      .validate = d->pipeline.validate,
                                      .tail = stage->next,
                                      .clip_xy = d->clip_xy,
                                      .clip_z = d->clip_z,
                                      .clip_user = d->clip_user};
}
static bool same_producer(const struct draw_stage *stage, const struct producer_identity *p)
{
    struct producer_identity now = producer_identity(stage);
    return now.rasterizer == p->rasterizer && now.render == p->render &&
           now.rasterize == p->rasterize && now.clip == p->clip && now.cull == p->cull &&
           now.twoside == p->twoside && now.validate == p->validate && now.tail == p->tail &&
           now.clip_xy == p->clip_xy && now.clip_z == p->clip_z && now.clip_user == p->clip_user;
}
static bool canonical_tail(const struct poulsback_stage *s)
{
    const struct draw_context *d = s->base.draw;
    const struct draw_stage *next = &s->base;
    if (!s->canonical || !same_producer(&s->base, &s->producer))
        return false;
    /* Mesa26.2.4 validate_pipeline builds clip -> determinant/cull ->
     * twoside -> rasterize. Even CULL_NONE needs cull's determinant when
     * twoside is active. Every other intervening mutation remains refused. */
    if (s->rasterizer.light_twoside) {
        if (!s->producer.twoside || s->producer.twoside->draw != d ||
            s->producer.twoside->next != next)
            return false;
        next = s->producer.twoside;
    }
    if (s->rasterizer.cull_face || s->rasterizer.light_twoside) {
        if (s->producer.cull->draw != d || s->producer.cull->next != next)
            return false;
        next = s->producer.cull;
    }
    return s->producer.clip->draw == d && s->producer.clip->next == next;
}
static bool canonical_graph(const struct poulsback_stage *s)
{
    return canonical_tail(s) && s->base.draw->pipeline.first == s->producer.clip;
}
static bool state_supported(const struct draw_context *d)
{
    const struct pipe_rasterizer_state *r = d->rasterizer;
    /* noLLVM post-VS writes clip_pos only with an enabled clip producer.
     * Guard-band selection alone does not enable one. LLVM stores it itself;
     * CLIPDIST-only noLLVM modes remain outside this bounded profile. */
    return r && (d->llvm || d->clip_xy || d->clip_z || d->clip_user) && d->vs.vertex_shader &&
           d->fs.fragment_shader && !d->gs.geometry_shader && !d->tes.tess_eval_shader &&
           !d->tcs.tess_ctrl_shader && r->fill_front == PIPE_POLYGON_MODE_FILL &&
           r->fill_back == PIPE_POLYGON_MODE_FILL && !r->offset_tri && !r->offset_line &&
           !r->offset_point && !r->rasterizer_discard && !r->poly_smooth &&
           !r->poly_stipple_enable && !r->multisample && !r->force_persample_interp &&
           !r->depth_clamp && !r->clip_halfz && r->depth_clip_near && r->depth_clip_far &&
           !r->rasterization_stream && r->conservative_raster_mode == PIPE_CONSERVATIVE_RASTER_OFF;
}
/* A clip-space interval's producer: noLLVM with every clip producer and the
 * viewport bypassed (an identity viewport), no cull or two-sided stage, so the
 * pipeline is decomposition straight into this stage and data[POSITION] stays
 * the VS's clip-space output. */
static bool clip_space_supported(const struct draw_context *d)
{
    const struct pipe_rasterizer_state *r = d->rasterizer;
    return r && !d->llvm && !d->clip_xy && !d->clip_z && !d->clip_user && d->bypass_viewport &&
           !d->ms.mesh_shader && d->vs.vertex_shader && d->fs.fragment_shader &&
           !d->gs.geometry_shader && !d->tes.tess_eval_shader && !d->tcs.tess_ctrl_shader &&
           r->cull_face == PIPE_FACE_NONE && !r->light_twoside &&
           r->fill_front == PIPE_POLYGON_MODE_FILL && r->fill_back == PIPE_POLYGON_MODE_FILL &&
           !r->offset_tri && !r->offset_line && !r->offset_point && !r->rasterizer_discard &&
           !r->poly_smooth && !r->poly_stipple_enable && !r->multisample &&
           !r->force_persample_interp && !r->depth_clamp && !r->clip_halfz && r->depth_clip_near &&
           r->depth_clip_far && !r->rasterization_stream &&
           r->conservative_raster_mode == PIPE_CONSERVATIVE_RASTER_OFF;
}
static bool position_output(const struct draw_context *d, uint32_t *slot)
{
    const struct tgsi_shader_info *vs = draw_get_shader_info(d);
    unsigned position = draw_current_shader_position_output(d);
    if (vs->properties[TGSI_PROPERTY_VS_WINDOW_SPACE_POSITION] ||
        vs->num_outputs > PIPE_MAX_SHADER_OUTPUTS || position >= PIPE_MAX_SHADER_OUTPUTS ||
        position >= vs->num_outputs ||
        vs->output_semantic_name[position] != TGSI_SEMANTIC_POSITION ||
        vs->output_semantic_index[position] || vs->output_usagemask[position] != 15u)
        return false;
    *slot = position;
    return true;
}
static bool window_position_output(const struct draw_context *d, uint32_t *slot)
{
    /* An identity viewport also bypasses Mesa's divide for an unclipped vertex.
     * Value shape alone therefore cannot establish this representation. */
    if (d->llvm || d->ms.mesh_shader || d->bypass_viewport)
        return false;
    return position_output(d, slot);
}
static bool unchanged(const struct poulsback_stage *s)
{
    const struct draw_context *d = s->base.draw;
    uint32_t position;
    if (s->clip_space)
        return clip_space_supported(d) && position_output(d, &position) &&
               position == s->position && same_producer(&s->base, &s->producer) &&
               d->pipeline.first == &s->base && d->vs.vertex_shader == s->vs &&
               d->fs.fragment_shader == s->fs &&
               !memcmp(d->rasterizer, &s->rasterizer, sizeof(s->rasterizer));
    if (!state_supported(d) || (d->rasterizer->light_twoside && !s->canonical) ||
        !same_producer(&s->base, &s->producer) || d->vs.vertex_shader != s->vs ||
        d->fs.fragment_shader != s->fs ||
        memcmp(d->rasterizer, &s->rasterizer, sizeof(s->rasterizer)))
        return false;
    if (s->window && (!window_position_output(d, &position) || position != s->position ||
                      memcmp(d->viewports, s->viewports, sizeof(s->viewports))))
        return false;
    return true;
}
static bool linked(const struct draw_context *d, const struct poulsback_draw_layout *l)
{
    const struct tgsi_shader_info *vs = draw_get_shader_info(d), *fs = &d->fs.fragment_shader->info;
    unsigned i;
    uint32_t used = 0, required = 0;
    if (vs->num_outputs > PIPE_MAX_SHADER_OUTPUTS ||
        l->provoking_first != d->rasterizer->flatshade_first ||
        fs->num_inputs > POULSBACK_DRAW_SLOTS)
        return false;
    for (i = 0; i < fs->num_inputs; i++)
        if (fs->input_usage_mask[i])
            required |= 1u << i;
    for (i = 0; i < POULSBACK_DRAW_SLOTS; i++) {
        const struct poulsback_draw_attribute *a = &l->slot[i];
        unsigned interpolation, mask;
        if (!a->components)
            continue;
        if (a->source >= vs->num_outputs || a->fragment_input >= fs->num_inputs ||
            (used & (1u << a->fragment_input)))
            return false;
        if (vs->output_semantic_name[a->source] != fs->input_semantic_name[a->fragment_input] ||
            vs->output_semantic_index[a->source] != fs->input_semantic_index[a->fragment_input])
            return false;
        if (fs->input_semantic_name[a->fragment_input] != TGSI_SEMANTIC_GENERIC &&
            fs->input_semantic_name[a->fragment_input] != TGSI_SEMANTIC_COLOR)
            return false;
        if (fs->input_interpolate_loc[a->fragment_input] != TGSI_INTERPOLATE_LOC_CENTER)
            return false;
        interpolation = fs->input_interpolate[a->fragment_input];
        if (interpolation == TGSI_INTERPOLATE_COLOR)
            interpolation =
                d->rasterizer->flatshade ? TGSI_INTERPOLATE_CONSTANT : TGSI_INTERPOLATE_PERSPECTIVE;
        if ((a->interpolation == POULSBACK_DRAW_SMOOTH &&
             interpolation != TGSI_INTERPOLATE_PERSPECTIVE) ||
            (a->interpolation == POULSBACK_DRAW_FLAT && interpolation != TGSI_INTERPOLATE_CONSTANT))
            return false;
        mask = (1u << a->components) - 1u;
        /* Every component copied by the collector must be declared by VS. */
        if ((mask & ~vs->output_usagemask[a->source]) ||
            (fs->input_usage_mask[a->fragment_input] & ~mask))
            return false;
        if (d->rasterizer->light_twoside &&
            vs->output_semantic_name[a->source] == TGSI_SEMANTIC_COLOR) {
            unsigned front = 0, back = 0, index = vs->output_semantic_index[a->source];
            /* Mesa's stage copies complete vec4 COLOR0/1 pairs and picks the
             * last matching declaration. Require unique complete pairs so
             * declared linkage cannot select unrepresented BCOLOR words. */
            if (index > 1 || a->components != 4 || vs->output_usagemask[a->source] != 15)
                return false;
            for (unsigned j = 0; j < vs->num_outputs; j++) {
                if (vs->output_semantic_index[j] != index)
                    continue;
                if (vs->output_semantic_name[j] == TGSI_SEMANTIC_COLOR)
                    front++;
                if (vs->output_semantic_name[j] == TGSI_SEMANTIC_BCOLOR) {
                    if (vs->output_usagemask[j] != 15)
                        return false;
                    back++;
                }
            }
            if (front != 1 || back != 1)
                return false;
        }
        used |= 1u << a->fragment_input;
    }
    return used == required;
}
static bool vertex(const struct poulsback_stage *s, const struct prim_header *prim, unsigned which,
                   uint32_t *out, struct poulsback_draw_window_position *window)
{
    const struct vertex_header *v = prim->v[which];
    const struct pipe_viewport_state *viewport = &s->base.draw->viewports[0];
    float clip[4];
    int plane[3] = {0, 0, 0};
    unsigned i, c, n = 4;
    if (s->clip_space) {
        /* The VS's position as it is: the MTE clips, so W may be any finite
         * value, also zero or negative. */
        if (!v)
            return false;
        for (c = 0; c < 4; c++) {
            if (!finite_word(v->data[s->position][c]))
                return false;
            memcpy(out + c, &v->data[s->position][c], sizeof(*out));
        }
        goto varyings;
    }
    if (!v || v->clipmask || !finite_word(v->clip_pos[3]) || v->clip_pos[3] <= 0.0f)
        return false;
    memcpy(clip, v->clip_pos, sizeof(clip));
    for (c = 0; c < 3; c++) {
        if (!finite_word(clip[c]))
            return false;
        /* Mesa's clipper puts its new vertices on a clip plane only up to
         * rounding: within 2^-16 w, snap them onto it (the window edge). */
        if (clip[c] < -clip[3] || clip[c] > clip[3]) {
            if (fabsf(clip[c]) > clip[3] * (1.0f + 0x1p-16f))
                return false;
            plane[c] = clip[c] < 0.0f ? -1 : 1;
            clip[c] = (float)plane[c] * clip[3];
        }
    }
    memcpy(out, clip, sizeof(clip));
    if (window)
        for (c = 0; c < 4; c++) {
            uint32_t bits;
            float edge;
            memcpy(&bits, &v->data[s->position][c], sizeof(bits));
            if (c < 3 && plane[c]) {
                edge = viewport->translate[c] + viewport->scale[c] * (float)plane[c];
                memcpy(&bits, &edge, sizeof(bits));
            }
            if ((bits & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) ||
                (c == 3 && ((bits & UINT32_C(0x80000000)) || !bits)))
                return false;
            window->window_xyz_reciprocal_w[c] = bits;
        }
varyings:
    for (i = 0; i < POULSBACK_DRAW_SLOTS; i++) {
        const struct poulsback_draw_attribute *a = &s->layout.slot[i];
        const struct vertex_header *source = v;
        if (!a->components)
            continue;
        if (a->interpolation == POULSBACK_DRAW_FLAT)
            source = prim->v[s->layout.provoking_first ? 0 : 2];
        if (!source)
            return false;
        for (c = 0; c < a->components; c++) {
            if (!finite_word(source->data[a->source][c]))
                return false;
            memcpy(out + n++, &source->data[a->source][c], sizeof(*out));
        }
    }
    return n == s->stride;
}
/* Per triangle: the producers, shaders and rasterizer object are still the
 * ones begin accepted, so the vertex records hold what the layout reads. The
 * contents of those objects, the linkage and the viewports follow from them
 * or are checked again in full by stage_end. */
static bool same_objects(const struct poulsback_stage *s)
{
    const struct draw_context *d = s->base.draw;
    return (s->clip_space ? clip_space_supported(d) : state_supported(d)) &&
           same_producer(&s->base, &s->producer) && d->vs.vertex_shader == s->vs &&
           d->fs.fragment_shader == s->fs;
}
static void collect_triangle(struct draw_stage *stage, struct prim_header *prim)
{
    struct poulsback_stage *s = owner(stage);
    uint32_t *result;
    struct poulsback_draw_window_position *window;
    unsigned v;
    if (s->status != POULSBACK_DRAW_OK)
        return;
    if (!s->active || !prim) {
        s->status = POULSBACK_DRAW_ERR_ARGUMENT;
        return;
    }
    if (!same_objects(s)) {
        s->status = POULSBACK_DRAW_ERR_STATE;
        return;
    }
    if (s->canonical && (s->forwarded != prim || !canonical_graph(s) ||
                         s->rasterizer.cull_face == PIPE_FACE_FRONT_AND_BACK)) {
        s->status = POULSBACK_DRAW_ERR_STATE;
        return;
    }
    if (s->count == s->limit) {
        s->status = POULSBACK_DRAW_ERR_LIMIT;
        return;
    }
    /* Written in place: a refused triangle leaves count, so its words stay
     * outside the batch. */
    result = s->words + (size_t)s->count * 3u * s->stride;
    window = s->window ? s->window + (size_t)s->count * 3u : NULL;
    for (v = 0; v < 3; v++)
        if (!vertex(s, prim, v, result + v * s->stride, window ? window + v : NULL)) {
            s->status = POULSBACK_DRAW_ERR_VERTEX;
            return;
        }
    s->count++;
    if (s->canonical)
        s->witnessed++;
}
static void triangle(struct draw_stage *stage, struct prim_header *prim)
{
    struct poulsback_stage *s = owner(stage);
    /* This observer is the installed rasterize endpoint. The only private
     * collector call is synchronous, inside the selected Mesa clip/cull/twoside graph;
     * a foreign final tap can collect diagnostics but cannot mint a receipt. */
    if (s->forwarded || (s->canonical && !canonical_graph(s))) {
        if (s->status == POULSBACK_DRAW_OK)
            s->status = POULSBACK_DRAW_ERR_STATE;
        return;
    }
    s->forwarded = prim;
    collect_triangle(stage, prim);
    s->forwarded = NULL;
}
static void unsupported(struct draw_stage *stage, struct prim_header *prim)
{
    struct poulsback_stage *s = owner(stage);
    (void)prim;
    if (s->status == POULSBACK_DRAW_OK)
        s->status = POULSBACK_DRAW_ERR_PRIMITIVE;
}
static void flush(struct draw_stage *stage, unsigned flags)
{
    (void)stage;
    (void)flags;
}
static void reset_stipple(struct draw_stage *stage)
{
    (void)stage;
}
static void destroy(struct draw_stage *stage)
{
    free(owner(stage)->word_buffer);
    free(owner(stage)->window_buffer);
    free(stage);
}
struct draw_stage *poulsback_draw_stage_create(struct draw_context *draw, poulsback_draw_sink sink,
                                               void *user)
{
    struct poulsback_stage *s;
    if (!draw || !sink)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    /* Every interval reuses these. The bound is 4095 nonindexed vertices of
     * at most 44 words and a four-word window record each: below 768 KiB on
     * i386, and only the pages an interval writes become resident. */
    s->word_buffer = calloc((size_t)POULSBACK_DRAW_MAX_TRIANGLES * 3u * POULSBACK_DRAW_MAX_STRIDE,
                            sizeof(*s->word_buffer));
    if (s->word_buffer)
        s->window_buffer =
            calloc((size_t)POULSBACK_DRAW_MAX_TRIANGLES * 3u, sizeof(*s->window_buffer));
    if (!s->window_buffer) {
        destroy(&s->base);
        return NULL;
    }
    s->base.draw = draw;
    s->base.name = "poulsback-triangle-collector";
    s->base.point = unsupported;
    s->base.line = unsupported;
    s->base.tri = triangle;
    s->base.flush = flush;
    s->base.reset_stipple_counter = reset_stipple;
    s->base.destroy = destroy;
    s->sink = sink;
    s->user = user;
    s->created_clip = draw->pipeline.clip;
    s->created_cull = draw->pipeline.cull;
    s->created_twoside = draw->pipeline.twoside;
    s->created_validate = draw->pipeline.validate;
    return &s->base;
}
enum interval { CLIP_WORDS, WINDOW, CLIP_SPACE };
static enum poulsback_draw_status begin(struct draw_stage *stage,
                                        const struct poulsback_draw_layout *input, uint32_t limit,
                                        enum interval interval)
{
    const bool retain_window = interval == WINDOW, clip_space = interval == CLIP_SPACE;
    struct poulsback_stage *s;
    struct poulsback_draw_layout layout;
    unsigned i, stride = 4;
    uint32_t position = 0;
    struct producer_identity producer;
    struct pipe_rasterizer_state rasterizer;
    bool canonical;
    if (!stage || !input || !limit || limit > POULSBACK_DRAW_MAX_TRIANGLES)
        return POULSBACK_DRAW_ERR_ARGUMENT;
    s = owner(stage);
    if (s->active)
        return POULSBACK_DRAW_ERR_ACTIVE;
    layout = *input;
    if (layout.provoking_first > 1)
        return POULSBACK_DRAW_ERR_LAYOUT;
    for (i = 0; i < POULSBACK_DRAW_SLOTS; i++) {
        const struct poulsback_draw_attribute *a = &layout.slot[i];
        if ((!a->components &&
             (a->source || a->fragment_input || a->interpolation != POULSBACK_DRAW_UNUSED)) ||
            a->components > 4 ||
            (a->components && (a->interpolation != POULSBACK_DRAW_SMOOTH &&
                               a->interpolation != POULSBACK_DRAW_FLAT)))
            return POULSBACK_DRAW_ERR_LAYOUT;
        stride += a->components;
    }
    if (clip_space ? !clip_space_supported(stage->draw) || !position_output(stage->draw, &position)
                   : !state_supported(stage->draw))
        return POULSBACK_DRAW_ERR_STATE;
    /* A malformed POSITION producer is a state failure even when the same
     * shader metadata also makes varying linkage invalid. */
    if (retain_window && !window_position_output(stage->draw, &position))
        return POULSBACK_DRAW_ERR_STATE;
    if (!linked(stage->draw, &layout))
        return POULSBACK_DRAW_ERR_LAYOUT;
    producer = producer_identity(stage);
    rasterizer = *stage->draw->rasterizer;
    canonical = retain_window && !producer.render && !producer.tail &&
                producer.rasterize == stage && producer.clip == s->created_clip &&
                producer.cull == s->created_cull && producer.twoside == s->created_twoside &&
                producer.validate == s->created_validate && producer.clip && producer.cull &&
                producer.twoside && producer.validate &&
                (producer.clip_xy || producer.clip_z || producer.clip_user);
    if ((rasterizer.light_twoside && !canonical) ||
        (clip_space && (producer.render || producer.tail || producer.rasterize != stage)))
        return POULSBACK_DRAW_ERR_STATE;
    s->words = s->word_buffer;
    s->layout = layout;
    s->stride = stride;
    s->limit = limit;
    s->count = 0;
    s->window = retain_window ? s->window_buffer : NULL;
    s->position = position;
    if (retain_window)
        memcpy(s->viewports, stage->draw->viewports, sizeof(s->viewports));
    s->vs = stage->draw->vs.vertex_shader;
    s->fs = stage->draw->fs.fragment_shader;
    s->rasterizer = rasterizer;
    s->producer = producer;
    s->canonical = canonical;
    s->clip_space = clip_space;
    s->witnessed = 0;
    s->forwarded = NULL;
    s->status = POULSBACK_DRAW_OK;
    s->active = true;
    return POULSBACK_DRAW_OK;
}
enum poulsback_draw_status
poulsback_draw_stage_begin_clip_space(struct draw_stage *stage,
                                      const struct poulsback_draw_layout *input, uint32_t limit)
{
    return begin(stage, input, limit, CLIP_SPACE);
}
enum poulsback_draw_status poulsback_draw_stage_end(struct draw_stage *stage)
{
    struct poulsback_stage *s;
    if (!stage)
        return POULSBACK_DRAW_ERR_ARGUMENT;
    s = owner(stage);
    if (!s->active)
        return POULSBACK_DRAW_ERR_ARGUMENT;
    draw_flush(stage->draw);
    if (s->status == POULSBACK_DRAW_OK && (!unchanged(s) || !linked(stage->draw, &s->layout)))
        s->status = POULSBACK_DRAW_ERR_STATE;
    if (s->status == POULSBACK_DRAW_OK && s->count) {
        struct poulsback_draw_batch batch = {
            .stride_words = s->stride,
            .triangle_count = s->count,
            .layout = s->layout,
            .words = s->words,
            .window_position = s->window,
            .position_output = s->position,
        };
        const struct sink_scope scope = {.owner = s, .batch = &batch, .previous = live_sink};
        live_sink = &scope;
        if (!s->sink(s->user, &batch))
            s->status = POULSBACK_DRAW_ERR_SINK;
        live_sink = scope.previous;
    }
    s->words = NULL;
    s->window = NULL;
    s->active = false;
    return s->status;
}
bool poulsback_draw_batch_residual_raster(const struct draw_context *d,
                                          const struct poulsback_draw_batch *b,
                                          struct poulsback_draw_residual_raster *out)
{
    const struct poulsback_stage *s;
    if (!d || !b || !out || !live_sink || live_sink->batch != b || !(s = live_sink->owner) ||
        s->base.draw != d || !s->active || !s->canonical || s->status != POULSBACK_DRAW_OK ||
        !s->count || s->witnessed != s->count ||
        s->rasterizer.cull_face == PIPE_FACE_FRONT_AND_BACK || !unchanged(s) ||
        !canonical_tail(s) || !linked(d, &s->layout) || b->words != s->words ||
        b->window_position != s->window || b->triangle_count != s->count ||
        b->stride_words != s->stride || b->position_output != s->position ||
        memcmp(&b->layout, &s->layout, sizeof(s->layout)))
        return false;
    *out = (struct poulsback_draw_residual_raster){
        .version = POULSBACK_DRAW_RESIDUAL_RASTER_VERSION,
        .producer = POULSBACK_DRAW_RESIDUAL_FINAL_COLLECTOR,
        .source_cull_face = s->rasterizer.cull_face,
        .source_front_ccw = s->rasterizer.front_ccw,
        .source_flatshade_first = s->rasterizer.flatshade_first,
        .residual_cull_face = PIPE_FACE_NONE,
        .triangle_count = s->count,
        .position_output = s->position,
        .clip_xy = s->producer.clip_xy,
        .clip_z = s->producer.clip_z,
        .clip_user = s->producer.clip_user,
        .source_light_twoside = s->rasterizer.light_twoside,
        .residual_light_twoside = 0};
    return true;
}

struct poulsback_draw_window_snapshot {
    struct poulsback_draw_window_view view;
    uint32_t *clip;
    struct poulsback_draw_window_position *window;
    unsigned refs;
};
