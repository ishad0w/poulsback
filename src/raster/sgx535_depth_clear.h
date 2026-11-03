// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_CLEAR_H
#define SGX535_DEPTH_CLEAR_H
#include "sgx535_depth_state.h"
#include "sgx535_task_packet.h"
#define SGX535_DEPTH_CLEAR_VERSION 1u
#define SGX535_DEPTH_CLEAR_SECTIONS 11u
#define SGX535_DEPTH_CLEAR_RELOCS 16u
#define SGX535_DEPTH_CLEAR_BYTES 512u
/* A complete source-derived geometry/state/program prefix, not a whole job.
 * Append it before the ordinary draw, restore the ordinary MTE state, then use
 * the independently checked event/termination suffix. The consumer must bind
 * equal logical color/depth dimensions, as the matched clear producer checks.
 * No end-of-stream word,
 * kick, event task, implicit depth owner or caller-supplied address is present.
 * Placements are disjoint from the existing v3 packet within the same owners.
 * The separate typed depth preparation is required for the future packet4;
 * neither this object nor its missing masks authorize GPU execution. */
struct sgx535_depth_clear_packet {
    uint32_t version, byte_count, section_count, relocation_count;
    uint32_t missing_packet_contracts, ta_prefix_section, reserved[2];
    struct sgx535_depth_preparation depth;
    struct sgx535_task_packet_section sections[SGX535_DEPTH_CLEAR_SECTIONS];
    struct sgx535_task_packet_reloc relocations[SGX535_DEPTH_CLEAR_RELOCS];
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    uint8_t bytes[SGX535_DEPTH_CLEAR_BYTES];
};
struct sgx535_depth_clear_workspace {
    struct sgx535_depth_clear_packet result;
};
/* Fixed depth-clear value +1, no color writes, four W1 window vertices and
 * six indices. No stencil/API-conformance claim follows from the opaque ISP
 * word3; its effects, layout and full GPU write extents remain unqualified.
 * preserve_after must be1; content is still not an initialization certificate.
 * Errors preserve output. All input/output/workspace overlaps are refused. */
enum sgx535_depth_status sgx535_depth_clear_build(const struct sgx535_depth_request *,
                                                  struct sgx535_depth_clear_packet *,
                                                  struct sgx535_depth_clear_workspace *);
enum sgx535_depth_status sgx535_depth_clear_validate(const struct sgx535_depth_request *,
                                                     const struct sgx535_depth_clear_packet *,
                                                     struct sgx535_depth_clear_workspace *);
#endif
