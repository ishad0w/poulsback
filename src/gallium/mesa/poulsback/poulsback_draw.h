// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Mesa draw's final triangle stage. No rasterizer, GPU submission or screen. */
#ifndef POULSBACK_DRAW_H
#define POULSBACK_DRAW_H
#include <stdbool.h>
#include <stdint.h>
#include "poulsback_render_ir.h"

#define POULSBACK_DRAW_SLOTS POULSBACK_RENDER_INPUT_SLOTS
#define POULSBACK_DRAW_MAX_STRIDE (4u + 4u * POULSBACK_DRAW_SLOTS)
#define POULSBACK_DRAW_MAX_TRIANGLES (POULSBACK_RENDER_MAX_VERTICES / 3u)

struct draw_context;
struct draw_stage;

enum poulsback_draw_interpolation {
    POULSBACK_DRAW_UNUSED,
    POULSBACK_DRAW_SMOOTH,
    POULSBACK_DRAW_FLAT
};
struct poulsback_draw_attribute {
    uint32_t source;         /* Mesa post-transform shader output index. */
    uint32_t fragment_input; /* Matching bound fragment shader input index. */
    uint32_t components;
    enum poulsback_draw_interpolation interpolation;
};
struct poulsback_draw_layout {
    struct poulsback_draw_attribute slot[POULSBACK_DRAW_SLOTS];
    uint32_t provoking_first;
};
/* Already produced by Mesa's ordinary post-VS/clipping viewport path. These
 * are window XYZ and reciprocal W, not homogeneous clip positions. No native
 * viewport, sample/edge, interpolation or physical execution is qualified. */
struct poulsback_draw_window_position {
    uint32_t window_xyz_reciprocal_w[4];
};
struct poulsback_draw_batch {
    uint32_t stride_words;
    uint32_t triangle_count;
    struct poulsback_draw_layout layout;
    /* Tightly packed triangle-list vertices: homogeneous clip position then
     * varying prefixes in ascending logical slot order. Exact extent is
     * triangle_count * 3 * stride_words. Valid only during the sink call. */
    const uint32_t *words;
    /* NULL for the clip-space interval, the one the product begins.
     * position_output identifies the checked real Mesa POSITION output slot. */
    const struct poulsback_draw_window_position *window_position;
    uint32_t position_output;
};
/* CPU producer receipt, not a native capability or a caller-settable promise.
 * Only the live canonical final-stage sink can obtain this record. Original
 * face/provoking/two-sided state remains intact; residual NONE/zero means
 * Mesa's selected clip/cull/twoside chain has consumed that state for every
 * emitted triangle. Two-sided selection requires declared COLOR/BCOLOR pairs;
 * native face-dependent color selection is not requested.
 * This does not authenticate arbitrary code running in the same process. */
#define POULSBACK_DRAW_RESIDUAL_RASTER_VERSION 2u
#define POULSBACK_DRAW_RESIDUAL_FINAL_COLLECTOR 1u
struct poulsback_draw_residual_raster {
    uint32_t version, producer;
    uint32_t source_cull_face, source_front_ccw, source_flatshade_first;
    uint32_t residual_cull_face, triangle_count, position_output;
    uint32_t clip_xy, clip_z, clip_user;
    uint32_t source_light_twoside, residual_light_twoside;
};
bool poulsback_draw_batch_residual_raster(const struct draw_context *,
                                          const struct poulsback_draw_batch *,
                                          struct poulsback_draw_residual_raster *);
/* Private userspace handoff version, not a render UAPI/capability or GPU
 * qualification. The envelope keeps ABI2's homogeneous clip meaning. */
#define POULSBACK_DRAW_WINDOW_VERSION 1u
#define POULSBACK_DRAW_WINDOW_ORIGIN_UPPER_LEFT 1u
struct poulsback_draw_window_raster {
    uint32_t origin;
    uint32_t half_pixel_center, bottom_edge_rule;
    uint32_t sample_mask, min_samples;
    uint32_t viewport_scale[3], viewport_translate[3]; /* Exact F32 state bits. */
};
struct poulsback_draw_window_view {
    uint32_t version;
    struct poulsback_render_submit_v2 geometry;
    struct poulsback_draw_window_raster raster;
    const struct poulsback_draw_window_position *window_position;
    uint32_t window_vertex_count;
    uint32_t position_output; /* Checked Mesa producer slot, not a packet selector. */
};
struct poulsback_draw_window_snapshot;
enum poulsback_draw_status {
    POULSBACK_DRAW_OK,
    POULSBACK_DRAW_ERR_ARGUMENT,
    POULSBACK_DRAW_ERR_LAYOUT,
    POULSBACK_DRAW_ERR_PRIMITIVE,
    POULSBACK_DRAW_ERR_VERTEX,
    POULSBACK_DRAW_ERR_LIMIT,
    POULSBACK_DRAW_ERR_SINK,
    POULSBACK_DRAW_ERR_ACTIVE,
    POULSBACK_DRAW_ERR_STATE,
    POULSBACK_DRAW_ERR_NOMEM
};
/* The sink must copy all data before returning. It returns true only after
 * accepting ownership of its own copy. false is a sticky error, not a retry.
 * This CPU stage has no authority to allocate/publish native GPU resources. */
typedef bool (*poulsback_draw_sink)(void *, const struct poulsback_draw_batch *);

/* Caller sets draw_set_rasterize_stage; Mesa then owns stage destruction.
 * Before attachment, the caller owns stage->destroy. No implicit attachment.
 * The stage allocates its worst-case collection buffers here, once; begin and
 * end never allocate. */
struct draw_stage *poulsback_draw_stage_create(struct draw_context *, poulsback_draw_sink, void *);
/* Clip-space collection: the words are the VS's POSITION as it
 * is, then the varyings; nothing is clipped, culled or viewport-transformed on
 * the CPU, the MTE does all three. Requires noLLVM with every clip producer
 * and the viewport bypassed (draw_set_driver_clipping and an identity
 * viewport), cull NONE and no two-sided lighting in the draw module's
 * rasterizer: the pipeline is primitive decomposition straight into this
 * stage. Positions may have any finite W. No window sidecar. */
enum poulsback_draw_status
poulsback_draw_stage_begin_clip_space(struct draw_stage *, const struct poulsback_draw_layout *,
                                      uint32_t triangle_limit);
/* Flushes actual Mesa draw and calls the sink once only if the entire CPU
 * interval succeeded and contains triangles. Errors publish no batch. The
 * batch's arrays are valid only during the sink call: the next interval
 * reuses them, independently of any kernel copy or HOLD resource owned by a
 * future submission consumer. */
enum poulsback_draw_status poulsback_draw_stage_end(struct draw_stage *);
#endif
