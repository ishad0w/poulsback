// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa NIR fixture; no GL screen, device, or software presentation. */
#include "sgx535_fragment_bool32.h"
int bool32_legacy_raw_sign_main(void);
#define main bool32_legacy_raw_sign_main
#include "test_nir_fragment_raw_sign.c"
#undef main

static void bool32_refuse(nir_shader *shader)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x1234;
    char *before = snapshot(shader), *after;
    memset(ir, 0x6d, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0xa7, sizeof(linkage));
    old_linkage = linkage;
    CHECK(sgx535_nir_fragment_bool32_iand_ir(shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                             SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                             &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(count == 0x1234 && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static nir_builder bool32_builder(void)
{
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "bool32-iand");
    b.constant_fold_alu = false;
    return b;
}

static void positive(unsigned width, unsigned a, unsigned bmask, bool original32, bool integer)
{
    nir_builder b = bool32_builder();
    nir_def *left[4], *right[4], *out[4], *value;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t expected[4], actual[4], dummy[4] = {0};
    size_t count = 0, i;
    unsigned lane, and_count = 0, canonical_masks = 0;
    char *before, *after;
    for (lane = 0; lane < width; lane++) {
        bool av = (a & (1u << lane)) != 0, bv = (bmask & (1u << lane)) != 0;
        left[lane] = original32 ? nir_imm_int(&b, av ? -1 : 0) : nir_imm_bool(&b, av);
        right[lane] = original32 ? nir_imm_int(&b, bv ? -1 : 0) : nir_imm_bool(&b, bv);
    }
    value = nir_iand(&b, nir_vec(&b, left, width), nir_vec(&b, right, width));
    /* Exercise both retained canonical-mask MOV and exact numeric conversions. */
    if (original32)
        value = nir_b2b1(&b, value);
    value = integer ? nir_b2i32(&b, value) : nir_b2f32(&b, value);
    for (lane = 0; lane < 4; lane++) {
        unsigned source = (3u - lane) % width;
        bool truth = (a & bmask & (1u << source)) != 0;
        out[lane] = nir_channel(&b, value, source);
        expected[lane] = truth ? (integer ? 1u : UINT32_C(0x3f800000)) : 0;
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "bool32-original");
    before = snapshot(b.shader);
    CHECK(sgx535_nir_fragment_bool32_iand_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                             SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                             &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        and_count += ir[i].opcode == SGX535_FRAGMENT_RAW_AND;
        canonical_masks += ir[i].opcode == SGX535_FRAGMENT_CONSTANT &&
                           (ir[i].immediate == 0 || ir[i].immediate == UINT32_MAX);
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(and_count == width * 2 && canonical_masks >= width * 2);
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    raw_words(&program.encoding, dummy, 0, actual);
    CHECK(!memcmp(actual, expected, sizeof(actual)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    /* The ordinary strict adapter retains its original rejection. */
    refuse(b.shader);
    ralloc_free(b.shader);
}

static void malformed_masks(void)
{
    static const uint32_t bad[] = {1, 2, 0x80000000u, 0x7fffffffu, 0xfffffffeu, 0x7fc12345u};
    unsigned i;
    for (i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        nir_builder b = bool32_builder();
        nir_def *v = nir_iand(&b, nir_imm_int(&b, (int32_t)bad[i]), nir_imm_int(&b, -1));
        v = nir_b2f32(&b, nir_b2b1(&b, v));
        result(&b, nir_vec4(&b, v, v, v, v));
        nir_validate_shader(b.shader, "bool32-noncanonical-mask");
        bool32_refuse(b.shader);
        ralloc_free(b.shader);
    }
}

static void unsupported(bool select, bool flow)
{
    nir_builder b = bool32_builder();
    nir_def *yes = nir_imm_bool(&b, true), *no = nir_imm_bool(&b, false);
    nir_def *v = nir_b2f32(&b, nir_iand(&b, yes, no));
    nir_def *zero = nir_imm_float(&b, 0.0f), *one = nir_imm_float(&b, 1.0f);
    if (select)
        nir_bcsel(&b, yes, zero, one); /* Dead selects must not disappear. */
    else
        nir_ieq(&b, nir_imm_int(&b, 0), nir_imm_int(&b, 1));
    if (flow) {
        nir_push_if(&b, yes);
        nir_iand(&b, yes, no);
        nir_pop_if(&b, NULL);
    }
    result(&b, nir_vec4(&b, v, v, v, v));
    nir_validate_shader(b.shader, "bool32-unimplemented-leaf");
    bool32_refuse(b.shader);
    ralloc_free(b.shader);
}

#ifndef POULSBACK_BOOL32_TEST_MAIN
#define POULSBACK_BOOL32_TEST_MAIN main
#endif

int POULSBACK_BOOL32_TEST_MAIN(void)
{
    unsigned width, a, b, original32, integer;
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (a = 0; a < 2; a++)
            for (b = 0; b < 2; b++)
                for (original32 = 0; original32 < 2; original32++)
                    for (integer = 0; integer < 2; integer++)
                        positive(width, a ? (1u << width) - 1u : 0, b ? (1u << width) - 1u : 0,
                                 original32 != 0, integer != 0);
    for (width = 1; width <= 4; width++)
        for (original32 = 0; original32 < 2; original32++)
            for (integer = 0; integer < 2; integer++)
                positive(width, 0x5u, 0xau, original32 != 0, integer != 0);
    malformed_masks();
    unsupported(false, false);
    unsupported(true, false);
    unsupported(true, true);
    glsl_type_singleton_decref();
    CHECK(bool32_legacy_raw_sign_main() == 0);
    printf("actual NIR canonical bool32/iand: %u checks PASS (CPU only; native closed)\n", checks);
    return 0;
}
