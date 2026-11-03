// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_WINDOW_MESH_H
#define SGX535_DEPTH_WINDOW_MESH_H
#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "sgx535_window_mesh.h"
#include "sgx535_depth_clear.h"
#include "sgx535_single_sample.h"

/* A separate pure composition class, not a new selector for packet14 or BG5.
 * The old eight ordinary roles and separate BG companion remain unchanged. */
#define SGX535_DEPTH_WINDOW_MESH_VERSION 1u
#define SGX535_DEPTH_WINDOW_MESH_SECTIONS 20u
#define SGX535_DEPTH_WINDOW_MESH_RELOCS 52u
#define SGX535_DEPTH_WINDOW_MESH_PDS_BYTES 1184u
/* Legacy placement retained when the foreground/event fits its old floor. */
#define SGX535_DEPTH_WINDOW_MESH_CLEAR_USE_OFFSET 1568u
#define SGX535_DEPTH_WINDOW_MESH_DEPTH_VA UINT64_C(0x50000000)

struct sgx535_depth_window_mesh_request {
    /* no_depth 1: the frame has no depth attachment (single-sample ordered
     * frames only): every fragment passes and nothing is written. */
    uint32_t version, format, no_depth, reserved;
    struct sgx535_window_mesh_background_request scene;
};
enum sgx535_depth_window_mesh_source {
    SGX535_DEPTH_WINDOW_MESH_GEOMETRY = SGX535_WINDOW_MESH_GEOMETRY,
    SGX535_DEPTH_WINDOW_MESH_PDS = SGX535_WINDOW_MESH_PDS,
    SGX535_DEPTH_WINDOW_MESH_VERTEX_USE = SGX535_WINDOW_MESH_VERTEX_USE,
    SGX535_DEPTH_WINDOW_MESH_FRAGMENT_USE = SGX535_WINDOW_MESH_FRAGMENT_USE,
    SGX535_DEPTH_WINDOW_MESH_CLEAR_USE = 4,
    SGX535_DEPTH_WINDOW_MESH_SECONDARY_UPLOAD = 5
};
struct sgx535_depth_window_mesh_task {
    uint32_t version, section_count, relocation_count, ta_stream_section;
    uint32_t event_pds_section, missing_depth_contracts, reserved[2];
    /* Exact regenerated task14/BG5 source. It is not mutated or cast to a
     * depth-capable task. The composed sections below name the live payload. */
    struct sgx535_window_mesh_background_image seed;
    struct sgx535_depth_preparation depth;
    struct sgx535_window_mesh_section sections[SGX535_DEPTH_WINDOW_MESH_SECTIONS + 1u];
    struct sgx535_task_packet_reloc relocations[SGX535_DEPTH_WINDOW_MESH_RELOCS + 5u];
    uint8_t pds[SGX535_DEPTH_WINDOW_MESH_PDS_BYTES], clear_use[8];
};
struct sgx535_depth_window_mesh_owner {
    uint64_t storage_id, gpu_va_bytes, allocation_bytes;
    uint32_t access, reserved;
};
struct sgx535_depth_window_mesh_binding {
    struct sgx535_window_mesh_binding scene;
    struct sgx535_background_binding background;
    struct sgx535_depth_window_mesh_owner depth;
};
struct sgx535_depth_window_mesh_bound {
    uint32_t version, resolved_relocations, missing_launch_contracts, missing_depth_contracts;
    uint32_t hardware_qualified, layout_qualified, visibility_qualified, retirement_qualified;
    uint32_t bank_base[2], ta_stream_va_bytes, event_pds_va_bytes;
    struct sgx535_depth_window_mesh_binding binding;
    struct sgx535_depth_window_mesh_task image;
    struct sgx535_background_bound background;
    /* Mandatory SOURCE prerequisite, not executable actions in these stages. */
    struct sgx535_engine121_action required_isp_reset[2];
    uint32_t reset_actions, depth_base_words[SGX535_DEPTH_ADDRESS_PAIRS];
    struct sgx535_task_launch_stage_plan stages[SGX535_TASK_LAUNCH_STAGE_COUNT];
};
struct sgx535_depth_window_mesh_workspace {
    /* Private construction snapshots; this workspace must remain off stack. */
    struct sgx535_depth_window_mesh_request bind_request;
    /* Single-sample conversion returns before nested regeneration. Its source
     * must stay separate from both the bind and regeneration snapshots. */
    struct sgx535_depth_window_mesh_request single_source_request;
    /* Regeneration is nested below bind: it needs an independent snapshot. */
    struct sgx535_depth_window_mesh_request regenerate_request;
    struct sgx535_depth_window_mesh_binding bind_binding;
    struct sgx535_window_mesh_workspace mesh;
    struct sgx535_window_mesh_background_bound seed_bound;
    struct sgx535_depth_clear_packet clear;
    struct sgx535_depth_clear_workspace clear_work;
    struct sgx535_depth_window_mesh_task image;
    struct sgx535_depth_window_mesh_bound bound;
};

/* Whole fresh scene: own fullscreen mask -> depth clear+1 -> complete ordered
 * task14 WINDOW mesh with LESS/write -> unchanged termination/event. Mask and
 * clear reuse only their identical checked4-word fullscreen vertices/indices;
 * ordinary geometry retains eight words per vertex and its exact index order.
 * No per-draw reset, clip reconstruction, scene split or inherited HOLD state.
 *
 * D16/D24S8 dimensions derive from the scene; the depth owner is a separate
 * READ|WRITE extent, never RGB storage. The selected numeric projection requires
 * VA50000000 and the exact page-rounded required size. It does not establish
 * general source-mask/address-unit semantics or a native mapping class.
 *
 * Build and numeric binding support task14's construction domain. BG5's
 * dimension-effect missing mask remains set: a wide/rectangular pure projection
 * is not background execution qualification. ISP reset20->0, depth packing/write
 * envelope/drain, interpolation, wide scene and all native gates stay blocked.
 * No allocation, MMIO, physical map, cache effect or GPU execution is performed.
 *
 * All input, output, workspace and scratch spans must be disjoint, including
 * the complete outer request prefix. The compiler can allocate/sleep. Full
 * typed regeneration and final source/header/binding checks precede publishing
 * any output. Scratch needs twice the exact geometry byte count, for task14's
 * disjoint transactional source/result buffers. Failure preserves image, geometry and bound output bytes;
 * explicit scratch/workspace may change. Keep the workspace off kernel stack. */
enum sgx535_task_status
sgx535_depth_window_mesh_build(const struct sgx535_depth_window_mesh_request *,
                               struct sgx535_depth_window_mesh_task *, uint8_t *geometry,
                               uint32_t capacity, struct sgx535_depth_window_mesh_workspace *,
                               uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_depth_window_mesh_validate(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *geometry, uint32_t bytes, struct sgx535_depth_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_depth_window_mesh_bind(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *,
    struct sgx535_depth_window_mesh_bound *, struct sgx535_depth_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_depth_window_mesh_bound_validate(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_depth_window_mesh_binding *,
    const struct sgx535_depth_window_mesh_bound *, struct sgx535_depth_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status
sgx535_depth_window_mesh_native_admission(const struct sgx535_depth_window_mesh_bound *);

struct sgx535_depth_window_mesh_single_sample_request;
struct sgx535_depth_window_mesh_ordered;
struct sgx535_depth_window_mesh_ordered_bound;
struct sgx535_depth_window_mesh_ordered_group {
    uint32_t first_vertex, vertex_count, interpolation, profile;
    uint32_t fragment_section, vertex_pds_section, state_section, mte_section, ta_byte_offset;
    struct sgx535_window_mesh_window window;
    struct sgx535_single_sample_source samples;
    struct sgx535_single_sample_lowering lowering;
};
struct sgx535_depth_window_mesh_ordered_view {
    uint32_t group_count, vertex_count, section_count, relocation_count, byte_count;
    uint32_t ta_stream_section, event_pds_section, missing_packet_contracts;
    uint32_t missing_mesh_contracts, missing_depth_contracts;
    const struct sgx535_depth_window_mesh_ordered_group *groups;
    /* source0 names this object's single owned byte span. Resource placements
     * remain relative to the existing eight roles, never device addresses. */
    const struct sgx535_window_mesh_section *sections;
    const struct sgx535_task_packet_reloc *relocations;
    const uint8_t *bytes;
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    struct sgx535_engine121_scene scene;
    struct sgx535_depth_preparation depth;
    struct sgx535_background_image background;
};
struct sgx535_depth_window_mesh_ordered_bind_sizes {
    uint32_t workspace_bytes, source_metadata_bytes, bound_metadata_bytes;
};
struct sgx535_depth_window_mesh_ordered_bound_view {
    const struct sgx535_depth_window_mesh_ordered *source_owner;
    const struct sgx535_depth_window_mesh_ordered_view *source;
    const uint8_t *bytes;
    uint32_t byte_count, metadata_bytes, resolved_relocations;
    uint32_t missing_launch_contracts, missing_depth_contracts;
    uint32_t hardware_qualified, layout_qualified, visibility_qualified, retirement_qualified;
    uint32_t bank_base[2], ta_stream_va_bytes, event_pds_va_bytes;
    struct sgx535_depth_window_mesh_binding binding;
    struct sgx535_background_bound background;
    struct sgx535_engine121_action required_isp_reset[2];
    uint32_t reset_actions, depth_base_words[SGX535_DEPTH_ADDRESS_PAIRS];
    struct sgx535_task_launch_stage_plan stages[SGX535_TASK_LAUNCH_STAGE_COUNT];
};

/* Additive internal ordered composition. Each source is an ordinary checked
 * GL/W1/raw0 clear+store request, with its own program/interpolation/sample
 * record. Target, background and depth load/store must agree. The constructor
 * emits one mask/clear prefix, ordered foreground records, and one terminal
 * and event; it never splits a frame or synthesizes a homogeneous request.
 * Existing fixed layouts and native entrypoints do not accept this object.
 *
 * Scratch is caller-owned, count-derived and disjoint from all source/output
 * spans. All source headers and payloads are copied there before allocation
 * or compilation, then compared again before publishing the owned root.
 * Failure preserves *out. Build/validate return0 for CPU construction only;
 * no bound packet, lease, launch, numerical or hardware authority is granted.
 * Oversized geometry/code banks refuse instead of lifting existing budgets. */
int sgx535_depth_window_mesh_ordered_workspace(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t, uint32_t *);
int sgx535_depth_window_mesh_ordered_create(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    struct sgx535_depth_window_mesh_ordered **, void *, uint32_t);
const struct sgx535_depth_window_mesh_ordered_view *
sgx535_depth_window_mesh_ordered_view(const struct sgx535_depth_window_mesh_ordered *);
int sgx535_depth_window_mesh_ordered_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_ordered *, void *, uint32_t);
void sgx535_depth_window_mesh_ordered_destroy(struct sgx535_depth_window_mesh_ordered *);
/* Whether section I of a one-group ordered view belongs to its draw, i.e. a
 * later group of an ordered frame repeats it (vertices, indices, fragment
 * program, vertex/fragment/state PDS, MTE state, and with uniforms the static
 * secondary and event programs), rather than to the frame. The frame writer
 * (Mesa poulsback_frame.c) builds its draw templates from such views. */
bool sgx535_depth_window_mesh_ordered_draw_section(
    const struct sgx535_depth_window_mesh_ordered_view *, uint32_t i);

/* Distinct internal GENERIC0/F32 physical IO candidate. Logical COLOR0,
 * WINDOW xyz/RHW/RGBA and geometry stay unchanged. One shared emitter couples
 * OUTSELECTS/TEXSIZE/TEXFLOAT, DOUTI, PA mapping and the11-word PSB state upload.
 * Positive finite RHW is copied, never reconstructed. Same workspace, one
 * mask/clear and LOAD continuation. Old getters/validators/binders refuse this
 * IO class. No native binding API, admission, public caps, W/precision or depth
 * qualification. Construction and validation grant CPU ownership only. */
#define SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE (1u << 31)
struct sgx535_depth_window_mesh_f32_ordered;
int sgx535_depth_window_mesh_f32_ordered_create(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    struct sgx535_depth_window_mesh_f32_ordered **, void *, uint32_t);
int sgx535_depth_window_mesh_f32_ordered_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_f32_ordered *, void *, uint32_t);
const struct sgx535_depth_window_mesh_ordered_view *
sgx535_depth_window_mesh_f32_ordered_view(const struct sgx535_depth_window_mesh_f32_ordered *);
void sgx535_depth_window_mesh_f32_ordered_destroy(struct sgx535_depth_window_mesh_f32_ordered *);

/* Same canonical numeric binding and counted relocations, with an exact F32
 * source clone. The missing linkage marker remains in that owned clone. These
 * types have no kernel/native provider or hardware admission path. */
#define SGX535_DEPTH_WINDOW_MESH_F32_BIND_CPU_V1 1u
struct sgx535_depth_window_mesh_f32_ordered_bound;
int sgx535_depth_window_mesh_f32_ordered_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_f32_ordered *,
    const struct sgx535_depth_window_mesh_binding *,
    struct sgx535_depth_window_mesh_f32_ordered_bound **, void *, uint32_t);
int sgx535_depth_window_mesh_f32_ordered_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_f32_ordered *,
    const struct sgx535_depth_window_mesh_binding *,
    const struct sgx535_depth_window_mesh_f32_ordered_bound *, void *, uint32_t);
const struct sgx535_depth_window_mesh_ordered_bound_view *
sgx535_depth_window_mesh_f32_ordered_bound_view(
    const struct sgx535_depth_window_mesh_f32_ordered_bound *);
void sgx535_depth_window_mesh_f32_ordered_bound_destroy(
    struct sgx535_depth_window_mesh_f32_ordered_bound *);

/* Internal numeric binding only. The bound owner retains a freshly regenerated
 * source clone and its own relocated byte span. Sizes account for that clone;
 * the separate original source allocation is reported independently. Scratch
 * is temporary workspace, not another retained source allocation. Full owner
 * extents, every counted relocation and all source inputs are checked before
 * publishing *out. Failure preserves *out. No native admission is granted. */
int sgx535_depth_window_mesh_ordered_bind_sizes(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    struct sgx535_depth_window_mesh_ordered_bind_sizes *);
int sgx535_depth_window_mesh_ordered_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_ordered *,
    const struct sgx535_depth_window_mesh_binding *,
    struct sgx535_depth_window_mesh_ordered_bound **, void *, uint32_t);
const struct sgx535_depth_window_mesh_ordered_bound_view *
sgx535_depth_window_mesh_ordered_bound_view(const struct sgx535_depth_window_mesh_ordered_bound *);
int sgx535_depth_window_mesh_ordered_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *, uint32_t,
    const struct sgx535_depth_window_mesh_ordered *,
    const struct sgx535_depth_window_mesh_binding *,
    const struct sgx535_depth_window_mesh_ordered_bound *, void *, uint32_t);
void sgx535_depth_window_mesh_ordered_bound_destroy(
    struct sgx535_depth_window_mesh_ordered_bound *);
#endif
