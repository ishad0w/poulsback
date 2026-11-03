// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_linear.h"
#include "sgx535_usse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static struct sgx535_nir_program output, unchanged;
static void rejected(const struct sgx535_linear_instruction *input, size_t count,
                     enum sgx535_linear_profile mode, enum sgx535_nir_status expected)
{
    memset(&output, 0xa5, sizeof(output));
    unchanged = output;
    CHECK(sgx535_linear_compile(input, count, mode, &output) == expected);
    CHECK(memcmp(&output, &unchanged, sizeof(output)) == 0);
}

static uint32_t seed = UINT32_C(0xd193af67);
static uint32_t random_word(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* Independent CPU word interpreter for the integer profile. */
static uint32_t execute_integer(const struct sgx535_nir_program *program)
{
    uint32_t values[SGX535_NIR_TEMP_COUNT] = {0};
    unsigned initialized[SGX535_NIR_TEMP_COUNT] = {0};
    unsigned i;
    uint32_t result = 0;
    CHECK(program->word_count && !(program->word_count % 2u));
    for (i = 0; i < program->word_count; i += 2u) {
        uint32_t low = program->words[i], high = program->words[i + 1u];
        unsigned destination = (low >> 21) & 127u;
        if ((high & UINT32_C(0xfffc0e0f)) == UINT32_C(0xfca00000)) {
            CHECK(destination < SGX535_NIR_TEMP_COUNT);
            values[destination] = (low & UINT32_C(0x001fffff)) | ((high >> 4) & 31u) << 21 |
                                  ((high >> 12) & 63u) << 26;
            initialized[destination] = 1;
        } else {
            unsigned source = (low >> 7) & 127u;
            CHECK(source < SGX535_NIR_TEMP_COUNT && initialized[source]);
            if (high == UINT32_C(0x28a50001)) {
                CHECK(destination == 0 && i + 2u == program->word_count);
                result = values[source];
            } else {
                CHECK(high == UINT32_C(0x28a10000) && destination < SGX535_NIR_TEMP_COUNT);
                CHECK(source != destination);
                values[destination] = values[source];
                initialized[destination] = 1;
            }
        }
    }
    CHECK(program->words[program->word_count - 1u] == UINT32_C(0x28a50001));
    return result;
}

static void graph_cases(void)
{
    struct sgx535_linear_instruction stream[96];
    uint32_t expected[96];
    unsigned trial, i;
    for (trial = 0; trial < 2000u; ++trial) {
        unsigned definitions = 2u + random_word() % 90u;
        unsigned selected = random_word() % definitions;
        memset(stream, 0, sizeof(stream));
        for (i = 0; i < definitions; ++i) {
            if (!i || random_word() % 3u == 0u) {
                stream[i].opcode = SGX535_LINEAR_CONSTANT;
                expected[i] = stream[i].immediate = random_word();
            } else {
                unsigned source = random_word() % i;
                stream[i].opcode = SGX535_LINEAR_MOV;
                stream[i].source[0] = source;
                expected[i] = expected[source];
            }
        }
        stream[definitions].opcode = SGX535_LINEAR_OUTPUT;
        stream[definitions].source[0] = selected;
        CHECK(sgx535_linear_compile(stream, definitions + 1u, SGX535_LINEAR_INTEGER, &output) ==
              SGX535_NIR_OK);
        CHECK(execute_integer(&output) == expected[selected]);
        CHECK(output.word_count == 2u * (definitions + 1u));
        CHECK(output.peak_live_temps <= output.temp_register_count &&
              output.temp_register_count <= SGX535_NIR_TEMP_COUNT);
    }
}

int main(void)
{
    struct sgx535_linear_instruction stream[SGX535_NIR_MAX_INSTRUCTIONS] = {0};
    struct sgx535_mov move;
    struct sgx535_limm constant;
    unsigned i;
    stream[0].immediate = UINT32_C(0xffff00ff);
    stream[1].opcode = SGX535_LINEAR_MOV;
    stream[2].opcode = SGX535_LINEAR_OUTPUT;
    stream[2].source[0] = 1;
    CHECK(sgx535_linear_compile(stream, 3, SGX535_LINEAR_INTEGER, &output) == SGX535_NIR_OK);
    CHECK(output.word_count == 6 && output.peak_live_temps == 2 && output.temp_register_count == 2);
    CHECK(sgx535_limm_decode(output.words, &constant) == SGX535_LIMM_OK);
    CHECK(constant.immediate == UINT32_C(0xffff00ff) && constant.end == 0);
    CHECK(sgx535_mov_decode(output.words + 4, &move) == SGX535_MOV_OK);
    CHECK(move.destination.bank == SGX535_MOV_OUTPUT && move.destination.number == 0 &&
          move.source.number == 1 && move.end == 1);
    rejected(stream, 0, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_LIMIT);
    rejected(stream, 513, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_LIMIT);
    rejected(NULL, 3, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_ARGUMENT);
    stream[1].source[0] = 1;
    rejected(stream, 3, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_SSA);
    stream[1].source[0] = 0;
    stream[1].opcode = SGX535_LINEAR_RCP;
    rejected(stream, 3, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(sgx535_linear_compile(stream, 3, SGX535_LINEAR_F32_WORD_PROJECTION, &output) ==
          SGX535_NIR_OK);
    CHECK(output.words[3] == UINT32_C(0x08a00000));
    stream[1].source[2] = 1;
    rejected(stream, 3, SGX535_LINEAR_F32_WORD_PROJECTION, SGX535_NIR_ERR_UNSUPPORTED);
    memset(stream, 0, sizeof(stream));
    for (i = 1; i < SGX535_NIR_MAX_INSTRUCTIONS; ++i) {
        stream[i].opcode = SGX535_LINEAR_MOV;
        stream[i].source[0] = i - 1;
    }
    stream[511].opcode = SGX535_LINEAR_OUTPUT;
    CHECK(sgx535_linear_compile(stream, 512, SGX535_LINEAR_INTEGER, &output) == SGX535_NIR_OK);
    CHECK(output.word_count == 1024 && output.peak_live_temps == 2);
    memset(stream, 0, sizeof(stream));
    for (i = 124; i < 248; ++i) {
        stream[i].opcode = SGX535_LINEAR_MOV;
        stream[i].source[0] = i - 124;
    }
    stream[248].opcode = SGX535_LINEAR_OUTPUT;
    stream[248].source[0] = 247;
    rejected(stream, 249, SGX535_LINEAR_INTEGER, SGX535_NIR_ERR_PRESSURE);
    graph_cases();
    printf("Linear compiler CPU checks: %u; floating semantics unqualified\n", checks);
    return 0;
}
