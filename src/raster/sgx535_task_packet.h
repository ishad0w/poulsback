// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TASK_PACKET_H
#define SGX535_TASK_PACKET_H
#include "sgx535_task_image.h"

/* Additive internal v2 construction. The v1 task-image contract is unchanged.
 * No member is an admission token or a caller-selected native control word. */
#define SGX535_TASK_PACKET_VERSION 2u
/* Additive matched DRI event producer. Existing v2 bytes/semantics persist. */
#define SGX535_TASK_PACKET_DRI_VERSION 3u
/* Distinct typed six-vertex triangle-list candidate; old requests stay 3-vertex. */
#define SGX535_TASK_PAIR_PACKET_VERSION 8u
#define SGX535_TASK_PAIR_W1_PACKED_CONSTANT 5u
/* Distinct fixed16 first-scene candidate: source-defined fullscreen ISP-mask
 * set followed by the same two ordinary triangles. No admission is granted. */
#define SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT 7u
/* Internal second immutable generation. It has fixed disjoint placements
 * within the same private owners; ordinary profile5/7 frontdoors reject it. */
#define SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT 9u
#define SGX535_TASK_PAIR_SECOND_PRIVATE_OFFSET 1024u
#define SGX535_TASK_PACKET_BYTES 2048u
#define SGX535_TASK_PACKET_SECTIONS 16u
#define SGX535_TASK_PACKET_RELOCS 40u
#define SGX535_PACKET_MISSING_BANK_OWNERSHIP (1u << 0)
#define SGX535_PACKET_MISSING_PDS_ADDRESS_DOMAIN (1u << 1)
#define SGX535_PACKET_MISSING_VERTEX_EFFECTS (1u << 2)
#define SGX535_PACKET_MISSING_POSITION_CONVENTION (1u << 3)
#define SGX535_PACKET_MISSING_MTE_PROFILE (1u << 4)
#define SGX535_PACKET_MISSING_PBE_PROFILE (1u << 5)
#define SGX535_PACKET_MISSING_PDS_READSET (1u << 6)
#define SGX535_PACKET_MISSING_ALL 0x7fu

enum sgx535_task_packet_kind {
    SGX535_PACKET_VERTICES = 1,
    SGX535_PACKET_INDICES,
    SGX535_PACKET_VERTEX_USE,
    SGX535_PACKET_FRAGMENT_USE,
    SGX535_PACKET_STATE_USE,
    SGX535_PACKET_EVENT_USE,
    SGX535_PACKET_VERTEX_PDS,
    SGX535_PACKET_FRAGMENT_PDS,
    SGX535_PACKET_STATE_PDS,
    SGX535_PACKET_EVENT_PDS,
    SGX535_PACKET_MTE_STATE,
    SGX535_PACKET_PBE_STATE,
    SGX535_PACKET_TA_STREAM,
    SGX535_PACKET_SECONDARY_PDS,
    SGX535_PACKET_TERMINATE_PDS,
    SGX535_PACKET_TERMINATE_STATE
};
enum sgx535_task_packet_reloc_kind {
    /* Exact selected legacy OFFSET producer, not PDS_OFFSET. The consumer's
     * address-domain qualification is independent of projecting these bits. */
    SGX535_PACKET_DRI_OFFSET = 1,
    SGX535_PACKET_USE_BANK,
    SGX535_PACKET_USE_ENTRY
};
struct sgx535_task_packet_section {
    uint32_t kind, byte_offset, byte_count;
    uint32_t role, resource_byte_offset, alignment;
};
struct sgx535_task_packet_reloc {
    uint32_t kind, section, section_byte_offset;
    uint32_t role, resource_byte_offset, span_bytes, alignment;
    uint32_t mask, right_shift, left_shift;
};
struct sgx535_task_packet {
    uint32_t version, profile, core_id, revision;
    uint32_t width, height, stride_bytes, packed_argb;
    uint32_t missing_contracts, byte_count, section_count, relocation_count;
    struct sgx535_task_packet_section sections[SGX535_TASK_PACKET_SECTIONS];
    struct sgx535_task_packet_reloc relocations[SGX535_TASK_PACKET_RELOCS];
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    uint32_t ta_stream_section, event_pds_section, reserved[2];
    uint8_t bytes[SGX535_TASK_PACKET_BYTES];
};

struct sgx535_task_pair_request {
    uint32_t version, profile, core_id, revision;
    uint32_t clip_position[2][3][4];   /* Two ordered triangles, W exactly +1. */
    uint32_t packed_argb, reserved[3]; /* One arbitrary constant for the job. */
    /* Internal target extent. Zero/zero retains the original 64x64 packet;
     * explicit 16x16 and 64x64 are supported. Pitch is derived, not supplied. */
    uint32_t width, height;
};

/* Request.version selects 2 (retained event) or 3 (matched DRI event). Addresses/banks are absent from the request and
 * remain resource-relative relocation records. The explicit workspace avoids
 * a multi-kilobyte automatic object on an i386 kernel stack; it must be outside
 * both request and output. Workspace bytes may change on refusal. Errors
 * preserve output, and stable request/output overlap is permitted. A valid construction is
 * BLOCKED until the explicitly named remaining contracts are qualified. */
enum sgx535_task_status sgx535_task_packet_build(const struct sgx535_task_request *,
                                                 struct sgx535_task_packet *,
                                                 struct sgx535_task_packet *workspace);
enum sgx535_task_status sgx535_task_packet_validate(const struct sgx535_task_request *,
                                                    const struct sgx535_task_packet *,
                                                    struct sgx535_task_packet *workspace);

/* One complete 16x16 or 64x64 packet: six independent copied positions, U16 indices
 * 0..5, one state/program set, one index-list record and one terminated stream.
 * No coordinate/color recognition, CPU fragment evaluation or second job.
 * All physical missing bits remain set, including sample/edge convention.
 * Output/workspace must be disjoint from each other and the complete stable
 * request. Errors preserve output; workspace is scratch. This version is not
 * accepted by the old three-vertex constructors or any public transport. */
enum sgx535_task_status sgx535_task_pair_packet_build(const struct sgx535_task_pair_request *,
                                                      struct sgx535_task_packet *,
                                                      struct sgx535_task_packet *workspace);
enum sgx535_task_status sgx535_task_pair_packet_validate(const struct sgx535_task_pair_request *,
                                                         const struct sgx535_task_packet *,
                                                         struct sgx535_task_packet *workspace);
/* Explicit typed profile7 entry points. The pair facade dispatches5 or7;
 * three-vertex versions and profile5 bytes retain their prior contracts.
 * Profile7 requires explicit16x16, retains16 sections and all missing bits,
 * and owns no additional resource or caller-selected control/address. */
enum sgx535_task_status
sgx535_task_pair_mask_set_packet_build(const struct sgx535_task_pair_request *,
                                       struct sgx535_task_packet *,
                                       struct sgx535_task_packet *workspace);
enum sgx535_task_status
sgx535_task_pair_mask_set_packet_validate(const struct sgx535_task_pair_request *,
                                          const struct sgx535_task_packet *,
                                          struct sgx535_task_packet *workspace);
#endif
