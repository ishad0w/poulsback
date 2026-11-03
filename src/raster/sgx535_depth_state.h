// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_STATE_H
#define SGX535_DEPTH_STATE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Internal typed preparation, separate from RGB images and packet v2/v3.
 * These source-format identities never grant a depth allocation or GPU access. */
#define SGX535_DEPTH_VERSION 1u
#define SGX535_DEPTH_MAX_DIMENSION 2048u /* bounded construction policy */
#define SGX535_DEPTH_ADDRESS_PAIRS 5u
#define SGX535_DEPTH_MISSING_INITIALIZATION (1u << 0)
#define SGX535_DEPTH_MISSING_FORMAT_RESET (1u << 1)
#define SGX535_DEPTH_MISSING_DEVICE_LAYOUT (1u << 2)
#define SGX535_DEPTH_MISSING_ADDRESS_DOMAIN (1u << 3)
#define SGX535_DEPTH_MISSING_WRITE_SET (1u << 4)
#define SGX535_DEPTH_MISSING_VISIBILITY (1u << 5)
#define SGX535_DEPTH_MISSING_RETIREMENT (1u << 6)
#define SGX535_DEPTH_MISSING_ALL 0x7fu
enum sgx535_depth_format { SGX535_DEPTH_D16 = 1, SGX535_DEPTH_D24S8 = 2 };
enum sgx535_depth_content { SGX535_DEPTH_FRESH = 0, SGX535_DEPTH_PREVIOUS_SCENE = 2 };
enum sgx535_depth_compare {
    SGX535_DEPTH_NEVER = 0,
    SGX535_DEPTH_LESS,
    SGX535_DEPTH_EQUAL,
    SGX535_DEPTH_LEQUAL,
    SGX535_DEPTH_GREATER,
    SGX535_DEPTH_NOTEQUAL,
    SGX535_DEPTH_GEQUAL,
    SGX535_DEPTH_ALWAYS
};
enum sgx535_depth_status { SGX535_DEPTH_INVALID = -1, SGX535_DEPTH_BLOCKED = 1 };
struct sgx535_depth_request {
    uint32_t version, width, height, format;
    uint32_t content, preserve_after, compare, write_enable;
    uint32_t reserved[4];
};
struct sgx535_depth_address_pair {
    /* Exact masks passed to the selected source relocation producer. Their
     * requester/register semantics remain unqualified; no address is patched.
     * alignment is a conservative future owner-binding policy, not a decoded
     * register alignment or proof of the historical allocation's GPU domain. */
    uint32_t register_offset, source_relocation_mask, resource_byte_offset, alignment;
    uint64_t required_span_bytes;
};
struct sgx535_depth_preparation {
    uint32_t version, format, width, height;
    uint32_t bytes_per_element, pitch_pixels, stride_bytes, allocation_height;
    uint64_t allocation_bytes;
    uint32_t zls_control, clear_depth_f32, clear_stencil, missing_contracts;
    /* Replace only the depth-compare/write portion of the ISP primary word.
     * The remaining ISP, stencil and MTE words need their own checked profile. */
    uint32_t isp_clear_mask, isp_set_bits, address_pair_count, reserved;
    struct sgx535_depth_address_pair address_pairs[SGX535_DEPTH_ADDRESS_PAIRS];
};
/* Exact selected DRI storage, ZLS branch and ISP depth-field construction.
 * PREVIOUS_SCENE selects historical numeric state2; it is not a certificate of
 * initialized content. All physical blockers remain set, including for FRESH.
 * allocation_bytes is the computed padded storage requirement; physical backing
 * validation remains pending. It does not prove containment of decoded GPU writes.
 * No supplied GPU VA, raw format flags, shader words, mapping, memory mutation
 * or register writes.
 * Success is BLOCKED; errors preserve output. Stable input/output overlap works. */
enum sgx535_depth_status sgx535_depth_prepare(const struct sgx535_depth_request *,
                                              struct sgx535_depth_preparation *);
#endif
