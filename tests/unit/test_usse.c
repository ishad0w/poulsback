// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_linear.h"
#include "sgx535_usse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t state = UINT32_C(0x61c7e48a);
static uint32_t random_word(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void limm_and_constant(void)
{
    unsigned bank, reg, end, bit;
    const uint32_t magenta[2] = {UINT32_C(0x001f00ff), UINT32_C(0xfca7f1f1)};
    uint32_t words[2], decoded_argb, guard[2] = {23, 45};
    unsigned char bytes[12];
    struct sgx535_limm decoded;
    CHECK(sgx535_shader_encode_argb(UINT32_C(0xffff00ff), words) == SGX535_SHADER_OK);
    CHECK(memcmp(words, magenta, sizeof(words)) == 0);
    CHECK(sgx535_shader_decode_argb(words, &decoded_argb) == SGX535_SHADER_OK);
    CHECK(decoded_argb == UINT32_C(0xffff00ff));
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(sgx535_shader_pack_le(words, bytes + 1, 8) == SGX535_SHADER_OK);
    CHECK(bytes[0] == 0xa5 && bytes[9] == 0xa5);
    CHECK(bytes[1] == 0xff && bytes[2] == 0x00 && bytes[3] == 0x1f && bytes[4] == 0x00);
    CHECK(sgx535_shader_unpack_le(bytes + 1, 8, guard) == SGX535_SHADER_OK);
    CHECK(memcmp(words, guard, sizeof(words)) == 0);
    for (bank = 0; bank < 2; ++bank) {
        for (reg = 0; reg <= (bank ? 127u : 123u); ++reg) {
            for (end = 0; end < 2; ++end) {
                for (bit = 0; bit < 32; ++bit) {
                    struct sgx535_limm input = {(enum sgx535_limm_bank)bank, reg,
                                                UINT32_C(1) << bit, end};
                    CHECK(sgx535_limm_encode(&input, words) == SGX535_LIMM_OK);
                    CHECK(sgx535_limm_decode(words, &decoded) == SGX535_LIMM_OK);
                    CHECK(decoded.bank == input.bank && decoded.register_number == reg &&
                          decoded.immediate == input.immediate && decoded.end == end);
                }
            }
        }
    }
    {
        struct sgx535_limm input = {SGX535_LIMM_TEMP, 124, 0, 0};
        words[0] = 23;
        words[1] = 45;
        CHECK(sgx535_limm_encode(&input, words) == SGX535_LIMM_ERR_RANGE);
        CHECK(words[0] == 23 && words[1] == 45);
        input.bank = (enum sgx535_limm_bank) - 1;
        CHECK(sgx535_limm_encode(&input, words) == SGX535_LIMM_ERR_UNSUPPORTED);
        CHECK(sgx535_limm_encode(NULL, words) == SGX535_LIMM_ERR_ARGUMENT);
        CHECK(sgx535_shader_pack_le(words, bytes, 7) == SGX535_SHADER_ERR_BOUNDS);
    }
    for (bit = 0; bit < 64; ++bit) {
        struct sgx535_limm sentinel = {SGX535_LIMM_OUTPUT, 47, 123, 1};
        uint32_t corrupt[2] = {magenta[0], magenta[1]};
        corrupt[bit / 32] ^= UINT32_C(1) << (bit % 32);
        decoded = sentinel;
        if (sgx535_limm_decode(corrupt, &decoded) != SGX535_LIMM_OK)
            CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
    }
}

static void moves(void)
{
    unsigned destination_bank, source_bank, d, s;
    uint32_t words[2];
    for (destination_bank = 0; destination_bank < 2; ++destination_bank) {
        for (source_bank = 0; source_bank < 4; ++source_bank) {
            for (d = 0; d <= (destination_bank ? 127u : 123u); ++d) {
                for (s = 0; s <= (source_bank ? 127u : 123u); ++s) {
                    struct sgx535_mov input = {{(enum sgx535_mov_bank)destination_bank, d},
                                               {(enum sgx535_mov_bank)source_bank, s},
                                               (d + s) % 2};
                    struct sgx535_mov output;
                    CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_OK);
                    CHECK(sgx535_mov_decode(words, &output) == SGX535_MOV_OK);
                    CHECK(output.source.bank == input.source.bank && output.source.number == s &&
                          output.destination.bank == input.destination.bank &&
                          output.destination.number == d && output.end == input.end);
                }
            }
        }
    }
    {
        struct sgx535_mov input = {{SGX535_MOV_OUTPUT, 0}, {SGX535_MOV_TEMP, 0}, 1};
        CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_OK);
        CHECK(words[0] == UINT32_C(0x20000000) && words[1] == UINT32_C(0x28a50001));
        input.end = 2;
        CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_ERR_RANGE);
        CHECK(words[0] == UINT32_C(0x20000000) && words[1] == UINT32_C(0x28a50001));
        input.end = 1;
        input.source.number = 124;
        CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_ERR_RANGE);
        words[0] |= 1;
        CHECK(sgx535_mov_decode(words, &input) == SGX535_MOV_ERR_UNSUPPORTED);
        CHECK(input.source.number == 124);
    }
    {
        static const struct {
            struct sgx535_mov input;
            uint32_t words[2];
        } cases[] = {{{{SGX535_MOV_TEMP, 0}, {SGX535_MOV_PRIMARY, 0}, 0},
                      {UINT32_C(0xa0000000), UINT32_C(0x28a10000)}},
                     {{{SGX535_MOV_TEMP, 1}, {SGX535_MOV_PRIMARY, 3}, 0},
                      {UINT32_C(0xa0200180), UINT32_C(0x28a10000)}},
                     {{{SGX535_MOV_OUTPUT, 0}, {SGX535_MOV_SECONDARY, 127}, 1},
                      {UINT32_C(0xe0003f80), UINT32_C(0x28a50001)}}};
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct sgx535_mov decoded, sentinel = cases[i].input;
            unsigned bit;
            CHECK(sgx535_mov_encode(&cases[i].input, words) == SGX535_MOV_OK);
            CHECK(memcmp(words, cases[i].words, sizeof(words)) == 0);
            CHECK(sgx535_mov_decode(cases[i].words, &decoded) == SGX535_MOV_OK);
            CHECK(memcmp(&decoded, &cases[i].input, sizeof(decoded)) == 0);
            for (bit = 0; bit < 64; ++bit) {
                uint32_t altered[2] = {words[0], words[1]}, canonical[2];
                altered[bit / 32] ^= UINT32_C(1) << (bit % 32);
                decoded = sentinel;
                if (sgx535_mov_decode(altered, &decoded) == SGX535_MOV_OK) {
                    CHECK(sgx535_mov_encode(&decoded, canonical) == SGX535_MOV_OK);
                    CHECK(memcmp(altered, canonical, sizeof(canonical)) == 0);
                } else {
                    CHECK(memcmp(&decoded, &sentinel, sizeof(decoded)) == 0);
                }
            }
        }
        {
            struct sgx535_mov input = {{SGX535_MOV_TEMP, 0}, {SGX535_MOV_PRIMARY, 128}, 0};
            const uint32_t guard[2] = {23, 45};
            memcpy(words, guard, sizeof(guard));
            CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_ERR_RANGE);
            CHECK(memcmp(words, guard, sizeof(guard)) == 0);
            input.source.number = 127;
            input.source.bank = SGX535_MOV_SECONDARY;
            input.destination.bank = SGX535_MOV_PRIMARY;
            CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_ERR_UNSUPPORTED);
            CHECK(memcmp(words, guard, sizeof(guard)) == 0);
            input.destination.bank = SGX535_MOV_TEMP;
            input.source.bank = (enum sgx535_mov_bank)4;
            CHECK(sgx535_mov_encode(&input, words) == SGX535_MOV_ERR_UNSUPPORTED);
            CHECK(memcmp(words, guard, sizeof(guard)) == 0);
        }
    }
}

static void fractional_words(void)
{
    struct sgx535_f32 input = {SGX535_F32_FRC,
                               {SGX535_F32_TEMP, 5},
                               {{SGX535_F32_TEMP, 7}, {SGX535_F32_TEMP, 9}, {0}},
                               0},
                      saved = input;
    uint32_t words[4] = {0x13572468, 0, 0, 0x24681357};
    /* Matched FARITH/FRC two-source fields. Unary FRACT duplication belongs
     * to the scalar compiler; this codec does not claim arithmetic results. */
    CHECK(sgx535_f32_encode(&input, words + 1) == SGX535_F32_OK);
    CHECK(words[0] == 0x13572468 && words[3] == 0x24681357);
    CHECK(words[1] == 0x00a00389 && words[2] == 0x00a00600);
    CHECK(!memcmp(&input, &saved, sizeof(input)));
    input.source[2].number = 1;
    CHECK(sgx535_f32_encode(&input, words + 1) == SGX535_F32_ERR_UNSUPPORTED);
    CHECK(words[1] == 0x00a00389 && words[2] == 0x00a00600);
    input = saved;
    input.source[2].bank = SGX535_F32_OUTPUT;
    CHECK(sgx535_f32_encode(&input, words + 1) == SGX535_F32_ERR_UNSUPPORTED);
    input = saved;
    input.source[1].number = 124;
    CHECK(sgx535_f32_encode(&input, words + 1) == SGX535_F32_ERR_RANGE);
    input = saved;
    input.opcode = (enum sgx535_f32_opcode)6;
    CHECK(sgx535_f32_encode(&input, words + 1) == SGX535_F32_ERR_UNSUPPORTED);
    CHECK(words[1] == 0x00a00389 && words[2] == 0x00a00600);
    for (unsigned offset = 0; offset < sizeof(input) / sizeof(uint32_t) - 1; offset++) {
        union {
            struct sgx535_f32 input;
            uint32_t words[sizeof(input) / sizeof(uint32_t)];
        } alias;
        alias.input = saved;
        CHECK(sgx535_f32_encode(&alias.input, alias.words + offset) == SGX535_F32_OK);
        CHECK(alias.words[offset] == 0x00a00389 && alias.words[offset + 1] == 0x00a00600);
    }
}

static void arithmetic_and_store(void)
{
    unsigned family, trial, bit;
    uint32_t words[2];
    const uint32_t bases[] = {UINT32_C(0x00a00000), UINT32_C(0x18a00000), UINT32_C(0x18a00200),
                              UINT32_C(0x08a00000), UINT32_C(0x08a00200), UINT32_C(0x00a00600)};
    for (family = 0; family < sizeof(bases) / sizeof(bases[0]); ++family) {
        struct sgx535_f32 zero = {0};
        zero.opcode = (enum sgx535_f32_opcode)family;
        CHECK(sgx535_f32_encode(&zero, words) == SGX535_F32_OK);
        CHECK(words[0] == 0 && words[1] == bases[family]);
        for (trial = 0; trial < 4000; ++trial) {
            struct sgx535_f32 input = {0}, output = {0};
            unsigned i, count = family == 0 ? 3u : family == 3 || family == 4 ? 1u : 2u;
            input.opcode = (enum sgx535_f32_opcode)family;
            input.destination.bank = (enum sgx535_f32_bank)(random_word() % 2);
            input.destination.number = random_word() % (input.destination.bank ? 128u : 124u);
            input.end = random_word() % 2;
            for (i = 0; i < count; ++i) {
                input.source[i].bank =
                    (enum sgx535_f32_bank)(family == 0 && i == 0 ? 0 : random_word() % 2);
                input.source[i].number = random_word() % (input.source[i].bank ? 128u : 124u);
            }
            CHECK(sgx535_f32_encode(&input, words) == SGX535_F32_OK);
            CHECK(sgx535_f32_decode(words, &output) == SGX535_F32_OK);
            CHECK(memcmp(&input, &output, sizeof(input)) == 0);
            for (bit = 0; bit < 64; ++bit) {
                uint32_t altered[2] = {words[0], words[1]}, canonical[2];
                struct sgx535_f32 before = output;
                altered[bit / 32] ^= UINT32_C(1) << (bit % 32);
                if (sgx535_f32_decode(altered, &output) == SGX535_F32_OK) {
                    CHECK(sgx535_f32_encode(&output, canonical) == SGX535_F32_OK);
                    CHECK(memcmp(altered, canonical, sizeof(canonical)) == 0);
                } else
                    CHECK(memcmp(&before, &output, sizeof(output)) == 0);
            }
        }
    }
    for (family = 0; family < 2; ++family) {
        for (trial = 0; trial < 16000; ++trial) {
            struct sgx535_store input = {(enum sgx535_store_opcode)family, random_word() % 124,
                                         random_word() % 124, random_word() % 124},
                                output;
            CHECK(sgx535_store_encode(&input, words) == SGX535_STORE_OK);
            CHECK(sgx535_store_decode(words, &output) == SGX535_STORE_OK);
            CHECK(memcmp(&input, &output, sizeof(input)) == 0);
        }
    }
    {
        struct sgx535_f32 invalid = {0};
        invalid.opcode = SGX535_F32_RCP;
        invalid.source[2].number = 1;
        CHECK(sgx535_f32_encode(&invalid, words) == SGX535_F32_ERR_UNSUPPORTED);
        invalid.source[2].number = 0;
        invalid.destination.number = 124;
        CHECK(sgx535_f32_encode(&invalid, words) == SGX535_F32_ERR_RANGE);
        words[0] = 0;
        words[1] = UINT32_C(0xf0a40000);
        {
            struct sgx535_store output = {SGX535_STORE_STAD, 3, 4, 5}, before = output;
            CHECK(sgx535_store_decode(words, &output) == SGX535_STORE_ERR_UNSUPPORTED);
            CHECK(memcmp(&output, &before, sizeof(output)) == 0);
        }
    }
}

static void tasks_and_aliases(void)
{
    struct sgx535_pds_linear input = {{UINT32_C(0x12345678), UINT32_C(0x9abcdef0), 42}, 0}, output;
    uint32_t words[16];
    unsigned i;
    memset(words, 0xa5, sizeof(words));
    CHECK(sgx535_pds_linear_encode(&input, words, 16) == SGX535_PDS_LINEAR_OK);
    CHECK(words[0] == input.task_control[0] && words[1] == input.task_control[1] && words[8] == 42);
    CHECK(words[12] == UINT32_C(0x07000365) && words[13] == UINT32_C(0xaf000000));
    CHECK(words[14] == UINT32_C(0xa5a5a5a5) && words[15] == UINT32_C(0xa5a5a5a5));
    CHECK(sgx535_pds_linear_decode(words, 14, &output) == SGX535_PDS_LINEAR_OK);
    CHECK(memcmp(&input, &output, sizeof(input)) == 0);
    for (i = 2; i < 12; ++i) {
        if (i == 8)
            continue;
        words[i] = 1;
        CHECK(sgx535_pds_linear_decode(words, 14, &output) == SGX535_PDS_LINEAR_ERR_UNSUPPORTED);
        CHECK(memcmp(&input, &output, sizeof(input)) == 0);
        words[i] = 0;
    }
    CHECK(sgx535_pds_linear_encode(&input, words, 13) == SGX535_PDS_LINEAR_ERR_CAPACITY);
    input.iterator_count = 1;
    CHECK(sgx535_pds_linear_encode(&input, words, 14) == SGX535_PDS_LINEAR_ERR_UNSUPPORTED);
    {
        union {
            struct sgx535_limm instruction;
            uint32_t words[4];
        } alias;
        alias.instruction = (struct sgx535_limm){SGX535_LIMM_OUTPUT, 0, UINT32_C(0xffff00ff), 1};
        CHECK(sgx535_limm_encode(&alias.instruction, alias.words) == SGX535_LIMM_OK);
        CHECK(sgx535_limm_decode(alias.words, &alias.instruction) == SGX535_LIMM_OK);
        CHECK(alias.instruction.immediate == UINT32_C(0xffff00ff));
    }
}
int main(void)
{
    limm_and_constant();
    moves();
    arithmetic_and_store();
    fractional_words();
    tasks_and_aliases();
    printf("USSE/PDS CPU checks: %u; hardware dispatch: none\n", checks);
    return 0;
}
