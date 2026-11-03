// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_WINDOW_LIST_H
#define SGX535_NORMALIZED_COLOR0_WINDOW_LIST_H
#include "sgx535_normalized_color0_mask_list.h"
#include "sgx535_window_vertex.h"

#define SGX535_NORMALIZED_COLOR0_WINDOW_LIST_VERSION 13u
#define SGX535_NORMALIZED_COLOR0_WINDOW_LIST_PROFILE 12u
#define SGX535_NORMALIZED_COLOR0_WINDOW_LIST_BACKGROUND_VERSION 4u
#define SGX535_NORMALIZED_COLOR0_WINDOW_LIST_INPUT_TRIANGLES                                       \
    SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES
#define SGX535_WINDOW_LIST_MISSING_SOURCE_PROVENANCE 1u
#define SGX535_WINDOW_LIST_MISSING_RHW_INTERPOLATION 2u
#define SGX535_WINDOW_LIST_MISSING_ALL 3u

/* The tuple is the actual CPU raster-pipeline output: window X/Y/Z and 1/W.
 * It is never a homogeneous clip vector. The original clip positions and
 * viewport remain separate source-side objects; a consumer must authenticate
 * that provenance rather than infer it from four finite numbers. */
struct sgx535_normalized_color0_window_list_request {
    uint32_t version, profile, core_id, revision;
    const struct sgx535_window_color0_triangle *triangles;
    uint32_t triangle_count, interpolation;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
struct sgx535_normalized_color0_window_list_task {
    struct sgx535_normalized_color0_mask_list_task list;
    uint32_t missing_window_contracts;
};
struct sgx535_normalized_color0_window_list_workspace {
    struct sgx535_window_color0_triangle
        triangles[SGX535_NORMALIZED_COLOR0_WINDOW_LIST_INPUT_TRIANGLES];
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t vertex_words[SGX535_NORMALIZED_COLOR0_WINDOW_LIST_INPUT_TRIANGLES * 24u + 16u];
    struct sgx535_normalized_color0_task seed;
    struct sgx535_normalized_color0_workspace single;
    struct sgx535_normalized_color0_window_list_task result;
};
struct sgx535_normalized_color0_window_list_launch {
    struct sgx535_normalized_color0_launch normalized;
    uint32_t missing_window_contracts;
};
struct sgx535_normalized_color0_window_list_launch_workspace {
    struct sgx535_normalized_color0_window_list_workspace packet;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_window_list_launch result;
};
struct sgx535_normalized_color0_window_list_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_normalized_color0_window_list_request foreground;
};
struct sgx535_normalized_color0_window_list_background_image {
    uint32_t version, missing_background_contracts;
    struct sgx535_normalized_color0_window_list_task foreground;
    struct sgx535_background_image background;
};
struct sgx535_normalized_color0_window_list_background_bound {
    struct sgx535_normalized_color0_background_bound normalized;
    uint32_t missing_window_contracts;
};
struct sgx535_normalized_color0_window_list_background_workspace {
    struct sgx535_normalized_color0_window_list_background_image image;
    struct sgx535_normalized_color0_window_list_workspace foreground;
    struct sgx535_background_bound background;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_window_list_background_bound result;
};

/* Complete pure task13: one mask prefix, one ordered indexed list, one shared
 * immutable normalized COLOR0 program/state/event suffix, and optional outer4
 * constant BG. Window XYZ must be finite in [0,16]/[0,16]/[0,1]; signed zero XYZ
 * is preserved. Reciprocal W is any positive finite nonzero binary32. RGBA is
 * finite nonnegative [0,1], refusing negative zero. Flat mode requires the
 * existing replicated-flat field and bit-identical colors for each triangle;
 * smooth mode requires that field zero. There is no implicit clip/divide/
 * viewport recomputation, W reinterpretation, numerical rescaling or extra
 * MTE/iterator setting. Hardware RHW/interpolation and PACK numerics remain
 * unqualified, along with the existing source/owner/cache/execution gates.
 *
 * Request, triangles, IR and bindings must remain stable through all calls.
 * Copies are checked after allocation; no input pointer escapes. Workspaces
 * are explicit/off-stack and disjoint from inputs and outputs. Failure keeps
 * output bytes unchanged, including whole-capacity refusal; never split the
 * scene or reset mask/BG per draw. Old task12 frontdoors reject this identity.
 * The retained Mesa producer defines the tuple, but these bounds cannot prove
 * its provenance. No native admission, MMIO, release or reuse is granted. */
enum sgx535_task_status sgx535_normalized_color0_window_list_build(
    const struct sgx535_normalized_color0_window_list_request *,
    struct sgx535_normalized_color0_window_list_task *,
    struct sgx535_normalized_color0_window_list_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_validate(
    const struct sgx535_normalized_color0_window_list_request *,
    const struct sgx535_normalized_color0_window_list_task *,
    struct sgx535_normalized_color0_window_list_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_launch_build(
    const struct sgx535_normalized_color0_window_list_request *,
    const struct sgx535_normalized_color0_window_list_task *,
    const struct sgx535_task_launch_binding *, struct sgx535_normalized_color0_window_list_launch *,
    struct sgx535_normalized_color0_window_list_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_launch_validate(
    const struct sgx535_normalized_color0_window_list_request *,
    const struct sgx535_normalized_color0_window_list_task *,
    const struct sgx535_task_launch_binding *,
    const struct sgx535_normalized_color0_window_list_launch *,
    struct sgx535_normalized_color0_window_list_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_background_build(
    const struct sgx535_normalized_color0_window_list_background_request *,
    struct sgx535_normalized_color0_window_list_background_image *,
    struct sgx535_normalized_color0_window_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_background_validate(
    const struct sgx535_normalized_color0_window_list_background_request *,
    const struct sgx535_normalized_color0_window_list_background_image *,
    struct sgx535_normalized_color0_window_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_background_bind(
    const struct sgx535_normalized_color0_window_list_background_request *,
    const struct sgx535_normalized_color0_window_list_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    struct sgx535_normalized_color0_window_list_background_bound *,
    struct sgx535_normalized_color0_window_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_window_list_background_bound_validate(
    const struct sgx535_normalized_color0_window_list_background_request *,
    const struct sgx535_normalized_color0_window_list_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    const struct sgx535_normalized_color0_window_list_background_bound *,
    struct sgx535_normalized_color0_window_list_background_workspace *);
#endif
