// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa producer plus the existing independent CPU word consumer. */
int ior_legacy_ieq_main(void);
#define POULSBACK_IEQ_TEST_MAIN ior_legacy_ieq_main
#include "test_nir_fragment_bool32_ieq.c"
#undef POULSBACK_IEQ_TEST_MAIN

typedef enum sgx535_nir_status (*bool_entry)(const nir_shader *,
                                             enum sgx535_fragment_uniform_layout,
                                             struct sgx535_fragment_instruction *, size_t, size_t *,
                                             struct sgx535_fragment_linkage *);

static void ior_failure(nir_shader *shader, bool_entry entry, size_t capacity,
                        enum sgx535_nir_status expected)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        saved[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x1876;
    char *before = snapshot(shader), *after;
    memset(ir, 0x79, sizeof(ir));
    memcpy(saved, ir, sizeof(ir));
    memset(&linkage, 0x2b, sizeof(linkage));
    old_linkage = linkage;
    CHECK(entry(shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir, capacity, &count, &linkage) ==
          expected);
    CHECK(count == 0x1876 && !memcmp(ir, saved, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static nir_def *ior_source(nir_builder *b, bool value, bool original32, bool compare)
{
    nir_def *source;
    if (compare) {
        source = nir_ieq(b, nir_imm_intN_t(b, UINT32_C(0x80000000), 32),
                         nir_imm_intN_t(b, value ? UINT32_C(0x80000000) : 0, 32));
        return original32 ? nir_b2b32(b, source) : source;
    }
    return original32 ? nir_imm_intN_t(b, value ? UINT32_MAX : 0, 32) : nir_imm_bool(b, value);
}

static void ior_positive(unsigned width, unsigned pattern, bool original32, bool integer,
                         bool invert)
{
    static const unsigned left_masks[] = {0, 15, 5, 10, 9};
    static const unsigned right_masks[] = {0, 0, 10, 3, 6};
    nir_builder b = bool32_builder();
    nir_def *left[4], *right[4], *out[4], *a, *other, *value;
    nir_alu_instr *op;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t expected[4], actual[4], primary[4] = {0};
    unsigned lane, ands = 0, xors = 0, comparisons = 0;
    size_t count = 0, i;
    char *before, *after;
    for (lane = 0; lane < width; lane++) {
        left[lane] =
            ior_source(&b, (left_masks[pattern] & (1u << lane)) != 0, original32, pattern == 3);
        right[lane] =
            ior_source(&b, (right_masks[pattern] & (1u << lane)) != 0, original32, pattern == 3);
    }
    a = nir_vec(&b, left, width);
    other = pattern == 4 ? a : nir_vec(&b, right, width);
    value = nir_ior(&b, a, other);
    op = nir_def_as_alu(value);
    for (lane = 0; lane < width; lane++) {
        op->src[0].swizzle[lane] = width - 1u - lane;
        op->src[1].swizzle[lane] = (lane + 1u) % width;
    }
    if (invert)
        value = nir_inot(&b, value);
    value = integer ? nir_b2i32(&b, value) : nir_b2f32(&b, value);
    for (lane = 0; lane < 4; lane++) {
        unsigned source = (3u - lane) % width;
        unsigned l = width - 1u - source, r = (source + 1u) % width;
        unsigned right_mask = pattern == 4 ? left_masks[pattern] : right_masks[pattern];
        bool truth = ((left_masks[pattern] >> l) & 1u) || ((right_mask >> r) & 1u);
        if (invert)
            truth = !truth;
        expected[lane] = truth ? (integer ? 1u : UINT32_C(0x3f800000)) : 0;
        out[lane] = nir_channel(&b, value, source);
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "canonical-ior-original-swizzles-and-shared-operands");
    before = snapshot(b.shader);
    memset(ir, 0, sizeof(ir));
    CHECK(sgx535_nir_fragment_bool32_ior_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                            SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                            &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        ands += ir[i].opcode == SGX535_FRAGMENT_RAW_AND;
        xors += ir[i].opcode == SGX535_FRAGMENT_RAW_XOR;
        comparisons += ir[i].opcode == SGX535_FRAGMENT_RAW_IEQ;
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(ands >= width && xors >= 3u * width);
    CHECK(comparisons == (pattern == 3 ? 2u * width : 0));
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    raw_words(&program.encoding, primary, 0, actual);
    CHECK(!memcmp(expected, actual, sizeof(actual)));
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_iand_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_inot_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_ieq_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_ior_ir, 1, SGX535_NIR_ERR_LIMIT);
    ralloc_free(b.shader);
}

static unsigned ior_negative(void)
{
    static const uint32_t bad[] = {1, 2, UINT32_C(0x80000000), UINT32_C(0xfffffffe)};
    unsigned index, side, dead, mode, cases = 0;
    for (index = 0; index < 4; index++)
        for (side = 0; side < 2; side++)
            for (dead = 0; dead < 2; dead++) {
                nir_builder b = bool32_builder();
                nir_def *good = nir_imm_intN_t(&b, UINT32_MAX, 32);
                nir_def *other = nir_imm_intN_t(&b, bad[index], 32);
                nir_def *value = side ? nir_ior(&b, other, good) : nir_ior(&b, good, other);
                value = dead ? nir_imm_float(&b, 0) : nir_b2f32(&b, value);
                result(&b, nir_vec4(&b, value, value, value, value));
                nir_validate_shader(b.shader, "ior-noncanonical-original-live-or-dead");
                ior_failure(b.shader, sgx535_nir_fragment_bool32_ior_ir,
                            SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_UNSUPPORTED);
                cases++;
                ralloc_free(b.shader);
            }
    for (mode = 0; mode < 3; mode++) {
        nir_builder b = bool32_builder();
        nir_def *value = nir_ior(&b, nir_imm_bool(&b, false), nir_imm_bool(&b, true));
        nir_alu_instr *op = nir_def_as_alu(value);
        value = nir_b2f32(&b, value);
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "ior-before-original-control-mutation");
        if (mode == 0)
            op->no_signed_wrap = true;
        else if (mode == 1)
            op->no_unsigned_wrap = true;
        else
            op->fp_math_ctrl = nir_fp_preserve_signed_zero;
        ior_failure(b.shader, sgx535_nir_fragment_bool32_ior_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                    SGX535_NIR_ERR_UNSUPPORTED);
        cases++;
        ralloc_free(b.shader);
    }
    for (mode = 0; mode < 6; mode++) {
        nir_builder b = bool32_builder();
        nir_def *value = nir_ior(&b, nir_imm_bool(&b, false), nir_imm_bool(&b, true));
        if (mode == 0)
            nir_ine(&b, nir_imm_int(&b, 1), nir_imm_int(&b, 0));
        else if (mode == 1)
            nir_ieq32(&b, nir_imm_int(&b, 1), nir_imm_int(&b, 1));
        else if (mode == 2)
            nir_bcsel(&b, nir_imm_bool(&b, true), nir_imm_int(&b, 1), nir_imm_int(&b, 0));
        else if (mode == 3)
            nir_ddx(&b, nir_imm_float(&b, 1));
        else if (mode == 4) {
            nir_push_if(&b, nir_imm_bool(&b, true));
            nir_ior(&b, nir_imm_bool(&b, true), nir_imm_bool(&b, false));
            nir_pop_if(&b, NULL);
        } else
            nir_ior(&b, nir_imm_intN_t(&b, 0, 64), nir_imm_intN_t(&b, 1, 64));
        value = nir_b2f32(&b, value);
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "ior-whole-original-unsupported-dead-operation");
        ior_failure(b.shader, sgx535_nir_fragment_bool32_ior_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                    SGX535_NIR_ERR_UNSUPPORTED);
        cases++;
        ralloc_free(b.shader);
    }
    return cases;
}

#ifndef POULSBACK_IOR_TEST_MAIN
#define POULSBACK_IOR_TEST_MAIN main
#endif

int POULSBACK_IOR_TEST_MAIN(void)
{
    unsigned width, pattern, original32, integer, invert, cases = 0, negatives;
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (pattern = 0; pattern < 5; pattern++)
            for (original32 = 0; original32 < 2; original32++)
                for (integer = 0; integer < 2; integer++)
                    for (invert = 0; invert < 2; invert++) {
                        ior_positive(width, pattern, original32 != 0, integer != 0, invert != 0);
                        cases++;
                    }
    negatives = ior_negative();
    glsl_type_singleton_decref();
    CHECK(cases == 160 && negatives == 25);
    CHECK(ior_legacy_ieq_main() == 0);
    printf(
        "actual NIR canonical bool32/ior: %u positive constructions, %u negative controls, %u cumulative checks PASS (CPU only; native closed)\n",
        cases, negatives, checks);
    return 0;
}
