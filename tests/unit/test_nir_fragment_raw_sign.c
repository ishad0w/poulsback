// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifdef NDEBUG
#error "Actual NIR validation must remain enabled"
#endif
#include "compiler/glsl_types.h"
#include "nir_builder.h"
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
#include "fragment_raw_sign_reference.h"
static const nir_shader_compiler_options options = {0};

static nir_intrinsic_instr *varying(nir_builder *b, unsigned components)
{
    nir_def *bary = nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
    nir_def *offset = nir_imm_int(b, 0);
    nir_intrinsic_instr *in =
        nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_interpolated_input);
    nir_io_semantics semantic = {0};
    semantic.location = VARYING_SLOT_COL0;
    semantic.num_slots = 1;
    in->num_components = components;
    in->src[0] = nir_src_for_ssa(bary);
    in->src[1] = nir_src_for_ssa(offset);
    nir_intrinsic_set_base(in, 0);
    nir_intrinsic_set_dest_type(in, nir_type_float32);
    nir_intrinsic_set_io_semantics(in, semantic);
    nir_def_init(&in->instr, &in->def, components, 32);
    nir_builder_instr_insert(b, &in->instr);
    return in;
}

static void result(nir_builder *b, nir_def *value)
{
    nir_def *off = nir_imm_int(b, 0);
    nir_intrinsic_instr *out = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_output);
    nir_io_semantics sem = {0};
    sem.location = FRAG_RESULT_DATA0;
    sem.num_slots = 1;
    out->num_components = 4;
    out->src[0] = nir_src_for_ssa(value);
    out->src[1] = nir_src_for_ssa(off);
    nir_intrinsic_set_range(out, 1);
    nir_intrinsic_set_write_mask(out, 15);
    nir_intrinsic_set_src_type(out, nir_type_float32);
    nir_intrinsic_set_io_semantics(out, sem);
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

static void refuse(nir_shader *shader)
{
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
        before[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage, old_linkage;
    size_t count = 0x1234;
    memset(ir, 0x6d, sizeof(ir));
    memcpy(before, ir, sizeof(ir));
    memset(&linkage, 0xa7, sizeof(linkage));
    old_linkage = linkage;
    CHECK(sgx535_nir_fragment_raw_sign_ir(shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                          SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                          &linkage) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(count == 0x1234 && !memcmp(ir, before, sizeof(ir)) &&
          !memcmp(&linkage, &old_linkage, sizeof(linkage)));
}

int main(void)
{
    static const uint32_t primary[][4] = {{0x00000000, 0x80000000, 0x7f800000, 0xff800000},
                                          {0x7fc12345, 0xffc12345, 0x7f812345, 0xff812345},
                                          {0x00000001, 0x80000001, 0x007fffff, 0x807fffff},
                                          {0x3f7fffff, 0xbf7fffff, 0x3f800001, 0xbf800001}};
    unsigned absolute, width;
    glsl_type_singleton_init_or_ref();
    for (absolute = 0; absolute < 2; absolute++)
        for (width = 1; width <= 4; width++) {
            nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                           "raw-sign-candidate");
            nir_intrinsic_instr *in;
            nir_def *value, *lane[4];
            nir_alu_instr *op;
            struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS],
                old[SGX535_NIR_MAX_INSTRUCTIONS];
            struct sgx535_fragment_linkage linkage, old_linkage;
            struct sgx535_fragment_program generic;
            struct sgx535_fragment_dri_color0_program program;
            size_t count = 0, old_count = 0, i;
            unsigned n = 0, mad = 0, max = 0, c;
            char *before, *after;
            b.constant_fold_alu = false;
            in = varying(&b, width);
            value = absolute ? nir_fabs(&b, &in->def) : nir_fneg(&b, &in->def);
            op = nir_instr_as_alu(nir_def_instr(value));
            for (c = 0; c < 4; c++)
                lane[c] = nir_channel(&b, value, (3 - c) % width);
            result(&b, nir_vec4(&b, lane[0], lane[1], lane[2], lane[3]));
            nir_validate_shader(b.shader, "raw-sign-before");
            before = snapshot(b.shader);
            CHECK(sgx535_nir_fragment_raw_sign_ir(b.shader, SGX535_FRAGMENT_UNIFORM_DWORDS, ir,
                                                  SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                  &linkage) == SGX535_NIR_OK);
            CHECK(linkage.count == 1 && linkage.varying[0].semantic == SGX535_FRAGMENT_COLOR &&
                  linkage.varying[0].components == width && linkage.varying[0].driver_base == 0);
            for (i = 0; i < count; i++) {
                n += ir[i].opcode == (absolute ? SGX535_FRAGMENT_RAW_AND : SGX535_FRAGMENT_RAW_XOR);
                mad += ir[i].opcode == SGX535_FRAGMENT_MAD;
                max += ir[i].opcode == SGX535_FRAGMENT_MAX;
                if (ir[i].opcode == SGX535_FRAGMENT_RAW_AND ||
                    ir[i].opcode == SGX535_FRAGMENT_RAW_XOR) {
                    CHECK(ir[ir[i].source[1]].opcode == SGX535_FRAGMENT_CONSTANT);
                    CHECK(ir[ir[i].source[1]].immediate == (absolute ? 0x7fffffffu : 0x80000000u));
                }
            }
            CHECK(n == width && !mad && !max);
            CHECK(sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, &generic) ==
                  SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, count, &generic) == SGX535_NIR_OK);
            /* The unchanged matched COLOR0 API requires a complete vec4 binding. */
            if (width == 4) {
                CHECK(sgx535_fragment_compile_dri_color0_raw_sign_candidate(
                          ir, count, &linkage, &program) == SGX535_NIR_OK);
                CHECK(program.primary_register_count == 4 && !program.arithmetic_qualified &&
                      !program.linkage_qualified && !program.output_qualified);
                for (i = 0; i < sizeof(primary) / sizeof(*primary); i++) {
                    uint32_t output[4];
                    static const unsigned dri_lane[4] = {2, 1, 0, 3};
                    raw_words(&program.encoding, primary[i], 0, output);
                    for (c = 0; c < 4; c++) {
                        uint32_t bits = primary[i][dri_lane[(3 - c) % width]];
                        CHECK(output[c] ==
                              (absolute ? (bits & 0x7fffffffu) : (bits ^ 0x80000000u)));
                    }
                }
            }
            /* Old entrypoint retains its MAD/MAX stream and never sees new opcodes. */
            CHECK(sgx535_nir_fragment_options_ir(
                      b.shader,
                      &(struct sgx535_fragment_nir_options){SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                            SGX535_FRAGMENT_IO_STRICT},
                      old, SGX535_NIR_MAX_INSTRUCTIONS, &old_count, &old_linkage) == SGX535_NIR_OK);
            n = 0;
            mad = 0;
            max = 0;
            for (i = 0; i < old_count; i++) {
                n += old[i].opcode == SGX535_FRAGMENT_RAW_AND ||
                     old[i].opcode == SGX535_FRAGMENT_RAW_XOR;
                mad += old[i].opcode == SGX535_FRAGMENT_MAD;
                max += old[i].opcode == SGX535_FRAGMENT_MAX;
            }
            CHECK(!n && mad == width && max == (absolute ? width : 0));
            after = snapshot(b.shader);
            CHECK(!strcmp(before, after));
            free(before);
            free(after);
            /* No floating control, rounding, signed-zero/NaN promise is dropped. */
            op->fp_math_ctrl = 1;
            refuse(b.shader);
            op->fp_math_ctrl = 0;
            op->no_signed_wrap = true;
            refuse(b.shader);
            op->no_signed_wrap = false;
            op->no_unsigned_wrap = true;
            refuse(b.shader);
            op->no_unsigned_wrap = false;
            b.shader->info.float_controls_execution_mode = 1;
            refuse(b.shader);
            b.shader->info.float_controls_execution_mode = 0;
            /* Unsupported dead source nodes still reject the whole candidate. */
            b.cursor = nir_before_instr(&op->instr);
            nir_fsin(&b, &in->def);
            refuse(b.shader);
            ralloc_free(b.shader);
        }
    glsl_type_singleton_decref();
    printf("actual NIR raw-sign candidate: %u checks PASS (CPU only)\n", checks);
    return 0;
}
