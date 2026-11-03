// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_USE_CODE_BANK_H
#define SGX535_USE_CODE_BANK_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

enum sgx535_use_code_bank_status {
    SGX535_USE_CODE_BANK_OK = 0,
    SGX535_USE_CODE_BANK_INVALID,
    SGX535_USE_CODE_BANK_RANGE
};

struct sgx535_use_code_bank_input {
    uint32_t code_va_bytes;
    uint32_t code_size_bytes;
    uint32_t entry_va_bytes;
    uint32_t bank_index;
    uint32_t data_master; /* 0 vertex, 1 pixel; privileged masters refuse. */
};

struct sgx535_use_code_bank_projection {
    uint32_t bank_base_va_bytes;
    uint32_t entry_offset_bytes;
    uint32_t register_offset_bytes;
    uint32_t register_word;
};

/* CPU-only projection of a 512-KiB bank, 128-byte base units and a byte entry
 * offset. Code and entry are whole 8-byte instructions. The complete nonempty
 * extent lies inside one bank and may end exactly at its end: the 16-bit
 * instruction index covers all 512 KiB. Indices 0..15 are field values, not a
 * bank allocation policy. No PDS task controls, mappings, register ownership,
 * MMIO, upload, execution or retirement permission is returned. Output remains
 * unchanged on refusal; stable input and output storage may overlap.
 */
enum sgx535_use_code_bank_status
sgx535_use_code_bank_project(const struct sgx535_use_code_bank_input *input,
                             struct sgx535_use_code_bank_projection *out);
#endif
