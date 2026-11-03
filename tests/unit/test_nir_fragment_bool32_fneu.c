// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa FNEU producer; the IEEE truth reference uses no packet words. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

struct fneu_pair {
    uint32_t left, right;
};
static const struct fneu_pair fneu_pairs[] = {
    {0, UINT32_C(0x80000000)},
    {UINT32_C(0x80000000), 0},
    {UINT32_C(0x80000000), UINT32_C(0x80000000)},
    {0, 0},
    {UINT32_C(0x7fc12345), UINT32_C(0x7fc12345)},
    {UINT32_C(0x7f812345), UINT32_C(0x7f812345)},
    {UINT32_C(0x7fc12345), UINT32_C(0x7f812345)},
    {UINT32_C(0xffc12345), UINT32_C(0xffc12345)},
    {UINT32_C(0xff812345), UINT32_C(0xff812345)},
    {UINT32_C(0x7fc12345), UINT32_C(0x3f800000)},
    {UINT32_C(0x3f800000), UINT32_C(0xffc54321)},
    {UINT32_C(0x7f800001), 0},
    {0, UINT32_C(0xff800001)},
    {UINT32_C(0x7f800000), UINT32_C(0x7f800000)},
    {UINT32_C(0x7f800000), UINT32_C(0xff800000)},
    {UINT32_C(0xff800000), UINT32_C(0xff800000)},
    {UINT32_C(0x7f7fffff), UINT32_C(0x7f800000)},
    {1, 0},
    {UINT32_C(0x80000001), UINT32_C(0x80000000)},
    {UINT32_C(0x007fffff), UINT32_C(0x00800000)},
    {UINT32_C(0x3f800000), UINT32_C(0x3f800000)},
    {UINT32_C(0x3f800000), UINT32_C(0xbf800000)},
    {UINT32_C(0x3f800001), UINT32_C(0x3f800000)},
    {UINT32_C(0xbf800001), UINT32_C(0xbf800000)},
};

static bool fneu_float_reference(uint32_t left, uint32_t right)
{
    float a, b;
    memcpy(&a, &left, sizeof(a));
    memcpy(&b, &right, sizeof(b));
    return a != b;
}

static bool fneu_bit_model(uint32_t left, uint32_t right)
{
    bool both_zero = !(left & UINT32_C(0x7fffffff)) && !(right & UINT32_C(0x7fffffff));
    bool left_nan =
        (left & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) && (left & UINT32_C(0x007fffff)) != 0;
    bool right_nan = (right & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
                     (right & UINT32_C(0x007fffff)) != 0;
    return (!(left == right || both_zero)) || left_nan || right_nan;
}

static void fneu_model_check(uint32_t left, uint32_t right)
{
    if (fneu_float_reference(left, right) != fneu_bit_model(left, right)) {
        fprintf(stderr, "IEEE32 FNEU truth differs for %08x/%08x\n", left, right);
        abort();
    }
}

static unsigned fneu_reference_checks(void)
{
    static const uint32_t values[] = {
        0,
        UINT32_C(0x80000000),
        1,
        UINT32_C(0x80000001),
        UINT32_C(0x007fffff),
        UINT32_C(0x807fffff),
        UINT32_C(0x00800000),
        UINT32_C(0x80800000),
        UINT32_C(0x3f800000),
        UINT32_C(0xbf800000),
        UINT32_C(0x3f800001),
        UINT32_C(0xbf800001),
        UINT32_C(0x7f7fffff),
        UINT32_C(0xff7fffff),
        UINT32_C(0x7f800000),
        UINT32_C(0xff800000),
        UINT32_C(0x7f800001),
        UINT32_C(0xff800001),
        UINT32_C(0x7fbfffff),
        UINT32_C(0xffbfffff),
        UINT32_C(0x7fc00000),
        UINT32_C(0xffc00000),
        UINT32_C(0x7fc12345),
        UINT32_C(0xffc12345),
        UINT32_C(0x7fffffff),
        UINT32_MAX,
    };
    unsigned i, j, count = 0;
    uint32_t state = UINT32_C(0x17d4a392);
    if (sizeof(float) != 4 || FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128) {
        fputs("IEEE32 float reference unavailable\n", stderr);
        abort();
    }
    for (i = 0; i < sizeof(values) / sizeof(*values); i++)
        for (j = 0; j < sizeof(values) / sizeof(*values); j++) {
            fneu_model_check(values[i], values[j]);
            count++;
        }
    for (i = 0; i < sizeof(fneu_pairs) / sizeof(*fneu_pairs); i++) {
        fneu_model_check(fneu_pairs[i].left, fneu_pairs[i].right);
        count++;
    }
    for (i = 0; i < 65536; i++) {
        uint32_t left, right;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        left = state;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        right = state;
        fneu_model_check(left, right);
        fneu_model_check(left, left);
        count += 2;
    }
    return count;
}

#ifdef POULSBACK_FNEU_REFERENCE_ONLY
int main(void)
{
    printf("independent IEEE32 FNEU truth: %u checks PASS (model only)\n", fneu_reference_checks());
    return 0;
}
#else
int fneu_legacy_ior_main(void);
#define POULSBACK_IOR_TEST_MAIN fneu_legacy_ior_main
#include "test_nir_fragment_bool32_ior.c"
#undef POULSBACK_IOR_TEST_MAIN

static void fneu_positive(unsigned width, unsigned pair, unsigned controls)
{
    nir_builder b = bool32_builder();
    nir_def *left[4], *right[4], *out[4], *a, *other, *value;
    nir_alu_instr *op;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t expected[4], actual[4], primary[4] = {0};
    unsigned lane, ands = 0, xors = 0, comparisons = 0, constant_words = 0;
    bool alias = pair == 4 || pair == 5;
    bool integer = (pair & 1u) != 0, invert = (pair & 2u) != 0;
    size_t count = 0, i;
    char *before, *after;
    for (lane = 0; lane < width; lane++) {
        unsigned index = (pair + lane) % 24;
        left[lane] = nir_imm_intN_t(&b, fneu_pairs[index].left, 32);
        right[lane] = nir_imm_intN_t(&b, fneu_pairs[index].right, 32);
    }
    a = nir_vec(&b, left, width);
    other = alias ? a : nir_vec(&b, right, width);
    value = nir_fneu(&b, a, other);
    op = nir_def_as_alu(value);
    op->fp_math_ctrl = controls;
    for (lane = 0; lane < width; lane++) {
        op->src[0].swizzle[lane] = width - 1u - lane;
        op->src[1].swizzle[lane] = (lane + 1u) % width;
    }
    if (invert)
        value = nir_inot(&b, value);
    value = integer ? nir_b2i32(&b, value) : nir_b2f32(&b, value);
    for (lane = 0; lane < 4; lane++) {
        unsigned source = (3u - lane) % width;
        unsigned l = (pair + width - 1u - source) % 24;
        unsigned r = (pair + (source + 1u) % width) % 24;
        bool truth = fneu_float_reference(fneu_pairs[l].left,
                                          alias ? fneu_pairs[r].left : fneu_pairs[r].right);
        if (invert)
            truth = !truth;
        expected[lane] = truth ? (integer ? 1u : UINT32_C(0x3f800000)) : 0;
        out[lane] = nir_channel(&b, value, source);
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "raw-ieee32-fneu-original-swizzles-and-controls");
    before = snapshot(b.shader);
    CHECK(sgx535_nir_fragment_bool32_fneu_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                             SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                             &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        ands += ir[i].opcode == SGX535_FRAGMENT_RAW_AND;
        xors += ir[i].opcode == SGX535_FRAGMENT_RAW_XOR;
        comparisons += ir[i].opcode == SGX535_FRAGMENT_RAW_IEQ;
        constant_words += ir[i].opcode == SGX535_FRAGMENT_CONSTANT;
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(ands == 13u * width && xors == (12u + (invert ? 1u : 0u)) * width);
    CHECK(comparisons == 7u * width);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(count <= SGX535_NIR_MAX_INSTRUCTIONS &&
          program.encoding.word_count <= SGX535_NIR_MAX_WORDS);
    CHECK(program.encoding.temp_register_count <= SGX535_NIR_TEMP_COUNT);
    CHECK(program.encoding.word_count == 2u * count + 4u * comparisons);
    CHECK(constant_words >= 3u);
    raw_words(&program.encoding, primary, 0, actual);
    CHECK(!memcmp(expected, actual, sizeof(actual)));
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_ior_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_fneu_ir, 1, SGX535_NIR_ERR_LIMIT);
    ralloc_free(b.shader);
}

static unsigned fneu_negative(void)
{
    unsigned mode, cases = 0;
    for (mode = 0; mode < 10; mode++) {
        nir_builder b = bool32_builder();
        nir_def *a = nir_imm_intN_t(&b, UINT32_C(0x7fc00001), 32);
        nir_def *value = nir_fneu(&b, a, a);
        nir_alu_instr *op = nir_def_as_alu(value);
        if (mode == 0)
            op->no_signed_wrap = true;
        else if (mode == 1)
            op->no_unsigned_wrap = true;
        else if (mode == 2)
            op->fp_math_ctrl = nir_fp_preserve_signed_zero;
        else if (mode == 3)
            op->fp_math_ctrl =
                nir_fp_preserve_inf | nir_fp_preserve_nan | nir_fp_preserve_signed_zero;
        else if (mode == 4)
            nir_fneu32(&b, a, a);
        else if (mode == 5)
            nir_fneu(&b, nir_imm_intN_t(&b, 0, 64), nir_imm_intN_t(&b, 0, 64));
        else if (mode == 6)
            nir_iand(&b, nir_imm_int(&b, 1), nir_imm_int(&b, -1));
        else if (mode == 7)
            nir_bcsel(&b, value, a, a);
        else if (mode == 8)
            nir_ddx(&b, nir_imm_float(&b, 1.0f));
        else {
            nir_push_if(&b, value);
            nir_fneu(&b, a, a);
            nir_pop_if(&b, NULL);
        }
        value = nir_b2f32(&b, value);
        result(&b, nir_vec4(&b, value, value, value, value));
        /* Deliberate controls can be invalid Mesa inputs; the defensive gate
         * still has to refuse them before clone/lowering and preserve output. */
        ior_failure(b.shader, sgx535_nir_fragment_bool32_fneu_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                    SGX535_NIR_ERR_UNSUPPORTED);
        cases++;
        ralloc_free(b.shader);
    }
    {
        nir_builder b = bool32_builder();
        nir_def *value = nir_imm_bool(&b, false);
        unsigned i;
        for (i = 0; i < 32; i++)
            value = nir_fneu(&b, nir_imm_int(&b, i), nir_imm_int(&b, i + 1));
        value = nir_b2f32(&b, value);
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "fneu-original-small-expanded-over-capacity");
        ior_failure(b.shader, sgx535_nir_fragment_bool32_fneu_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                    SGX535_NIR_ERR_LIMIT);
        cases++;
        ralloc_free(b.shader);
    }
    return cases;
}

#ifndef POULSBACK_FNEU_TEST_MAIN
#define POULSBACK_FNEU_TEST_MAIN main
#endif

int POULSBACK_FNEU_TEST_MAIN(void)
{
    static const unsigned controls[] = {0, nir_fp_preserve_inf, nir_fp_preserve_nan,
                                        nir_fp_preserve_inf | nir_fp_preserve_nan};
    unsigned width, pair, control, cases = 0, negatives, model_checks;
    model_checks = fneu_reference_checks();
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (pair = 0; pair < 24; pair++)
            for (control = 0; control < 4; control++) {
                fneu_positive(width, pair, controls[control]);
                cases++;
            }
    negatives = fneu_negative();
    glsl_type_singleton_decref();
    CHECK(cases == 384 && negatives == 11 && model_checks == 131772);
    CHECK(fneu_legacy_ior_main() == 0);
    printf(
        "actual NIR raw IEEE32 FNEU: %u positive constructions, %u negative controls, %u independent truth checks, %u cumulative checks PASS (CPU only; native closed)\n",
        cases, negatives, model_checks, checks);
    return 0;
}
#endif
