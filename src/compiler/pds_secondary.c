// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_secondary.h"
#include <string.h>

/* Select the largest exact DMA factor up to 16 with a quotient up to 16.
 * Counts outside that factorization use the bounded two-transfer path. */
static uint32_t dma_factor(uint32_t count)
{
    uint32_t factor;

    for (factor = 16; factor != 0; --factor) {
        if (count % factor == 0 && count / factor <= 16)
            return factor;
    }
    return 16;
}

static enum sgx535_pds_secondary_status
validate(const struct sgx535_pds_secondary *input,
         const struct sgx535_pds_secondary_resource *resource)
{
    const uint64_t address_space_end = UINT64_C(1) << 32;
    uint64_t resource_end, source_end;

    if (!input->dword_count || input->dword_count > SGX535_PDS_SECONDARY_MAX_DWORDS ||
        (input->source_address & 3))
        return SGX535_PDS_SECONDARY_ERR_RANGE;
    resource_end = (uint64_t)resource->gpu_address + resource->size_bytes;
    source_end = (uint64_t)input->source_address + input->dword_count * UINT64_C(4);
    if (!resource->size_bytes || resource_end > address_space_end ||
        input->source_address < resource->gpu_address || source_end > resource_end)
        return SGX535_PDS_SECONDARY_ERR_RESOURCE;
    return SGX535_PDS_SECONDARY_OK;
}

enum sgx535_pds_secondary_status
sgx535_pds_secondary_encode(const struct sgx535_pds_secondary *input,
                            const struct sgx535_pds_secondary_resource *resource,
                            struct sgx535_pds_secondary_program *output)
{
    struct sgx535_pds_secondary_program result = {{0}, 0};
    struct sgx535_pds_secondary request;
    struct sgx535_pds_secondary_resource extent;
    enum sgx535_pds_secondary_status status;
    uint32_t first_count, remainder = 0, task_offset = 2;

    if (!input || !resource || !output)
        return SGX535_PDS_SECONDARY_ERR_ARGUMENT;
    memcpy(&request, input, sizeof(request));
    memcpy(&extent, resource, sizeof(extent));
    status = validate(&request, &extent);
    if (status != SGX535_PDS_SECONDARY_OK)
        return status;

    result.words[0] = request.source_address;
    first_count = request.dword_count;
    if (request.dword_count <= 16) {
        result.words[1] = request.dword_count - 1;
    } else {
        uint32_t factor = dma_factor(request.dword_count);
        uint32_t lines = request.dword_count / factor;

        first_count = lines * factor;
        remainder = request.dword_count - first_count;
        result.words[1] = (factor - 1) | ((factor - 1) << 21) | ((lines - 1) << 4);
    }
    result.words[12] = UINT32_C(0x07030223);
    if (remainder) {
        /* The full validated interval bounds this interior address too. */
        result.words[2] = request.source_address + first_count * 4;
        result.words[3] = (remainder - 1) | (first_count << 8);
        task_offset = 4;
        result.words[13] = UINT32_C(0x07070223);
        result.words[14] = UINT32_C(0x07080345);
        result.words[15] = UINT32_C(0xaf000000);
        result.word_count = 16;
    } else {
        result.words[13] = UINT32_C(0x07040345);
        result.words[14] = UINT32_C(0xaf000000);
        result.word_count = 15;
    }
    result.words[task_offset] = request.task_control[0];
    result.words[task_offset + 1] = request.task_control[1];
    result.words[8] = request.task_control[2];
    memcpy(output, &result, sizeof(result));
    return SGX535_PDS_SECONDARY_OK;
}

enum sgx535_pds_secondary_status
sgx535_pds_secondary_decode(const struct sgx535_pds_secondary_program *input,
                            const struct sgx535_pds_secondary_resource *resource,
                            struct sgx535_pds_secondary *output)
{
    struct sgx535_pds_secondary_program program, canonical;
    struct sgx535_pds_secondary request;
    enum sgx535_pds_secondary_status status;
    uint32_t task_offset;

    if (!input || !resource || !output)
        return SGX535_PDS_SECONDARY_ERR_ARGUMENT;
    memcpy(&program, input, sizeof(program));
    if (program.word_count != 15 && program.word_count != 16)
        return SGX535_PDS_SECONDARY_ERR_UNSUPPORTED;

    task_offset = program.word_count == 15 ? 2 : 4;
    request.source_address = program.words[0];
    request.dword_count = ((program.words[1] & 15) + 1) * (((program.words[1] >> 4) & 15) + 1);
    if (program.word_count == 16)
        request.dword_count += (program.words[3] & 15) + 1;
    request.task_control[0] = program.words[task_offset];
    request.task_control[1] = program.words[task_offset + 1];
    request.task_control[2] = program.words[8];
    status = sgx535_pds_secondary_encode(&request, resource, &canonical);
    if (status == SGX535_PDS_SECONDARY_ERR_RESOURCE)
        return status;
    if (status != SGX535_PDS_SECONDARY_OK || memcmp(&program, &canonical, sizeof(program)))
        return SGX535_PDS_SECONDARY_ERR_UNSUPPORTED;
    memcpy(output, &request, sizeof(request));
    return SGX535_PDS_SECONDARY_OK;
}
