// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Signed truth uses int64_t arithmetic, independently of NIR and emitted words. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t compare_values[] = {0,
                                          1,
                                          2,
                                          3,
                                          4,
                                          5,
                                          6,
                                          7,
                                          8,
                                          9,
                                          UINT32_MAX,
                                          UINT32_MAX - 1,
                                          UINT32_C(0x7ffffffe),
                                          UINT32_C(0x7fffffff),
                                          UINT32_C(0x80000000),
                                          UINT32_C(0x80000001),
                                          UINT32_C(0x40000000),
                                          UINT32_C(0xc0000000),
                                          UINT32_C(0x55555555),
                                          UINT32_C(0xaaaaaaaa),
                                          UINT32_C(0x0000ffff),
                                          UINT32_C(0xffff0000),
                                          UINT32_C(0x01010101),
                                          UINT32_C(0x80808080)};

static int64_t compare_signed_value(uint32_t value)
{
    return value <= INT32_MAX ? (int64_t)value : (int64_t)value - INT64_C(4294967296);
}

static bool compare_reference(uint32_t left, uint32_t right)
{
    return compare_signed_value(left) >= compare_signed_value(right);
}

static unsigned compare_reference_checks(void)
{
    uint32_t random = UINT32_C(0x748d53a1);
    unsigned count = 0;
    for (unsigned i = 0; i < sizeof(compare_values) / sizeof(*compare_values); i++)
        for (unsigned j = 0; j < sizeof(compare_values) / sizeof(*compare_values); j++) {
            uint32_t left = compare_values[i], right = compare_values[j];
            if (compare_reference(left, right) !=
                ((left ^ UINT32_C(0x80000000)) >= (right ^ UINT32_C(0x80000000))))
                abort();
            count++;
        }
    for (unsigned i = 0; i < 65536; i++) {
        uint32_t left, right;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        left = random;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        right = random;
        if (compare_reference(left, right) !=
            ((left ^ UINT32_C(0x80000000)) >= (right ^ UINT32_C(0x80000000))))
            abort();
        count++;
    }
    return count;
}

#ifdef POULSBACK_COMPARE_REFERENCE_ONLY
int main(void)
{
    printf("independent signed32/sign-normalized ordering: %u checks PASS (model only)\n",
           compare_reference_checks());
    return 0;
}
#else
#include "sgx535_fragment_compare.h"
#include "poulsback_program.h"
/* Reuse the existing actual-NIR and independent raw-word harness. Its legacy
 * entry is compiled but not rerun by this fixture. */
int compare_legacy_fneu_main(void);
#define POULSBACK_FNEU_TEST_MAIN compare_legacy_fneu_main
#include "test_nir_fragment_bool32_fneu.c"
#undef POULSBACK_FNEU_TEST_MAIN

static enum sgx535_nir_status compare_old_options(const nir_shader *shader,
                                                  enum sgx535_fragment_uniform_layout layout,
                                                  struct sgx535_fragment_instruction *ir,
                                                  size_t capacity, size_t *count,
                                                  struct sgx535_fragment_linkage *linkage)
{
    const struct sgx535_fragment_nir_options selected = {layout, SGX535_FRAGMENT_IO_F32};
    return sgx535_nir_fragment_bool32_options_ir(shader, &selected, ir, capacity, count, linkage);
}

static void compare_refuse(nir_shader *shader, const struct sgx535_fragment_nir_options *selected,
                           size_t capacity, enum sgx535_nir_status expected)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x873a;
    enum sgx535_nir_status status;
    char *before = snapshot(shader), *after;
    memset(ir, 0xb7, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0x49, sizeof(linkage));
    old_linkage = linkage;
    status =
        sgx535_nir_fragment_compare_options_ir(shader, selected, ir, capacity, &count, &linkage);
    if (status != expected) {
        fprintf(stderr, "constant-IGE refusal: got %d, expected %d, capacity %zu\n", status,
                expected, capacity);
        fputs(before, stderr);
    }
    CHECK(status == expected);
    CHECK(count == 0x873a && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static nir_builder compare_shader(unsigned width, uint32_t threshold, unsigned order,
                                  unsigned medium, nir_alu_instr **comparison)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *input = varying(&b, 4);
    nir_io_semantics semantics = nir_intrinsic_io_semantics(input);
    nir_def *value, *out[4];
    semantics.medium_precision = medium;
    nir_intrinsic_set_io_semantics(input, semantics);
    /* Keep the constant operand a direct load_const vector, with independent
     * source swizzles. A vec-of-constants is deliberately not guessed constant. */
    nir_def *c = nir_imm_ivec4(&b, threshold, threshold + 1u, threshold + 2u, threshold + 3u);
    value = order ? nir_ige(&b, c, &input->def) : nir_ige(&b, &input->def, c);
    nir_alu_instr *alu = nir_def_as_alu(value);
    alu->def.num_components = width;
    for (unsigned lane = 0; lane < width; lane++) {
        alu->src[order ? 0 : 1].swizzle[lane] = (lane * 3u + 1u) % 4;
        alu->src[order ? 1 : 0].swizzle[lane] = (3u - lane) % 4;
    }
    value = nir_b2b32(&b, value);
    for (unsigned lane = 0; lane < 4; lane++)
        out[lane] = nir_channel(&b, value, (lane + 1u) % width);
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "signed-IGE-constant-original");
    *comparison = alu;
    return b;
}

static unsigned compare_positive(unsigned width, uint32_t threshold, unsigned order,
                                 unsigned medium, unsigned layout)
{
    nir_alu_instr *alu;
    nir_builder b = compare_shader(width, threshold, order, medium, &alu);
    const struct sgx535_fragment_nir_options selected = {layout, SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t primary[4], actual[4], expected[4];
    size_t count = 0;
    enum sgx535_nir_status status;
    char *before = snapshot(b.shader), *after;
    unsigned cases = 0;
    status = sgx535_nir_fragment_compare_options_ir(b.shader, &selected, ir,
                                                    SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status != SGX535_NIR_OK)
        fprintf(stderr,
                "constant-IGE width=%u threshold=%08x order=%u medium=%u layout=%u status=%d\n",
                width, threshold, order, medium, layout, status);
    CHECK(status == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    for (unsigned pattern = 0; pattern < 32; pattern++) {
        for (unsigned lane = 0; lane < 4; lane++)
            primary[lane] = pattern < 24   ? compare_values[(pattern + lane) % 24]
                            : pattern < 27 ? threshold + pattern - 25u + lane
                                           : UINT32_C(0x80000000) + pattern - 28u + lane;
        for (unsigned lane = 0; lane < 4; lane++) {
            unsigned logical = (lane + 1u) % width;
            uint32_t dynamic = primary[alu->src[order ? 1 : 0].swizzle[logical]];
            uint32_t constant = threshold + alu->src[order ? 0 : 1].swizzle[logical];
            bool truth =
                order ? compare_reference(constant, dynamic) : compare_reference(dynamic, constant);
            expected[lane] = truth ? UINT32_MAX : 0;
        }
        raw_words(&program.encoding, primary, 0, actual);
        if (memcmp(actual, expected, sizeof(actual))) {
            fprintf(
                stderr,
                "constant-IGE numeric width=%u threshold=%08x order=%u medium=%u layout=%u pattern=%u\n",
                width, threshold, order, medium, layout, pattern);
            for (unsigned lane = 0; lane < 4; lane++)
                fprintf(stderr, "lane%u input=%08x actual=%08x expected=%08x\n", lane,
                        primary[lane], actual[lane], expected[lane]);
        }
        CHECK(!memcmp(actual, expected, sizeof(actual)));
        cases++;
    }
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    /* Both older APIs retain their original IGE refusal. */
    ior_failure(b.shader, compare_old_options, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ralloc_free(b.shader);
    return cases;
}

static unsigned compare_negative(void)
{
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 20; mode++) {
        nir_alu_instr *alu;
        nir_builder b = compare_shader(1, 3, 0, 1, &alu);
        struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                       SGX535_FRAGMENT_IO_F32};
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        size_t capacity = SGX535_NIR_MAX_INSTRUCTIONS;
        if (mode == 0)
            nir_src_rewrite(&alu->src[1].src, alu->src[0].src.ssa);
        else if (mode == 1)
            alu->src[0].src.ssa->bit_size = 16;
        else if (mode == 2)
            alu->src[1].src.ssa->bit_size = 16;
        else if (mode == 3)
            alu->def.bit_size = 32;
        else if (mode == 4) {
            alu->src[0].src.ssa->num_components = 1;
            alu->src[0].swizzle[0] = 1;
        } else if (mode == 5)
            alu->fp_math_ctrl = nir_fp_preserve_inf;
        else if (mode == 6)
            alu->fp_math_ctrl = nir_fp_preserve_signed_zero;
        else if (mode == 7)
            alu->no_signed_wrap = true;
        else if (mode == 8)
            alu->no_unsigned_wrap = true;
        else if (mode == 9) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_flt(&b, nir_imm_float(&b, 0), nir_imm_float(&b, 1));
        } else if (mode == 10) {
            selected.uniform_layout = (enum sgx535_fragment_uniform_layout)2;
        } else if (mode == 11) {
            selected.io_precision = (enum sgx535_fragment_io_precision_policy)2;
        } else if (mode == 12) {
            capacity = 1;
            expected = SGX535_NIR_ERR_LIMIT;
        } else if (mode == 13) {
            expected = SGX535_NIR_ERR_ARGUMENT;
        } else if (mode == 14) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_def *noncanonical = nir_b2b1(&b, nir_imm_int(&b, 1));
            nir_iand(&b, noncanonical, nir_imm_bool(&b, true));
        } else if (mode == 15) {
            nir_push_if(&b, nir_imm_bool(&b, true));
            nir_pop_if(&b, NULL);
        } else if (mode == 16) {
            b.cursor = nir_before_instr(&alu->instr);
            nir_iand(&b, nir_imm_int(&b, 3), nir_imm_int(&b, 7));
        } else if (mode == 17) {
            alu->op = nir_op_ige32;
            alu->def.bit_size = 32;
        } else if (mode == 18) {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(nir_def_instr(alu->src[0].src.ssa));
            nir_io_semantics semantics = nir_intrinsic_io_semantics(in);
            semantics.high_16bits = 1;
            nir_intrinsic_set_io_semantics(in, semantics);
        } else {
            b.cursor = nir_before_instr(&alu->instr);
            nir_def *constant = nir_imm_int(&b, 3);
            nir_src_rewrite(&alu->src[1].src, nir_vec4(&b, constant, constant, constant, constant));
        }
        compare_refuse(b.shader, mode == 13 ? NULL : &selected, capacity, expected);
        cases++;
        ralloc_free(b.shader);
    }
    {
        nir_alu_instr *alu;
        nir_builder b = compare_shader(4, UINT32_C(0x55555555), 0, 0, &alu);
        const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                             SGX535_FRAGMENT_IO_F32};
        compare_refuse(b.shader, &selected, SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_LIMIT);
        cases++;
        ralloc_free(b.shader);
    }
    return cases;
}

static void compare_unchanged_without_ige(void)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *in = varying(&b, 4);
    nir_io_semantics semantics = nir_intrinsic_io_semantics(in);
    semantics.medium_precision = 1;
    nir_intrinsic_set_io_semantics(in, semantics);
    nir_def *condition = nir_fneu(&b, nir_channel(&b, &in->def, 0), nir_imm_int(&b, 0));
    nir_def_as_alu(condition)->fp_math_ctrl = nir_fp_preserve_inf | nir_fp_preserve_nan;
    result(&b, nir_bcsel(&b, condition, &in->def, nir_imm_ivec4(&b, 1, -1, 0x7fc12345, 0)));
    nir_validate_shader(b.shader, "no-IGE-identical-Boolean-construction");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction old[SGX535_NIR_MAX_INSTRUCTIONS],
        current[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage old_linkage, current_linkage;
    size_t old_count = 0, current_count = 0;
    char *before = snapshot(b.shader), *after;
    CHECK(sgx535_nir_fragment_bool32_options_ir(b.shader, &selected, old,
                                                SGX535_NIR_MAX_INSTRUCTIONS, &old_count,
                                                &old_linkage) == SGX535_NIR_OK);
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, current,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &current_count,
                                                 &current_linkage) == SGX535_NIR_OK);
    CHECK(old_count == current_count && !memcmp(old, current, old_count * sizeof(*old)) &&
          !memcmp(&old_linkage, &current_linkage, sizeof(old_linkage)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
}

static unsigned compare_composition(void)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *in = varying(&b, 4);
    nir_def *condition = nir_ige(&b, nir_channel(&b, &in->def, 0), nir_imm_int(&b, 3));
    nir_def *unequal = nir_fneu(&b, nir_channel(&b, &in->def, 1), nir_imm_int(&b, 0));
    nir_def_as_alu(unequal)->fp_math_ctrl = nir_fp_preserve_inf | nir_fp_preserve_nan;
    condition = nir_iand(&b, condition, unequal);
    nir_def *otherwise = nir_imm_ivec4(&b, 0x7fc12345, 0x80000000, 0x7f800000, 0xff800000);
    result(&b, nir_bcsel(&b, condition, &in->def, otherwise));
    nir_validate_shader(b.shader, "signed-IGE-FNEU-BCSEL-composition");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    static const uint32_t alternative[4] = {0x7fc12345, 0x80000000, 0x7f800000, 0xff800000};
    uint32_t primary[4], actual[4], expected[4];
    size_t count = 0;
    unsigned cases = 0;
    char *before = snapshot(b.shader), *after;
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, ir,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                 &linkage) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    for (unsigned pattern = 0; pattern < 24; pattern++) {
        for (unsigned lane = 0; lane < 4; lane++)
            primary[lane] = fneu_pairs[(pattern + lane) % 24].left;
        bool truth = compare_reference(primary[0], 3) && fneu_float_reference(primary[1], 0);
        for (unsigned lane = 0; lane < 4; lane++)
            expected[lane] = truth ? primary[lane] : alternative[lane];
        raw_words(&program.encoding, primary, 0, actual);
        CHECK(!memcmp(actual, expected, sizeof(actual)));
        cases++;
    }
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
    return cases;
}

static nir_builder compare_uniform_shader(unsigned width, unsigned base, unsigned range,
                                          unsigned offset, unsigned threshold, unsigned order,
                                          bool passthrough, nir_intrinsic_instr **load)
{
    nir_builder b = bool32_builder();
    nir_def *value = nir_load_uniform(&b, width, 32, nir_imm_int(&b, offset), .base = base,
                                      .range = range, .dest_type = nir_type_int32);
    *load = nir_def_as_intrinsic(value);
    if (!passthrough) {
        nir_def *constant = nir_imm_int(&b, threshold);
        value = order ? nir_ige(&b, constant, value) : nir_ige(&b, value, constant);
        value = nir_b2b32(&b, value);
    }
    nir_def *out[4];
    for (unsigned lane = 0; lane < 4; lane++)
        out[lane] = nir_channel(&b, value, (lane + 1u) % width);
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "typed-int32-uniform-constant-comparison");
    return b;
}

static unsigned compare_uniform_case(unsigned layout, unsigned width, unsigned base, unsigned range,
                                     unsigned offset, unsigned threshold, unsigned order,
                                     bool passthrough)
{
    const struct sgx535_fragment_nir_options selected = {layout, SGX535_FRAGMENT_IO_F32};
    nir_intrinsic_instr *load;
    nir_builder b =
        compare_uniform_shader(width, base, range, offset, threshold, order, passthrough, &load);
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    uint32_t buffer[128], copy[128], primary[4] = {0}, actual[4], expected[4];
    uint32_t bitmap[4] = {0};
    unsigned first = (base + offset) * (layout == SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS ? 4u : 1u);
    size_t count = 0, words;
    char *before = snapshot(b.shader), *after;
    enum sgx535_nir_status status = sgx535_nir_fragment_compare_options_ir(
        b.shader, &selected, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status != SGX535_NIR_OK)
        fprintf(stderr, "int32 uniform layout=%u width=%u base=%u range=%u off=%u status=%d\n",
                layout, width, base, range, offset, status);
    CHECK(status == SGX535_NIR_OK);
    CHECK(nir_intrinsic_dest_type(load) == nir_type_int32);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    for (unsigned lane = 0; lane < width; lane++)
        bitmap[(first + lane) / 32u] |= UINT32_C(1) << ((first + lane) % 32u);
    CHECK(!memcmp(bitmap, program.uniform_words_used, sizeof(bitmap)));
    CHECK(program.secondary_register_count == first + width);
    for (unsigned pattern = 0; pattern < 32; pattern++) {
        for (unsigned word = 0; word < 128; word++)
            buffer[word] = UINT32_C(0x748d0000) + word;
        for (unsigned lane = 0; lane < width; lane++)
            buffer[first + lane] = pattern < 24 ? compare_values[(pattern + lane) % 24]
                                                : fneu_pairs[(pattern - 24u + lane) % 24].left;
        memset(copy, 0xb9, sizeof(copy));
        words = SIZE_MAX;
        CHECK(poulsback_fragment_uniform_snapshot(&program, buffer, sizeof(buffer), copy, 128,
                                                  &words) == SGX535_NIR_OK);
        CHECK(words == first + width && !memcmp(copy, buffer, words * sizeof(*copy)));
        for (size_t i = words; i < 128; i++)
            CHECK(copy[i] == UINT32_C(0xb9b9b9b9));
        for (unsigned lane = 0; lane < 4; lane++) {
            uint32_t payload = buffer[first + (lane + 1u) % width];
            bool truth = order ? compare_reference(threshold, payload)
                               : compare_reference(payload, threshold);
            expected[lane] = passthrough ? payload : truth ? UINT32_MAX : 0;
        }
        raw_words_with_secondary(&program.encoding, primary, copy, 0, words, actual);
        CHECK(!memcmp(actual, expected, sizeof(actual)));
    }
    /* The copied prefix becomes independent of subsequent buffer updates. */
    memset(buffer, 0, sizeof(buffer));
    raw_words_with_secondary(&program.encoding, primary, copy, 0, words, actual);
    CHECK(!memcmp(actual, expected, sizeof(actual)));
    memset(copy, 0xb9, sizeof(copy));
    words = SIZE_MAX;
    CHECK(poulsback_fragment_uniform_snapshot(&program, buffer, (first + width - 1u) * 4u, copy,
                                              128, &words) == SGX535_NIR_ERR_LIMIT);
    CHECK(words == SIZE_MAX);
    for (unsigned word = 0; word < 128; word++)
        CHECK(copy[word] == UINT32_C(0xb9b9b9b9));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    /* The old options and strict APIs retain float-only uniform admission. */
    ior_failure(b.shader, compare_old_options, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ralloc_free(b.shader);
    return 32;
}

static unsigned compare_uniforms(void)
{
    unsigned cases = 0;
    for (unsigned layout = 0; layout < 2; layout++) {
        unsigned stride = layout == SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS ? 4u : 1u;
        for (unsigned order = 0; order < 2; order++)
            for (unsigned threshold = 1; threshold <= 8; threshold++) {
                /* Actual n_stop shape: scalar int32, base9/range1/offset0. */
                cases += compare_uniform_case(layout, 1, 9, 1, 0, threshold, order, false);
                cases += compare_uniform_case(layout, 4, 5, 6, 2, threshold, order, false);
            }
        cases += compare_uniform_case(layout, stride, 128u / stride - 1u, 1, 0, 3, 0, false);
        cases += compare_uniform_case(layout, 4, 0, 4, 0, 0, 0, true);
    }
    return cases;
}

static unsigned compare_uniform_refusals(void)
{
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                         SGX535_FRAGMENT_IO_F32};
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 12; mode++) {
        nir_intrinsic_instr *load;
        nir_builder b = compare_uniform_shader(1, 9, 1, 0, 3, 0, true, &load);
        if (mode < 4)
            nir_intrinsic_set_dest_type(load, mode == 0   ? nir_type_uint32
                                              : mode == 1 ? nir_type_bool32
                                              : mode == 2 ? nir_type_int16
                                                          : nir_type_int64);
        else if (mode == 4)
            nir_intrinsic_set_range(load, 0);
        else if (mode == 5)
            nir_intrinsic_set_base(load, UINT32_MAX);
        else if (mode == 6)
            nir_intrinsic_set_range(load, UINT32_MAX);
        else if (mode == 7)
            nir_intrinsic_set_range(load, 120);
        else if (mode == 8) {
            b.cursor = nir_before_instr(&load->instr);
            nir_src_rewrite(&load->src[0], nir_imm_int(&b, 1));
        } else if (mode == 9)
            nir_intrinsic_set_access(load, ACCESS_CAN_REORDER);
        else if (mode == 10) {
            b.cursor = nir_before_instr(&load->instr);
            nir_def *index = nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = 0, .range = 1,
                                              .dest_type = nir_type_int32);
            nir_src_rewrite(&load->src[0], index);
        } else {
            b.cursor = nir_before_instr(&load->instr);
            nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = 0, .range = 1,
                             .dest_type = nir_type_uint32);
        }
        compare_refuse(b.shader, &selected, SGX535_NIR_MAX_INSTRUCTIONS,
                       SGX535_NIR_ERR_UNSUPPORTED);
        ralloc_free(b.shader);
        cases++;
    }
    return cases;
}

static void compare_float_uniform_unchanged(void)
{
    nir_intrinsic_instr *load;
    nir_builder b = compare_uniform_shader(4, 7, 4, 0, 0, 0, true, &load);
    nir_intrinsic_set_dest_type(load, nir_type_float32);
    nir_validate_shader(b.shader, "float-uniform-old-entry-contract");
    const struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                         SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_instruction old[SGX535_NIR_MAX_INSTRUCTIONS],
        current[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage old_linkage, current_linkage;
    size_t old_count = 0, current_count = 0;
    char *before = snapshot(b.shader), *after;
    CHECK(sgx535_nir_fragment_bool32_options_ir(b.shader, &selected, old,
                                                SGX535_NIR_MAX_INSTRUCTIONS, &old_count,
                                                &old_linkage) == SGX535_NIR_OK);
    CHECK(sgx535_nir_fragment_compare_options_ir(b.shader, &selected, current,
                                                 SGX535_NIR_MAX_INSTRUCTIONS, &current_count,
                                                 &current_linkage) == SGX535_NIR_OK);
    CHECK(old_count == current_count && !memcmp(old, current, old_count * sizeof(*old)) &&
          !memcmp(&old_linkage, &current_linkage, sizeof(old_linkage)));
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ralloc_free(b.shader);
}

int main(void)
{
    unsigned reference = compare_reference_checks(), cases = 0, negative, composition, uniforms,
             uniform_refusals;
    glsl_type_singleton_init_or_ref();
    for (unsigned order = 0; order < 2; order++)
        for (unsigned medium = 0; medium < 2; medium++)
            for (unsigned layout = 0; layout < 2; layout++) {
                for (unsigned i = 0; i < 24; i++)
                    cases += compare_positive(1, compare_values[i], order, medium, layout);
                for (unsigned width = 2; width <= 4; width++)
                    for (unsigned threshold = 1; threshold <= 8; threshold++)
                        cases += compare_positive(width, threshold, order, medium, layout);
            }
    negative = compare_negative();
    composition = compare_composition();
    compare_unchanged_without_ige();
    uniforms = compare_uniforms();
    uniform_refusals = compare_uniform_refusals();
    compare_float_uniform_unchanged();
    glsl_type_singleton_decref();
    CHECK(reference == 66112 && cases == 12288 && negative == 21 && composition == 24);
    CHECK(uniforms == 2176 && uniform_refusals == 12);
    printf(
        "actual NIR constant signed IGE: %u raw-word comparisons, %u combined selects, %u refusals, %u independent truth checks PASS (CPU only; native closed)\n",
        cases, composition, negative, reference);
    printf(
        "actual NIR int32 uniforms: %u SECONDARY raw-word cases, %u typed/address refusals PASS (CPU only; no uniform DMA qualification)\n",
        uniforms, uniform_refusals);
    return 0;
}
#endif
