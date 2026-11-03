// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_use_link.h"
#include <stdio.h>
#include <string.h>

static unsigned int checks;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "PDS USE link check %u failed at line %u\n", checks, __LINE__);        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* An individual-bit placement oracle, independent of the implementation's
 * division/remainder construction. The contract moves byte-offset bits
 * 4..14 to task bits 8..18, and byte-offset bits 15..18 to task bits 4..7. */
static uint32_t bit_oracle(uint32_t bank, uint32_t offset)
{
    uint32_t result = bank;
    unsigned int bit;

    for (bit = 4; bit <= 18; bit++) {
        unsigned int destination = bit <= 14 ? bit + 4 : bit - 11;
        if (offset & (UINT32_C(1) << bit))
            result |= UINT32_C(1) << destination;
    }
    return result;
}

static int refused(struct sgx535_use_code_bank_input input)
{
    struct sgx535_pds_use_link_projection output, before;

    memset(&output, 0xa5, sizeof(output));
    before = output;
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_ERR_RANGE);
    CHECK(!memcmp(&output, &before, sizeof(output)));
    return 0;
}

int main(void)
{
    static const uint32_t bases[] = {0, UINT32_C(0x40000000), UINT32_C(0xfff80000)};
    struct sgx535_use_code_bank_input input = {0x40012340, 8, 0x40012340, 3, 0};
    struct sgx535_pds_use_link_projection output, before;
    unsigned int base, bank;
    uint32_t offset;
    union {
        struct sgx535_use_code_bank_input input;
        struct sgx535_pds_use_link_projection output;
    } overlap;

    CHECK(sgx535_pds_use_link_project(NULL, &output) == SGX535_PDS_USE_LINK_ERR_ARGUMENT);
    CHECK(sgx535_pds_use_link_project(&input, NULL) == SGX535_PDS_USE_LINK_ERR_ARGUMENT);
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
    CHECK(output.bank_selector_fields == 3);
    CHECK(output.entry_high_fields == 0x20);
    CHECK(output.entry_low_fields == 0x23400);
    CHECK(output.address_fields == 0x23423);
    before = output;
    input.data_master = 1;
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
    CHECK(!memcmp(&output, &before, sizeof(output)));

    for (base = 0; base < sizeof(bases) / sizeof(bases[0]); base++) {
        for (bank = 0; bank < 16; bank++) {
            for (offset = 0; offset < 0x80000; offset += 16) {
                input = (struct sgx535_use_code_bank_input){bases[base] + offset, 8,
                                                            bases[base] + offset, bank, 0};
                CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
                CHECK(output.bank_selector_fields == bank);
                CHECK(output.address_fields == bit_oracle(bank, offset));
                CHECK(!(output.address_fields & ~UINT32_C(0x7ffff)));
                CHECK(output.address_fields ==
                      (output.bank_selector_fields | output.entry_high_fields |
                       output.entry_low_fields));
                CHECK(!(output.entry_high_fields & ~UINT32_C(0xf0)) &&
                      !(output.entry_low_fields & ~UINT32_C(0x7ff00)));
            }
        }
    }

    input = (struct sgx535_use_code_bank_input){0x40000008, 24, 0x40000010, 4, 0};
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
    CHECK(output.address_fields == bit_oracle(4, 16));
    input.entry_va_bytes = 0x40000008; /* Eight-byte USE alignment is insufficient. */
    CHECK(!refused(input));
    /* Only the entry must stay in the code start's 32-KiB block. */
    input = (struct sgx535_use_code_bank_input){0x40007ff0, 32, 0x40007ff0, 3, 0};
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
    CHECK(output.address_fields == bit_oracle(3, 0x7ff0));
    input.entry_va_bytes = 0x40008000;
    CHECK(!refused(input));
    input = (struct sgx535_use_code_bank_input){0x40007fd0, 48, 0x40007ff0, 3, 0};
    CHECK(sgx535_pds_use_link_project(&input, &output) == SGX535_PDS_USE_LINK_OK);
    CHECK(output.address_fields == bit_oracle(3, 0x7ff0));
    input = (struct sgx535_use_code_bank_input){0x40000000, 0x80000, 0x40000000, 3, 0};
    CHECK(sgx535_pds_use_link_project(&input, &output) ==
          SGX535_PDS_USE_LINK_OK); /* A whole bank. */
    input.code_size_bytes = 0x80008;
    CHECK(!refused(input)); /* One instruction past the bank end. */
    input.code_size_bytes = 8;
    input.entry_va_bytes = 0x40000010;
    CHECK(!refused(input)); /* Aligned entry outside the complete code extent. */
    input.entry_va_bytes = 0x3ffffff0;
    CHECK(!refused(input));
    input.entry_va_bytes = 0x40000000;
    input.code_size_bytes = 0;
    CHECK(!refused(input));
    input.code_size_bytes = 9;
    CHECK(!refused(input));
    input.code_size_bytes = 8;
    input.bank_index = 16;
    CHECK(!refused(input));
    input.bank_index = UINT32_MAX;
    CHECK(!refused(input));
    input.bank_index = 3;
    input.data_master = 2;
    CHECK(!refused(input));
    input.data_master = UINT32_MAX;
    CHECK(!refused(input));
    input = (struct sgx535_use_code_bank_input){0xfffffff0, 32, 0xfffffff0, 3, 0};
    CHECK(!refused(input));

    overlap.input = (struct sgx535_use_code_bank_input){0x40012340, 8, 0x40012340, 3, 0};
    CHECK(sgx535_pds_use_link_project(&overlap.input, &overlap.output) == SGX535_PDS_USE_LINK_OK);
    CHECK(overlap.output.address_fields == 0x23423);
    printf("PDS USE link CPU checks: %u; hardware qualification: none\n", checks);
    return 0;
}
