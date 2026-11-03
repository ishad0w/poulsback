// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_CLIP_VIEWPORT_MESH_H
#define SGX535_CLIP_VIEWPORT_MESH_H
#include "sgx535_window_mesh.h"

#define SGX535_CLIP_VIEWPORT_MESH_VERSION 15u
#define SGX535_CLIP_VIEWPORT_MESH_PROFILE 14u
#define SGX535_CLIP_VIEWPORT_MESH_BACKGROUND_VERSION 7u
#define SGX535_CLIP_VIEWPORT_MESH_PDS_BYTES 1024u
#define SGX535_CLIP_VIEWPORT_MESH_VERTICES 6u

/* Separate unpublished diagnostic, not a WINDOW14 request or normal renderer.
 * Six copied CLIP/W1 records and their actual WINDOW sidecar must agree with
 * the retained64x64 negative-Y viewport. XYZ are exact multiples of1/128 in
 * [-1,1], making this selected viewport arithmetic exact in binary32 before
 * the final result; the sidecar is never uploaded in place of CLIP.
 * Source flags1/1, min1 and all sample-mask bits remain unchanged. An explicit
 * six-word MTE viewport is emitted before MTE state, whose PRETRANSFORM bit is
 * clear only for foreground. The fullscreen mask keeps its complete separate
 * pretransformed state. BG has fixed PDS1024/1088 and USE1536 placement.
 *
 * Every image and binding is regenerated. Inputs/header/IR/sidecar must remain
 * stable through compiler allocations and are rechecked afterwards; output,
 * workspace, scratch and all source spans must be disjoint. Failed calls leave
 * output/geometry unchanged. Scratch/workspace are unpublished and may change.
 * No raw addresses, physical ownership, cache, execution, edge-rule, visibility,
 * retirement, reuse or API qualification is granted by SGX535_TASK_BLOCKED.
 * This profile has no depth owner and always disables foreground depth test
 * and writes. Its emitted ST Z pair1/2,1/2 is not identified with a PSB1/1
 * normalization or a qualified hardware Z transform; no depth use may inherit
 * this color-only candidate's CPU sidecar comparison.
 */
struct sgx535_clip_viewport_mesh_vertex {
    uint32_t clip_xyzw[4], rgba[4];
};
struct sgx535_clip_viewport_mesh_request {
    uint32_t version, profile, core_id, revision, width, height;
    uint32_t interpolation;
    struct sgx535_window_mesh_window conventions;
    const struct sgx535_clip_viewport_mesh_vertex *vertices;
    const struct sgx535_window_mesh_vertex *window_sidecar;
    const uint32_t *indices;
    uint32_t vertex_count, index_count;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
/* Reuses only the existing low-level section/relocation/role representations.
 * It is not layout-compatible with task14 and has no conversion/cast API. */
struct sgx535_clip_viewport_mesh_task {
    uint32_t version, profile, core_id, revision, width, height, stride_bytes;
    uint32_t interpolation, vertex_count, index_count;
    struct sgx535_window_mesh_window conventions;
    uint32_t viewport_words[6]; /* Tx,Sx,Ty,Sy,Tz,Sz, binary32 source words */
    struct sgx535_window_mesh_storage geometry;
    struct sgx535_engine121_scene scene;
    uint32_t missing_packet_contracts, missing_mesh_contracts;
    uint32_t section_count, relocation_count, ta_stream_section, event_pds_section;
    struct sgx535_window_mesh_section sections[SGX535_TASK_PACKET_SECTIONS];
    struct sgx535_task_packet_reloc relocations[SGX535_TASK_PACKET_RELOCS];
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    struct sgx535_fragment_normalized_candidate fragment;
    uint32_t fragment_code_bytes, fragment_temp_words, dms_temp_allocation_bytes;
    uint32_t event_use_offset_bytes, fragment_use_extent_bytes;
    uint8_t pds[SGX535_CLIP_VIEWPORT_MESH_PDS_BYTES];
    uint8_t vertex_use[SGX535_WINDOW_MESH_VERTEX_USE_BYTES];
    uint8_t fragment_use[SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES];
};
struct sgx535_clip_viewport_mesh_bound {
    struct sgx535_clip_viewport_mesh_task image;
    struct sgx535_window_mesh_binding binding;
    uint32_t bank_base[2], resolved_relocations, missing_launch_contracts;
    uint32_t ta_stream_va_bytes, event_pds_va_bytes;
    struct sgx535_task_launch_stage_plan stages[SGX535_TASK_LAUNCH_STAGE_COUNT];
};
struct sgx535_clip_viewport_mesh_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_clip_viewport_mesh_request foreground;
};
struct sgx535_clip_viewport_mesh_background_image {
    uint32_t version, missing_background_contracts;
    struct sgx535_clip_viewport_mesh_task foreground;
    struct sgx535_background_image background;
};
struct sgx535_clip_viewport_mesh_background_bound {
    struct sgx535_clip_viewport_mesh_bound foreground;
    struct sgx535_background_bound background;
    uint32_t missing_background_contracts;
};
struct sgx535_clip_viewport_mesh_workspace {
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_normalized_color0_workspace seed;
    struct sgx535_clip_viewport_mesh_background_image result;
    struct sgx535_clip_viewport_mesh_background_bound bound;
};
enum sgx535_task_status
sgx535_clip_viewport_mesh_build(const struct sgx535_clip_viewport_mesh_request *,
                                struct sgx535_clip_viewport_mesh_task *, uint8_t *, uint32_t,
                                struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_validate(
    const struct sgx535_clip_viewport_mesh_request *, const struct sgx535_clip_viewport_mesh_task *,
    const uint8_t *, uint32_t, struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status
sgx535_clip_viewport_mesh_bind(const struct sgx535_clip_viewport_mesh_request *,
                               const struct sgx535_clip_viewport_mesh_task *, const uint8_t *,
                               uint32_t, const struct sgx535_window_mesh_binding *,
                               struct sgx535_clip_viewport_mesh_bound *,
                               struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_bound_validate(
    const struct sgx535_clip_viewport_mesh_request *, const struct sgx535_clip_viewport_mesh_task *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_clip_viewport_mesh_bound *, struct sgx535_clip_viewport_mesh_workspace *,
    uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_background_build(
    const struct sgx535_clip_viewport_mesh_background_request *,
    struct sgx535_clip_viewport_mesh_background_image *, uint8_t *, uint32_t,
    struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_background_validate(
    const struct sgx535_clip_viewport_mesh_background_request *,
    const struct sgx535_clip_viewport_mesh_background_image *, const uint8_t *, uint32_t,
    struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_background_bind(
    const struct sgx535_clip_viewport_mesh_background_request *,
    const struct sgx535_clip_viewport_mesh_background_image *, const uint8_t *, uint32_t,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    struct sgx535_clip_viewport_mesh_background_bound *,
    struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_clip_viewport_mesh_background_bound_validate(
    const struct sgx535_clip_viewport_mesh_background_request *,
    const struct sgx535_clip_viewport_mesh_background_image *, const uint8_t *, uint32_t,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    const struct sgx535_clip_viewport_mesh_background_bound *,
    struct sgx535_clip_viewport_mesh_workspace *, uint8_t *, uint32_t);
#endif
