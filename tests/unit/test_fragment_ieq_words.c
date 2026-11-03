// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment.h"
#include "sgx535_fragment_raw_sign.h"
#include "sgx535_bitwise.h"
#include "sgx535_usse.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define POULSBACK_TEST_RAW_IEQ
#include "fragment_raw_sign_reference.h"

/* Independent source-field consumer, not the production encoder/decoder. */
static bool mask_interval(const uint32_t words[6], unsigned a, unsigned b, unsigned d,
                          uint32_t test_base, uint32_t test_control)
{
    unsigned i;
    if (words[1] != test_control || (words[0] & ~0x3fffu) != test_base ||
        ((words[0] >> 7) & 127u) != a || (words[0] & 127u) != b)
        return false;
    for (i = 0; i < 2; i++) {
        uint32_t lo = words[2 + i * 2], hi = words[3 + i * 2];
        uint32_t payload = (lo & 0x1fffffu) | ((hi >> 4) & 31u) << 21 | ((hi >> 12) & 63u) << 26;
        if ((lo >> 28) || ((lo >> 21) & 127u) != d || (hi & ~0x3fff0u) != 0xfca00000u ||
            (hi & 0xe00u) != (i ? 0x400u : 0) || payload != (i ? UINT32_MAX : 0))
            return false;
    }
    return a < 124 && b < 124 && d < 124;
}

static bool interval(const uint32_t words[6], unsigned a, unsigned b, unsigned d)
{
    return mask_interval(words, a, b, d, 0x001c8000u, 0x48801184u);
}

static void less_candidate(void)
{
    static const uint32_t registers[] = {0, 1, 63, 122, 123};
    /* Selected source-emitter projection, not a captured GPU observation. */
    static const uint32_t expected[6] = {0x00138389u, 0x48801684u, 0x00a00000u,
                                         0xfca00000u, 0x00bfffffu, 0xfca3f5f0u};
    struct sgx535_f32_less_candidate literal = {5, {7, 9}}, saved;
    uint32_t words[8], mutant[6], before[8];
    memset(words, 0x69, sizeof(words));
    CHECK(sgx535_f32_less_candidate_encode(&literal, words + 1) == SGX535_F32_LESS_CANDIDATE_OK);
    CHECK(!memcmp(words + 1, expected, sizeof(expected)));
    CHECK(words[0] == 0x69696969u && words[7] == 0x69696969u);
    for (size_t d = 0; d < sizeof(registers) / sizeof(*registers); d++)
        for (size_t a = 0; a < sizeof(registers) / sizeof(*registers); a++)
            for (size_t b = 0; b < sizeof(registers) / sizeof(*registers); b++) {
                struct sgx535_f32_less_candidate request = {registers[d],
                                                            {registers[a], registers[b]}};
                saved = request;
                CHECK(sgx535_f32_less_candidate_encode(&request, words + 1) ==
                      SGX535_F32_LESS_CANDIDATE_OK);
                CHECK(!memcmp(&request, &saved, sizeof(request)));
                CHECK(mask_interval(words + 1, registers[a], registers[b], registers[d],
                                    0x00138000u, 0x48801684u));
                CHECK(words[0] == 0x69696969u && words[7] == 0x69696969u);
                /* Exact TEST and two-write interval: foreign predicate,
                 * format/bank, modifiers, END and all extra bits refuse. */
                for (unsigned word = 0; word < 6; word++)
                    for (unsigned bit = 0; bit < 32; bit++) {
                        memcpy(mutant, words + 1, sizeof(mutant));
                        mutant[word] ^= UINT32_C(1) << bit;
                        CHECK(!mask_interval(mutant, registers[a], registers[b], registers[d],
                                             0x00138000u, 0x48801684u));
                    }
            }
    for (unsigned field = 0; field < 3; field++)
        for (unsigned large = 0; large < 2; large++) {
            struct sgx535_f32_less_candidate request = literal;
            uint32_t value = large ? UINT32_MAX : 124;
            if (!field)
                request.destination_temp = value;
            else
                request.source_temp[field - 1] = value;
            memcpy(before, words, sizeof(words));
            CHECK(sgx535_f32_less_candidate_encode(&request, words + 1) ==
                  SGX535_F32_LESS_CANDIDATE_ERR_RANGE);
            CHECK(!memcmp(before, words, sizeof(words)));
        }
    CHECK(sgx535_f32_less_candidate_encode(NULL, words + 1) ==
          SGX535_F32_LESS_CANDIDATE_ERR_ARGUMENT);
    CHECK(!memcmp(before, words, sizeof(words)));
    CHECK(sgx535_f32_less_candidate_encode(&literal, NULL) ==
          SGX535_F32_LESS_CANDIDATE_ERR_ARGUMENT);
    for (unsigned offset = 0; offset < 3; offset++) {
        union {
            struct sgx535_f32_less_candidate input;
            uint32_t words[8];
        } overlap;
        overlap.input = literal;
        CHECK(sgx535_f32_less_candidate_encode(&overlap.input, overlap.words + offset) ==
              SGX535_F32_LESS_CANDIDATE_OK);
        CHECK(!memcmp(overlap.words + offset, expected, sizeof(expected)));
    }
    /* Fields and layout only: this fixture does not interpret floating TEST. */
}

static void primitive(uint32_t a, uint32_t b, unsigned d)
{
    struct sgx535_raw_ieq request = {d, {0, 1}};
    struct sgx535_nir_program program = {0};
    uint32_t words[6], mutant[6], actual[4], primary[4] = {0};
    struct sgx535_limm value = {SGX535_LIMM_TEMP, 0, a, 0};
    unsigned i;
    CHECK(sgx535_raw_ieq_encode(&request, words) == SGX535_RAW_IEQ_OK);
    CHECK(interval(words, 0, 1, d));
    CHECK(sgx535_limm_encode(&value, program.words) == SGX535_LIMM_OK);
    value.register_number = 1;
    value.immediate = b;
    CHECK(sgx535_limm_encode(&value, program.words + 2) == SGX535_LIMM_OK);
    memcpy(program.words + 4, words, sizeof(words));
    program.word_count = 10;
    for (i = 0; i < 4; i++) {
        struct sgx535_mov output = {{SGX535_MOV_OUTPUT, i}, {SGX535_MOV_TEMP, d}, i == 3};
        CHECK(sgx535_mov_encode(&output, program.words + program.word_count) == SGX535_MOV_OK);
        program.word_count += 2;
    }
    raw_words(&program, primary, 0, actual);
    for (i = 0; i < 4; i++)
        CHECK(actual[i] == (a == b ? UINT32_MAX : 0));
    for (i = 0; i < 32; i++) {
        unsigned word;
        for (word = 0; word < 6; word++) {
            memcpy(mutant, words, sizeof(words));
            mutant[word] ^= UINT32_C(1) << i;
            CHECK(!interval(mutant, 0, 1, d));
        }
    }
}

static void scalar(uint32_t a, uint32_t b)
{
    struct sgx535_fragment_instruction ir[7] = {0};
    struct sgx535_fragment_program program, output, before;
    uint32_t actual[4], primary[4] = {0};
    unsigned i;
    ir[0].opcode = ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[0].immediate = a;
    ir[1].immediate = b;
    ir[2].opcode = SGX535_FRAGMENT_RAW_IEQ;
    ir[2].source[1] = 1;
    for (i = 0; i < 4; i++) {
        ir[i + 3].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 3].index = i;
        ir[i + 3].source[0] = 2;
    }
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, 7, &program) == SGX535_NIR_OK);
    CHECK(program.encoding.word_count == 18);
    raw_words(&program.encoding, primary, 0, actual);
    for (i = 0; i < 4; i++)
        CHECK(actual[i] == (a == b ? UINT32_MAX : 0));
    memset(&output, 0x45, sizeof(output));
    before = output;
    CHECK(sgx535_fragment_compile(ir, 7, SGX535_FRAGMENT_RAW_F32_WORDS, &output) ==
          SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!memcmp(&output, &before, sizeof(output)));
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 7, &output) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!memcmp(&output, &before, sizeof(output)));
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    program.arithmetic_qualified = program.linkage_qualified = program.output_qualified = 1;
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    ir[2].source[2] = 1;
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, 7, &output) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!memcmp(&output, &before, sizeof(output)));
}

static void limits(void)
{
    struct sgx535_fragment_instruction ir[175] = {0};
    struct sgx535_fragment_program output, before;
    unsigned count, i;
    for (count = 168; count <= 169; count++) {
        memset(ir, 0, sizeof(ir));
        ir[0].opcode = ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
        for (i = 2; i < count + 2; i++) {
            ir[i].opcode = SGX535_FRAGMENT_RAW_IEQ;
            ir[i].source[0] = i - 1;
        }
        for (i = 0; i < 4; i++) {
            ir[count + 2 + i].opcode = SGX535_FRAGMENT_OUTPUT;
            ir[count + 2 + i].index = i;
            ir[count + 2 + i].source[0] = count + 1;
        }
        memset(&output, 0x8d, sizeof(output));
        before = output;
        if (count == 168) {
            CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count + 6, &output) ==
                  SGX535_NIR_OK);
            CHECK(output.encoding.word_count == 1020);
        } else {
            CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count + 6, &output) ==
                  SGX535_NIR_ERR_LIMIT);
            CHECK(!memcmp(&output, &before, sizeof(output)));
        }
    }
    {
        struct sgx535_raw_ieq request = {124, {0, 1}};
        uint32_t words[6], before_words[6];
        memset(words, 0x42, sizeof(words));
        memcpy(before_words, words, sizeof(words));
        CHECK(sgx535_raw_ieq_encode(&request, words) == SGX535_RAW_IEQ_ERR_RANGE);
        CHECK(!memcmp(words, before_words, sizeof(words)));
        CHECK(sgx535_raw_ieq_encode(NULL, words) == SGX535_RAW_IEQ_ERR_ARGUMENT);
        CHECK(!memcmp(words, before_words, sizeof(words)));
    }
}

static void constant_shifts(void)
{
    static const uint32_t shifts[] = {1, 2, 4, 8, 16, 31};
    static const uint32_t payloads[] = {0, 1, UINT32_MAX, 0x80000000u, 0x7fc12345u};
    struct sgx535_fragment_instruction ir[6] = {0};
    struct sgx535_fragment_program program, output, before;
    ir[0].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[1].opcode = SGX535_FRAGMENT_RAW_USHR_IMM;
    for (unsigned c = 0; c < 4; c++) {
        ir[2 + c].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[2 + c].index = c;
        ir[2 + c].source[0] = 1;
    }
    memset(&output, 0x37, sizeof(output));
    before = output;
    for (unsigned k = 0; k < sizeof(shifts) / sizeof(*shifts); k++)
        for (unsigned p = 0; p < sizeof(payloads) / sizeof(*payloads); p++) {
            uint32_t literal, source, destination, value;
            ir[0].immediate = payloads[p];
            ir[1].immediate = shifts[k];
            CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, 6, &program) == SGX535_NIR_OK);
            CHECK(program.encoding.word_count == 12);
            /* Independent raw word consumer: one LIMM, one literal-count
             * logical SHR, four OUTPUT MOVs. CPU intent only, never GPU FP. */
            CHECK((program.encoding.words[1] & ~0x7f1f0u) == 0xfca00000u);
            literal = (program.encoding.words[0] & 0x1fffffu) |
                      ((program.encoding.words[1] >> 4) & 31u) << 21 |
                      ((program.encoding.words[1] >> 12) & 63u) << 26;
            CHECK(literal == payloads[p]);
            source = (program.encoding.words[2] >> 7) & 127u;
            destination = (program.encoding.words[2] >> 21) & 127u;
            CHECK(source == ((program.encoding.words[0] >> 21) & 127u));
            CHECK(source < 124 && destination < 124);
            CHECK(program.encoding.words[3] == 0x68a10000u);
            CHECK((program.encoding.words[2] & ~0x0fe03f80u) == (0x20000000u | shifts[k]));
            value = literal >> (program.encoding.words[2] & 127u);
            CHECK(value == (payloads[p] >> shifts[k]));
            for (unsigned c = 0; c < 4; c++) {
                uint32_t lo = program.encoding.words[4 + 2 * c];
                uint32_t hi = program.encoding.words[5 + 2 * c];
                CHECK(lo == 0x20000000u + c * 0x200000u + destination * 128u);
                CHECK(hi == (c == 3 ? 0x28a50001u : 0x28a10001u));
            }
            CHECK(!program.arithmetic_qualified && !program.linkage_qualified &&
                  !program.output_qualified);
            CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
            program.arithmetic_qualified = program.linkage_qualified = program.output_qualified = 1;
            CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(sgx535_fragment_compile(ir, 6, SGX535_FRAGMENT_RAW_F32_WORDS, &output) ==
                  SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(!memcmp(&output, &before, sizeof(output)));
            CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 6, &output) ==
                  SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(!memcmp(&output, &before, sizeof(output)));
        }
    for (unsigned field = 0; field < 8; field++) {
        struct sgx535_fragment_instruction bad[6];
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        memcpy(bad, ir, sizeof(bad));
        if (field == 0)
            bad[1].index = 1;
        else if (field == 1)
            bad[1].component = 1;
        else if (field == 2)
            bad[1].source[1] = 1;
        else if (field == 3)
            bad[1].source[2] = 1;
        else if (field == 4 || field == 5) {
            bad[1].source[0] = field == 4 ? 1 : UINT32_MAX;
            expected = SGX535_NIR_ERR_SSA;
        } else
            bad[1].immediate = field == 6 ? 0 : 3;
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(bad, 6, &output) == expected);
        CHECK(!memcmp(&output, &before, sizeof(output)));
    }
    {
        union {
            struct sgx535_fragment_instruction ir[6];
            struct sgx535_fragment_program output;
        } overlap;
        memcpy(overlap.ir, ir, sizeof(ir));
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, 6, &program) == SGX535_NIR_OK);
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(overlap.ir, 6, &overlap.output) ==
              SGX535_NIR_OK);
        CHECK(!memcmp(&overlap.output, &program, sizeof(program)));
    }
    {
        struct sgx535_fragment_instruction boundary[SGX535_NIR_MAX_INSTRUCTIONS + 1] = {0};
        unsigned count = SGX535_NIR_MAX_INSTRUCTIONS;
        boundary[0].opcode = SGX535_FRAGMENT_CONSTANT;
        for (unsigned i = 1; i < count - 4; i++) {
            boundary[i].opcode = SGX535_FRAGMENT_RAW_USHR_IMM;
            boundary[i].immediate = 1;
            boundary[i].source[0] = i - 1;
        }
        for (unsigned c = 0; c < 4; c++) {
            boundary[count - 4 + c].opcode = SGX535_FRAGMENT_OUTPUT;
            boundary[count - 4 + c].index = c;
            boundary[count - 4 + c].source[0] = count - 5;
        }
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(boundary, count, &program) ==
              SGX535_NIR_OK);
        CHECK(program.encoding.word_count == SGX535_NIR_MAX_WORDS);
        boundary[1].opcode = SGX535_FRAGMENT_RAW_IEQ;
        boundary[1].immediate = 0;
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(boundary, count, &output) ==
              SGX535_NIR_ERR_LIMIT);
        CHECK(!memcmp(&output, &before, sizeof(output)));
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(boundary, count + 1, &output) ==
              SGX535_NIR_ERR_LIMIT);
        CHECK(!memcmp(&output, &before, sizeof(output)));
    }
}

int main(void)
{
    static const uint32_t values[] = {0,           1,           2,           UINT32_MAX,
                                      0x80000000u, 0x7fffffffu, 0x7f800000u, 0xff800000u,
                                      0x7fc01234u, 0xaaaaaaaau};
    unsigned a, b, d;
    for (a = 0; a < sizeof(values) / sizeof(values[0]); a++)
        for (b = 0; b < sizeof(values) / sizeof(values[0]); b++) {
            scalar(values[a], values[b]);
            for (d = 0; d < 3; d++)
                primitive(values[a], values[b], d);
        }
    limits();
    less_candidate();
    constant_shifts();
    printf("raw IEQ TEST/P1/full-word consumer: %u checks PASS (CPU only; NIR NOTRUN)\n", checks);
    puts(
        "F32 LESS candidate TEST/P1 fields and extent PASS (encoding only; native FP NOTQUALIFIED)");
    return 0;
}
