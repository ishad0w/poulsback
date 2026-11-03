// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Raw32 selection truth is independent of the compiler's instruction words. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t bcsel_values[] = {
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
    UINT32_C(0x7fc00000),
    UINT32_C(0xffc00000),
    UINT32_C(0x7fc12345),
    UINT32_C(0xffc12345),
    UINT32_C(0x7fffffff),
    UINT32_MAX,
};

static uint32_t bcsel_ternary_reference(bool condition, uint32_t left, uint32_t right)
{
    return condition ? left : right;
}

static uint32_t bcsel_mask_model(bool condition, uint32_t left, uint32_t right)
{
    uint32_t mask = condition ? UINT32_MAX : 0;
    return (mask & left) | (~mask & right);
}

static void bcsel_model_check(bool condition, uint32_t left, uint32_t right)
{
    if (bcsel_ternary_reference(condition, left, right) !=
        bcsel_mask_model(condition, left, right)) {
        fprintf(stderr, "BCSEL raw32 truth differs for %u/%08x/%08x\n", condition ? 1u : 0u, left,
                right);
        abort();
    }
}

static unsigned bcsel_reference_checks(void)
{
    unsigned i, j, condition, count = 0;
    uint32_t state = UINT32_C(0x629f380d);
    for (i = 0; i < 24; i++)
        for (j = 0; j < 24; j++)
            for (condition = 0; condition < 2; condition++) {
                bcsel_model_check(condition != 0, bcsel_values[i], bcsel_values[j]);
                count++;
            }
    for (i = 0; i < 32; i++)
        for (j = 0; j < 32; j++)
            for (condition = 0; condition < 2; condition++) {
                bcsel_model_check(condition != 0, UINT32_C(1) << i, UINT32_C(1) << j);
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
        for (condition = 0; condition < 2; condition++) {
            bcsel_model_check(condition != 0, left, right);
            count++;
        }
    }
    for (i = 0; i < 24; i++)
        for (condition = 0; condition < 2; condition++) {
            bcsel_model_check(condition != 0, bcsel_values[i], bcsel_values[i]);
            count++;
        }
    return count;
}

#ifdef POULSBACK_BCSEL_REFERENCE_ONLY
int main(void)
{
    printf("independent raw32 BCSEL truth: %u checks PASS (model only)\n",
           bcsel_reference_checks());
    return 0;
}
#else
int bcsel_legacy_fneu_main(void);
#define POULSBACK_FNEU_TEST_MAIN bcsel_legacy_fneu_main
#include "test_nir_fragment_bool32_fneu.c"
#undef POULSBACK_FNEU_TEST_MAIN

static nir_def *bcsel_condition(nir_builder *b, unsigned kind, unsigned pattern, unsigned lane,
                                bool *truth)
{
    unsigned index = (pattern + lane) % 24;
    nir_def *value;
    if (kind == 0) {
        *truth = false;
        return nir_imm_bool(b, false);
    }
    if (kind == 1) {
        *truth = ((pattern + lane) & 1u) != 0;
        return nir_imm_bool(b, *truth);
    }
    if (kind == 2) {
        *truth = (index & 1u) != 0;
        return nir_ieq(b, nir_imm_intN_t(b, bcsel_values[index], 32),
                       nir_imm_intN_t(b, *truth ? bcsel_values[index] : ~bcsel_values[index], 32));
    }
    *truth = fneu_float_reference(fneu_pairs[index].left, fneu_pairs[index].right);
    value = nir_fneu(b, nir_imm_intN_t(b, fneu_pairs[index].left, 32),
                     nir_imm_intN_t(b, fneu_pairs[index].right, 32));
    nir_def_as_alu(value)->fp_math_ctrl = nir_fp_preserve_inf | nir_fp_preserve_nan;
    return value;
}

static void bcsel_positive(unsigned width, unsigned pattern, unsigned kind, unsigned alias)
{
    nir_builder b = bool32_builder();
    nir_def *condition[4], *left[4], *right[4], *out[4], *cond, *a, *other, *value;
    nir_alu_instr *op;
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    bool truth[4];
    uint32_t av[4], bv[4], first[4], final[4], expected[4], actual[4], primary[4] = {0};
    unsigned lane, condition_width = pattern & 1u ? width : 1u;
    unsigned ands = 0, xors = 0, comparisons = 0, selections = alias >= 2 ? 2 : 1;
    unsigned predicate_ands = kind == 3 ? 12u * condition_width : 0;
    unsigned predicate_xors = kind == 3 ? 12u * condition_width : 0;
    unsigned predicate_ieqs = kind == 3 ? 7u * condition_width : kind == 2 ? condition_width : 0;
    size_t count = 0, i;
    char *before, *after;
    for (lane = 0; lane < condition_width; lane++)
        condition[lane] = bcsel_condition(&b, kind, pattern, lane, &truth[lane]);
    cond = nir_vec(&b, condition, condition_width);
    for (lane = 0; lane < width; lane++) {
        av[lane] = pattern < 8 ? UINT32_C(1) << (pattern * 4u + lane)
                               : bcsel_values[(pattern * 3u + lane) % 24];
        bv[lane] = bcsel_values[(pattern * 5u + lane + 7u) % 24];
        left[lane] = nir_imm_intN_t(&b, av[lane], 32);
        right[lane] = nir_imm_intN_t(&b, bv[lane], 32);
    }
    a = nir_vec(&b, left, width);
    other = alias == 1 ? a : nir_vec(&b, right, width);
    if (alias == 2) {
        nir_def *mask = nir_b2b32(&b, cond), *expanded[4];
        for (lane = 0; lane < width; lane++)
            expanded[lane] = nir_channel(&b, mask, lane % condition_width);
        a = nir_vec(&b, expanded, width);
        other = nir_inot(&b, a);
    }
    value = nir_bcsel(&b, cond, a, other);
    op = nir_def_as_alu(value);
    for (lane = 0; lane < width; lane++) {
        unsigned c = (lane * 3u + 1u) % condition_width;
        unsigned l = width - 1u - lane, r = (lane + 1u) % width;
        uint32_t lvalue = alias == 2 ? (truth[l % condition_width] ? UINT32_MAX : 0) : av[l];
        uint32_t rvalue = alias == 1   ? av[r]
                          : alias == 2 ? (truth[r % condition_width] ? 0 : UINT32_MAX)
                                       : bv[r];
        op->src[0].swizzle[lane] = c;
        op->src[1].swizzle[lane] = l;
        op->src[2].swizzle[lane] = r;
        first[lane] = bcsel_ternary_reference(truth[c], lvalue, rvalue);
        final[lane] = first[lane];
    }
    if (alias == 2) {
        value =
            nir_bcsel(&b, nir_b2b1(&b, value), nir_vec(&b, left, width), nir_vec(&b, right, width));
        for (lane = 0; lane < width; lane++)
            final[lane] = bcsel_ternary_reference(first[lane] != 0, av[lane], bv[lane]);
    } else if (alias == 3) {
        value = nir_bcsel(&b, cond, value, a);
        op = nir_def_as_alu(value);
        for (lane = 0; lane < width; lane++) {
            unsigned c = (lane + 2u) % condition_width;
            unsigned l = (lane + 1u) % width, r = width - 1u - lane;
            op->src[0].swizzle[lane] = c;
            op->src[1].swizzle[lane] = l;
            op->src[2].swizzle[lane] = r;
            final[lane] = bcsel_ternary_reference(truth[c], first[l], av[r]);
        }
    }
    for (lane = 0; lane < 4; lane++) {
        unsigned component = (3u - lane) % width;
        out[lane] = nir_channel(&b, value, component);
        expected[lane] = final[component];
    }
    result(&b, nir_vec4(&b, out[0], out[1], out[2], out[3]));
    nir_validate_shader(b.shader, "bcsel-whole-original-three-swizzles-aliases-and-provenance");
    before = snapshot(b.shader);
    CHECK(sgx535_nir_fragment_bool32_bcsel_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                              SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                              &linkage) == SGX535_NIR_OK);
    for (i = 0; i < count; i++) {
        ands += ir[i].opcode == SGX535_FRAGMENT_RAW_AND;
        xors += ir[i].opcode == SGX535_FRAGMENT_RAW_XOR;
        comparisons += ir[i].opcode == SGX535_FRAGMENT_RAW_IEQ;
        CHECK(ir[i].opcode != SGX535_FRAGMENT_MAD);
    }
    CHECK(ands == predicate_ands + selections * 3u * width);
    CHECK(xors == predicate_xors + selections * 4u * width + (alias == 2 ? width : 0));
    CHECK(comparisons == predicate_ieqs);
    CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) == SGX535_NIR_OK);
    CHECK(count <= SGX535_NIR_MAX_INSTRUCTIONS &&
          program.encoding.word_count <= SGX535_NIR_MAX_WORDS);
    CHECK(program.encoding.temp_register_count <= SGX535_NIR_TEMP_COUNT);
    CHECK(program.encoding.word_count == 2u * count + 4u * comparisons);
    raw_words(&program.encoding, primary, 0, actual);
    CHECK(!memcmp(expected, actual, sizeof(actual)));
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    after = snapshot(b.shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_fneu_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                SGX535_NIR_ERR_UNSUPPORTED);
    ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, 1, SGX535_NIR_ERR_LIMIT);
    ralloc_free(b.shader);
}

static enum sgx535_nir_status options_default(const nir_shader *shader,
                                              enum sgx535_fragment_uniform_layout layout,
                                              struct sgx535_fragment_instruction *ir,
                                              size_t capacity, size_t *count,
                                              struct sgx535_fragment_linkage *linkage)
{
    const struct sgx535_fragment_nir_options selected = {layout, SGX535_FRAGMENT_IO_F32};
    return sgx535_nir_fragment_bool32_options_ir(shader, &selected, ir, capacity, count, linkage);
}

static void options_refuse(nir_shader *shader, const struct sgx535_fragment_nir_options *selected,
                           enum sgx535_nir_status expected)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        old[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x8a9b;
    char *before = snapshot(shader), *after;
    memset(ir, 0x37, sizeof(ir));
    memcpy(old, ir, sizeof(ir));
    memset(&linkage, 0xb9, sizeof(linkage));
    old_linkage = linkage;
    CHECK(sgx535_nir_fragment_bool32_options_ir(shader, selected, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                                &count, &linkage) == expected);
    CHECK(count == 0x8a9b && !memcmp(ir, old, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
    after = snapshot(shader);
    CHECK(!strcmp(before, after));
    free(before);
    free(after);
}

static nir_builder options_shader(unsigned input_medium, unsigned output_medium,
                                  nir_intrinsic_instr **input, nir_intrinsic_instr **output)
{
    nir_builder b = bool32_builder();
    nir_intrinsic_instr *in = varying(&b, 4), *out = NULL;
    nir_io_semantics semantic = nir_intrinsic_io_semantics(in);
    nir_def *condition = nir_fneu(&b, nir_channel(&b, &in->def, 0), nir_imm_int(&b, 0));
    nir_def_as_alu(condition)->fp_math_ctrl = nir_fp_preserve_inf | nir_fp_preserve_nan;
    /* Include every already-supported original Boolean family. The integer
     * equality is true, but no folding may remove its source contract. */
    condition =
        nir_ior(&b, nir_iand(&b, condition, nir_ieq(&b, nir_imm_int(&b, 0), nir_imm_int(&b, 0))),
                nir_inot(&b, nir_imm_bool(&b, true)));
    nir_def *value = nir_bcsel(&b, condition, &in->def,
                               nir_imm_ivec4(&b, 0x7fc12345, 0x80000000, 0x7f800000, 0xff800000));
    result(&b, value);
    semantic.medium_precision = input_medium;
    nir_intrinsic_set_io_semantics(in, semantic);
    nir_foreach_function_impl(impl, b.shader) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block)
    {
        if (instr->type == nir_instr_type_intrinsic &&
            nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_store_output) {
            CHECK(!out);
            out = nir_instr_as_intrinsic(instr);
        }
    }
    CHECK(out);
    semantic = nir_intrinsic_io_semantics(out);
    semantic.medium_precision = output_medium;
    nir_intrinsic_set_io_semantics(out, semantic);
    nir_validate_shader(b.shader, "bool32-F32-IO-retained-annotation");
    *input = in;
    *output = out;
    return b;
}

static unsigned options_positive(void)
{
    unsigned layout, input_medium, output_medium, cases = 0;
    for (layout = 0; layout < 2; layout++)
        for (input_medium = 0; input_medium < 2; input_medium++)
            for (output_medium = 0; output_medium < 2; output_medium++) {
                nir_intrinsic_instr *in, *out;
                nir_builder b = options_shader(input_medium, output_medium, &in, &out);
                struct sgx535_fragment_nir_options selected = {layout, SGX535_FRAGMENT_IO_F32};
                struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
                    strict[SGX535_NIR_MAX_INSTRUCTIONS];
                struct sgx535_fragment_linkage linkage, strict_linkage;
                struct sgx535_fragment_program program;
                size_t count = 0, strict_count = 0;
                uint32_t actual[4], expected[4], primary[4];
                static const uint32_t otherwise[4] = {0x7fc12345, 0x80000000, 0x7f800000,
                                                      0xff800000};
                char *before = snapshot(b.shader), *after;
                CHECK(sgx535_nir_fragment_bool32_options_ir(b.shader, &selected, ir,
                                                            SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                            &linkage) == SGX535_NIR_OK);
                CHECK(sgx535_fragment_compile_raw_ieq_candidate(ir, count, &program) ==
                      SGX535_NIR_OK);
                CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
                for (unsigned pattern = 0; pattern < 24; pattern++) {
                    for (unsigned lane = 0; lane < 4; lane++)
                        primary[lane] = bcsel_values[(pattern + lane) % 24];
                    for (unsigned lane = 0; lane < 4; lane++)
                        expected[lane] = bcsel_ternary_reference(
                            fneu_float_reference(primary[0], 0), primary[lane], otherwise[lane]);
                    raw_words(&program.encoding, primary, 0, actual);
                    CHECK(!memcmp(actual, expected, sizeof(actual)));
                    cases++;
                }
                if (input_medium || output_medium) {
                    ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir,
                                SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_UNSUPPORTED);
                    selected.io_precision = SGX535_FRAGMENT_IO_STRICT;
                    options_refuse(b.shader, &selected, SGX535_NIR_ERR_UNSUPPORTED);
                } else {
                    CHECK(sgx535_nir_fragment_bool32_bcsel_ir(
                              b.shader, layout, strict, SGX535_NIR_MAX_INSTRUCTIONS, &strict_count,
                              &strict_linkage) == SGX535_NIR_OK);
                    CHECK(count == strict_count && !memcmp(ir, strict, count * sizeof(*ir)) &&
                          !memcmp(&linkage, &strict_linkage, sizeof(linkage)));
                }
                after = snapshot(b.shader);
                CHECK(!strcmp(before, after));
                CHECK(nir_intrinsic_io_semantics(in).medium_precision == input_medium &&
                      nir_intrinsic_io_semantics(out).medium_precision == output_medium);
                free(before);
                free(after);
                ralloc_free(b.shader);
            }
    return cases;
}

static unsigned options_negative(void)
{
    unsigned mode, cases = 0;
    for (mode = 0; mode < 10; mode++) {
        nir_intrinsic_instr *in, *out;
        nir_builder b = options_shader(1, 1, &in, &out);
        struct sgx535_fragment_nir_options selected = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                       SGX535_FRAGMENT_IO_F32};
        nir_io_semantics semantic = nir_intrinsic_io_semantics(in);
        if (mode == 0)
            semantic.high_16bits = 1;
        else if (mode == 1)
            semantic.num_slots = 2;
        else if (mode == 2)
            semantic.per_view = 1;
        else if (mode == 3)
            nir_intrinsic_set_dest_type(in, nir_type_float16);
        else if (mode == 4) {
            semantic = nir_intrinsic_io_semantics(out);
            semantic.fb_fetch_output = 1;
            nir_intrinsic_set_io_semantics(out, semantic);
            semantic = nir_intrinsic_io_semantics(in);
        } else if (mode == 5) {
            semantic = nir_intrinsic_io_semantics(out);
            semantic.dual_source_blend_index = 1;
            nir_intrinsic_set_io_semantics(out, semantic);
            semantic = nir_intrinsic_io_semantics(in);
        } else if (mode == 6)
            selected.io_precision = (enum sgx535_fragment_io_precision_policy)2;
        else if (mode == 7)
            selected.uniform_layout = (enum sgx535_fragment_uniform_layout)2;
        else if (mode == 8)
            in->def.bit_size = 16;
        nir_intrinsic_set_io_semantics(in, semantic);
        options_refuse(b.shader, mode == 9 ? NULL : &selected,
                       mode == 9 ? SGX535_NIR_ERR_ARGUMENT : SGX535_NIR_ERR_UNSUPPORTED);
        cases++;
        ralloc_free(b.shader);
    }
    return cases;
}

static unsigned bcsel_negative(void)
{
    unsigned mode, dead, cases = 0;
    for (mode = 0; mode < 12; mode++)
        for (dead = 0; dead < 2; dead++) {
            nir_builder b = bool32_builder();
            nir_def *cond = nir_imm_bool(&b, true), *a = nir_imm_int(&b, 1),
                    *other = nir_imm_int(&b, -1);
            nir_def *value;
            nir_alu_instr *op;
            if (mode == 0)
                value = nir_b32csel(&b, nir_imm_int(&b, -1), a, other);
            else if (mode == 1 || mode == 2) {
                cond = nir_b2b1(&b, nir_imm_intN_t(&b, mode == 1 ? 1 : UINT32_C(0x80000000), 32));
                value = nir_bcsel(&b, cond, a, other);
            } else if (mode == 3)
                value = nir_bcsel(&b, cond, nir_imm_intN_t(&b, 1, 64), nir_imm_intN_t(&b, 2, 64));
            else if (mode == 4) {
                /* The false branch is noncanonical even when constant true
                 * selects the canonical branch. No fold may hide that fact. */
                value = nir_bcsel(&b, cond, nir_imm_int(&b, 0), a);
                value = nir_b2f32(&b, nir_b2b1(&b, value));
            } else {
                value = nir_bcsel(&b, cond, a, other);
                op = nir_def_as_alu(value);
                if (mode == 5)
                    op->no_signed_wrap = true;
                else if (mode == 6)
                    op->no_unsigned_wrap = true;
                else if (mode == 7)
                    op->fp_math_ctrl = nir_fp_preserve_nan;
                else if (mode == 8)
                    nir_src_rewrite(&op->src[0].src, other);
                else if (mode == 9)
                    nir_ine(&b, a, other);
                else if (mode == 10)
                    nir_ddx(&b, nir_imm_float(&b, 1.0f));
                else {
                    nir_push_if(&b, cond);
                    nir_bcsel(&b, cond, a, other);
                    nir_pop_if(&b, NULL);
                }
            }
            if (dead || mode == 3)
                value = nir_imm_float(&b, 0);
            result(&b, nir_vec4(&b, value, value, value, value));
            ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                        SGX535_NIR_ERR_UNSUPPORTED);
            ior_failure(b.shader, options_default, SGX535_NIR_MAX_INSTRUCTIONS,
                        SGX535_NIR_ERR_UNSUPPORTED);
            cases++;
            ralloc_free(b.shader);
        }
    {
        nir_builder b = bool32_builder();
        nir_def *value = nir_imm_int(&b, 0);
        unsigned i;
        for (i = 0; i < 64; i++)
            value = nir_bcsel(&b, nir_imm_bool(&b, (i & 1u) != 0), nir_imm_int(&b, i),
                              nir_imm_int(&b, i + 1));
        result(&b, nir_vec4(&b, value, value, value, value));
        nir_validate_shader(b.shader, "bcsel-original-small-expanded-capacity-refusal");
        ior_failure(b.shader, sgx535_nir_fragment_bool32_bcsel_ir, SGX535_NIR_MAX_INSTRUCTIONS,
                    SGX535_NIR_ERR_LIMIT);
        ior_failure(b.shader, options_default, SGX535_NIR_MAX_INSTRUCTIONS, SGX535_NIR_ERR_LIMIT);
        cases++;
        ralloc_free(b.shader);
    }
    return cases;
}

int main(void)
{
    unsigned width, pattern, kind, alias, cases = 0, negatives, reference, io_cases, io_negatives;
    reference = bcsel_reference_checks();
    glsl_type_singleton_init_or_ref();
    for (width = 1; width <= 4; width++)
        for (pattern = 0; pattern < 16; pattern++)
            for (kind = 0; kind < 4; kind++)
                for (alias = 0; alias < 4; alias++) {
                    bcsel_positive(width, pattern, kind, alias);
                    cases++;
                }
    negatives = bcsel_negative();
    io_cases = options_positive();
    io_negatives = options_negative();
    glsl_type_singleton_decref();
    CHECK(cases == 1024 && negatives == 25 && reference == 134320);
    CHECK(io_cases == 192 && io_negatives == 10);
    CHECK(bcsel_legacy_fneu_main() == 0);
    printf(
        "actual NIR raw32 BCSEL: %u positive constructions, %u negative controls, %u independent truth checks, %u cumulative checks PASS (CPU only; native closed)\n",
        cases, negatives, reference, checks);
    printf(
        "actual NIR bool32 F32 IO: %u raw-word selections and %u refusal controls PASS (CPU only)\n",
        io_cases, io_negatives);
    return 0;
}
#endif
