// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_BACKGROUND_OBJECT_H
#define POULSBACK_SGX535_BACKGROUND_OBJECT_H
#include "sgx535_task_image.h"
#include "sgx535_task_packet.h"
#include "sgx535_engine121.h"
#include "sgx535_use_code_bank.h"

/* Additive internal component. Existing packet/image role counts and bytes
 * stay unchanged. This is not UAPI, an owner certificate or device admission. */
#define SGX535_BACKGROUND_VERSION 1u
#define SGX535_BACKGROUND_BYTES 192u
#define SGX535_BACKGROUND_SECTIONS 4u
#define SGX535_BACKGROUND_RELOCS 5u
#define SGX535_BACKGROUND_ROLES 3u
#define SGX535_BACKGROUND_MISSING_OWNER_GRAPH 1u
#define SGX535_BACKGROUND_MISSING_POPULATION 2u
#define SGX535_BACKGROUND_UNQUALIFIED_DEVICE_EFFECTS 4u
#define SGX535_BACKGROUND_UNQUALIFIED_RETIREMENT 8u
#define SGX535_BACKGROUND_MISSING_ALL 15u

/* Internal construction choice, fixed before owner preparation. The selected
 * diagnostic changes the BG task's entry inside the existing fragment owner;
 * it neither copies a shader nor accepts a caller address or permission. */
enum sgx535_background_task_policy {
    SGX535_BACKGROUND_TASK_OWN_CONSTANT,
    SGX535_BACKGROUND_TASK_CANONICAL_FOREGROUND
};
#define SGX535_BACKGROUND_SELECTED_TASK_POLICY SGX535_BACKGROUND_TASK_OWN_CONSTANT


enum sgx535_background_role {
    SGX535_BACKGROUND_USE = 1,
    SGX535_BACKGROUND_PDS,
    SGX535_BACKGROUND_RASTGEOM
};
enum sgx535_background_section_kind {
    SGX535_BACKGROUND_USE_SECTION = 1,
    SGX535_BACKGROUND_PRIMARY_SECTION,
    SGX535_BACKGROUND_SECONDARY_SECTION,
    SGX535_BACKGROUND_OBJECT_SECTION
};
enum sgx535_background_reloc_kind {
    SGX535_BACKGROUND_USE_BANK = 1,
    SGX535_BACKGROUND_USE_ENTRY,
    SGX535_BACKGROUND_OFFSET
};
struct sgx535_background_request {
    uint32_t version, core_id, revision, width, height;
    uint32_t packed_argb, reserved[2];
};
struct sgx535_background_section {
    uint32_t kind, byte_offset, byte_count, role, resource_offset, alignment;
};
struct sgx535_background_reloc {
    uint32_t kind, section, section_offset, role, resource_offset;
    uint32_t span_bytes, alignment, mask, right_shift;
};
struct sgx535_background_image {
    uint32_t version, core_id, revision, width, height, packed_argb;
    uint32_t missing_contracts, byte_count, section_count, relocation_count;
    struct sgx535_background_section sections[SGX535_BACKGROUND_SECTIONS];
    struct sgx535_background_reloc relocations[SGX535_BACKGROUND_RELOCS];
    uint8_t bytes[SGX535_BACKGROUND_BYTES];
};
/* Coordinator supplies these from an actual canonical private job. Storage
 * IDs distinguish owned backing; this CPU codec cannot authenticate that
 * assertion or discover physical aliases. All three whole owners are 4KiB.
 * RASTGEOM is a distinct READ owner, not a DPM-table/parameter suballocation. */
struct sgx535_background_resource {
    uint64_t storage_id, gpu_va_bytes, allocation_bytes;
    uint32_t role, access, reserved[2];
};
struct sgx535_background_binding {
    uint64_t epoch, parameter_storage_id;
    uint32_t fragment_bank_index, pds_exec_base, bif_3d_req_base;
    uint32_t parameter_va, parameter_bytes, reserved;
    struct sgx535_background_resource resources[SGX535_BACKGROUND_ROLES];
};
struct sgx535_background_bound {
    struct sgx535_background_image image;
    struct sgx535_background_binding binding;
    struct sgx535_use_code_bank_projection bank;
    uint32_t bgobjtag_word, bgobj_word, bgdepth_word, resolved_relocations;
};

/* Square16 and64 construction only; arbitrary packed color, no CPU float,
 * texture, uniform, interpolation or fragment input. Depth is literal+1.
 * The fixed placements leave constant packet8 code/data prefixes intact:
 * USE+64, PDS+768/+832, new RASTGEOM+0. The compositor must also check against
 * its actual complete base scene. A valid result always returns BLOCKED and
 * preserves every qualification bit. Errors preserve output; scratch may
 * change. All request/image/binding/output/workspace ranges must be disjoint.
 */
enum sgx535_task_status sgx535_background_build(const struct sgx535_background_request *,
                                                struct sgx535_background_image *,
                                                struct sgx535_background_image *workspace);
enum sgx535_task_status sgx535_background_validate(const struct sgx535_background_request *,
                                                   const struct sgx535_background_image *,
                                                   struct sgx535_background_image *workspace);
enum sgx535_task_status sgx535_background_bind(const struct sgx535_background_request *,
                                               const struct sgx535_background_image *,
                                               const struct sgx535_background_binding *,
                                               struct sgx535_background_bound *,
                                               struct sgx535_background_bound *workspace);
enum sgx535_task_status sgx535_background_bound_validate(const struct sgx535_background_request *,
                                                         const struct sgx535_background_image *,
                                                         const struct sgx535_background_binding *,
                                                         const struct sgx535_background_bound *,
                                                         struct sgx535_background_bound *workspace);

/* One scene package: unchanged complete six-vertex packet8 plus its complete
 * constant background. No extra triangle job is created. Explicit heap
 * workspace is required; neither large packet is allocated on a kernel stack.
 * Base packet qualification and BG qualification remain separate and set.
 */
struct sgx535_background_pair_image {
    struct sgx535_task_packet triangles;
    struct sgx535_background_image background;
};
struct sgx535_background_pair_workspace {
    struct sgx535_background_pair_image result;
    struct sgx535_task_packet packet_scratch;
    struct sgx535_background_image background_scratch;
};
enum sgx535_task_status sgx535_background_pair_build(const struct sgx535_task_pair_request *,
                                                     const struct sgx535_background_request *,
                                                     struct sgx535_background_pair_image *,
                                                     struct sgx535_background_pair_workspace *);
enum sgx535_task_status sgx535_background_pair_validate(const struct sgx535_task_pair_request *,
                                                        const struct sgx535_background_request *,
                                                        const struct sgx535_background_pair_image *,
                                                        struct sgx535_background_pair_workspace *);

#endif
