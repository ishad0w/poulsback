// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_use_link.h"

enum sgx535_pds_use_link_status
sgx535_pds_use_link_project(const struct sgx535_use_code_bank_input *input,
                            struct sgx535_pds_use_link_projection *output)
{
    struct sgx535_use_code_bank_input request;
    struct sgx535_use_code_bank_projection bank;
    struct sgx535_pds_use_link_projection result;
    uint32_t entry_units;

    if (!input || !output)
        return SGX535_PDS_USE_LINK_ERR_ARGUMENT;
    request = *input;
    if (sgx535_use_code_bank_project(&request, &bank) != SGX535_USE_CODE_BANK_OK ||
        (bank.entry_offset_bytes & 15u) ||
        (request.code_va_bytes >> 15) != (request.entry_va_bytes >> 15))
        return SGX535_PDS_USE_LINK_ERR_RANGE;

    /* Divide the bank-relative byte offset into sixteen-byte units, then
     * independently place its low eleven and high four bits. These are
     * address fields only, not an executable PDS task word. */
    entry_units = bank.entry_offset_bytes / 16u;
    result.bank_selector_fields = request.bank_index;
    result.entry_high_fields = (entry_units / 2048u) * 16u;
    result.entry_low_fields = (entry_units % 2048u) * 256u;
    result.address_fields =
        result.bank_selector_fields | result.entry_high_fields | result.entry_low_fields;
    *output = result;
    return SGX535_PDS_USE_LINK_OK;
}
