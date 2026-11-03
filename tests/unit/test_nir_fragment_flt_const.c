// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual NIR and emitted raw-word execution, with independent IEEE32 truth. */
#include "sgx535_fragment_compare.h"
#include <math.h>
int flt_const_legacy_fneu_main(void);
#define POULSBACK_FNEU_TEST_MAIN flt_const_legacy_fneu_main
#include "test_nir_fragment_bool32_fneu.c"
#undef POULSBACK_FNEU_TEST_MAIN

static bool ordered_ge, ordered_ge_diamond;

static const uint32_t flt_values[] = {
    0,          0x80000000, 1,          0x80000001, 0x007fffff, 0x807fffff, 0x00800000,
    0x80800000, 0x3f800000, 0xbf800000, 0x3f800001, 0xbf800001, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7f800001, 0xff800001, 0x7fc00000, 0xffc00000, 0x7fc12345,
    0xffc12345, 0x7fffffff, UINT32_MAX, 0x40000000, 0x358637bd,
};

static bool flt_reference(uint32_t left, uint32_t right)
{
    float a, b;
    memcpy(&a, &left, sizeof(a));
    memcpy(&b, &right, sizeof(b));
    return ordered_ge ? a >= b : a < b;
}

static enum sgx535_nir_status flt_entry(const nir_shader *shader,
                                        const struct sgx535_fragment_nir_options *selected,
                                        struct sgx535_fragment_instruction *ir, size_t capacity,
                                        size_t *count, struct sgx535_fragment_linkage *linkage)
{
#ifdef POULSBACK_FLT_CONST_BASELINE
    return sgx535_nir_fragment_compare_options_ir(shader, selected, ir, capacity, count, linkage);
#else
    return sgx535_nir_fragment_flt_const_options_ir(shader, selected, ir, capacity, count, linkage);
#endif
}

static nir_builder flt_shader(unsigned width, uint32_t constant, unsigned order, unsigned controls,
                              bool both_constant, bool mixed_constant, nir_alu_instr **comparison)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *input = varying(&b, 4);
    nir_io_semantics semantics = nir_intrinsic_io_semantics(input);
    semantics.medium_precision = 1;
    nir_intrinsic_set_io_semantics(input, semantics);
    nir_def *dynamic = both_constant
                           ? nir_imm_ivec4(&b, 0x80000000, 0x3f800000, 0x7fc12345, 0xff800000)
                           : &input->def;
    nir_def *c = mixed_constant ? nir_imm_ivec4(&b, constant, constant ^ 0x80000000, 0, 0x7f800000)
                                : nir_imm_ivec4(&b, constant, constant, constant, constant);
    nir_def *value = ordered_ge ? (order ? nir_fge(&b, c, dynamic) : nir_fge(&b, dynamic, c))
                                : (order ? nir_flt(&b, c, dynamic) : nir_flt(&b, dynamic, c));
    nir_alu_instr *alu = nir_def_as_alu(value);
    alu->def.num_components = width;
    alu->fp_math_ctrl = controls;
    for (unsigned lane = 0; lane < width; lane++) {
        alu->src[order ? 0 : 1].swizzle[lane] = (lane * 3u + 1u) % 4;
        alu->src[order ? 1 : 0].swizzle[lane] = (3u - lane) % 4;
    }
    value = nir_b2b32(&b, value);
    nir_def *out[4];
    for (unsigned lane = 0; lane < 4; lane++)
        out[lane] = nir_channel(&b, value, (lane + 1u) % width);
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "constant-FLT-original");
    *comparison = alu;
    return b;
}

static void flt_refuse(nir_shader *shader, const struct sgx535_fragment_nir_options *selected,
                       size_t capacity, enum sgx535_nir_status expected)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x5387;
    memset(ir, 0x9b, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0xe7, sizeof(linkage));
    old_linkage = linkage;
    char *before = snapshot(shader), *after;
    enum sgx535_nir_status status = flt_entry(shader, selected, ir, capacity, &count, &linkage);
    if (status != expected)
        fprintf(stderr, "constant-FLT refusal got=%d expected=%d capacity=%zu\n", status, expected,
                capacity);
    CHECK(status == expected);
    CHECK(count == 0x5387 && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static unsigned flt_positive(unsigned width, uint32_t constant, unsigned order, unsigned controls,
                             bool both_constant, bool mixed_constant)
{
    nir_alu_instr *alu;
    nir_builder b =
        flt_shader(width, constant, order, controls, both_constant, mixed_constant, &alu);
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    size_t count = 0;
    char *before = snapshot(b.shader), *after;
    enum sgx535_nir_status status =
        flt_entry(b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status != SGX535_NIR_OK)
        fprintf(stderr,
                "constant-FLT width=%u constant=%08x order=%u controls=%u both=%u status=%d\n",
                width, constant, order, controls, both_constant, status);
    CHECK(status == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(!program.arithmetic_qualified && !program.linkage_qualified && !program.output_qualified);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(count <= SGX535_NIR_MAX_INSTRUCTIONS &&
          program.encoding.word_count <= SGX535_NIR_MAX_WORDS &&
          program.encoding.temp_register_count <= SGX535_NIR_TEMP_COUNT);
    for (size_t i = 0; i < count; i++)
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    uint32_t primary[4], expected[4], actual[4], random = 0x217d9a3b;
    unsigned cases = 0;
    for (unsigned pattern = 0; pattern < 157; pattern++) {
        for (unsigned lane = 0; lane < 4; lane++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            primary[lane] = pattern < 26   ? flt_values[(pattern + lane) % 26]
                            : pattern < 29 ? constant + pattern - 27u
                                           : random;
        }
        for (unsigned lane = 0; lane < 4; lane++) {
            unsigned logical = (lane + 1u) % width;
            unsigned swizzle = alu->src[order ? 1 : 0].swizzle[logical];
            const uint32_t known[4] = {0x80000000, 0x3f800000, 0x7fc12345, 0xff800000};
            uint32_t dynamic = both_constant ? known[swizzle] : primary[swizzle];
            const uint32_t mixed[4] = {constant, constant ^ 0x80000000, 0, 0x7f800000};
            uint32_t threshold =
                mixed_constant ? mixed[alu->src[order ? 0 : 1].swizzle[logical]] : constant;
            bool truth =
                order ? flt_reference(threshold, dynamic) : flt_reference(dynamic, threshold);
            expected[lane] = truth ? UINT32_MAX : 0;
        }
        raw_words(&program.encoding, primary, 0, actual);
        if (memcmp(actual, expected, sizeof(actual)))
            fprintf(stderr, "constant-FLT raw mismatch constant=%08x order=%u pattern=%u\n",
                    constant, order, pattern);
        CHECK(!memcmp(actual, expected, sizeof(actual)));
        cases++;
    }
    /* Capacity is exact, with no partial publication on the smaller limit. */
    size_t exact_count = 0;
    CHECK(flt_entry(b.shader, &selected, ir, count, &exact_count, &linkage) == SGX535_NIR_OK);
    CHECK(exact_count == count);
    flt_refuse(b.shader, &selected, count - 1u, SGX535_NIR_ERR_LIMIT);
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    /* Existing compare entry and older Boolean entries retain FLT refusal. */
    size_t old_count = 0x83;
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, ir,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &old_count,
                                                 &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(old_count == 0x83);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ralloc_free(b.shader);
    return cases;
}

static unsigned flt_negative(void)
{
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 21; mode++) {
        nir_alu_instr *alu;
        nir_builder b = flt_shader(1, 0x40000000, 0, nir_fp_preserve_inf, false, false, &alu);
        struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                       SGX535_FRAGMENT_IO_F32};
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        if (mode == 0)
            nir_src_rewrite(&alu->src[1].src, alu->src[0].src.ssa);
        else if (mode == 1)
            alu->src[0].src.ssa->bit_size = 16;
        else if (mode == 2)
            alu->src[1].src.ssa->bit_size = 16;
        else if (mode == 3)
            alu->def.bit_size = 32;
        else if (mode == 4)
            alu->src[0].swizzle[0] = 4;
        else if (mode == 5)
            alu->src[1].swizzle[0] = 4;
        else if (mode == 6)
            alu->fp_math_ctrl |= nir_fp_preserve_signed_zero;
        else if (mode == 7)
            alu->no_signed_wrap = true;
        else if (mode == 8)
            alu->no_unsigned_wrap = true;
        else if (mode == 9) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_flt(&b, alu->src[0].src.ssa, alu->src[0].src.ssa); /* Dead dynamic FLT. */
        } else if (mode == 10) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_fge(&b, alu->src[0].src.ssa, alu->src[0].src.ssa); /* Dead dynamic FGE. */
        } else if (mode == 11) {
            selected.uniform_layout = (enum sgx535_fragment_uniform_layout)2;
        } else if (mode == 12) {
            selected.io_precision = (enum sgx535_fragment_io_precision_policy)2;
        } else if (mode == 13) {
            expected = SGX535_NIR_ERR_ARGUMENT;
        } else if (mode == 14) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_iand(&b, nir_imm_int(&b, 3), nir_imm_int(&b, 7));
        } else if (mode == 15) {
            nir_push_if(&b, nir_imm_bool(&b, true));
            nir_pop_if(&b, NULL);
        } else if (mode == 16) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_def *c = nir_imm_int(&b, 0x40000000);
            nir_src_rewrite(&alu->src[1].src, nir_vec4(&b, c, c, c, c));
        } else if (mode == 17) {
            b.cursor = nir_before_instr(&alu->instr);
            for (unsigned i = 0; i < SGX535_NIR_MAX_INSTRUCTIONS; i++)
                nir_imm_int(&b, i);
            expected = SGX535_NIR_ERR_LIMIT;
        } else if (mode == 18)
            b.shader->info.float_controls_execution_mode = 1;
        else if (mode == 19)
            alu->def.num_components = 0;
        else {
            alu->src[1].src.ssa->num_components = 0;
        }
        flt_refuse(b.shader, mode == 13 ? NULL : &selected, SGX535_NIR_MAX_INSTRUCTIONS, expected);
        ralloc_free(b.shader);
        cases++;
    }
    {
        nir_alu_instr *alu;
        nir_builder b = flt_shader(4, 0x55555555, 0, 0, false, false, &alu);
        const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                             SGX535_FRAGMENT_IO_F32};
        /* The original is small; private lowering must retain its fixed
         * expanded instruction budget rather than publishing a prefix. */
        flt_refuse(b.shader, &selected, SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_LIMIT);
        ralloc_free(b.shader);
        cases++;
    }
    return cases;
}

static void flt_unchanged_without_flt(void)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *in = varying(&b, 4);
    nir_def *condition = nir_ige(&b, nir_channel(&b, &in->def, 0), nir_imm_int(&b, 3));
    result(&b, nir_bcsel(&b, condition, &in->def, nir_imm_ivec4(&b, 0, -1, 1, 0x80000000)));
    nir_validate_shader(b.shader, "no-FLT-unchanged-comparison-grammar");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction old[SGX535_NIR_MAX_INSTRUCTIONS],
        now[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage old_linkage, now_linkage;
    size_t old_count = 0, now_count = 0;
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, old,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &old_count,
                                                 &old_linkage) == SGX535_NIR_OK);
    CHECK(flt_entry(b.shader, &selected, now, SGX535_NIR_MAX_INSTRUCTIONS, &now_count,
                    &now_linkage) == SGX535_NIR_OK);
    CHECK(old_count == now_count && !memcmp(old, now, now_count * sizeof(*now)) &&
          !memcmp(&old_linkage, &now_linkage, sizeof(old_linkage)));
    size_t floor_count = 0;
    CHECK(sgx535_nir_fragment_floor_options_ir(b.shader, &selected, now,
                                               SGX535_NIR_MAX_INSTRUCTIONS, &floor_count,
                                               &now_linkage) == SGX535_NIR_OK);
    CHECK(old_count == floor_count && !memcmp(old, now, floor_count * sizeof(*now)) &&
          !memcmp(&old_linkage, &now_linkage, sizeof(old_linkage)));
    ralloc_free(b.shader);
}

static unsigned flt_composition(void)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *in = varying(&b, 4);
    nir_def *gap = nir_channel(&b, &in->def, 0);
    nir_def *large = nir_flt(&b, nir_imm_float(&b, 2), gap);
    nir_def *small = nir_flt(&b, gap, nir_imm_int(&b, 0x358637bd));
    nir_def_as_alu(large)->fp_math_ctrl = nir_fp_preserve_inf;
    nir_def_as_alu(small)->fp_math_ctrl = nir_fp_preserve_inf;
    nir_def *condition = nir_ior(&b, large, small);
    result(&b, nir_bcsel(&b, condition, nir_imm_ivec4(&b, 1, -1, 0x7fc12345, 0), &in->def));
    nir_validate_shader(b.shader, "linear-small-two-general-constant-thresholds");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    size_t count = 0;
    char *before = snapshot(b.shader), *after;
    CHECK(flt_entry(b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage) ==
          SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    for (unsigned pattern = 0; pattern < 26; pattern++) {
        uint32_t primary[4] = {flt_values[pattern], 0x80000000, 0xff800000, 0x7fc12345};
        uint32_t alternate[4] = {1, UINT32_MAX, 0x7fc12345, 0}, actual[4];
        bool truth = flt_reference(0x40000000, primary[0]) || flt_reference(primary[0], 0x358637bd);
        raw_words(&program.encoding, primary, 0, actual);
        CHECK(!memcmp(actual, truth ? alternate : primary, sizeof(actual)));
    }
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
    return 26;
}

static nir_builder inverse_equality_shader(bool integer, unsigned width, unsigned mode,
                                           nir_alu_instr **comparison)
{
    nir_builder b = bool32_builder();
    nir_def *left = &varying(&b, 4)->def;
    nir_def *right =
        mode == 1   ? left
        : mode == 2 ? nir_imm_ivec4(&b, 0, 0x80000000, 0x7fc12345, 0x7f800000)
                    : nir_load_uniform(&b, 4, 32, nir_imm_int(&b, 2), .base = 1, .range = 6,
                                       .dest_type = integer ? nir_type_int32 : nir_type_float32);
    nir_def *value = integer ? nir_ine(&b, left, right) : nir_feq(&b, left, right);
    nir_alu_instr *alu = nir_def_as_alu(value);
    alu->def.num_components = width;
    alu->fp_math_ctrl = integer ? 0 : nir_fp_preserve_inf | nir_fp_preserve_nan;
    for (unsigned lane = 0; lane < width; lane++) {
        alu->src[0].swizzle[lane] = 3u - lane;
        alu->src[1].swizzle[lane] = (lane + 1u) % 4;
    }
    value = nir_b2b32(&b, value);
    nir_def *out[4];
    for (unsigned lane = 0; lane < 4; lane++)
        out[lane] = nir_channel(&b, value, (lane + 1u) % width);
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, integer ? "raw-INE-original" : "ordered-IEEE32-FEQ-original");
    *comparison = alu;
    return b;
}

static unsigned inverse_equality_positive(bool integer, unsigned width, unsigned layout,
                                          unsigned mode)
{
    nir_alu_instr *alu;
    nir_builder b = inverse_equality_shader(integer, width, mode, &alu);
    const struct sgx535_fragment_nir_options selected = {layout ? SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS
                                                                : SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    size_t count = 0;
    char *before = snapshot(b.shader), *after;
    enum sgx535_nir_status status =
        flt_entry(b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status != SGX535_NIR_OK)
        fprintf(stderr, "inverse equality integer=%u width=%u layout=%u mode=%u status=%d\n",
                integer, width, layout, mode, status);
    CHECK(status == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    for (size_t i = 0; i < count; i++)
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    uint32_t primary[4], secondary[SGX535_FRAGMENT_UNIFORM_WORDS] = {0}, expected[4], actual[4];
    unsigned address = layout ? 12u : 3u, cases = 0;
    for (unsigned a = 0; a < 26; a++)
        for (unsigned other = 0; other < (mode ? 1u : 26u); other++) {
            for (unsigned lane = 0; lane < 4; lane++) {
                primary[lane] = flt_values[(a + lane) % 26];
                secondary[address + lane] = flt_values[(other + lane) % 26];
            }
            const uint32_t constant[4] = {0, 0x80000000, 0x7fc12345, 0x7f800000};
            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned component = (lane + 1u) % width;
                unsigned right_lane = alu->src[1].swizzle[component];
                uint32_t left = primary[alu->src[0].swizzle[component]];
                uint32_t right = mode == 1   ? primary[right_lane]
                                 : mode == 2 ? constant[right_lane]
                                             : secondary[address + right_lane];
                float left_float, right_float;
                memcpy(&left_float, &left, sizeof(left_float));
                memcpy(&right_float, &right, sizeof(right_float));
                /* Host IEEE comparison and integer inequality do not consume
                 * the lowering's NaN masks, IR or encoded instruction words. */
                bool truth = integer ? left != right : left_float == right_float;
                expected[lane] = truth ? UINT32_MAX : 0;
            }
            raw_words_with_secondary(&program.encoding, primary, secondary, 0,
                                     SGX535_FRAGMENT_UNIFORM_WORDS, actual);
            CHECK(!memcmp(actual, expected, sizeof(actual)));
            cases++;
        }
    size_t exact_count = 0;
    CHECK(flt_entry(b.shader, &selected, ir, count, &exact_count, &linkage) == SGX535_NIR_OK);
    CHECK(exact_count == count);
    flt_refuse(b.shader, &selected, count - 1u, SGX535_NIR_ERR_LIMIT);
    unsigned char saved[sizeof(ir)];
    memcpy(saved, ir, sizeof(ir));
    CHECK(flt_entry(b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, (size_t *)ir, &linkage) ==
          SGX535_NIR_ERR_ARGUMENT);
    CHECK(!memcmp(saved, ir, sizeof(ir)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    size_t old_count = 0x83;
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, ir,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &old_count,
                                                 &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(old_count == 0x83);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_fneu_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ralloc_free(b.shader);
    return cases;
}

static unsigned inverse_equality_negative(void)
{
    unsigned cases = 0;
    for (unsigned integer = 0; integer < 2; integer++)
        for (unsigned mode = 0; mode < 16; mode++) {
            nir_alu_instr *alu;
            nir_builder b = inverse_equality_shader(integer != 0, 2, 0, &alu);
            struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                           SGX535_FRAGMENT_IO_F32};
            enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
            if (mode == 0)
                alu->no_signed_wrap = true;
            else if (mode == 1)
                alu->no_unsigned_wrap = true;
            else if (mode == 2)
                alu->fp_math_ctrl |= nir_fp_preserve_signed_zero;
            else if (mode == 3)
                alu->src[0].src.ssa->bit_size = 16;
            else if (mode == 4)
                alu->src[1].src.ssa->bit_size = 16;
            else if (mode == 5)
                alu->def.bit_size = 32;
            else if (mode == 6)
                alu->src[0].swizzle[0] = 4;
            else if (mode == 7)
                alu->def.num_components = 0;
            else if (mode == 8)
                b.shader->info.float_controls_execution_mode = 1;
            else if (mode == 9) {
                b.cursor = nir_before_instr(&alu->instr);
                nir_ddx(&b, nir_imm_float(&b, 1)); /* Dead unsupported source still refuses. */
            } else if (mode == 10) {
                b.cursor = nir_before_instr(&alu->instr);
                nir_fge(&b, alu->src[0].src.ssa, alu->src[0].src.ssa); /* Dead dynamic FGE. */
            } else if (mode == 11) {
                nir_push_if(&b, nir_imm_bool(&b, true));
                nir_pop_if(&b, NULL);
            } else if (mode == 12) {
                selected.uniform_layout = (enum sgx535_fragment_uniform_layout)2;
            } else if (mode == 13) {
                b.cursor = nir_before_instr(&alu->instr);
                for (unsigned i = 0; i < SGX535_NIR_MAX_INSTRUCTIONS; i++)
                    nir_imm_int(&b, i);
                expected = SGX535_NIR_ERR_LIMIT;
            } else if (mode == 14) {
                alu->op = integer ? nir_op_ine32 : nir_op_feq32;
                alu->def.bit_size = 32; /* Original prelowered compare cannot bypass bool1. */
            } else {
                nir_intrinsic_instr *load =
                    nir_instr_as_intrinsic(nir_def_instr(alu->src[1].src.ssa));
                nir_intrinsic_set_range(load, 3); /* Offset 2 cannot hold four DWORDs. */
            }
            flt_refuse(b.shader, &selected, SGX535_NIR_MAX_INSTRUCTIONS, expected);
            ralloc_free(b.shader);
            cases++;
        }
    return cases;
}

static unsigned inverse_equality_diamond(bool integer, bool unsupported)
{
    nir_builder b = bool32_builder();
    nir_def *input = &varying(&b, 4)->def;
    nir_def *x = nir_channel(&b, input, 0), *y = nir_channel(&b, input, 1);
    nir_def *condition = ordered_ge_diamond ? nir_fge(&b, x, nir_imm_float(&b, 1))
                         : integer          ? nir_ine(&b, x, y)
                                            : nir_feq(&b, x, y);
    nir_if *branch = nir_push_if(&b, unsupported ? nir_imm_bool(&b, true) : condition);
    nir_def *left = nir_imm_ivec4(&b, 0x7f812345, 0x80000000, 1, 0xff800000);
    nir_push_else(&b, branch);
    nir_def *right = nir_imm_ivec4(&b, 0xffc12345, 0, 0x80000001, 0x7f800000);
    if (unsupported)
        nir_fge(&b, x, y); /* The constant condition cannot hide this unselected arm. */
    nir_pop_if(&b, branch);
    result(&b, nir_if_phi(&b, left, right));
    nir_validate_shader(b.shader, "inverse-equality pure diamond");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        saved[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    struct sgx535_fragment_program program;
    size_t count = 0x5387;
    memset(ir, 0x9b, sizeof(ir));
    memcpy(saved, ir, sizeof(ir));
    memset(&linkage, 0xe7, sizeof(linkage));
    old_linkage = linkage;
    char *before = snapshot(b.shader), *after;
    CHECK(sgx535_nir_fragment_pure_diamond_options_ir(
              b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage) ==
          (unsupported ? SGX535_NIR_ERR_UNSUPPORTED : SGX535_NIR_OK));
    unsigned cases = 0;
    if (unsupported) {
        CHECK(count == 0x5387 && !memcmp(ir, saved, sizeof(ir)) &&
              !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    } else {
        CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
        CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
        for (unsigned a = 0; a < 26; a++)
            for (unsigned other = 0; other < 26; other++) {
                uint32_t primary[4] = {flt_values[a], flt_values[other], 0, 0}, actual[4];
                const uint32_t on_true[4] = {0x7f812345, 0x80000000, 1, 0xff800000};
                const uint32_t on_false[4] = {0xffc12345, 0, 0x80000001, 0x7f800000};
                float left_float, right_float;
                memcpy(&left_float, primary, sizeof(left_float));
                memcpy(&right_float, primary + 1, sizeof(right_float));
                bool truth = ordered_ge_diamond ? left_float >= 1.0f
                             : integer          ? primary[0] != primary[1]
                                                : left_float == right_float;
                raw_words(&program.encoding, primary, 0, actual);
                CHECK(!memcmp(actual, truth ? on_true : on_false, sizeof(actual)));
                cases++;
            }
    }
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
    return cases;
}

/* General IEEE32 floor, checked by actual NIR and independently decoded
 * emitted words. HOST MAD execution below is CPU evidence, not GPU precision. */
static nir_builder floor_shader(unsigned width, nir_alu_instr **operation)
{
    nir_builder b = bool32_builder();
    nir_def *value = nir_ffloor(&b, &varying(&b, 4)->def);
    nir_alu_instr *alu = nir_def_as_alu(value);
    alu->def.num_components = width;
    for (unsigned lane = 0; lane < width; lane++)
        alu->src[0].swizzle[lane] = 3u - lane;
    nir_def *out[4];
    for (unsigned lane = 0; lane < 4; lane++)
        out[lane] = nir_channel(&b, value, (lane + 1u) % width);
    result(&b, nir_vec(&b, out, 4));
    *operation = alu;
    return b;
}

static uint32_t floor_reference_word(uint32_t raw)
{
    if ((raw & UINT32_C(0x7fffffff)) >= UINT32_C(0x7f800000))
        return raw;
    float value, rounded;
    float (*volatile reference)(float) = floorf;
    uint32_t result;
    memcpy(&value, &raw, sizeof(value));
    rounded = reference(value);
    memcpy(&result, &rounded, sizeof(result));
    return result;
}

static void floor_refuse(nir_shader *shader, const struct sgx535_fragment_nir_options *options,
                         size_t capacity, enum sgx535_nir_status expected)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x6f31;
    memset(ir, 0xc7, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0xa9, sizeof(linkage));
    old_linkage = linkage;
    char *before = snapshot(shader);
    enum sgx535_nir_status actual =
        sgx535_nir_fragment_floor_options_ir(shader, options, ir, capacity, &count, &linkage);
    if (actual != expected)
        fprintf(stderr, "floor refusal actual=%d expected=%d\n", actual, expected);
    CHECK(actual == expected);
    CHECK(count == 0x6f31 && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    char *after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static unsigned floor_positive(unsigned width)
{
    nir_alu_instr *alu;
    nir_builder b = floor_shader(width, &alu);
    alu->fp_math_ctrl = nir_fp_preserve_signed_zero | nir_fp_preserve_inf | nir_fp_preserve_nan;
    nir_validate_shader(b.shader, "general-floor-original");
    const struct sgx535_fragment_nir_options options = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                        SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    size_t count = 0;
    char *before = snapshot(b.shader);
    enum sgx535_nir_status status = sgx535_nir_fragment_floor_options_ir(
        b.shader, &options, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status)
        fprintf(stderr, "floor width=%u status=%d\n", width, status);
    CHECK(status == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    unsigned shifts = 0;
    for (size_t i = 0; i < count; i++)
        shifts += ir[i].opcode == SGX535_FRAGMENT_RAW_USHR_IMM;
    CHECK(shifts == width * 5u);
    CHECK(count <= 512 && program.encoding.word_count <= 1024 &&
          program.encoding.temp_register_count <= 124);
    printf("floor width=%u IR=%zu DWORDs=%u TEMP=%u (CPU only)\n", width, count,
           program.encoding.word_count, program.encoding.temp_register_count);
    uint32_t random = 0x2714f3a9;
    unsigned cases = 0;
    static const uint32_t mantissa[] = {0, 1, 2, 0x3fffff, 0x400000, 0x7ffffe, 0x7fffff};
    for (unsigned pattern = 0; pattern < 8192; pattern++) {
        uint32_t input[4], expected[4], output[4];
        for (unsigned lane = 0; lane < 4; lane++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            unsigned value = pattern * 4u + lane;
            input[lane] = pattern < 896 ? ((value / 7u % 256u) << 23) | ((value / 1792u) << 31) |
                                              mantissa[value % 7u]
                                        : random;
        }
        for (unsigned lane = 0; lane < 4; lane++) {
            unsigned source = alu->src[0].swizzle[(lane + 1u) % width];
            expected[lane] = floor_reference_word(input[source]);
        }
        raw_words(&program.encoding, input, 0, output);
        if (memcmp(output, expected, sizeof(output)))
            fprintf(stderr, "floor word mismatch width=%u pattern=%u input=%08x/%08x/%08x/%08x\n",
                    width, pattern, input[0], input[1], input[2], input[3]);
        CHECK(!memcmp(output, expected, sizeof(output)));
        cases++;
    }
    size_t exact = 0;
    CHECK(sgx535_nir_fragment_floor_options_ir(b.shader, &options, ir, count, &exact, &linkage) ==
              SGX535_NIR_OK &&
          exact == count);
    floor_refuse(b.shader, &options, count - 1u, SGX535_NIR_ERR_LIMIT);
    flt_refuse(b.shader, &options, SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_UNSUPPORTED);
    char *after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
    return cases;
}

static unsigned floor_negative(void)
{
    for (unsigned mode = 0; mode < 12; mode++) {
        nir_alu_instr *alu;
        nir_builder b = floor_shader(1, &alu);
        struct sgx535_fragment_nir_options options = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                      SGX535_FRAGMENT_IO_F32};
        if (mode == 0)
            alu->def.bit_size = 16;
        else if (mode == 1)
            alu->src[0].src.ssa->bit_size = 16;
        else if (mode == 2)
            alu->src[0].swizzle[0] = 4;
        else if (mode == 3)
            alu->fp_math_ctrl = nir_fp_no_transform;
        else if (mode == 4)
            alu->no_signed_wrap = true;
        else if (mode == 5)
            alu->no_unsigned_wrap = true;
        else if (mode == 6)
            b.shader->info.float_controls_execution_mode = 1;
        else if (mode == 7) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_ushr(&b, alu->src[0].src.ssa, nir_imm_int(&b, 1));
        } else if (mode == 8) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_flt(&b, alu->src[0].src.ssa, alu->src[0].src.ssa);
        } else if (mode == 9) {
            nir_push_if(&b, nir_imm_bool(&b, true));
            nir_pop_if(&b, NULL);
        } else if (mode == 10)
            options.io_precision = (enum sgx535_fragment_io_precision_policy)2;
        floor_refuse(b.shader, mode == 11 ? NULL : &options, SGX535_NIR_MAX_INSTRUCTIONS,
                     mode == 11 ? SGX535_NIR_ERR_ARGUMENT : SGX535_NIR_ERR_UNSUPPORTED);
        ralloc_free(b.shader);
    }
    return 12;
}

int main(void)
{
    CHECK(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128);
    glsl_type_singleton_init_or_ref();
#ifdef POULSBACK_FGE_CONST_BASELINE
    ordered_ge = true;
    flt_positive(1, 0x3f800000, 0, nir_fp_preserve_inf, false, false);
    return 0;
#endif
#ifdef POULSBACK_FLT_CONST_BASELINE
    /* The real linear-small constant 2.0 < stop_delta, preserve-inf NIR shape. */
    if (false)
        flt_negative();
    if (false) {
        flt_unchanged_without_flt();
        flt_composition();
        inverse_equality_positive(false, 1, 0, 0);
        inverse_equality_negative();
        inverse_equality_diamond(false, false);
    }
    flt_positive(1, 0x40000000, 1, nir_fp_preserve_inf, false, false);
#else
    unsigned cases = 0;
    for (unsigned integer = 0; integer < 2; integer++)
        for (unsigned width = 1; width <= 4; width++)
            for (unsigned layout = 0; layout < 2; layout++)
                for (unsigned mode = 0; mode < 3; mode++)
                    cases += inverse_equality_positive(integer != 0, width, layout, mode);
    unsigned equality_refused = inverse_equality_negative();
    for (unsigned integer = 0; integer < 2; integer++) {
        cases += inverse_equality_diamond(integer != 0, false);
        inverse_equality_diamond(integer != 0, true);
        equality_refused++;
    }
    for (unsigned constant = 0; constant < 26; constant++)
        for (unsigned order = 0; order < 2; order++)
            cases += flt_positive(1, flt_values[constant], order,
                                  nir_fp_preserve_inf | nir_fp_preserve_nan, false, false);
    for (unsigned width = 1; width <= 4; width++)
        for (unsigned order = 0; order < 2; order++) {
            cases += flt_positive(width, 0x40000000, order, nir_fp_preserve_inf, false, false);
            cases += flt_positive(width, 0, order, 0, true, false);
            cases += flt_positive(width, 0x40000000, order, nir_fp_preserve_nan, false, true);
        }
    cases += flt_composition();
    flt_unchanged_without_flt();
    unsigned refused = flt_negative() + equality_refused;
    ordered_ge = true;
    for (unsigned constant = 0; constant < 26; constant++)
        for (unsigned order = 0; order < 2; order++)
            cases += flt_positive(1, flt_values[constant], order,
                                  nir_fp_preserve_inf | nir_fp_preserve_nan, false, false);
    for (unsigned width = 1; width <= 4; width++)
        for (unsigned order = 0; order < 2; order++) {
            cases += flt_positive(width, 0x3f800000, order, nir_fp_preserve_inf, false, false);
            cases += flt_positive(width, 0, order, 0, true, false);
            cases += flt_positive(width, 0x40000000, order, nir_fp_preserve_nan, false, true);
        }
    refused += flt_negative();
    ordered_ge = false;
    ordered_ge_diamond = true;
    cases += inverse_equality_diamond(false, false);
    inverse_equality_diamond(false, true);
    refused++;
    ordered_ge_diamond = false;
    cases += floor_positive(1);
    cases += floor_positive(2);
    refused += floor_negative();
    CHECK(flt_const_legacy_fneu_main() == 0);
    printf("actual NIR constant FLT/FGE and FEQ/INE: %u raw-word vectors, %u refusals PASS "
           "(CPU only; native closed)\n",
           cases, refused);
#endif
    glsl_type_singleton_decref();
    return 0;
}
