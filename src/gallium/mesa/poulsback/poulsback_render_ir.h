/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_RENDER_IR_H
#define POULSBACK_RENDER_IR_H
#include <linux/types.h>

/* Mesa-internal limits and records named after the former render UAPI: the
 * fragment program limits, the depth-scene ABI a healthy render reports, and
 * the draw stage's copied geometry. No kernel interface carries them. */

/* Scene grammar versions. */
#define POULSBACK_RENDER_GEOMETRY_ABI 2U
#define POULSBACK_RENDER_DEPTH_SCENE_ABI 4U

#define POULSBACK_RENDER_MAX_INSTRUCTIONS 512U
#define POULSBACK_RENDER_IR_CONSTANT 0U
#define POULSBACK_RENDER_IR_INPUT 1U
#define POULSBACK_RENDER_IR_UNIFORM 2U
#define POULSBACK_RENDER_IR_MOV 3U
#define POULSBACK_RENDER_IR_MAD 4U
#define POULSBACK_RENDER_IR_MIN 5U
#define POULSBACK_RENDER_IR_MAX 6U
#define POULSBACK_RENDER_IR_RCP 7U
#define POULSBACK_RENDER_IR_OUTPUT 8U
#define POULSBACK_RENDER_IR_SIGN_XOR 10U /* Exact constant sign mask as source1. */
#define POULSBACK_RENDER_IR_SIGN_AND 11U /* Exact constant magnitude mask. */
#define POULSBACK_RENDER_IR_RAW32_IEQ_V1 12U
#define POULSBACK_RENDER_IR_RAW32_AND_V1 13U
#define POULSBACK_RENDER_IR_RAW32_XOR_V1 14U
/* Profile6 only: source[0] logical shift; immediate is 1, 2, 4, 8, 16 or 31. */
#define POULSBACK_RENDER_IR_RAW32_USHR_IMM_V1 15U
#define POULSBACK_RENDER_IR2_SAMPLE2D_COMPONENT 16U
#define POULSBACK_RENDER_PROFILE_COLOR0_NORMALIZED 4U
#define POULSBACK_RENDER_PROFILE_COLOR0_RAW_SIGN_NORMALIZED 5U
#define POULSBACK_RENDER_PROFILE_COLOR0_RAW_MASK_NORMALIZED_V1 6U
#define POULSBACK_RENDER_PROFILE_SAMPLED_NEAREST_BGRA8_V1 7U
#define POULSBACK_RENDER_SAMPLED_BGRA8_U8 1U
#define POULSBACK_RENDER_INPUT_SLOTS 10U
#define POULSBACK_RENDER_UNIFORM_WORDS 128U

/* Scalar32 straight-line SSA validation IR, not a shader instruction encoding.
 * Definitions reference earlier indices; final OUTPUT components are 0..3.
 * Unused fields are zero. */
struct poulsback_render_instruction {
    __u32 opcode, immediate, index, component, source[3];
};
/* Symbolic sampled inputs, never PDS, USE words or image addresses. */
struct poulsback_render_sampled_input {
    __u32 texture_slot, sampler_slot, coordinate_slot, coordinate_components;
    __u32 interpolation, result_format, result_dwords, reserved;
};

#define POULSBACK_RENDER_DRAW 1U
#define POULSBACK_RENDER_CPU_NOP 2U
struct poulsback_render_submit {
    __u32 abi, operation, context, vertex_program;
    __u32 fragment_program, flags, source_image, target_image;
    __u32 x, y, width, height;
    __u32 input_syncobj, output_syncobj, reserved[2];
    __u64 job_id, reserved2;
};

/* A copied post-vertex plan. Position is homogeneous F32 clip-space XYZW,
 * followed by varying prefixes in ascending slot order. Filled triangle list
 * only: no restart, strip, fan or offset. */
#define POULSBACK_RENDER_TRIANGLES 1U
#define POULSBACK_RENDER_MAX_VERTICES 4096U
#define POULSBACK_RENDER_MAX_INDICES 12288U
#define POULSBACK_RENDER_INTERPOLATION_NONE 0U
#define POULSBACK_RENDER_INTERPOLATION_SMOOTH 1U
#define POULSBACK_RENDER_INTERPOLATION_FLAT 2U
#define POULSBACK_RENDER_CLEAR_DEPTH 2U
struct poulsback_render_varying {
    __u32 components, interpolation;
};
struct poulsback_render_state {
    __u32 cull_mode, front_face;                   /* none/front/back/both; CCW/CW. */
    __u32 depth_test, depth_write, depth_function; /* Boolean, Boolean, 0..7. */
    __u32 clear_mask, clear_color[4], clear_depth; /* F32 [0,1] words. */
    __u32 scissor_enabled, scissor_x, scissor_y, scissor_width, scissor_height;
    __u32 depth_attachment;
    __u32 reserved[3];
};
struct poulsback_render_submit_v2 {
    struct poulsback_render_submit plan; /* abi=2, vertex_program=0. */
    __u32 primitive, vertex_count, index_count, vertex_stride_words;
    __u64 vertices, indices; /* Copied F32 words / optional copied U32 indices. */
    struct poulsback_render_varying varyings[POULSBACK_RENDER_INPUT_SLOTS];
    struct poulsback_render_state state;
    __u64 reserved[2];
};

/* Post-VS scene: clip XYZW and Mesa's window XYZ/reciprocal-W words per vertex. */
#define POULSBACK_RENDER_SCENE_MAX_DRAWS 64U
#define POULSBACK_RENDER_SCENE_PRESERVE 0U
#define POULSBACK_RENDER_SCENE_CLEAR_COLOR 1U
#define POULSBACK_RENDER_SCENE_RGBA_F32 1U
#define POULSBACK_RENDER_SCENE_STORE_COLOR 1U
#define POULSBACK_RENDER_SCENE_ORIGIN_UPPER_LEFT 1U
#define POULSBACK_RENDER_SCENE_DEPTH_CLEAR 2U
#define POULSBACK_RENDER_SCENE_DEPTH_STORE 1U
#define POULSBACK_RENDER_SCENE_DEPTH_F64 1U

struct poulsback_render_scene_window {
    __u32 origin, half_pixel_center, bottom_edge_rule, sample_mask;
    __u32 min_samples, reserved;
    __u32 viewport_scale_f32[3], viewport_translate_f32[3];
};
struct poulsback_render_scene_load {
    __u32 operation, color_encoding, clear_rgba_f32[4];
    __u32 store_operation, reserved;
};
/* CLEAR carries the original finite Gallium clear(double) bits. */
struct poulsback_render_scene_depth {
    __u32 attachment, load_operation, store_operation, clear_encoding;
    __aligned_u64 clear_depth_f64;
    __u32 reserved[2];
};
/* Draws are deindexed triangle lists: clip[4]+COLOR0[4] and a separate
 * window[4] record per vertex. Flat COLOR0 must be replicated by Mesa. */
struct poulsback_render_scene_draw {
    __u32 fragment_program, vertex_count, vertex_stride_words, varying_components;
    __aligned_u64 clip_varyings, window_position;
    struct poulsback_render_scene_window window;
    __u32 interpolation, depth_test, depth_write, depth_function;
};
#endif
