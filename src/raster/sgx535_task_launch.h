// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TASK_LAUNCH_H
#define SGX535_TASK_LAUNCH_H
#include "sgx535_task_packet.h"
#include "sgx535_engine121.h"
#include "sgx535_compiled_color0_packet.h"

/* Internal trusted input, never UAPI. Numeric identities and bank indices must
 * come from one canonical source/epoch-bound kernel lease. The pure builder
 * checks their consistency; it cannot authenticate pages or reserve banks. */
#define SGX535_TASK_LAUNCH_VERSION 1u
#define SGX535_TASK_LAUNCH_ACTIONS 32u
#define SGX535_LAUNCH_MISSING_PDS_DOMAIN (1u << 0)
/* v3 fixes fresh-context depth/stencil clear defaults; v2 remains unresolved. */
#define SGX535_LAUNCH_MISSING_CALLER_SCALARS (1u << 1)
#define SGX535_LAUNCH_MISSING_BACKGROUND_PROFILE (1u << 2)
#define SGX535_LAUNCH_MISSING_EVENT_PROFILE (1u << 3)
#define SGX535_LAUNCH_MISSING_READSET (1u << 4)
#define SGX535_LAUNCH_MISSING_COLD_SOURCE (1u << 5)
#define SGX535_LAUNCH_MISSING_REGISTER_EFFECTS (1u << 6)
#define SGX535_LAUNCH_MISSING_ALL 0x7fu
#define SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT 0u
#define SGX535_PAIR_BACKGROUND_DISABLED_FIXED16 1u
struct sgx535_task_launch_role {
    uint64_t storage_id, gpu_va_bytes, allocation_bytes;
    uint32_t access, reserved;
};
struct sgx535_task_launch_binding {
    uint64_t epoch;
    struct sgx535_task_launch_role roles[SGX535_TASK_IMAGE_ROLES];
    uint32_t bank_index[2]; /* canonical normal-pool reservations, vertex/pixel */
    uint32_t reserved[2];
};
enum sgx535_task_launch_stage {
    SGX535_TASK_LAUNCH_CODE = 0,
    SGX535_TASK_LAUNCH_TA,
    SGX535_TASK_LAUNCH_RASTER,
    SGX535_TASK_LAUNCH_STAGE_COUNT
};
struct sgx535_task_launch_stage_plan {
    uint32_t count, reserved[3];
    struct sgx535_engine121_action actions[SGX535_TASK_LAUNCH_ACTIONS];
    /* Explicit unresolved value bits; never execute an action with any set. */
    uint32_t unknown_value_masks[SGX535_TASK_LAUNCH_ACTIONS];
};
/* Each range is a full canonical owner boundary. READ is conservative rather
 * than a claimed decoded PDS read set. No depth owner, texture or arbitrary
 * extra allocation belongs to the bounded no-depth/no-texture profile. */
struct sgx535_task_launch {
    uint32_t version, packet_version, core_id, revision;
    uint64_t epoch;
    uint32_t missing_packet_contracts, missing_launch_contracts;
    struct sgx535_task_launch_role roles[SGX535_TASK_IMAGE_ROLES];
    uint32_t bank_index[2], bank_base[2];
    uint32_t ta_stream_va_bytes, event_pds_va_bytes;
    uint32_t resolved_relocations, byte_count;
    struct sgx535_task_launch_stage_plan stages[SGX535_TASK_LAUNCH_STAGE_COUNT];
    uint8_t bytes[SGX535_TASK_PACKET_BYTES];
};
struct sgx535_task_launch_workspace {
    struct sgx535_task_packet regenerated;
    struct sgx535_task_launch result;
};
struct sgx535_compiled_color0_launch_workspace {
    struct sgx535_compiled_color0_workspace packet;
    struct sgx535_task_launch_workspace launch;
};

/* Regenerate the whole packet, validate canonical full extents and bindings,
 * then derive every relocation and fixed stage plan. No caller-cleared mask,
 * numeric bank index or action transcript grants physical permission. The
 * kernel consumer must reconstruct/compare this descriptor under its source
 * gate before executing any independently admitted stage. No MMIO, allocation,
 * upload, cache operation or kick occurs here. Errors preserve output. The
 * output and explicit off-stack workspace must be disjoint from every input
 * and from one another. */
enum sgx535_task_status sgx535_task_launch_build(const struct sgx535_task_request *,
                                                 const struct sgx535_task_packet *,
                                                 const struct sgx535_task_launch_binding *,
                                                 struct sgx535_task_launch *,
                                                 struct sgx535_task_launch_workspace *);
enum sgx535_task_status sgx535_task_launch_validate(const struct sgx535_task_request *,
                                                    const struct sgx535_task_packet *,
                                                    const struct sgx535_task_launch_binding *,
                                                    const struct sgx535_task_launch *,
                                                    struct sgx535_task_launch_workspace *);
/* Packet8 has a separate six-vertex request. Reconstructs the whole pair
 * packet before binding its one stream, one event suffix and eight owners.
 * Packet0x7f/launch0x75 missing contracts persist. No public consumer accepts
 * this profile, and no construction grants upload or execution permission.
 * The same complete-input/output/workspace disjointness rules apply. */
enum sgx535_task_status sgx535_task_pair_launch_build(const struct sgx535_task_pair_request *,
                                                      const struct sgx535_task_packet *,
                                                      const struct sgx535_task_launch_binding *,
                                                      struct sgx535_task_launch *,
                                                      struct sgx535_task_launch_workspace *);
enum sgx535_task_status sgx535_task_pair_launch_validate(const struct sgx535_task_pair_request *,
                                                         const struct sgx535_task_packet *,
                                                         const struct sgx535_task_launch_binding *,
                                                         const struct sgx535_task_launch *,
                                                         struct sgx535_task_launch_workspace *);
/* Complete internal profile6 path: regenerate and compare the entire compiled
 * symbolic-IR packet before projecting any canonical role/bank relocation or
 * launch action. No unchecked-packet projection entry is exported. Compilation
 * may allocate/sleep, so this is unpublished sleeping preparation only. Large
 * output/workspace must be disjoint from one another and every stable input,
 * including the complete symbolic IR array. All0x1ff packet unknowns persist;
 * no constructed stage or bank number grants physical execution authority. */
enum sgx535_task_status sgx535_compiled_color0_launch_build(
    const struct sgx535_compiled_color0_request *, const struct sgx535_task_packet *,
    const struct sgx535_task_launch_binding *, struct sgx535_task_launch *,
    struct sgx535_compiled_color0_launch_workspace *);
enum sgx535_task_status sgx535_compiled_color0_launch_validate(
    const struct sgx535_compiled_color0_request *, const struct sgx535_task_packet *,
    const struct sgx535_task_launch_binding *, const struct sgx535_task_launch *,
    struct sgx535_compiled_color0_launch_workspace *);
/* Distinct typed packet7 input; the same stable-input, sleeping preparation
 * and disjoint workspace rules apply. Regenerates every packet byte before
 * projecting all canonical relocations and fixed stages. Packet0x1ff and
 * launch0x75 unknowns remain; this performs no upload or native admission. */
enum sgx535_task_status sgx535_window_color0_launch_build(
    const struct sgx535_window_color0_request *, const struct sgx535_task_packet *,
    const struct sgx535_task_launch_binding *, struct sgx535_task_launch *,
    struct sgx535_compiled_color0_launch_workspace *);
enum sgx535_task_status sgx535_window_color0_launch_validate(
    const struct sgx535_window_color0_request *, const struct sgx535_task_packet *,
    const struct sgx535_task_launch_binding *, const struct sgx535_task_launch *,
    struct sgx535_compiled_color0_launch_workspace *);
#endif
