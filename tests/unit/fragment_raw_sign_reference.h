// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_TEST_RAW_SIGN_REFERENCE_H
#define POULSBACK_TEST_RAW_SIGN_REFERENCE_H
/* Unit-only raw-word interpreter, independent of the bitwise codec/decoder.
 * Logical shifts use their literal words. MAD uses HOST IEEE32 arithmetic for
 * CPU diagnostics only, including the floor path's exact integer subtraction;
 * it establishes no GPU arithmetic, memory, render or packet admission. */
static void raw_words_with_secondary(const struct sgx535_nir_program *program,
                                     const uint32_t primary[4], const uint32_t *secondary,
                                     unsigned first_secondary, unsigned secondary_count,
                                     uint32_t output[4])
{
    uint32_t temp[124] = {0};
    uint32_t valid[124] = {0};
    uint32_t written = 0;
#ifdef POULSBACK_TEST_RAW_IEQ
    unsigned predicate_valid = 0, predicate = 0;
#endif
    unsigned i;
    for (i = 0; i < program->word_count; i += 2) {
        uint32_t lo = program->words[i], hi = program->words[i + 1];
        unsigned dst = (lo >> 21) & 127u;
#ifdef POULSBACK_TEST_RAW_IEQ
        if (hi == 0x48801184u) {
            unsigned a = (lo >> 7) & 127u, b = lo & 127u;
            CHECK((lo & ~0x3fffu) == 0x001c8000u);
            CHECK(a < 124 && b < 124 && valid[a] && valid[b]);
            predicate = temp[a] == temp[b];
            predicate_valid = 1;
        } else
#endif
            if ((hi & 0xfff80000u) == 0xfca00000u) {
            uint32_t data =
                (lo & 0x1fffffu) | (((hi >> 4) & 31u) << 21) | (((hi >> 12) & 63u) << 26);
            CHECK(dst < 124 && !(hi & 3u));
#ifdef POULSBACK_TEST_RAW_IEQ
            CHECK(!(hi & 0xe00u) || ((hi & 0xe00u) == 0x400u && predicate_valid));
            if (!(hi & 0xe00u) || predicate) {
#endif
                temp[dst] = data;
                valid[dst] = 1;
#ifdef POULSBACK_TEST_RAW_IEQ
            }
#endif
        } else if ((hi & 0xfffbffffu) == 0x28a10000u || (hi & 0xfffbffffu) == 0x28a10001u) {
            unsigned bank = lo >> 30, source = (lo >> 7) & 127u;
            uint32_t data;
            if (bank == 0) {
                CHECK(source < 124 && valid[source]);
                data = temp[source];
            } else if (bank == 2) {
                CHECK(source < 4);
                data = primary[source];
            } else {
                CHECK(bank == 3 && source >= first_secondary &&
                      source - first_secondary < secondary_count);
                data = secondary[source - first_secondary];
            }
            if (hi & 1u) {
                CHECK(dst < 4 && !(written & (1u << dst)));
                written |= 1u << dst;
                output[dst] = data;
            } else {
                CHECK(dst < 124);
                temp[dst] = data;
                valid[dst] = 1;
            }
        } else if (hi == 0x68a10000u) {
            unsigned source = (lo >> 7) & 127u, amount = lo & 127u;
            CHECK((lo & ~UINT32_C(0x0fe03fff)) == UINT32_C(0x20000000));
            CHECK(dst < 124 && source < 124 && valid[source]);
            CHECK(amount == 1 || amount == 2 || amount == 4 || amount == 8 || amount == 16 ||
                  amount == 31);
            temp[dst] = temp[source] >> amount;
            valid[dst] = 1;
        } else if (hi == 0x00a00000u) {
            unsigned a = (lo >> 14) & 127u, b = (lo >> 7) & 127u, c = lo & 127u;
            float left, right, addend;
            CHECK(!(lo & UINT32_C(0xf0000000)) && dst < 124 && a < 124 && b < 124 && c < 124 &&
                  valid[a] && valid[b] && valid[c]);
            memcpy(&left, temp + a, sizeof(left));
            memcpy(&right, temp + b, sizeof(right));
            memcpy(&addend, temp + c, sizeof(addend));
            volatile float product = left * right;
            volatile float sum = product + addend;
            float result = sum;
            memcpy(temp + dst, &result, sizeof(result));
            valid[dst] = 1;
        } else {
            unsigned a = (lo >> 7) & 127u, b = lo & 127u;
            CHECK(dst < 124 && a < 124 && b < 124 && valid[a] && valid[b]);
            CHECK((lo & 0xf01fc000u) == 0);
            if (hi == 0x50a00000u)
                temp[dst] = temp[a] & temp[b];
            else {
                CHECK(hi == 0x58a00000u);
                temp[dst] = temp[a] ^ temp[b];
            }
            valid[dst] = 1;
        }
        CHECK((i + 2 == program->word_count) == !!(hi & 0x40000u));
    }
    CHECK(written == 15);
}

static void raw_words(const struct sgx535_nir_program *program, const uint32_t primary[4],
                      uint32_t secondary, uint32_t output[4])
{
    raw_words_with_secondary(program, primary, &secondary, 127, 1, output);
}

#endif
