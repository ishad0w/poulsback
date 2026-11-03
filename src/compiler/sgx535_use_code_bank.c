// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_use_code_bank.h"

enum sgx535_use_code_bank_status
sgx535_use_code_bank_project(const struct sgx535_use_code_bank_input *input,
                             struct sgx535_use_code_bank_projection *out)
{
    struct sgx535_use_code_bank_input request;
    struct sgx535_use_code_bank_projection result;
    uint64_t code_end, bank_end;

    if (!input || !out)
        return SGX535_USE_CODE_BANK_INVALID;
    request = *input;
    if (!request.code_size_bytes ||
        ((request.code_va_bytes | request.code_size_bytes | request.entry_va_bytes) & 7u) ||
        request.bank_index >= 16u || request.data_master > 1u)
        return SGX535_USE_CODE_BANK_RANGE;

    result.bank_base_va_bytes = request.code_va_bytes & ~0x7ffffu;
    code_end = (uint64_t)request.code_va_bytes + request.code_size_bytes;
    bank_end = (uint64_t)result.bank_base_va_bytes + 0x80000u;
    if (code_end > bank_end || request.entry_va_bytes < request.code_va_bytes ||
        (uint64_t)request.entry_va_bytes >= code_end)
        return SGX535_USE_CODE_BANK_RANGE;

    result.entry_offset_bytes = request.entry_va_bytes - result.bank_base_va_bytes;
    result.register_offset_bytes = 0xa0cu + request.bank_index * 4u;
    result.register_word = (result.bank_base_va_bytes >> 7) | (request.data_master << 25);
    *out = result;
    return SGX535_USE_CODE_BANK_OK;
}
