// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_TEXTURE2D_H
#define POULSBACK_SGX535_TEXTURE2D_H

#include "sgx535_linear.h"
#include "sgx535_pack.h"

enum sgx535_texture2d_status {
    SGX535_TEXTURE2D_OK = 0,
    SGX535_TEXTURE2D_ERR_ARGUMENT = -1,
    SGX535_TEXTURE2D_ERR_RANGE = -2,
    SGX535_TEXTURE2D_ERR_UNSUPPORTED = -3
};
enum sgx535_texture2d_format { SGX535_TEXTURE2D_BGRA8_UNORM = 1 };
enum sgx535_texture2d_wrap { SGX535_TEXTURE2D_REPEAT, SGX535_TEXTURE2D_CLAMP_TO_EDGE };
enum sgx535_texture2d_filter { SGX535_TEXTURE2D_NEAREST };
enum sgx535_texture2d_mip_filter { SGX535_TEXTURE2D_MIP_NONE };
enum sgx535_texture2d_result_format { SGX535_TEXTURE2D_RESULT_BGRA8_U8 = 1 };

/* Default SGX535 plain-U8 fetch layout for the matched BGRA8 surface class.
 * Byte selectors address one physical sample DWORD; they do not encode NIR
 * swizzles. Gamma, format overrides and expanded two-DWORD results refuse. */
struct sgx535_texture2d_result_layout {
    uint32_t format, dwords, rgba_byte[4];
};

/* Internal semantic request, not UAPI or a hardware descriptor. Slot counts
 * bound the caller's logical registries, not a claimed texture-unit limit.
 * SSA identities address the same immutable source graph; this builder checks
 * types/ranges, not graph dominance, ownership or whole-shader acceptance.
 * No GPU address, native instruction or image handle belongs in this record. */
struct sgx535_texture2d_nearest_request {
    uint32_t texture_slot, sampler_slot, texture_slots, sampler_slots;
    uint32_t coordinate_ssa, result_ssa, ssa_values;
    uint32_t coordinate_components, coordinate_bits, result_components, result_bits;
    uint32_t skip_helpers, can_speculate;
    uint32_t width, height, stride_bytes, format;
    uint32_t wrap_s, wrap_t, min_filter, mag_filter, mip_filter;
    uint32_t normalized_coords, compare_enable, reduction_mode, max_anisotropy;
    uint32_t raw_samples, raw_storage_samples;
    uint64_t byte_offset, allocation_bytes;
    uint32_t reserved[4];
};
struct sgx535_texture2d_nearest {
    uint32_t sampler_controls;
    uint32_t texture_slot, sampler_slot, coordinate_ssa, result_ssa;
    uint32_t skip_helpers, can_speculate;
    uint32_t width, height, stride_bytes, format, raw_samples, raw_storage_samples;
    uint64_t byte_offset, span_bytes, allocation_bytes;
    struct sgx535_texture2d_result_layout result;
};

/* Trusted CPU construction input, never a userspace launch/binding record.
 * The address names the allocation base; byte_offset is retained in nearest.
 * This checks representability, not image identity, residency or ownership. */
struct sgx535_texture2d_cpu_binding {
    uint64_t gpu_byte_address, allocation_bytes;
};

/* Selected non-gamma LINEAR BGRA8 three-DWORD texture state. The complete
 * allocation must fit the 32-bit address domain and match the copied span.
 * Surface dimensions are 1..4096; pitch remains 4*ALIGN32(width). These words
 * provide neither publication authority nor a cache/dependency transition.
 * Stable input/output may overlap; failure preserves every output byte. */
enum sgx535_texture2d_status
sgx535_texture2d_nearest_descriptor(const struct sgx535_texture2d_nearest *,
                                    const struct sgx535_texture2d_cpu_binding *, uint32_t[3]);

/* Selected normalized, nonmipmapped, nearest 2D BGRA8 construction with clamp
 * on both axes. The matched LINEAR branch fixes both native wrap fields;
 * a requested REPEAT is not silently changed to that state. The row pitch
 * must be 4*ALIGN32(width): custom stride reuses the native wrap fields and
 * is a separate contract. The checked span includes every padded row.
 * sampler_controls contains only the matched sampler portion of state word0.
 * Surface format/size/address, iterator/PDS delivery, filtering arithmetic,
 * cache visibility and native dispatch are not supplied or qualified here.
 * Keep original helper/speculation flags; this call never speculates a tex.
 * Input/output must be stable allocated objects and may overlap. Errors leave
 * every output byte unchanged; successful output has initialized padding. */
enum sgx535_texture2d_status
sgx535_texture2d_nearest_build(const struct sgx535_texture2d_nearest_request *,
                               struct sgx535_texture2d_nearest *);

struct sgx535_texture2d_unpack_request {
    struct sgx535_texture2d_result_layout layout;
    struct sgx535_mov_operand packed_source;
    uint32_t first_destination;
};
#define SGX535_TEXTURE2D_UNPACK_WORDS 8u
/* Construct four U8->F32 conversions into consecutive TEMP registers. The
 * caller supplies a completed sample in one TEMP/PRIMARY DWORD and a disjoint
 * TEMP extent; this function allocates neither. All instructions have END=0.
 * This is a checked layout/encoding join, not sample placement, a DRC wait,
 * execution order, conversion arithmetic or texture cache qualification.
 * Stable input/output may overlap. Errors preserve every output byte. */
enum sgx535_texture2d_status
sgx535_texture2d_unpack_build(const struct sgx535_texture2d_unpack_request *,
                              uint32_t[SGX535_TEXTURE2D_UNPACK_WORDS]);

/* Final ordered, live input records after input selection/packing. An extent
 * is in DWORDs, not components of the original texture NIR result. IDs name
 * that same source graph; this codec checks uniqueness and bounds, not whether
 * a value is eligible for nondependent sampling or PDS delivery. */
struct sgx535_texture2d_primary_input {
    uint32_t source_ssa, dwords;
};
struct sgx535_texture2d_primary_inputs {
    const struct sgx535_texture2d_primary_input *records;
    uint32_t count, primary_dwords, first_destination;
};
struct sgx535_texture2d_primary_plan {
    uint32_t result_ssa, record_index, result_dwords, primary_dwords;
    struct sgx535_mov_operand packed_source;
    uint32_t unpack[SGX535_TEXTURE2D_UNPACK_WORDS];
};
/* Allocate the matched sample's complete interval within the ordered fixed
 * PRIMARY group, base0. The selected result SSA occurs exactly once; sum of
 * all extents must equal primary_dwords within the existing direct-PA policy.
 * Join that precise interval to the checked UNPACKs. The same unrounded DWORD
 * total must reach later input metadata and task allocation; this function
 * supplies neither PDS texture issues nor DMS rounding/dependency controls.
 * Stable allocated inputs/records and output may overlap. Failure preserves
 * all output bytes; successful output initializes its padding. */
enum sgx535_texture2d_status
sgx535_texture2d_primary_unpack_build(const struct sgx535_texture2d_nearest *,
                                      const struct sgx535_texture2d_primary_inputs *,
                                      struct sgx535_texture2d_primary_plan *);

#endif
