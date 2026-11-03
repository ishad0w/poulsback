// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_TASK_H
#define SGX535_NORMALIZED_COLOR0_TASK_H
#include "sgx535_task_launch.h"
#include "sgx535_fragment_normalized.h"
#include "sgx535_task_packet_internal.h"

/* Separate internal window/COLOR0 candidate. No public/native selector. */
#define SGX535_NORMALIZED_COLOR0_TASK_VERSION 9u
#define SGX535_NORMALIZED_COLOR0_TASK_PROFILE 6u
/* Separate pure fixed16 clip-W1 candidate, without a native selector. */
#define SGX535_NORMALIZED_COLOR0_TILE16_VERSION 10u
#define SGX535_NORMALIZED_COLOR0_TILE16_PROFILE 8u
struct sgx535_normalized_color0_request {
    uint32_t version, profile, core_id, revision;
    struct sgx535_window_color0_triangle vertices;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
struct sgx535_normalized_color0_tile16_request {
    uint32_t version, profile, core_id, revision;
    uint32_t clip_position[3][4], rgba[3][4];
    uint32_t interpolation;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
struct sgx535_normalized_color0_task {
    struct sgx535_task_packet packet;
    struct sgx535_fragment_normalized_candidate fragment;
    uint32_t fragment_code_bytes, fragment_temp_words;
    uint32_t dms_temp_allocation_bytes;
    uint32_t event_use_offset_bytes, event_use_bytes, fragment_use_extent_bytes;
};
struct sgx535_normalized_color0_workspace {
    struct sgx535_varying_workspace varying;
    struct sgx535_task_packet seed;
    struct sgx535_normalized_color0_task result;
    struct sgx535_task_packet_word_workspace packet_words;
};
struct sgx535_normalized_color0_launch {
    struct sgx535_task_launch launch;
    uint32_t fragment_missing_contracts, reference_policy;
    uint32_t arithmetic_qualified, numerical_qualified, linkage_qualified;
    uint32_t output_qualified, hardware_qualified, nonfinite_behavior_qualified;
};
struct sgx535_normalized_color0_launch_workspace {
    struct sgx535_normalized_color0_workspace packet;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_launch result;
};

/* Complete packet construction from stable symbolic IR and validated window
 * vertices. Uses canonical smooth COLOR0 PA4, empty SECONDARY and the separate
 * normalized compiler candidate. Streams without a COLOR0 INPUT and all
 * nonempty SECONDARY usage remain outside this one-iterator profile. Actual code bytes and TEMP words determine
 * fragment PDS/DMS fields. The two matched event USE tasks move together to
 * the next 32-byte boundary after fragment code; every reference is relocated.
 * The complete packet must still fit SGX535_TASK_PACKET_BYTES. No existing
 * packet6/7 limit or byte changes, CPU fragment evaluation or native admission.
 *
 * The compiler may allocate/sleep. All inputs must remain stable. Output and off-stack workspace must be
 * disjoint from one another, the request and entire IR; errors preserve output. All packet
 * 0x1ff and normalized 0x3ff unknowns remain, and every qualification stays0.
 * In particular this is no claim about finite/NaN arithmetic, clamp, rounding,
 * masked writes, interpolation, PBE delivery, visibility or retirement. */
enum sgx535_task_status
sgx535_normalized_color0_task_build(const struct sgx535_normalized_color0_request *,
                                    struct sgx535_normalized_color0_task *,
                                    struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status
sgx535_normalized_color0_task_validate(const struct sgx535_normalized_color0_request *,
                                       const struct sgx535_normalized_color0_task *,
                                       struct sgx535_normalized_color0_workspace *);
/* Whole typed task regeneration precedes canonical role/bank projection. No
 * unchecked packet projector or old three-vertex/word-projection view accepts
 * this result. The same sleeping/stability/disjointness rules apply, including
 * binding and complete task. Packet0x1ff/launch0x75/normalized0x3ff persist. */
enum sgx535_task_status sgx535_normalized_color0_launch_build(
    const struct sgx535_normalized_color0_request *, const struct sgx535_normalized_color0_task *,
    const struct sgx535_task_launch_binding *, struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_launch_validate(
    const struct sgx535_normalized_color0_request *, const struct sgx535_normalized_color0_task *,
    const struct sgx535_task_launch_binding *, const struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_launch_workspace *);

/* One dynamic triangle at16x16 with128-byte pitch. XYZ are finite clip F32
 * in[-1,1], W exactly+1; the existing integer clip conversion performs one
 * nearest-even rounding. RGBA inputs are finite[0,1] without negative zero.
 * SMOOTH uses the source-derived one-iterator linkage. FLAT_REPLICATED requires
 * all three RGBA tuples identical; it does not encode an unproved flat flag.
 * All request/IR stability, non-aliasing, allocation, whole regeneration and
 * unknown/qualification rules above also apply. No background, mask pass,
 * native provider, kernel mapping or physical publication is added. In
 * particular the fragment's variable code extent cannot borrow BG+64. */
enum sgx535_task_status
sgx535_normalized_color0_tile16_task_build(const struct sgx535_normalized_color0_tile16_request *,
                                           struct sgx535_normalized_color0_task *,
                                           struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status sgx535_normalized_color0_tile16_task_validate(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status sgx535_normalized_color0_tile16_launch_build(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, const struct sgx535_task_launch_binding *,
    struct sgx535_normalized_color0_launch *, struct sgx535_normalized_color0_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_tile16_launch_validate(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, const struct sgx535_task_launch_binding *,
    const struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_launch_workspace *);

/* Explicit internal CPU candidate. The public COLOR0 and native frontdoors
 * never select this policy. Logical uniform i reads physical SECONDARY i+1;
 * SA0 is reserved. There is one combined128-DWORD footprint. Literal LIMM
 * constants use no SA; samplers, state uploads and memory constants refuse. */
#define SGX535_NORMALIZED_PA0_TASK_VERSION 16u
#define SGX535_NORMALIZED_PA0_TASK_PROFILE 15u
enum sgx535_normalized_program_policy { SGX535_NORMALIZED_PA0_RESERVED_SA0_V1 = 1 };
struct sgx535_normalized_pa0_request {
    uint32_t policy, core_id, revision;
    uint32_t clip_position[3][4];
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count;
    const uint32_t *uniforms;
    uint32_t uniform_word_count, constant_flags, reserved[2];
};
struct sgx535_normalized_pa0_task {
    struct sgx535_normalized_color0_task task;
    uint32_t logical_uniform_words_used[4], logical_uniform_extent;
    uint32_t physical_sa_words, reserved_sa_words, uniform_sa_words;
    uint32_t literal_sa_words, sampler_state_sa_words;
    uint32_t secondary_resource_offset_bytes, secondary_data_words;
    uint32_t secondary_code_offset_bytes, secondary_code_bytes;
    uint32_t secondary_program_bytes, secondary_capacity_bytes;
    uint32_t upload_resource_offset_bytes, upload_bytes;
    uint32_t dummy_use_offset_bytes, dummy_use_bytes, dms_sa_budget_words, dms_word;
};
struct sgx535_normalized_pa0_workspace {
    struct sgx535_normalized_color0_workspace common;
    struct sgx535_fragment_normalized_candidate logical;
    struct sgx535_fragment_instruction instructions[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t uniforms[SGX535_FRAGMENT_UNIFORM_WORDS];
    struct sgx535_normalized_pa0_request frozen;
    struct sgx535_normalized_pa0_task result;
};
struct sgx535_normalized_pa0_launch_workspace {
    struct sgx535_normalized_pa0_workspace packet;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_launch result;
};
/* Heap/preallocated workspace, stable fully allocated inputs, no aliases with
 * output/workspace. The original stream is checked before cloning/reindexing.
 * All source bytes are rechecked after sleeping compilation. Complete packet,
 * resource-page, relocation and qualification bounds are unchanged. */
enum sgx535_task_status
sgx535_normalized_pa0_task_build(const struct sgx535_normalized_pa0_request *,
                                 struct sgx535_normalized_pa0_task *,
                                 struct sgx535_normalized_pa0_workspace *);
enum sgx535_task_status
sgx535_normalized_pa0_task_validate(const struct sgx535_normalized_pa0_request *,
                                    const struct sgx535_normalized_pa0_task *,
                                    struct sgx535_normalized_pa0_workspace *);
enum sgx535_task_status sgx535_normalized_pa0_launch_build(
    const struct sgx535_normalized_pa0_request *, const struct sgx535_normalized_pa0_task *,
    const struct sgx535_task_launch_binding *, struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_pa0_launch_workspace *);
enum sgx535_task_status sgx535_normalized_pa0_launch_validate(
    const struct sgx535_normalized_pa0_request *, const struct sgx535_normalized_pa0_task *,
    const struct sgx535_task_launch_binding *, const struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_pa0_launch_workspace *);
#endif
