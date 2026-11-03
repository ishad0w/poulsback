// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa NIR fixture; new NOT path and unchanged AND/raw-sign controls. */
int bool32_inot_legacy_main(void);
#define POULSBACK_BOOL32_TEST_MAIN bool32_inot_legacy_main
#include "test_nir_fragment_bool32.c"
#undef POULSBACK_BOOL32_TEST_MAIN

static void inot_refuse(nir_shader *shader)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x2345;
    char *before = snapshot(shader), *after;
    memset(ir, 0x2d, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0x9b, sizeof(linkage));
    old_linkage = linkage;
    CHECK(sgx535_nir_fragment_bool32_inot_ir(shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                             SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                             &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(count == 0x2345 && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static void inot_positive(unsigned width, unsigned mask, bool original32, bool integer, bool twice)
{
    nir_builder b = bool32_builder();
    nir_def *lanes[4], *out[4], *value;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t expected[4], actual[4], dummy[4] = {0};
    size_t count = 0, i;
    unsigned lane, xors = 0, ands = 0;
    char *before, *after;
    for (lane = 0; lane < width; lane++) {
        bool truth = (mask & (1u << lane)) != 0;
        lanes[lane] = original32 ? nir_imm_int(&b, truth ? -1 : 0) : nir_imm_bool(&b, truth);
    }
    value = nir_inot(&b, nir_vec(&b, lanes, width));
    if (twice)
        value = nir_inot(&b, value);
    if (original32)
        value = nir_b2b1(&b, value);
    value = integer ? nir_b2i32(&b, value) : nir_b2f32(&b, value);
    for (lane = 0; lane < 4; lane++) {
        unsigned source = (3u - lane) % width;
        bool truth = (mask & (1u << source)) != 0;
        out[lane] = nir_channel(&b, value, source);
        truth = twice ? truth : !truth;
        expected[lane] = truth ? (integer ? 1u : UINT32_C(0x3f800000)) : 0;
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "bool32-inot-original");
    before = snapshot(b.shader);
    CHECK(sgx535_nir_fragment_bool32_inot_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                             SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                             &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        if (ir[i].opcode == SGX535_FRAGMENT_RAW_XOR) {
            unsigned index = ir[i].source[1];
            CHECK(index < i && ir[index].opcode == SGX535_FRAGMENT_CONSTANT &&
                  ir[index].immediate == UINT32_MAX);
            xors++;
        }
        ands += ir[i].opcode == SGX535_FRAGMENT_RAW_AND;
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(xors == width * (twice ? 2u : 1u) && ands == width);
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    raw_words(&program.encoding, dummy, 0, actual);
    CHECK(!memcmp(actual, expected, sizeof(actual)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    /* Both original entrypoints retain their refusal, including 32-bit NOT. */
    bool32_refuse(b.shader);
    refuse(b.shader);
    ralloc_free(b.shader);
}

static void inot_negatives(void)
{
    static const uint32_t bad[] = {1, 2, 0x80000000u, 0x7fffffffu, 0xfffffffeu, 0x7fc12345u};
    unsigned i;
    for (i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        nir_builder b = bool32_builder();
        nir_def *value = nir_inot(&b, nir_imm_int(&b, (int32_t)bad[i]));
        value = nir_b2f32(&b, nir_b2b1(&b, value));
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "bool32-inot-noncanonical");
        inot_refuse(b.shader);
        bool32_refuse(b.shader);
        ralloc_free(b.shader);
    }
    for (i = 0; i < 3; i++) {
        nir_builder b = bool32_builder();
        nir_def *yes = nir_imm_bool(&b, true);
        nir_def *value = nir_b2f32(&b, nir_inot(&b, yes));
        if (i == 0)
            nir_ieq(&b, nir_imm_int(&b, 0), nir_imm_int(&b, 1));
        else if (i == 1)
            nir_bcsel(&b, yes, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f));
        else {
            nir_push_if(&b, yes);
            nir_inot(&b, yes);
            nir_pop_if(&b, NULL);
        }
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "bool32-inot-unsupported-dead-producer");
        inot_refuse(b.shader);
        ralloc_free(b.shader);
    }
    for (i = 0; i < 3; i++) {
        nir_builder b = bool32_builder();
        nir_def *value = nir_inot(&b, nir_imm_bool(&b, false));
        nir_alu_instr *op = nir_def_as_alu(value);
        nir_def *converted = nir_b2f32(&b, value);
        result(&b, nir_vec4(&b, converted, converted, converted, converted));
        nir_validate_shader(b.shader, "bool32-inot-before-defensive-control-mutation");
        /* Defensive rejection only; no claim these flags form valid INOT NIR. */
        if (i == 0)
            op->fp_math_ctrl = nir_fp_preserve_signed_zero;
        else if (i == 1)
            op->no_signed_wrap = true;
        else
            op->no_unsigned_wrap = true;
        inot_refuse(b.shader);
        ralloc_free(b.shader);
    }
}

int main(void)
{
    unsigned width, mask, original32, integer, twice, cases = 0;
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (mask = 0; mask < (1u << width); mask++)
            for (original32 = 0; original32 < 2; original32++)
                for (integer = 0; integer < 2; integer++)
                    for (twice = 0; twice < 2; twice++) {
                        inot_positive(width, mask, original32 != 0, integer != 0, twice != 0);
                        cases++;
                    }
    inot_negatives();
    glsl_type_singleton_decref();
    CHECK(cases == 240);
    CHECK(bool32_inot_legacy_main() == 0);
    printf(
        "actual NIR canonical bool32/inot: %u positive constructions, %u cumulative checks PASS (CPU only; native closed)\n",
        cases, checks);
    return 0;
}
