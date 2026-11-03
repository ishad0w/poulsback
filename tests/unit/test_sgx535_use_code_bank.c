// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_use_code_bank.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void check_projection(const struct sgx535_use_code_bank_input *request)
{
    struct sgx535_use_code_bank_projection out;
    uint64_t base = ((uint64_t)request->code_va_bytes / 524288u) * 524288u;
    CHECK(sgx535_use_code_bank_project(request, &out) == SGX535_USE_CODE_BANK_OK);
    CHECK(out.bank_base_va_bytes == base);
    CHECK(out.entry_offset_bytes == (uint64_t)request->entry_va_bytes - base);
    CHECK(out.register_offset_bytes == 2572u + 4u * request->bank_index);
    CHECK(out.register_word == base / 128u + request->data_master * 33554432u);
}

static void literal_and_ranges(void)
{
    struct sgx535_use_code_bank_input r = {0x40001000u, 24, 0x40001008u, 3, 1};
    struct sgx535_use_code_bank_projection out;
    uint32_t bank, index, offset;
    CHECK(sgx535_use_code_bank_project(&r, &out) == SGX535_USE_CODE_BANK_OK);
    CHECK(out.bank_base_va_bytes == 0x40000000u && out.entry_offset_bytes == 0x1008u);
    CHECK(out.register_offset_bytes == 0xa18u && out.register_word == 0x02800000u);
    /* Exercise every base field value, including a final bank whose exclusive
     * boundary is 2^32. The oracle uses division, not production bit splitting.
     */
    for (bank = 0; bank < 8192u; ++bank) {
        r.code_va_bytes = bank * 524288u;
        r.code_size_bytes = 524288u;
        for (index = 0; index < 16u; ++index) {
            r.bank_index = index;
            r.data_master = index % 2u;
            r.entry_va_bytes = r.code_va_bytes + 8u * index;
            check_projection(&r);
            r.entry_va_bytes = r.code_va_bytes + 524280u;
            check_projection(&r);
        }
    }
    /* Every instruction start in a bank, up to the last one, with nonzero
     * code origin and both ordinary data masters.
     */
    for (offset = 0; offset < 524288u; offset += 8u) {
        r.code_va_bytes = 0xfff80000u + offset;
        r.code_size_bytes = 8u;
        r.entry_va_bytes = r.code_va_bytes;
        r.bank_index = offset / 8u % 16u;
        r.data_master = offset / 8u % 2u;
        check_projection(&r);
    }
}

static void expect_refusal(const struct sgx535_use_code_bank_input *r)
{
    struct sgx535_use_code_bank_projection out, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    out = sentinel;
    CHECK(sgx535_use_code_bank_project(r, &out) == SGX535_USE_CODE_BANK_RANGE);
    CHECK(!memcmp(&out, &sentinel, sizeof(out)));
}

static void refusals_and_overlap(void)
{
    const struct sgx535_use_code_bank_input good = {0x40000008u, 24, 0x40000010u, 15, 1};
    struct sgx535_use_code_bank_input r;
    struct sgx535_use_code_bank_projection out, expected;
    union {
        struct sgx535_use_code_bank_input input;
        struct sgx535_use_code_bank_projection output;
    } overlap;
    uint32_t bit;
    CHECK(sgx535_use_code_bank_project(NULL, &out) == SGX535_USE_CODE_BANK_INVALID);
    CHECK(sgx535_use_code_bank_project(&good, NULL) == SGX535_USE_CODE_BANK_INVALID);
    CHECK(sgx535_use_code_bank_project(NULL, NULL) == SGX535_USE_CODE_BANK_INVALID);
    r = good;
    r.code_size_bytes = 0;
    expect_refusal(&r);
    for (bit = 1; bit < 8; bit *= 2) {
        r = good;
        r.code_va_bytes |= bit;
        expect_refusal(&r);
        r = good;
        r.code_size_bytes |= bit;
        expect_refusal(&r);
        r = good;
        r.entry_va_bytes |= bit;
        expect_refusal(&r);
    }
    r = good;
    r.bank_index = 16;
    expect_refusal(&r);
    r = good;
    r.bank_index = UINT32_MAX;
    expect_refusal(&r);
    r = good;
    r.data_master = 2;
    expect_refusal(&r);
    r = good;
    r.data_master = 3;
    expect_refusal(&r);
    r = good;
    r.data_master = UINT32_MAX;
    expect_refusal(&r);
    r = good;
    r.entry_va_bytes = r.code_va_bytes - 8;
    expect_refusal(&r);
    r = good;
    r.entry_va_bytes = r.code_va_bytes + r.code_size_bytes;
    expect_refusal(&r);
    r = good;
    r.code_size_bytes = UINT32_MAX - 7u;
    expect_refusal(&r);
    r = good;
    r.code_va_bytes = 0x4007fff8u;
    r.entry_va_bytes = r.code_va_bytes;
    r.code_size_bytes = 8;
    check_projection(&r); /* Exact bank end. */
    r.code_size_bytes = 16;
    expect_refusal(&r); /* Cross-bank extent. */
    r.code_va_bytes = 0xfffffff8u;
    r.entry_va_bytes = r.code_va_bytes;
    r.code_size_bytes = 8;
    check_projection(&r); /* Exact 2^32 end. */
    r.code_size_bytes = 16;
    expect_refusal(&r); /* Wrapped extent. */
    CHECK(sgx535_use_code_bank_project(&good, &expected) == SGX535_USE_CODE_BANK_OK);
    overlap.input = good;
    CHECK(sgx535_use_code_bank_project(&overlap.input, &overlap.output) == SGX535_USE_CODE_BANK_OK);
    CHECK(!memcmp(&overlap.output, &expected, sizeof(expected)));
    overlap.input = good;
    overlap.input.data_master = 3;
    r = overlap.input;
    CHECK(sgx535_use_code_bank_project(&overlap.input, &overlap.output) ==
          SGX535_USE_CODE_BANK_RANGE);
    CHECK(!memcmp(&overlap.input, &r, sizeof(r)));
}

int main(void)
{
    literal_and_ranges();
    refusals_and_overlap();
    printf("PASS %lu USE code-bank CPU projection checks; no hardware permission\n", checks);
    return 0;
}
