// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_COLOR0_TASK_H
#define POULSBACK_COLOR0_TASK_H
#include "pipe/p_state.h"
#include "poulsback_draw.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_normalized_color0_mask.h"
#include "sgx535_normalized_color0_task.h"

/* Private CPU preparation, not a transport/version/capability selector. */
struct nir_shader;
struct poulsback_color0_source {
    struct poulsback_draw_layout layout;
    struct pipe_rasterizer_state rasterizer;
    struct pipe_viewport_state viewport[PIPE_MAX_VIEWPORTS];
    uint32_t position_output, color_output, fragment_input;
    uint32_t clip_xy, clip_z, clip_user;
};
struct poulsback_color0_task {
    struct poulsback_color0_source source;
    uint32_t clip_position[3][4];
    struct sgx535_window_color0_triangle vertices;
    struct sgx535_fragment_dri_nir_ir symbolic;
    struct sgx535_task_launch_binding binding;
    struct sgx535_normalized_color0_task task;
    struct sgx535_normalized_color0_launch launch;
};
struct poulsback_color0_workspace {
    struct poulsback_color0_task result;
    struct poulsback_color0_source after;
    struct sgx535_normalized_color0_workspace task;
    struct sgx535_normalized_color0_launch_workspace launch;
};

/* Complete owned task11/profile10/outerBG2 CPU image. Its foreground keeps
 * original semantic linkage, clip/window/color words and checked live IR. The
 * foreground task/launch here are mask-profile results, never relabeled task10.
 * Background code, both bindings and all numeric/physical blockers are owned.
 * There are no input pointers in the result. */
struct poulsback_color0_mask_task {
    struct poulsback_color0_task foreground;
    struct sgx535_background_binding background_binding;
    struct sgx535_background_image background;
    struct sgx535_background_bound bound_background;
    uint32_t version, packed_background_argb, missing_background_contracts;
};
struct poulsback_color0_mask_workspace {
    struct poulsback_color0_mask_task result;
    struct poulsback_color0_source after;
    struct sgx535_normalized_color0_background_image image;
    struct sgx535_normalized_color0_background_bound bound;
    struct sgx535_normalized_color0_background_workspace composition;
};
/* Compile the symbolic fragment program of this NIR, profile and resolved
 * interpolation (a solid program ignores it). It depends on nothing else, so a
 * caller compiles it once per shader. This may allocate; out is large and
 * belongs off stack. */
enum sgx535_nir_status poulsback_color0_program(const struct nir_shader *,
                                                enum sgx535_fragment_uniform_layout,
                                                uint32_t profile,
                                                enum sgx535_fragment_interpolation, bool solid,
                                                struct sgx535_fragment_dri_nir_ir *);
/* What the frame writer's sink checks of a draw (poulsback_frame.h): producer state,
 * raster request, linkage with PROGRAM and the uniform words it reads. */
struct poulsback_color0_draw {
    struct poulsback_color0_source source;
    struct poulsback_draw_residual_raster residual;
    bool has_residual;
    uint32_t uniform_word_count, uniform_words_used[4];
    uint32_t uniforms[SGX535_FRAGMENT_UNIFORM_WORDS];
};
enum poulsback_draw_status
poulsback_color0_draw_check(const struct draw_context *, const struct poulsback_draw_batch *,
                            const struct sgx535_fragment_dri_nir_ir *, uint32_t profile, bool solid,
                            const void *uniforms, size_t uniform_bytes, uint32_t width,
                            uint32_t height, const struct poulsback_draw_window_raster *,
                            struct poulsback_color0_draw *);
#endif
