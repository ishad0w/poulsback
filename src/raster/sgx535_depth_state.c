// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_state.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

enum sgx535_depth_status sgx535_depth_prepare(const struct sgx535_depth_request *input,
                                              struct sgx535_depth_preparation *out)
{
    struct sgx535_depth_request r;
    struct sgx535_depth_preparation p;
    static const uint32_t registers[SGX535_DEPTH_ADDRESS_PAIRS] = {0xcb0, 0x484, 0x488, 0x48c,
                                                                   0x490};
    uint32_t i;
    if (!input || !out)
        return SGX535_DEPTH_INVALID;
    r = *input;
    if (r.version != SGX535_DEPTH_VERSION || !r.width || !r.height ||
        r.width > SGX535_DEPTH_MAX_DIMENSION || r.height > SGX535_DEPTH_MAX_DIMENSION ||
        (r.format != SGX535_DEPTH_D16 && r.format != SGX535_DEPTH_D24S8) ||
        (r.content != SGX535_DEPTH_FRESH && r.content != SGX535_DEPTH_PREVIOUS_SCENE) ||
        r.preserve_after > 1 || r.compare > SGX535_DEPTH_ALWAYS || r.write_enable > 1)
        return SGX535_DEPTH_INVALID;
    for (i = 0; i < 4; i++)
        if (r.reserved[i])
            return SGX535_DEPTH_INVALID;
    memset(&p, 0, sizeof(p));
    p.version = SGX535_DEPTH_VERSION;
    p.format = r.format;
    p.width = r.width;
    p.height = r.height;
    p.bytes_per_element = r.format == SGX535_DEPTH_D16 ? 2u : 4u;
    /* The selected PSB backing allocation pads both axes to32; pitch is in
     * elements. This row-byte product is not a decoded GPU depth layout. */
    p.pitch_pixels = (r.width + 31u) & ~31u;
    p.allocation_height = (r.height + 31u) & ~31u;
    p.stride_bytes = p.pitch_pixels * p.bytes_per_element;
    p.allocation_bytes = (uint64_t)p.stride_bytes * p.allocation_height;
    p.zls_control = ((p.pitch_pixels / 16u) - 1u) << 8;
    p.zls_control |= r.format == SGX535_DEPTH_D16 ? 0xa0000u : 0x50000u;
    if (r.content == SGX535_DEPTH_PREVIOUS_SCENE)
        p.zls_control |= 0x200026u;
    if (r.preserve_after)
        p.zls_control |= 0x4000d0u;
    p.clear_depth_f32 = 0x3f800000u;
    p.missing_contracts = SGX535_DEPTH_MISSING_ALL;
    p.isp_clear_mask = 0x01d00000u;
    p.isp_set_bits = r.compare << 22;
    if (!r.write_enable)
        p.isp_set_bits |= 0x00100000u;
    p.address_pair_count = SGX535_DEPTH_ADDRESS_PAIRS;
    for (i = 0; i < SGX535_DEPTH_ADDRESS_PAIRS; i++) {
        p.address_pairs[i].register_offset = registers[i];
        p.address_pairs[i].source_relocation_mask = i ? 0x0ffffff0u : 0xf0000000u;
        p.address_pairs[i].alignment = 16;
        p.address_pairs[i].required_span_bytes = p.allocation_bytes;
    }
    *out = p;
    return SGX535_DEPTH_BLOCKED;
}
