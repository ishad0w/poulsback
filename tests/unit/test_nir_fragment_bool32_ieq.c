// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa fixture, kept separate from conditional source-policy models. */
#include "sgx535_fragment.h"
int ieq_legacy_bool32_main(void);
#define POULSBACK_BOOL32_TEST_MAIN ieq_legacy_bool32_main
#define POULSBACK_TEST_RAW_IEQ
#include "test_nir_fragment_bool32.c"
#undef POULSBACK_BOOL32_TEST_MAIN

static void ieq_refuse(nir_shader *shader)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x5678;
    char *before = snapshot(shader), *after;
    memset(ir, 0x6c, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0x3f, sizeof(linkage));
    old_linkage = linkage;
    CHECK(sgx535_nir_fragment_bool32_ieq_ir(shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                            SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                            &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(count == 0x5678 && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static void ieq_positive(unsigned width, unsigned pair, bool integer)
{
    static const uint32_t left[] = {0,           1,           2,           0x80000000u,
                                    UINT32_MAX,  0x7fc01234u, 0x7f800000u, 0xff800000u,
                                    0xaaaaaaaau, 0x12345678u};
    static const uint32_t right[] = {
        0, 0, 2, 0, 0xfffffffeu, 0x7fc01234u, 0x7f800000u, 0x7f800000u, 0x55555555u, 0x12345678u};
    nir_builder b = bool32_builder();
    nir_def *a[4], *other[4], *out[4], *value;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program, refused, old;
    uint32_t expected[4], actual[4], primary[4] = {0};
    char *before, *after;
    size_t count = 0, i;
    unsigned lane, comparisons = 0;
    for (lane = 0; lane < width; lane++) {
        unsigned index = (pair + lane) % 10;
        a[lane] = nir_imm_intN_t(&b, left[index], 32);
        other[lane] = nir_imm_intN_t(&b, right[index], 32);
    }
    value = nir_ieq(&b, nir_vec(&b, a, width), nir_vec(&b, other, width));
    if (pair & 1u)
        value = nir_inot(&b, value);
    value = nir_iand(&b, value, nir_imm_bool(&b, true));
    value = integer ? nir_b2i32(&b, value) : nir_b2f32(&b, value);
    for (lane = 0; lane < 4; lane++) {
        unsigned source = (3u - lane) % width, index = (pair + source) % 10;
        bool truth = left[index] == right[index];
        if (pair & 1u)
            truth = !truth;
        out[lane] = nir_channel(&b, value, source);
        expected[lane] = truth ? (integer ? 1u : UINT32_C(0x3f800000)) : 0;
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "raw-ieq-whole-original");
    before = snapshot(b.shader);
    memset(ir, 0, sizeof(ir));
    CHECK(sgx535_nir_fragment_bool32_ieq_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                            SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                            &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        comparisons += ir[i].opcode == SGX535_FRAGMENT_RAW_IEQ;
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(comparisons == width);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    raw_words(&program.encoding, primary, 0, actual);
    CHECK(!memcmp(actual, expected, sizeof(actual)));
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    memset(&refused, 0x48, sizeof(refused));
    old = refused;
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, count, &refused) ==
          SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!memcmp(&refused, &old, sizeof(old)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    bool32_refuse(b.shader);
    refuse(b.shader);
    {
        struct sgx535_fragment_linkage old_linkage = linkage;
        size_t old_count = count;
        struct sgx535_fragment_instruction saved[SGX535_NIR_MAX_INSTRUCTIONS];
        memcpy(saved, ir, sizeof(ir));
        CHECK(sgx535_nir_fragment_bool32_inot_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                 &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
        CHECK(old_count == count && !memcmp(saved, ir, sizeof(ir)) &&
              !memcmp(&old_linkage, &linkage, sizeof(linkage)));
    }
    ralloc_free(b.shader);
}

static void ieq_negative(void)
{
    unsigned mode;
    for (mode = 0; mode < 5; mode++) {
        nir_builder b = bool32_builder();
        nir_def *left = nir_imm_int(&b, 2), *right = nir_imm_int(&b, 3);
        nir_def *value = nir_b2f32(&b, nir_ieq(&b, left, right));
        if (mode == 0)
            nir_ine(&b, left, right); /* Unsupported even when dead. */
        else if (mode == 1)
            nir_bcsel(&b, nir_imm_bool(&b, true), left, right);
        else if (mode == 2) {
            nir_push_if(&b, nir_imm_bool(&b, true));
            nir_ieq(&b, left, right);
            nir_pop_if(&b, NULL);
        } else if (mode == 3)
            nir_ddx(&b, nir_imm_float(&b, 1.0f));
        else
            nir_inot(&b, nir_imm_int(&b, 1)); /* Noncanonical original bool32 cone. */
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "raw-ieq-unsupported-whole-original");
        ieq_refuse(b.shader);
        ralloc_free(b.shader);
    }
    for (mode = 0; mode < 3; mode++) {
        nir_builder b = bool32_builder();
        nir_def *compared = nir_ieq(&b, nir_imm_int(&b, 0), nir_imm_int(&b, 0));
        nir_def *value = nir_b2f32(&b, compared);
        nir_alu_instr *compare = nir_def_as_alu(compared);
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "raw-ieq-before-defensive-controls");
        if (mode == 0)
            compare->no_signed_wrap = true;
        else if (mode == 1)
            compare->no_unsigned_wrap = true;
        else
            compare->fp_math_ctrl = nir_fp_preserve_signed_zero;
        ieq_refuse(b.shader);
        ralloc_free(b.shader);
    }
    {
        nir_builder b = bool32_builder();
        nir_def *value = nir_ieq32(&b, nir_imm_int(&b, 1), nir_imm_int(&b, 1));
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "raw-ieq-prelowered-original-refused");
        ieq_refuse(b.shader);
        ralloc_free(b.shader);
    }
}

#ifndef POULSBACK_IEQ_TEST_MAIN
#define POULSBACK_IEQ_TEST_MAIN main
#endif

int POULSBACK_IEQ_TEST_MAIN(void)
{
    unsigned width, pair, integer, cases = 0;
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (pair = 0; pair < 10; pair++)
            for (integer = 0; integer < 2; integer++) {
                ieq_positive(width, pair, integer != 0);
                cases++;
            }
    ieq_negative();
    glsl_type_singleton_decref();
    CHECK(cases == 80);
    CHECK(ieq_legacy_bool32_main() == 0);
    printf(
        "actual NIR raw IEQ: %u positive constructions, %u cumulative checks PASS (CPU only; native closed)\n",
        cases, checks);
    return 0;
}
