// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_heap_layout.h"

static int extent_in_window(const struct sgx535_heap_extent *extent, uint64_t first, uint64_t limit)
{
    return extent->storage_id && extent->allocation_bytes &&
           !((extent->gpu_va_bytes | extent->allocation_bytes) & 4095u) &&
           extent->gpu_va_bytes >= first && extent->gpu_va_bytes < limit &&
           extent->allocation_bytes <= limit - extent->gpu_va_bytes;
}

enum sgx535_heap_layout_status
sgx535_heap_layout_project(const struct sgx535_heap_layout_input *input,
                           struct sgx535_heap_layout_projection *out)
{
    struct sgx535_heap_layout_input request;
    struct sgx535_heap_layout_projection result = {0};
    const struct sgx535_heap_extent *extents[4];
    unsigned int i, j;

    if (!input || !out)
        return SGX535_HEAP_LAYOUT_INVALID;
    request = *input;
    if (!extent_in_window(&request.pds, 0x20000000u, 0x30000000u) ||
        !extent_in_window(&request.parameters, 0x30000000u, 0x40000000u) ||
        !extent_in_window(&request.tables, 0x40000000u, 0x50000000u) ||
        !extent_in_window(&request.scene, 0x40000000u, 0x50000000u) ||
        (request.parameters.gpu_va_bytes & 0xfffffu))
        return SGX535_HEAP_LAYOUT_RANGE;

    extents[0] = &request.pds;
    extents[1] = &request.parameters;
    extents[2] = &request.tables;
    extents[3] = &request.scene;
    for (i = 0; i < 4; ++i) {
        for (j = i + 1; j < 4; ++j) {
            if (extents[i]->storage_id == extents[j]->storage_id ||
                (extents[i]->gpu_va_bytes <
                     extents[j]->gpu_va_bytes + extents[j]->allocation_bytes &&
                 extents[j]->gpu_va_bytes <
                     extents[i]->gpu_va_bytes + extents[i]->allocation_bytes))
                return SGX535_HEAP_LAYOUT_ALIAS;
        }
    }

    result.pds_exec_base.register_offset_bytes = 0xab8u;
    result.pds_exec_base.word = 0x20000000u;
    result.ta_request_base.register_offset_bytes = 0xc90u;
    result.ta_request_base.word = 0x30000000u;
    result.three_d_request_base.register_offset_bytes = 0xcacu;
    result.three_d_request_base.word = 0x30000000u;
    result.pds_allocation_offset_bytes = (uint32_t)(request.pds.gpu_va_bytes - 0x20000000u);
    result.parameter_first_page =
        (uint32_t)((request.parameters.gpu_va_bytes - 0x30000000u) / 4096u);
    result.parameter_last_page =
        result.parameter_first_page + (uint32_t)(request.parameters.allocation_bytes / 4096u) - 1u;
    result.table_va_bytes = (uint32_t)request.tables.gpu_va_bytes;
    result.scene_va_bytes = (uint32_t)request.scene.gpu_va_bytes;
    *out = result;
    return SGX535_HEAP_LAYOUT_OK;
}
