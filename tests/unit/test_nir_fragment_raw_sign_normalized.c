// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#error "Actual NIR validation must remain enabled"
#endif
#include "compiler/glsl_types.h"
#include "nir_builder.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_generic_f32.h"
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
static const nir_shader_compiler_options options = {0};
static int fail_after = -1;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
    if (fail_after == 0)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    return __real_calloc(n, size);
}

static nir_intrinsic_instr *color(nir_builder *b, unsigned base, bool flat)
{
    nir_def *off = nir_imm_int(b, 0);
    nir_intrinsic_instr *in = nir_intrinsic_instr_create(
        b->shader, flat ? nir_intrinsic_load_input : nir_intrinsic_load_interpolated_input);
    nir_io_semantics semantics = {0};
    semantics.location = VARYING_SLOT_COL0;
    semantics.num_slots = 1;
    in->num_components = 4;
    if (flat) {
        in->src[0] = nir_src_for_ssa(off);
        nir_intrinsic_set_range(in, 1);
    } else {
        nir_def *bary = nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
        in->src[0] = nir_src_for_ssa(bary);
        in->src[1] = nir_src_for_ssa(off);
    }
    nir_intrinsic_set_base(in, base);
    nir_intrinsic_set_dest_type(in, nir_type_float32);
    nir_intrinsic_set_io_semantics(in, semantics);
    nir_def_init(&in->instr, &in->def, 4, 32);
    nir_builder_instr_insert(b, &in->instr);
    return in;
}

static void store(nir_builder *b, nir_def *value)
{
    nir_def *off = nir_imm_int(b, 0);
    nir_intrinsic_instr *out = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_output);
    nir_io_semantics semantics = {0};
    semantics.location = FRAG_RESULT_DATA0;
    semantics.num_slots = 1;
    out->num_components = 4;
    out->src[0] = nir_src_for_ssa(value);
    out->src[1] = nir_src_for_ssa(off);
    nir_intrinsic_set_range(out, 1);
    nir_intrinsic_set_write_mask(out, 15);
    nir_intrinsic_set_src_type(out, nir_type_float32);
    nir_intrinsic_set_io_semantics(out, semantics);
    nir_builder_instr_insert(b, &out->instr);
}

static char *snapshot(nir_shader *shader)
{
    char *text = NULL;
    size_t bytes = 0;
    FILE *file = open_memstream(&text, &bytes);
    CHECK(file != NULL);
    nir_print_shader(shader, file);
    CHECK(fclose(file) == 0);
    return text;
}

static void refuse(nir_shader *shader, enum sgx535_nir_status expected)
{
    struct sgx535_fragment_dri_raw_sign_normalized_nir_program *out = malloc(sizeof(*out));
    struct sgx535_fragment_dri_raw_sign_normalized_nir_program *before = malloc(sizeof(*before));
    char *graph = snapshot(shader), *after;
    CHECK(out && before);
    memset(out, 0x79, sizeof(*out));
    *before = *out;
    CHECK(sgx535_nir_project_dri_color0_raw_sign_normalized_candidate(
              shader, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH, out) == expected);
    CHECK(!memcmp(out, before, sizeof(*out)));
    after = snapshot(shader);
    CHECK(!strcmp(graph, after));
    free(after);
    free(graph);
    free(before);
    free(out);
}

static void expected_ir(struct sgx535_fragment_instruction ir[16], bool absolute)
{
    unsigned i;
    memset(ir, 0, 16u * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[4u + i * 2u].opcode = SGX535_FRAGMENT_CONSTANT;
        ir[4u + i * 2u].immediate = absolute ? 0x7fffffffu : 0x80000000u;
        ir[5u + i * 2u].opcode = absolute ? SGX535_FRAGMENT_RAW_AND : SGX535_FRAGMENT_RAW_XOR;
        ir[5u + i * 2u].source[0] = i;
        ir[5u + i * 2u].source[1] = 4u + i * 2u;
        ir[12u + i].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[12u + i].index = i;
        ir[12u + i].source[0] = 5u + i * 2u;
    }
}

static void generic_secondary_uniforms(void)
{
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_generic_f32_secondary_candidate *out = malloc(sizeof(*out));
    struct sgx535_fragment_generic_f32_secondary_candidate *saved = malloc(sizeof(*saved));
    struct sgx535_fragment_normalized_candidate *logical = malloc(sizeof(*logical));
    CHECK(ir && out && saved && logical);
    for (unsigned layout = 0; layout < 2; layout++)
        for (unsigned boundary = 0; boundary < 2; boundary++) {
            const unsigned stride = layout ? 4u : 1u;
            const unsigned base = layout ? 30u + boundary : 123u + boundary;
            const unsigned logical_extent = base * stride + 4u;
            const struct sgx535_fragment_nir_options policy = {
                (enum sgx535_fragment_uniform_layout)layout, SGX535_FRAGMENT_IO_F32};
            struct sgx535_fragment_linkage linkage;
            size_t count = 0;
            nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                           "ordinary-secondary-color-uniform");
            b.constant_fold_alu = false;
            nir_intrinsic_instr *input = color(&b, 0, false);
            nir_def *uniforms =
                nir_load_uniform(&b, 4, 32, nir_imm_int(&b, 0), .base = base,
                                 .range = layout ? 1 : 4, .dest_type = nir_type_float32);
            store(&b, nir_fmul(&b, &input->def, uniforms));
            nir_validate_shader(b.shader, "actual color-uniform SECONDARY source");
            char *before = snapshot(b.shader);
            CHECK(sgx535_nir_fragment_options_ir(b.shader, &policy, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                                 &count, &linkage) == SGX535_NIR_OK);
            CHECK(linkage.count == 1 && linkage.varying[0].semantic == SGX535_FRAGMENT_COLOR &&
                  linkage.varying[0].driver_base == 0 &&
                  linkage.varying[0].interpolation == SGX535_FRAGMENT_SMOOTH);
            CHECK(sgx535_fragment_compile_generic_f32_normalized_candidate(
                      ir, count, &linkage, logical) == SGX535_NIR_OK);
            CHECK(logical->secondary_register_count == logical_extent);
            memset(out, 0xa5, sizeof(*out));
            *saved = *out;
            enum sgx535_nir_status status =
                sgx535_fragment_compile_generic_f32_secondary_candidate(ir, count, &linkage, out);
            if (boundary) {
                CHECK(status == SGX535_NIR_ERR_LIMIT && !memcmp(out, saved, sizeof(*out)));
            } else {
                CHECK(status == SGX535_NIR_OK && out->logical_uniform_extent == logical_extent &&
                      out->physical_sa_words == logical_extent + 1 && out->reserved_sa_words == 1 &&
                      out->uniform_sa_words == logical_extent && !out->literal_sa_words &&
                      !out->sampler_state_sa_words);
                for (unsigned word = 0; word < 4; word++) {
                    uint32_t logical_bits = 0, physical_bits = 0;
                    for (unsigned component = 0; component < 4; component++) {
                        unsigned index = base * stride + component;
                        if (index / 32 == word)
                            logical_bits |= UINT32_C(1) << (index % 32);
                        if ((index + 1) / 32 == word)
                            physical_bits |= UINT32_C(1) << ((index + 1) % 32);
                    }
                    CHECK(out->logical_uniform_words_used[word] == logical_bits &&
                          out->program.uniform_words_used[word] == physical_bits);
                }
                CHECK(out->program.missing_contracts == SGX535_NORMALIZED_MISSING_ALL &&
                      !out->program.hardware_qualified && !out->program.linkage_qualified &&
                      !out->program.numerical_qualified &&
                      sgx535_fragment_normalized_native_admission(&out->program) ==
                          SGX535_NIR_ERR_UNSUPPORTED);
            }
            char *after = snapshot(b.shader);
            CHECK(!strcmp(before, after));
            free(after);
            free(before);
            ralloc_free(b.shader);
        }
    free(logical);
    free(saved);
    free(out);
    free(ir);
    puts(
        "actual Mesa F32 COLOR0*uniform: explicit DWORD/vec4 units, reserved-SA0 combined128 budget; native closed");
}

int main(void)
{
    unsigned absolute, flat, flag;
    glsl_type_singleton_init_or_ref();
    generic_secondary_uniforms();
    for (absolute = 0; absolute < 2; absolute++)
        for (flat = 0; flat < 2; flat++) {
            nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                           "raw-sign-normalized");
            nir_intrinsic_instr *input;
            nir_def *value;
            nir_alu_instr *op;
            struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS], expected[16];
            struct sgx535_fragment_linkage linkage, canonical = {0};
            struct sgx535_fragment_dri_raw_sign_normalized_nir_program *out = malloc(sizeof(*out));
            struct sgx535_fragment_normalized_candidate *manual = malloc(sizeof(*manual));
            size_t count = 0;
            char *before, *after;
            enum sgx535_fragment_interpolation mode =
                flat ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_SMOOTH;
            CHECK(out && manual);
            b.constant_fold_alu = false;
            input = color(&b, 3, flat != 0);
            value = absolute ? nir_fabs(&b, &input->def) : nir_fneg(&b, &input->def);
            op = nir_instr_as_alu(nir_def_instr(value));
            store(&b, value);
            nir_validate_shader(b.shader, "raw-sign-normalized-before");
            expected_ir(expected, absolute != 0);
            canonical.count = 1;
            canonical.varying[0] = (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4,
                                                                    SGX535_FRAGMENT_SMOOTH};
            CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                      expected, 16, &canonical, manual) == SGX535_NIR_OK);
            for (flag = 0; flag < 2; flag++) {
                unsigned i, raw = 0, packs = 0;
                op->fp_math_ctrl = flag ? nir_fp_preserve_inf : 0;
                before = snapshot(b.shader);
                CHECK(sgx535_nir_fragment_raw_sign_inf_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                          ir, SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                          &linkage) == SGX535_NIR_OK);
                CHECK(linkage.count == 1 && linkage.varying[0].driver_base == 3 &&
                      linkage.varying[0].semantic == SGX535_FRAGMENT_COLOR &&
                      linkage.varying[0].components == 4 &&
                      linkage.varying[0].interpolation == mode);
                for (i = 0; i < count; i++)
                    if (ir[i].opcode == SGX535_FRAGMENT_RAW_AND ||
                        ir[i].opcode == SGX535_FRAGMENT_RAW_XOR) {
                        CHECK(ir[i].opcode ==
                              (absolute ? SGX535_FRAGMENT_RAW_AND : SGX535_FRAGMENT_RAW_XOR));
                        CHECK(ir[ir[i].source[1]].opcode == SGX535_FRAGMENT_CONSTANT &&
                              ir[ir[i].source[1]].immediate ==
                                  (absolute ? 0x7fffffffu : 0x80000000u));
                        raw++;
                    }
                CHECK(raw == 4);
                CHECK(sgx535_nir_project_dri_color0_raw_sign_normalized_candidate(
                          b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, mode, out) == SGX535_NIR_OK);
                CHECK(!memcmp(&out->program, manual, sizeof(*manual)));
                CHECK(!memcmp(&out->source_linkage, &linkage, sizeof(linkage)) &&
                      out->resolved_interpolation == mode);
                CHECK(out->program.encoding.word_count == 52 &&
                      out->program.source_instruction_count == 16);
                CHECK(out->program.missing_contracts == 0x3ffu &&
                      !out->program.arithmetic_qualified && !out->program.numerical_qualified &&
                      !out->program.linkage_qualified && !out->program.output_qualified &&
                      !out->program.hardware_qualified &&
                      !out->program.nonfinite_behavior_qualified);
                CHECK(sgx535_fragment_normalized_native_admission(&out->program) ==
                      SGX535_NIR_ERR_UNSUPPORTED);
                for (i = 1; i < out->program.encoding.word_count; i += 2) {
                    uint32_t hi = out->program.encoding.words[i];
                    if ((hi & 0xfffbffc3u) == 0x40a10c01u)
                        packs++;
                }
                CHECK(packs == 4);
                if (flag) {
                    size_t old_count = 0x5678;
                    CHECK(sgx535_nir_fragment_raw_sign_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                          ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                                          &old_count,
                                                          &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
                    CHECK(old_count == 0x5678);
                }
                after = snapshot(b.shader);
                CHECK(!strcmp(before, after));
                free(after);
                free(before);
            }
            if (!flat) {
                /* Only preserve_inf is accepted, only on the two sign operations.
             * No no-contract/rounding/NaN/zero flag is silently discarded. */
                for (flag = 1; flag < 64; flag++) {
                    if (flag == nir_fp_preserve_inf)
                        continue;
                    op->fp_math_ctrl = flag & 63u;
                    refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
                }
                op->fp_math_ctrl = 0;
                op->no_signed_wrap = true;
                refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
                op->no_signed_wrap = false;
                op->no_unsigned_wrap = true;
                refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
                op->no_unsigned_wrap = false;
                b.shader->info.float_controls_execution_mode = 1;
                refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
                b.shader->info.float_controls_execution_mode = 0;
                for (flag = 0; flag <= 16; flag++) {
                    enum sgx535_nir_status status;
                    struct sgx535_fragment_dri_raw_sign_normalized_nir_program *saved =
                        malloc(sizeof(*saved));
                    CHECK(saved != NULL);
                    *saved = *out;
                    fail_after = (int)flag;
                    status = sgx535_nir_project_dri_color0_raw_sign_normalized_candidate(
                        b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, mode, out);
                    fail_after = -1;
                    if (status == SGX535_NIR_OK) {
                        CHECK(flag > 0 && !memcmp(&out->program, manual, sizeof(*manual)));
                        free(saved);
                        break;
                    }
                    CHECK(status == SGX535_NIR_ERR_NOMEM && !memcmp(saved, out, sizeof(*out)));
                    free(saved);
                }
                CHECK(flag <= 16);
                b.cursor = nir_before_instr(&op->instr);
                nir_imm_int(
                    &b,
                    0x7fc12345); /* Unused floating-literal bits cannot be pruned into acceptance. */
                refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
            }
            free(manual);
            free(out);
            ralloc_free(b.shader);
        }
    /* preserve_inf on a different valid/dead ALU operation remains refused. */
    {
        nir_builder b =
            nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "unsupported-inf-ALU");
        nir_intrinsic_instr *input;
        nir_def *dead;
        b.constant_fold_alu = false;
        input = color(&b, 0, false);
        dead = nir_fmul(&b, &input->def, nir_imm_float(&b, 1));
        nir_instr_as_alu(nir_def_instr(dead))->fp_math_ctrl = nir_fp_preserve_inf;
        store(&b, &input->def);
        refuse(b.shader, SGX535_NIR_ERR_UNSUPPORTED);
        ralloc_free(b.shader);
    }
    glsl_type_singleton_decref();
    printf("actual NIR raw-sign normalized composition: %u checks PASS (CPU only)\n", checks);
    return 0;
}
