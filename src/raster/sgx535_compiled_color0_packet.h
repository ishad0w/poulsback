// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_COMPILED_COLOR0_PACKET_H
#define SGX535_COMPILED_COLOR0_PACKET_H
#include "sgx535_varying_packet.h"
#include "sgx535_fragment.h"
#include "sgx535_window_vertex.h"

#define SGX535_COMPILED_COLOR0_PACKET_VERSION 6u
#define SGX535_COMPILED_COLOR0_PROFILE 3u
#define SGX535_COMPILED_COLOR0_MAX_WORDS 32u
#define SGX535_COMPILED_COLOR0_MAX_TEMPS 4u
#define SGX535_COMPILED_COLOR0_MISSING_ALL SGX535_VARYING_MISSING_ALL
struct sgx535_compiled_color0_request {
    uint32_t version, profile, core_id, revision;
    uint32_t clip_position[3][4], rgba[3][4];
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, interpolation, reserved[2];
};
struct sgx535_compiled_color0_workspace {
    struct sgx535_varying_workspace varying;
    struct sgx535_task_packet seed;
    struct sgx535_fragment_dri_color0_program program;
    struct sgx535_task_packet result;
};
/* Trusted stable copied symbolic IR and W1/COLOR0 geometry only. The constructor
 * creates its own fixed smooth COLOR0 linkage, compiles the complete IR and
 * checks PA4/SA0/TEMP<=4/code<=128B before regenerating every packet section.
 * It accepts no native instruction, GPU address or caller-cleared qualifier.
 * FLAT_REPLICATED first requires all three admitted RGBA vectors to compare
 * bit-identically, then uses this same compiler-local smooth binding. Source
 * interpolation provenance belongs to the trusted request owner. No native
 * provoking-vertex policy or flat instruction is inferred. Uniforms, depth,
 * textures, blending and general perspective remain refused.
 * The compiler may allocate/sleep; use only an unpublished sleeping prepare.
 * Large output/workspace belong off stack and must be disjoint from each other,
 * the request and the entire stable fragment array. Errors preserve output;
 * workspace is scratch. A constructed packet remains BLOCKED, with all0x1ff
 * physical qualifiers retained. Existing packet2/3/5 behavior is unchanged. */
enum sgx535_task_status
sgx535_compiled_color0_packet_build(const struct sgx535_compiled_color0_request *,
                                    struct sgx535_task_packet *,
                                    struct sgx535_compiled_color0_workspace *);
enum sgx535_task_status
sgx535_compiled_color0_packet_validate(const struct sgx535_compiled_color0_request *,
                                       const struct sgx535_task_packet *,
                                       struct sgx535_compiled_color0_workspace *);
/* Distinct trusted packet7 input. Window words are already clipped/projected
 * CPU output; no homogeneous field is reinterpreted. Same complete code,
 * section/relocation construction and all0x1ff physical qualifiers. No public
 * transport or native source path selects this candidate. Lifetime/overlap and
 * transactional output rules are the same as the compiled COLOR0 constructor. */
struct sgx535_window_color0_request {
    uint32_t version, profile, core_id, revision;
    struct sgx535_window_color0_triangle vertices;
    const struct sgx535_fragment_instruction *fragment;
    uint32_t fragment_count, reserved[3];
};
enum sgx535_task_status
sgx535_window_color0_packet_build(const struct sgx535_window_color0_request *,
                                  struct sgx535_task_packet *,
                                  struct sgx535_compiled_color0_workspace *);
enum sgx535_task_status
sgx535_window_color0_packet_validate(const struct sgx535_window_color0_request *,
                                     const struct sgx535_task_packet *,
                                     struct sgx535_compiled_color0_workspace *);
#endif
