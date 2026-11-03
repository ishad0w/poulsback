// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_WINDOW_MESH_H
#define SGX535_WINDOW_MESH_H
#include "sgx535_normalized_color0_mask.h"
#include "sgx535_vertex_dma.h"
#include "sgx535_vertex_use.h"
#include "sgx535_pds_fragment.h"
#include "sgx535_texture2d.h"
#include "sgx535_fragment_generic_f32.h"

#define SGX535_WINDOW_MESH_VERSION 14u
/* Clip-space input, the next task identity (15 and 16 name the clip-viewport
 * mesh and PA0 tasks): the same request, but each vertex's four position words
 * are clip x, y, z, w, any finite values. The MTE applies the window's viewport,
 * clips (W may be zero or negative) and culls (cull). Only the single-sample GL
 * window constructors, so ordered frames, accept it; the mask and the clear
 * stay window-space draws. */
#define SGX535_WINDOW_MESH_CLIP_VERSION 17u
#define SGX535_WINDOW_MESH_PROFILE 13u
/* Separate CPU construction grammar; old entrypoints and every bind refuse. */
#define SGX535_WINDOW_MESH_RAW_MASK_PROFILE_V1 14u
#define SGX535_WINDOW_MESH_BACKGROUND_VERSION 5u
#define SGX535_WINDOW_MESH_MAX_VERTICES 4096u
#define SGX535_WINDOW_MESH_MAX_INDICES 12288u
/* These are existing independently bounded owner layouts, not scene caps. */
#define SGX535_WINDOW_MESH_PDS_BYTES 768u
#define SGX535_WINDOW_MESH_VERTEX_USE_BYTES 80u
/* Two bounded constant DMA sources plus the existing three-field USE entry. */
#define SGX535_WINDOW_MESH_RELOCS (SGX535_TASK_PACKET_RELOCS + 5u)
/* Retained placement floor for the legacy foreground/event reservation. */
#define SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES 1536u
/* Private construction scratch follows the existing compiler word bound.
 * Live sections and GPU owner extents use the checked emitted length. */
#define SGX535_WINDOW_MESH_FRAGMENT_STORAGE_BYTES                                                  \
    ((((SGX535_NIR_MAX_WORDS * 4u + 31u) & ~31u) + 32u) > SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES    \
         ? (((SGX535_NIR_MAX_WORDS * 4u + 31u) & ~31u) + 32u)                                      \
         : SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES)
#define SGX535_WINDOW_MESH_MISSING_PROVENANCE 1u
#define SGX535_WINDOW_MESH_MISSING_INTERPOLATION 2u
#define SGX535_WINDOW_MESH_MISSING_VIEWPORT 4u
#define SGX535_WINDOW_MESH_MISSING_PARAMETER_CAPACITY 8u
#define SGX535_WINDOW_MESH_MISSING_WIDE_EXECUTION 16u
#define SGX535_WINDOW_MESH_MISSING_ALL 31u
#define SGX535_WINDOW_MESH_BACKGROUND_MISSING_DIMENSION_EFFECTS 16u
#define SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL 31u

enum sgx535_window_mesh_order {
    SGX535_WINDOW_MESH_INDEXED = 1,
    /* Distinct deindexed producer: I == V, V is a multiple of three and every
     * supplied index equals its ordinal. ABI3's aggregate4096 is not ABI2's
     * separate4096/12288 indexed domain. Neither enum accepts clip positions. */
    SGX535_WINDOW_MESH_DEINDEXED = 2
};
/* Empty physical input interface; emitted geometry contains position only. */
#define SGX535_WINDOW_MESH_NO_INPUT 0u
/* The MTE's CULLMODE, the window-space winding it culls (in the target's
 * orientation, viewport y scale negative). Clip-space requests only: a
 * window-space request culls nothing and carries 0. */
enum sgx535_window_mesh_cull {
    SGX535_WINDOW_MESH_CULL_NONE = 0,
    SGX535_WINDOW_MESH_CULL_CW = 1,
    SGX535_WINDOW_MESH_CULL_CCW = 2
};
/* The position is window x, y, z and 1/W, or clip x, y, z, W for
 * SGX535_WINDOW_MESH_CLIP_VERSION. */
struct sgx535_window_mesh_vertex {
    uint32_t window_xyz_reciprocal_w[4], rgba[4];
};

#define SGX535_WINDOW_MESH_UV2_WORDS 6u
#define SGX535_WINDOW_MESH_UV2_STRIDE_BYTES (SGX535_WINDOW_MESH_UV2_WORDS * 4u)
/* Owned, copied V5 spans: clip XYZW+UV and a separate window XYZ/RHW record.
 * The logical GENERIC slot belongs to the retained program. This single-set
 * layout binds it explicitly to physical set 0; it never changes the program.
 * The caller serializes these spans for the complete call. */
struct sgx535_window_mesh_uv2_source {
    uint32_t width, height, vertex_count, logical_slot;
    const uint32_t *clip_varyings, *window_position;
};
struct sgx535_window_mesh_uv2_layout {
    uint32_t logical_slot, physical_set, vertex_count, geometry_bytes;
    uint32_t stride_bytes, output_dwords, outselects, texsize, texfloat;
    struct sgx535_vertex_dma_plan dma;
    struct sgx535_vertex_use_projection use;
};
/* CPU-only coordinate packing and coupled output-state construction. Every
 * finite UV is preserved, including coordinates outside 0..1: the selected
 * sampler's clamp rule is not a color-value bound. Outputs and complete input
 * spans must be disjoint. Refusal preserves output and geometry; success
 * initializes exactly vertex_count*24 bytes and leaves capacity guards intact.
 * The state-upload record/allocation, fragment body, image/cache/MMU ownership
 * and native publication are separate contracts. No profile or bind accepts
 * this layout as execution authority. */
enum sgx535_task_status sgx535_window_mesh_uv2_build(const struct sgx535_window_mesh_uv2_source *,
                                                     struct sgx535_window_mesh_uv2_layout *,
                                                     uint8_t *geometry, uint32_t capacity);
/* Pure comparison against the same immutable copied inputs and complete
 * canonical layout. No allocation, write, physical access or permission. */
enum sgx535_task_status
sgx535_window_mesh_uv2_validate(const struct sgx535_window_mesh_uv2_source *,
                                const struct sgx535_window_mesh_uv2_layout *,
                                const uint8_t *geometry, uint32_t bytes);
/* Copied source conventions. Values alone cannot authenticate Mesa provenance.
 * Only upper-left, zero half-pixel/bottom-edge controls and sample0 belong to
 * this candidate. Raw finite viewport words are retained without recomputing
 * positions, W, clipping or the viewport transform. */
struct sgx535_window_mesh_window {
    uint32_t origin, half_pixel_center, bottom_edge_rule, sample_mask;
    uint32_t min_samples, reserved;
    uint32_t viewport_scale_f32[3], viewport_translate_f32[3];
};
struct sgx535_window_mesh_request {
    uint32_t version, profile, core_id, revision, width, height;
    uint32_t order, interpolation;
    struct sgx535_window_mesh_window window;
    const struct sgx535_window_mesh_vertex *vertices;
    const uint32_t *indices;
    uint32_t vertex_count, index_count;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
    /* Exact logical static prefix for the no-input background composition.
     * Native placement reserves additional SA0; no fields reuse reserved words. */
    const uint32_t *uniforms;
    uint32_t uniform_word_count;
    uint32_t cull; /* enum sgx535_window_mesh_cull */
};
struct sgx535_window_mesh_storage {
    uint32_t foreground_vertex_bytes, mask_vertex_offset_bytes, vertex_bytes;
    uint32_t index_offset_bytes, foreground_index_offset_bytes;
    uint32_t foreground_index_bytes, index_bytes, bytes, allocation_bytes;
};
enum sgx535_window_mesh_source {
    SGX535_WINDOW_MESH_GEOMETRY = 0,
    SGX535_WINDOW_MESH_PDS,
    SGX535_WINDOW_MESH_VERTEX_USE,
    SGX535_WINDOW_MESH_FRAGMENT_USE,
    SGX535_WINDOW_MESH_SECONDARY_UPLOAD
};
struct sgx535_window_mesh_secondary_layout {
    uint32_t logical_uniform_words_used[4], logical_uniform_extent;
    uint32_t physical_sa_words, reserved_sa_words, uniform_sa_words;
    uint32_t literal_sa_words, sampler_state_sa_words;
    uint32_t secondary_resource_offset_bytes, secondary_program_bytes;
    uint32_t upload_resource_offset_bytes, upload_bytes, empty_use_offset_bytes;
    uint32_t dms_sa_budget_words;
};
struct sgx535_window_mesh_section {
    uint32_t kind, source, byte_offset, byte_count;
    uint32_t role, resource_byte_offset, alignment;
};
/* Geometry is external sized storage. Program/code/state images are separate
 * typed source domains; no old task_packet cast or serialized2KiB geometry
 * limit exists. Every source span is checked before a relocation is applied. */
struct sgx535_window_mesh_task {
    uint32_t version, profile, core_id, revision, width, height, stride_bytes;
    uint32_t order, interpolation, vertex_count, index_count;
    struct sgx535_window_mesh_window window;
    struct sgx535_window_mesh_storage geometry;
    struct sgx535_engine121_scene scene;
    uint32_t missing_packet_contracts, missing_mesh_contracts;
    uint32_t section_count, relocation_count, ta_stream_section, event_pds_section;
    struct sgx535_window_mesh_section sections[SGX535_TASK_PACKET_SECTIONS];
    struct sgx535_task_packet_reloc relocations[SGX535_WINDOW_MESH_RELOCS];
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    struct sgx535_fragment_normalized_candidate fragment;
    uint32_t fragment_code_bytes, fragment_temp_words, dms_temp_allocation_bytes;
    uint32_t event_use_offset_bytes, fragment_use_extent_bytes;
    uint8_t pds[SGX535_WINDOW_MESH_PDS_BYTES];
    uint8_t vertex_use[SGX535_WINDOW_MESH_VERTEX_USE_BYTES];
    uint8_t fragment_use[SGX535_WINDOW_MESH_FRAGMENT_STORAGE_BYTES];
    struct sgx535_window_mesh_secondary_layout secondary;
    uint8_t secondary_upload[96u + 128u * 4u];
};
struct sgx535_window_mesh_binding {
    uint64_t epoch;
    struct sgx535_task_launch_role roles[SGX535_TASK_IMAGE_ROLES];
    uint32_t bank_index[2], reserved[2];
};
struct sgx535_window_mesh_bound {
    struct sgx535_window_mesh_task image;
    struct sgx535_window_mesh_binding binding;
    uint32_t bank_base[2], resolved_relocations, missing_launch_contracts;
    uint32_t ta_stream_va_bytes, event_pds_va_bytes;
    struct sgx535_task_launch_stage_plan stages[SGX535_TASK_LAUNCH_STAGE_COUNT];
};
struct sgx535_window_mesh_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_window_mesh_request foreground;
};
struct sgx535_window_mesh_background_image {
    uint32_t version, missing_background_contracts;
    struct sgx535_window_mesh_task foreground;
    struct sgx535_background_image background;
};
struct sgx535_window_mesh_background_bound {
    struct sgx535_window_mesh_bound foreground;
    struct sgx535_background_bound background;
    uint32_t missing_background_contracts;
};
struct sgx535_window_mesh_workspace {
    /* Mutable private snapshots; callers keep this workspace off stack. */
    struct sgx535_window_mesh_binding bind_binding;
    struct sgx535_background_binding bind_background;
    struct sgx535_background_request bind_request;
    /* This source snapshot outlives the nested foreground regeneration. */
    struct sgx535_window_mesh_background_request background_request;
    /* Separate from the parent background request and binding snapshots. */
    struct sgx535_window_mesh_request foreground_request;
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_generic_f32_secondary_candidate secondary;
    uint32_t uniforms[SGX535_FRAGMENT_UNIFORM_WORDS];
    uint32_t uniform_word_count;
    struct sgx535_normalized_color0_workspace seed;
    struct sgx535_normalized_pa0_workspace pa0_seed;
    struct sgx535_normalized_pa0_task pa0_task;
    struct sgx535_window_mesh_background_image result;
    struct sgx535_window_mesh_background_bound bound;
};

/* Computes only count-derived geometry requirements, without touching input
 * pointers. The actual build validates/copies all vertices, indices and IR.
 * Geometry and scratch capacities are exact byte capacities; only the required
 * prefix is written. Outputs, workspace, scratch and all input spans must be
 * disjoint. Failure leaves every output byte unchanged. The compiler may
 * allocate/sleep; inputs must remain stable and are rechecked afterwards.
 *
 * Standard engine121 scene branch accepts1..2047 pixels per axis; this is not
 * qualification of partial tiles, window conventions, RHW interpolation or
 * parameter capacity. No target PRESERVE, depth, cull, scissor, blending or
 * texture state is silently added. One program/group, mask and BG per complete
 * ordered scene; no scene splitting or per-draw reset. All native admission,
 * population/cache/execution, numerical, retirement and reuse gates stay shut.
 * Old12/13 APIs do not accept this identity. */
/* Pointer/span/count and complete literal/MOV graph proof for the native
 * no-input slice. No allocation or execution; stable source bytes required.
 * BLOCKED means checked CPU construction only. The full compiler still runs
 * during task regeneration, before native publication. */
enum sgx535_task_status
sgx535_window_mesh_no_input_source(const struct sgx535_window_mesh_request *);
enum sgx535_task_status sgx535_window_mesh_storage_plan(uint32_t vertices, uint32_t indices,
                                                        struct sgx535_window_mesh_storage *);
enum sgx535_task_status sgx535_window_mesh_build(const struct sgx535_window_mesh_request *,
                                                 struct sgx535_window_mesh_task *,
                                                 uint8_t *geometry, uint32_t capacity,
                                                 struct sgx535_window_mesh_workspace *,
                                                 uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_validate(const struct sgx535_window_mesh_request *,
                                                    const struct sgx535_window_mesh_task *,
                                                    const uint8_t *geometry, uint32_t bytes,
                                                    struct sgx535_window_mesh_workspace *,
                                                    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_bind(const struct sgx535_window_mesh_request *,
                                                const struct sgx535_window_mesh_task *,
                                                const uint8_t *geometry, uint32_t bytes,
                                                const struct sgx535_window_mesh_binding *,
                                                struct sgx535_window_mesh_bound *,
                                                struct sgx535_window_mesh_workspace *,
                                                uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_bound_validate(
    const struct sgx535_window_mesh_request *, const struct sgx535_window_mesh_task *,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_window_mesh_binding *,
    const struct sgx535_window_mesh_bound *, struct sgx535_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status
sgx535_window_mesh_background_build(const struct sgx535_window_mesh_background_request *,
                                    struct sgx535_window_mesh_background_image *, uint8_t *geometry,
                                    uint32_t capacity, struct sgx535_window_mesh_workspace *,
                                    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_background_validate(
    const struct sgx535_window_mesh_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *, uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_background_bind(
    const struct sgx535_window_mesh_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    struct sgx535_window_mesh_background_bound *, struct sgx535_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);
enum sgx535_task_status sgx535_window_mesh_background_bound_validate(
    const struct sgx535_window_mesh_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    const struct sgx535_window_mesh_background_bound *, struct sgx535_window_mesh_workspace *,
    uint8_t *scratch, uint32_t scratch_capacity);

#endif
