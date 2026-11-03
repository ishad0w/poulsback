// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_bitwise.h"
#include "sgx535_fragment_raw_sign.h"
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

static void codec(void)
{
    static const struct {
        struct sgx535_bitwise op;
        uint32_t words[2];
    } literal[] = {{{SGX535_BITWISE_AND, 0, {0, 0}, 0}, {0, 0x50a00000}},
                   {{SGX535_BITWISE_XOR, 5, {7, 9}, 1}, {0x00a00389, 0x58a40000}},
                   {{SGX535_BITWISE_AND, 123, {121, 122}, 0}, {0x0f603cfa, 0x50a00000}},
                   {{SGX535_BITWISE_XOR, 1, {2, 3}, 0}, {0x00200103, 0x58a00000}}};
    struct sgx535_bitwise decoded, old;
    uint32_t words[2], before[2];
    size_t i;
    unsigned field, bit;
    for (i = 0; i < sizeof(literal) / sizeof(*literal); i++) {
        CHECK(sgx535_bitwise_encode(&literal[i].op, words) == SGX535_BITWISE_OK);
        CHECK(!memcmp(words, literal[i].words, sizeof(words)));
        CHECK(sgx535_bitwise_decode(literal[i].words, &decoded) == SGX535_BITWISE_OK);
        CHECK(!memcmp(&decoded, &literal[i].op, sizeof(decoded)));
    }
    /* Bits outside the admitted direct TEMP fields are never normalized. */
    for (field = 0; field < 2; field++) {
        uint32_t allowed = field ? 0x08040000u : 0x0fe03fffu;
        for (bit = 0; bit < 32; bit++) {
            if (allowed & (1u << bit))
                continue;
            memcpy(words, literal[0].words, sizeof(words));
            words[field] ^= 1u << bit;
            memset(&decoded, 0xa6, sizeof(decoded));
            old = decoded;
            CHECK(sgx535_bitwise_decode(words, &decoded) == SGX535_BITWISE_ERR_UNSUPPORTED);
            CHECK(!memcmp(&old, &decoded, sizeof(old)));
        }
    }
    for (field = 0; field < 4; field++) {
        struct sgx535_bitwise op = literal[1].op;
        if (field == 0)
            op.destination_temp = 124;
        if (field == 1)
            op.source_temp[0] = 124;
        if (field == 2)
            op.source_temp[1] = 124;
        if (field == 3)
            op.end = 2;
        words[0] = 0x12345678;
        words[1] = 0xabcdef01;
        memcpy(before, words, sizeof(words));
        CHECK(sgx535_bitwise_encode(&op, words) == SGX535_BITWISE_ERR_RANGE);
        CHECK(!memcmp(before, words, sizeof(words)));
    }
    decoded = literal[0].op;
    decoded.opcode = (enum sgx535_bitwise_opcode)2;
    CHECK(sgx535_bitwise_encode(&decoded, words) == SGX535_BITWISE_ERR_UNSUPPORTED);
    CHECK(sgx535_bitwise_encode(NULL, words) == SGX535_BITWISE_ERR_ARGUMENT);
    CHECK(sgx535_bitwise_encode(&decoded, NULL) == SGX535_BITWISE_ERR_ARGUMENT);
    CHECK(sgx535_bitwise_decode(NULL, &decoded) == SGX535_BITWISE_ERR_ARGUMENT);
    CHECK(sgx535_bitwise_decode(words, NULL) == SGX535_BITWISE_ERR_ARGUMENT);
    {
        union {
            struct sgx535_bitwise op;
            uint32_t words[5];
        } overlap;
        overlap.op = literal[1].op;
        CHECK(sgx535_bitwise_encode(&overlap.op, overlap.words) == SGX535_BITWISE_OK);
        CHECK(!memcmp(overlap.words, literal[1].words, 8));
        CHECK(sgx535_bitwise_decode(overlap.words, &overlap.op) == SGX535_BITWISE_OK);
        CHECK(!memcmp(&overlap.op, &literal[1].op, sizeof(overlap.op)));
    }
}

static void sign_ir(struct sgx535_fragment_instruction p[7], int absolute)
{
    unsigned c;
    memset(p, 0, sizeof(*p) * 7);
    p[0].opcode = SGX535_FRAGMENT_INPUT;
    p[1].opcode = SGX535_FRAGMENT_CONSTANT;
    p[1].immediate = absolute ? 0x7fffffffu : 0x80000000u;
    p[2].opcode = absolute ? SGX535_FRAGMENT_RAW_AND : SGX535_FRAGMENT_RAW_XOR;
    p[2].source[1] = 1;
    for (c = 0; c < 4; c++) {
        p[3 + c].opcode = SGX535_FRAGMENT_OUTPUT;
        p[3 + c].index = c;
        p[3 + c].source[0] = 2;
    }
}

#include "fragment_raw_sign_reference.h"

static void scalar_candidate(void)
{
    /* Literal bit expectations include both zero signs, subnormals, quiet and
     * signaling NaN payloads, infinities and adjacent finite values. These are
     * raw32 policy fixtures, not a claim about GPU NaN or rounding behavior. */
    static const uint32_t values[][3] = {
        {0x00000000, 0x80000000, 0x00000000}, {0x80000000, 0x00000000, 0x00000000},
        {0x7f800000, 0xff800000, 0x7f800000}, {0xff800000, 0x7f800000, 0x7f800000},
        {0x7fc12345, 0xffc12345, 0x7fc12345}, {0xffc12345, 0x7fc12345, 0x7fc12345},
        {0x7f812345, 0xff812345, 0x7f812345}, {0xff812345, 0x7f812345, 0x7f812345},
        {0x00000001, 0x80000001, 0x00000001}, {0x80000001, 0x00000001, 0x00000001},
        {0x007fffff, 0x807fffff, 0x007fffff}, {0x807fffff, 0x007fffff, 0x007fffff},
        {0x7f7fffff, 0xff7fffff, 0x7f7fffff}, {0xff7fffff, 0x7f7fffff, 0x7f7fffff},
        {0x3f800000, 0xbf800000, 0x3f800000}, {0xbf800000, 0x3f800000, 0x3f800000},
        {0x3f7fffff, 0xbf7fffff, 0x3f7fffff}, {0xbf7fffff, 0x3f7fffff, 0x3f7fffff},
        {0x3f800001, 0xbf800001, 0x3f800001}, {0xbf800001, 0x3f800001, 0x3f800001}};
    static const uint32_t neg_words[] = {0xa0000000, 0x28a10000, 0x00200000, 0xfca20000, 0x00400001,
                                         0x58a00000, 0x20000100, 0x28a10001, 0x20200100, 0x28a10001,
                                         0x20400100, 0x28a10001, 0x20600100, 0x28a50001};
    static const uint32_t abs_words[] = {0xa0000000, 0x28a10000, 0x003fffff, 0xfca1f1f0, 0x00400001,
                                         0x50a00000, 0x20000100, 0x28a10001, 0x20200100, 0x28a10001,
                                         0x20400100, 0x28a10001, 0x20600100, 0x28a50001};
    struct sgx535_fragment_instruction ir[7];
    struct sgx535_fragment_program output, before;
    struct sgx535_fragment_dri_color0_program dri;
    struct sgx535_fragment_linkage linkage = {0};
    unsigned absolute, c;
    size_t i;
    linkage.count = 1;
    linkage.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    for (absolute = 0; absolute < 2; absolute++) {
        const uint32_t *expected = absolute ? abs_words : neg_words;
        sign_ir(ir, (int)absolute);
        memset(&output, 0xa5, sizeof(output));
        before = output;
        CHECK(sgx535_fragment_compile(ir, 7, SGX535_FRAGMENT_RAW_F32_WORDS, &output) ==
              SGX535_NIR_ERR_UNSUPPORTED);
        CHECK(!memcmp(&output, &before, sizeof(output)));
        CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 7, &output) == SGX535_NIR_OK);
        CHECK(output.encoding.word_count == sizeof(neg_words) / sizeof(*neg_words));
        CHECK(!memcmp(output.encoding.words, expected, sizeof(neg_words)));
        CHECK(output.primary_register_count == 1 && output.input_count == 1);
        CHECK(!output.secondary_register_count &&
              output.output_profile == SGX535_FRAGMENT_RAW_F32_WORDS);
        CHECK(!output.arithmetic_qualified && !output.linkage_qualified &&
              !output.output_qualified);
        CHECK(sgx535_fragment_native_admission(&output) == SGX535_NIR_ERR_UNSUPPORTED);
        for (i = 0; i < sizeof(values) / sizeof(*values); i++) {
            uint32_t primary[4] = {values[i][0], 0, 0, 0}, result[4];
            raw_words(&output.encoding, primary, 0, result);
            for (c = 0; c < 4; c++)
                CHECK(result[c] == values[i][absolute + 1]);
        }
        ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
        ir[0].index = 127;
        CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 7, &output) == SGX535_NIR_OK);
        CHECK(output.secondary_register_count == 128 &&
              output.uniform_words_used[3] == 0x80000000u);
        for (i = 0; i < sizeof(values) / sizeof(*values); i++) {
            uint32_t primary[4] = {0}, result[4];
            raw_words(&output.encoding, primary, values[i][0], result);
            for (c = 0; c < 4; c++)
                CHECK(result[c] == values[i][absolute + 1]);
        }
        sign_ir(ir, (int)absolute);
        CHECK(sgx535_fragment_compile_dri_color0_raw_sign_candidate(ir, 7, &linkage, &dri) ==
              SGX535_NIR_OK);
        CHECK(dri.primary_register_count == 4 && dri.primary_component[0] == 2);
        CHECK(dri.encoding.words[0] == 0xa0000100u);
        CHECK(!dri.arithmetic_qualified && !dri.linkage_qualified && !dri.output_qualified);
        for (i = 0; i < sizeof(values) / sizeof(*values); i++) {
            uint32_t primary[4] = {0, 0, values[i][0], 0}, result[4];
            raw_words(&dri.encoding, primary, 0, result);
            for (c = 0; c < 4; c++)
                CHECK(result[c] == values[i][absolute + 1]);
        }
    }
    output.arithmetic_qualified = output.linkage_qualified = output.output_qualified = 1;
    CHECK(sgx535_fragment_native_admission(&output) == SGX535_NIR_ERR_UNSUPPORTED);
    dri.arithmetic_qualified = dri.linkage_qualified = dri.output_qualified = 1;
    CHECK(sgx535_fragment_dri_color0_native_admission(&dri) == SGX535_NIR_ERR_UNSUPPORTED);
}

static void failures(void)
{
    struct sgx535_fragment_instruction ir[7], bad[7], *wide;
    struct sgx535_fragment_program output, before;
    unsigned which, i, last;
    sign_ir(ir, 0);
    for (which = 0; which < 6; which++) {
        memcpy(bad, ir, sizeof(bad));
        if (which == 0)
            bad[2].source[0] = 2;
        if (which == 1)
            bad[2].source[2] = 1;
        if (which == 2)
            bad[2].immediate = 1;
        if (which == 3)
            bad[2].index = 1;
        if (which == 4)
            bad[2].component = 1;
        if (which == 5)
            bad[1].opcode = (enum sgx535_fragment_opcode)99;
        memset(&output, 0x5b, sizeof(output));
        before = output;
        CHECK(sgx535_fragment_compile_raw_sign_candidate(bad, 7, &output) ==
              (which == 0 ? SGX535_NIR_ERR_SSA : SGX535_NIR_ERR_UNSUPPORTED));
        CHECK(!memcmp(&before, &output, sizeof(output)));
    }
    CHECK(sgx535_fragment_compile_raw_sign_candidate(NULL, 7, &output) == SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 7, NULL) == SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 513, &output) == SGX535_NIR_ERR_LIMIT);
    wide = calloc(253, sizeof(*wide));
    CHECK(wide != NULL);
    for (i = 0; i < 125; i++)
        wide[i].opcode = SGX535_FRAGMENT_INPUT;
    last = 0;
    for (i = 1; i < 125; i++) {
        wide[124 + i].opcode = SGX535_FRAGMENT_RAW_XOR;
        wide[124 + i].source[0] = last;
        wide[124 + i].source[1] = i;
        last = 124 + i;
    }
    for (i = 0; i < 4; i++) {
        wide[249 + i].opcode = SGX535_FRAGMENT_OUTPUT;
        wide[249 + i].index = i;
        wide[249 + i].source[0] = last;
    }
    memset(&output, 0x7c, sizeof(output));
    before = output;
    CHECK(sgx535_fragment_compile_raw_sign_candidate(wide, 253, &output) ==
          SGX535_NIR_ERR_PRESSURE);
    CHECK(!memcmp(&before, &output, sizeof(output)));
    free(wide);
}

int main(void)
{
    codec();
    scalar_candidate();
    failures();
    printf("raw-sign codec/compiler candidate: %u checks PASS (CPU only)\n", checks);
    return 0;
}
