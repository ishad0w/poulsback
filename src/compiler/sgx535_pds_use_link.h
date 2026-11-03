// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_PDS_USE_LINK_H
#define POULSBACK_SGX535_PDS_USE_LINK_H

#include "sgx535_use_code_bank.h"

enum sgx535_pds_use_link_status {
    SGX535_PDS_USE_LINK_OK = 0,
    SGX535_PDS_USE_LINK_ERR_ARGUMENT = -1,
    SGX535_PDS_USE_LINK_ERR_RANGE = -2
};

struct sgx535_pds_use_link_projection {
    uint32_t bank_selector_fields;
    uint32_t entry_high_fields;
    uint32_t entry_low_fields;
    uint32_t address_fields;
};

/* CPU-only address-field projection for the selected PDS-to-USE producer.
 * The checked code-bank input retains the complete code extent and entry.
 * This profile requires a 16-byte entry offset: its offset field omits the
 * lower four byte-address bits, although a USE instruction is eight bytes.
 * Code start and entry must lie in one 32-KiB block: the high offset field
 * comes from the code start and the low offset field comes from the entry.
 * Only fields with masks 0xf, 0xf0 and 0x7ff00 are returned. Their union is
 * 0x7ffff; all other bits are zero. No task opcode/background, attribute
 * control, mapping ownership, register policy, upload, launch or retirement
 * permission is returned. Output is unchanged on refusal; stable input and
 * output storage may overlap.
 */
enum sgx535_pds_use_link_status
sgx535_pds_use_link_project(const struct sgx535_use_code_bank_input *,
                            struct sgx535_pds_use_link_projection *);

#endif
