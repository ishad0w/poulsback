// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_HEAP_LAYOUT_H
#define SGX535_HEAP_LAYOUT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

enum sgx535_heap_layout_status {
    SGX535_HEAP_LAYOUT_OK = 0,
    SGX535_HEAP_LAYOUT_INVALID,
    SGX535_HEAP_LAYOUT_RANGE,
    SGX535_HEAP_LAYOUT_ALIAS
};

struct sgx535_heap_extent {
    uint64_t storage_id;
    uint64_t gpu_va_bytes;
    uint64_t allocation_bytes;
};

struct sgx535_heap_layout_input {
    struct sgx535_heap_extent pds;
    struct sgx535_heap_extent parameters;
    struct sgx535_heap_extent tables;
    struct sgx535_heap_extent scene;
};

struct sgx535_heap_register_word {
    uint32_t register_offset_bytes;
    uint32_t word;
};

struct sgx535_heap_layout_projection {
    struct sgx535_heap_register_word pds_exec_base;
    struct sgx535_heap_register_word ta_request_base;
    struct sgx535_heap_register_word three_d_request_base;
    uint32_t pds_allocation_offset_bytes;
    uint32_t parameter_first_page;
    uint32_t parameter_last_page;
    uint32_t table_va_bytes;
    uint32_t scene_va_bytes;
};

/* CPU-only exact legacy profile: PDS [0x20000000,0x30000000), PARAMETERS
 * [0x30000000,0x40000000), TABLES/SCENE [0x40000000,0x50000000). The generic
 * range is a conservative modern subset. Whole-page extents are required;
 * PARAMETERS starts on a 1-MiB boundary. Nonzero distinct numeric storage IDs
 * and nonoverlapping VA extents do not prove distinct physical backing pages.
 * An internal typed owner must authenticate those identities separately.
 *
 * The result distinguishes the three register-word producers and retains full
 * table/scene VAs versus parameter-window page indices. This grants no heap
 * allocation, MMU mapping, register write, request effect, task linkage,
 * executable shader or retirement permission. No user UAPI or dynamic base
 * override is provided. Errors leave output unchanged; stable input and output
 * storage may overlap.
 */
enum sgx535_heap_layout_status
sgx535_heap_layout_project(const struct sgx535_heap_layout_input *input,
                           struct sgx535_heap_layout_projection *out);

#endif
