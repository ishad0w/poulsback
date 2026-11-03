// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_MASK_LIST_H
#define SGX535_NORMALIZED_COLOR0_MASK_LIST_H
#include "sgx535_normalized_color0_mask.h"

#define SGX535_NORMALIZED_COLOR0_MASK_LIST_VERSION 12u
#define SGX535_NORMALIZED_COLOR0_MASK_LIST_PROFILE 11u
#define SGX535_NORMALIZED_COLOR0_MASK_LIST_BACKGROUND_VERSION 3u
/* Copied-input storage bound only. The complete serialized task/code/indices
 * must also fit; this is neither a hardware nor an API primitive limit. */
#define SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES (SGX535_TASK_PACKET_BYTES / 96u)
struct sgx535_normalized_color0_mask_list_triangle {
    uint32_t clip_position[3][4], rgba[3][4];
};
struct sgx535_normalized_color0_mask_list_request {
    uint32_t version, profile, core_id, revision;
    const struct sgx535_normalized_color0_mask_list_triangle *triangles;
    uint32_t triangle_count, interpolation;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
struct sgx535_normalized_color0_mask_list_task {
    struct sgx535_normalized_color0_task task;
    uint32_t triangle_count, vertex_count, foreground_vertex_bytes;
    uint32_t mask_vertex_offset_bytes, vertex_storage_bytes;
    uint32_t index_offset_bytes, foreground_index_offset_bytes;
    uint32_t foreground_index_bytes, index_storage_bytes;
};
struct sgx535_normalized_color0_mask_list_workspace {
    struct sgx535_normalized_color0_mask_list_triangle
        triangles[SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES];
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t vertex_words[SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES * 24u + 16u];
    struct sgx535_normalized_color0_task seed;
    struct sgx535_normalized_color0_workspace single;
    struct sgx535_normalized_color0_mask_list_task result;
};
struct sgx535_normalized_color0_mask_list_launch_workspace {
    struct sgx535_normalized_color0_mask_list_workspace packet;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_launch result;
};
struct sgx535_normalized_color0_mask_list_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_normalized_color0_mask_list_request foreground;
};
struct sgx535_normalized_color0_mask_list_background_image {
    uint32_t version, missing_background_contracts;
    struct sgx535_normalized_color0_mask_list_task foreground;
    struct sgx535_background_image background;
};
struct sgx535_normalized_color0_mask_list_background_workspace {
    struct sgx535_normalized_color0_mask_list_background_image image;
    struct sgx535_normalized_color0_mask_list_workspace foreground;
    struct sgx535_background_bound background;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_background_bound result;
};

/* One complete task: one fullscreen mask, one ordered triangle-list record,
 * one shared immutable scalar IR/program/state/event suffix and, for outer3,
 * one constant BG. No per-triangle job or mask/BG reset is produced. Different
 * shader/state groups and preserving earlier jobs are outside this contract.
 * Clip-W1 and RGBA domain match the checked fixed16 codec. Flat inputs must be
 * replicated per triangle; no new flat flag is encoded. Whole-list capacity
 * failure is transactional: no prefix, split, resource or fence is published.
 *
 * Request, geometry, IR and bindings stay stable through allocation/sleep.
 * Geometry/IR are copied and rechecked; outputs own no input pointer. Explicit
 * off-stack workspaces and output must be disjoint from each other and every
 * input span. All missing masks and zero qualification fields persist. No
 * native admission, MMIO, population, cache operation or release is granted. */
enum sgx535_task_status
sgx535_normalized_color0_mask_list_build(const struct sgx535_normalized_color0_mask_list_request *,
                                         struct sgx535_normalized_color0_mask_list_task *,
                                         struct sgx535_normalized_color0_mask_list_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_validate(
    const struct sgx535_normalized_color0_mask_list_request *,
    const struct sgx535_normalized_color0_mask_list_task *,
    struct sgx535_normalized_color0_mask_list_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_launch_build(
    const struct sgx535_normalized_color0_mask_list_request *,
    const struct sgx535_normalized_color0_mask_list_task *,
    const struct sgx535_task_launch_binding *, struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_mask_list_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_launch_validate(
    const struct sgx535_normalized_color0_mask_list_request *,
    const struct sgx535_normalized_color0_mask_list_task *,
    const struct sgx535_task_launch_binding *, const struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_mask_list_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_background_build(
    const struct sgx535_normalized_color0_mask_list_background_request *,
    struct sgx535_normalized_color0_mask_list_background_image *,
    struct sgx535_normalized_color0_mask_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_background_validate(
    const struct sgx535_normalized_color0_mask_list_background_request *,
    const struct sgx535_normalized_color0_mask_list_background_image *,
    struct sgx535_normalized_color0_mask_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_background_bind(
    const struct sgx535_normalized_color0_mask_list_background_request *,
    const struct sgx535_normalized_color0_mask_list_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    struct sgx535_normalized_color0_background_bound *,
    struct sgx535_normalized_color0_mask_list_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_list_background_bound_validate(
    const struct sgx535_normalized_color0_mask_list_background_request *,
    const struct sgx535_normalized_color0_mask_list_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    const struct sgx535_normalized_color0_background_bound *,
    struct sgx535_normalized_color0_mask_list_background_workspace *);
#endif
