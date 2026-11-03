// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_texture2d.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

enum sgx535_texture2d_status
sgx535_texture2d_nearest_build(const struct sgx535_texture2d_nearest_request *input,
                               struct sgx535_texture2d_nearest *output)
{
    struct sgx535_texture2d_nearest_request request;
    struct sgx535_texture2d_nearest result;
    uint64_t row, span;

    if (!input || !output)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    memcpy(&request, input, sizeof(request));
    for (size_t i = 0; i < sizeof(request.reserved) / sizeof(request.reserved[0]); i++)
        if (request.reserved[i])
            return SGX535_TEXTURE2D_ERR_ARGUMENT;
    if (!request.texture_slots || request.texture_slot >= request.texture_slots ||
        !request.sampler_slots || request.sampler_slot >= request.sampler_slots ||
        !request.ssa_values || request.ssa_values > SGX535_NIR_MAX_INSTRUCTIONS ||
        request.coordinate_ssa >= request.ssa_values || request.result_ssa >= request.ssa_values ||
        request.coordinate_ssa == request.result_ssa || !request.width || !request.height ||
        (request.byte_offset & 3u))
        return SGX535_TEXTURE2D_ERR_RANGE;
    if (request.coordinate_components != 2 || request.coordinate_bits != 32 ||
        request.result_components != 4 || request.result_bits != 32 || request.skip_helpers > 1 ||
        request.can_speculate > 1 || request.format != SGX535_TEXTURE2D_BGRA8_UNORM ||
        request.wrap_s != SGX535_TEXTURE2D_CLAMP_TO_EDGE ||
        request.wrap_t != SGX535_TEXTURE2D_CLAMP_TO_EDGE ||
        request.min_filter != SGX535_TEXTURE2D_NEAREST ||
        request.mag_filter != SGX535_TEXTURE2D_NEAREST ||
        request.mip_filter != SGX535_TEXTURE2D_MIP_NONE || request.normalized_coords != 1 ||
        request.compare_enable || request.reduction_mode || request.max_anisotropy > 1 ||
        request.raw_samples > 1 || request.raw_storage_samples > 1)
        return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    row = (((uint64_t)request.width + 31u) & ~(uint64_t)31u) * 4u;
    if (row > UINT32_MAX)
        return SGX535_TEXTURE2D_ERR_RANGE;
    if (request.stride_bytes != row)
        return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    span = row * request.height;
    if (request.byte_offset > request.allocation_bytes ||
        span > request.allocation_bytes - request.byte_offset)
        return SGX535_TEXTURE2D_ERR_RANGE;

    memset(&result, 0, sizeof(result));
    /* Matched no-mip MAXLOD=15 and DADJUST=31; nearest min/mag are zero.
     * The selected LINEAR branch supplies clamp S/T, not the separate
     * surface-flag0x10 wrap-table branch.
     * These controls are not a complete surface state word or a program. */
    result.sampler_controls = UINT32_C(0x03fe0090);
    result.texture_slot = request.texture_slot;
    result.sampler_slot = request.sampler_slot;
    result.coordinate_ssa = request.coordinate_ssa;
    result.result_ssa = request.result_ssa;
    result.skip_helpers = request.skip_helpers;
    result.can_speculate = request.can_speculate;
    result.width = request.width;
    result.height = request.height;
    result.stride_bytes = request.stride_bytes;
    result.format = request.format;
    result.raw_samples = request.raw_samples;
    result.raw_storage_samples = request.raw_storage_samples;
    result.byte_offset = request.byte_offset;
    result.span_bytes = span;
    result.allocation_bytes = request.allocation_bytes;
    /* The native surface template and compiler format record are selected
     * together. Its plain-U8/default unpack consumer selects these bytes;
     * this is independent of any NIR or view swizzle. */
    result.result =
        (struct sgx535_texture2d_result_layout){SGX535_TEXTURE2D_RESULT_BGRA8_U8, 1, {2, 1, 0, 3}};
    memcpy(output, &result, sizeof(result));
    return SGX535_TEXTURE2D_OK;
}

enum sgx535_texture2d_status
sgx535_texture2d_nearest_descriptor(const struct sgx535_texture2d_nearest *input,
                                    const struct sgx535_texture2d_cpu_binding *binding,
                                    uint32_t output[3])
{
    struct sgx535_texture2d_nearest nearest;
    struct sgx535_texture2d_cpu_binding memory;
    static const uint32_t rgba_byte[] = {2, 1, 0, 3};
    uint32_t result[3];
    uint64_t row, span;

    if (!input || !binding || !output)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    memcpy(&nearest, input, sizeof(nearest));
    memcpy(&memory, binding, sizeof(memory));
    if (!nearest.width || nearest.width > 4096 || !nearest.height || nearest.height > 4096 ||
        (memory.gpu_byte_address & 3u) || (nearest.byte_offset & 3u) ||
        memory.gpu_byte_address > UINT32_MAX || !memory.allocation_bytes ||
        memory.allocation_bytes != nearest.allocation_bytes ||
        memory.allocation_bytes > (UINT64_C(1) << 32) - memory.gpu_byte_address)
        return SGX535_TEXTURE2D_ERR_RANGE;
    row = (((uint64_t)nearest.width + 31u) & ~(uint64_t)31u) * 4u;
    span = row * nearest.height;
    if (nearest.byte_offset > memory.allocation_bytes ||
        span > memory.allocation_bytes - nearest.byte_offset || nearest.span_bytes != span)
        return SGX535_TEXTURE2D_ERR_RANGE;
    if (nearest.stride_bytes != row || nearest.format != SGX535_TEXTURE2D_BGRA8_UNORM ||
        nearest.sampler_controls != UINT32_C(0x03fe0090) || nearest.raw_samples > 1 ||
        nearest.raw_storage_samples > 1 ||
        nearest.result.format != SGX535_TEXTURE2D_RESULT_BGRA8_U8 || nearest.result.dwords != 1 ||
        memcmp(nearest.result.rgba_byte, rgba_byte, sizeof(rgba_byte)))
        return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    result[0] = nearest.sampler_controls;
    result[1] = UINT32_C(0x6c000000) | ((nearest.width - 1u) << 12) | (nearest.height - 1u);
    result[2] = (uint32_t)(memory.gpu_byte_address + nearest.byte_offset);
    memcpy(output, result, sizeof(result));
    return SGX535_TEXTURE2D_OK;
}

enum sgx535_texture2d_status
sgx535_texture2d_unpack_build(const struct sgx535_texture2d_unpack_request *input,
                              uint32_t output[SGX535_TEXTURE2D_UNPACK_WORDS])
{
    struct sgx535_texture2d_unpack_request request;
    uint32_t result[SGX535_TEXTURE2D_UNPACK_WORDS];
    static const uint32_t rgba_byte[] = {2, 1, 0, 3};
    if (!input || !output)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    memcpy(&request, input, sizeof(request));
    if (request.layout.format != SGX535_TEXTURE2D_RESULT_BGRA8_U8 || request.layout.dwords != 1 ||
        memcmp(request.layout.rgba_byte, rgba_byte, sizeof(rgba_byte)) ||
        (request.packed_source.bank != SGX535_MOV_TEMP &&
         request.packed_source.bank != SGX535_MOV_PRIMARY))
        return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    if (request.first_destination > 120 ||
        request.packed_source.number >
            (request.packed_source.bank == SGX535_MOV_TEMP ? 123u : 127u))
        return SGX535_TEXTURE2D_ERR_RANGE;
    /* Reading the same packed DWORD four times requires all four destination
     * writes to be disjoint, including a later destination alias. */
    if (request.packed_source.bank == SGX535_MOV_TEMP &&
        request.packed_source.number >= request.first_destination &&
        request.packed_source.number - request.first_destination < 4)
        return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < 4; i++) {
        struct sgx535_unpack_u8_f32_component op = {
            {SGX535_MOV_TEMP, request.first_destination + i},
            request.packed_source,
            request.layout.rgba_byte[i],
            0};
        if (sgx535_unpack_u8_f32_component_encode(&op, &result[i * 2]) != SGX535_PACK_OK)
            return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
    }
    memcpy(output, result, sizeof(result));
    return SGX535_TEXTURE2D_OK;
}

enum sgx535_texture2d_status
sgx535_texture2d_primary_unpack_build(const struct sgx535_texture2d_nearest *texture,
                                      const struct sgx535_texture2d_primary_inputs *input,
                                      struct sgx535_texture2d_primary_plan *output)
{
    struct sgx535_texture2d_nearest nearest;
    struct sgx535_texture2d_primary_inputs inputs;
    struct sgx535_texture2d_primary_plan result;
    struct sgx535_texture2d_unpack_request unpack;
    uint32_t seen[(SGX535_NIR_MAX_INSTRUCTIONS + 31u) / 32u] = {0};
    uint32_t total = 0;
    enum sgx535_texture2d_status status;
    int found = 0;
    if (!texture || !input || !output)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    memcpy(&nearest, texture, sizeof(nearest));
    memcpy(&inputs, input, sizeof(inputs));
    if (!inputs.records)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    if (!inputs.count || inputs.count > 128 || !inputs.primary_dwords ||
        inputs.primary_dwords > 128 || nearest.result_ssa >= SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_TEXTURE2D_ERR_RANGE;
    memset(&result, 0, sizeof(result));
    for (uint32_t i = 0; i < inputs.count; i++) {
        struct sgx535_texture2d_primary_input record = inputs.records[i];
        if (record.source_ssa >= SGX535_NIR_MAX_INSTRUCTIONS || !record.dwords ||
            record.dwords > 128 - total)
            return SGX535_TEXTURE2D_ERR_RANGE;
        uint32_t mask = UINT32_C(1) << (record.source_ssa % 32u);
        if (seen[record.source_ssa / 32u] & mask)
            return SGX535_TEXTURE2D_ERR_ARGUMENT;
        seen[record.source_ssa / 32u] |= mask;
        if (record.source_ssa == nearest.result_ssa) {
            if (record.dwords != nearest.result.dwords)
                return SGX535_TEXTURE2D_ERR_UNSUPPORTED;
            result.record_index = i;
            result.result_ssa = record.source_ssa;
            result.result_dwords = record.dwords;
            result.packed_source = (struct sgx535_mov_operand){SGX535_MOV_PRIMARY, total};
            found = 1;
        }
        total += record.dwords;
    }
    if (!found || total != inputs.primary_dwords)
        return SGX535_TEXTURE2D_ERR_ARGUMENT;
    result.primary_dwords = total;
    unpack = (struct sgx535_texture2d_unpack_request){nearest.result, result.packed_source,
                                                      inputs.first_destination};
    status = sgx535_texture2d_unpack_build(&unpack, result.unpack);
    if (status != SGX535_TEXTURE2D_OK)
        return status;
    memcpy(output, &result, sizeof(result));
    return SGX535_TEXTURE2D_OK;
}
