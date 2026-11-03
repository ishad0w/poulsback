// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "nir.h"
#include "nir_builder.h"
#include "sgx535_fragment_linkage.h"
#include "poulsback_program.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The adapter's options entry with DWORD uniforms and strict F32 IO. */
static const struct sgx535_fragment_nir_options dwords_strict = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                                 SGX535_FRAGMENT_IO_STRICT};

static nir_shader *shader(unsigned base, unsigned range, unsigned off, unsigned variant)
{
    static const nir_shader_compiler_options options = {0};
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "uniform-units");
    nir_def *offset = nir_imm_int(&b, off), *value;
    if (variant == 1)
        offset = nir_load_uniform(&b, 1, 32, offset, .base = 0, .range = 1,
                                  .dest_type = nir_type_float32);
    value = nir_load_uniform(&b, 4, 32, offset, .base = base, .range = range,
                             .dest_type = variant == 2 ? nir_type_uint32 : nir_type_float32);
    nir_store_output(&b, value, nir_imm_int(&b, 0), .base = 0, .range = 1, .write_mask = 15,
                     .src_type = variant == 2 ? nir_type_uint32 : nir_type_float32,
                     .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
    nir_validate_shader(b.shader, "explicit uniform layout fixture");
    return b.shader;
}
int main(void)
{
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_instruction *saved = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*saved));
    struct sgx535_fragment_program *program = calloc(1, sizeof(*program));
    struct sgx535_fragment_linkage link = {0}, old;
    uint32_t buffer[128], copy[128], before[128];
    unsigned layout, base, i;
    size_t count, words;
    nir_shader *s;
    assert(ir && saved && program);
    for (i = 0; i < 128; i++)
        buffer[i] = 0x81234567u + i;
    buffer[0] = 0x80000000;
    buffer[1] = 0x7fc12345;
    buffer[2] = 0x7f800000;
    buffer[3] = 0xff800000;
    for (layout = 0; layout < 2; layout++) {
        unsigned stride = layout ? 4 : 1, range = layout ? 1 : 4;
        for (base = 0; base + range <= 128 / stride; base++) {
            unsigned seen = 0;
            s = shader(base, range, 0, 0);
            count = SIZE_MAX;
            assert(sgx535_nir_fragment_options_ir(
                       s,
                       &(struct sgx535_fragment_nir_options){
                           (enum sgx535_fragment_uniform_layout)layout, SGX535_FRAGMENT_IO_STRICT},
                       ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &link) == SGX535_NIR_OK);
            for (i = 0; i < count; i++)
                if (ir[i].opcode == SGX535_FRAGMENT_UNIFORM)
                    assert(ir[i].index == base * stride + seen++);
            assert(seen == 4 && !link.count);
            assert(sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
                   SGX535_NIR_OK);
            assert(program->secondary_register_count == base * stride + 4);
            memset(copy, 0x55, sizeof(copy));
            words = SIZE_MAX;
            assert(poulsback_fragment_uniform_snapshot(program, buffer, sizeof(buffer), copy, 128,
                                                       &words) == SGX535_NIR_OK);
            assert(words == base * stride + 4 && !memcmp(copy, buffer, words * 4));
            assert(!program->arithmetic_qualified && !program->linkage_qualified &&
                   !program->output_qualified);
            assert(sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED);
            ralloc_free(s);
        }
        s = shader(3, layout ? 3 : 8, 1, 0);
        count = SIZE_MAX;
        assert(sgx535_nir_fragment_options_ir(
                   s,
                   &(struct sgx535_fragment_nir_options){
                       (enum sgx535_fragment_uniform_layout)layout, SGX535_FRAGMENT_IO_STRICT},
                   ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &link) == SGX535_NIR_OK);
        for (i = 0; i < count; i++)
            if (ir[i].opcode == SGX535_FRAGMENT_UNIFORM) {
                assert(ir[i].index == 4 * stride);
                break;
            }
        ralloc_free(s);
    }
    memcpy(saved, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    old = link;
    for (layout = 0; layout < 2; layout++)
        for (i = 0; i < 7; i++) {
            unsigned stride = layout ? 4 : 1;
            s = shader(i == 0 ? 128 / stride : 0,
                       i == 1   ? 128 / stride + 1
                       : i == 6 ? 0
                                : (layout ? 1 : 4),
                       i == 2 ? (layout ? 1 : 4) : 0,
                       i == 3   ? 1
                       : i == 4 ? 2
                                : 0);
            count = SIZE_MAX;
            assert(sgx535_nir_fragment_options_ir(
                       s,
                       &(struct sgx535_fragment_nir_options){
                           i == 5 ? (enum sgx535_fragment_uniform_layout)99
                                  : (enum sgx535_fragment_uniform_layout)layout,
                           SGX535_FRAGMENT_IO_STRICT},
                       ir, SGX535_NIR_MAX_INSTRUCTIONS, &count,
                       &link) == SGX535_NIR_ERR_UNSUPPORTED);
            assert(count == SIZE_MAX &&
                   !memcmp(saved, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)) &&
                   !memcmp(&old, &link, sizeof(link)));
            ralloc_free(s);
        }
    s = shader(0, 4, 0, 0);
    count = SIZE_MAX;
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                          &count, &link) == SGX535_NIR_OK);
    assert(sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
           SGX535_NIR_OK);
    memcpy(copy, buffer, sizeof(copy));
    words = SIZE_MAX;
    assert(poulsback_fragment_uniform_snapshot(program, copy, sizeof(copy), copy, 128, &words) ==
           SGX535_NIR_OK);
    assert(words == 4 && !memcmp(copy, buffer, sizeof(copy)));
    memset(copy, 0x55, sizeof(copy));
    memcpy(before, copy, sizeof(copy));
    for (i = 0; i < 7; i++) {
        enum sgx535_nir_status status;
        words = SIZE_MAX;
        if (i == 0)
            status = poulsback_fragment_uniform_snapshot(program, buffer, 12, copy, 128, &words);
        else if (i == 1)
            status = poulsback_fragment_uniform_snapshot(program, buffer, 15, copy, 128, &words);
        else if (i == 2)
            status = poulsback_fragment_uniform_snapshot(program, buffer, sizeof(buffer), copy, 3,
                                                         &words);
        else if (i == 3)
            status = poulsback_fragment_uniform_snapshot(program, buffer, sizeof(buffer), copy,
                                                         SIZE_MAX, &words);
        else if (i == 4)
            status = poulsback_fragment_uniform_snapshot(program, NULL, 16, copy, 128, &words);
        else if (i == 5) {
            program->secondary_register_count++;
            status = poulsback_fragment_uniform_snapshot(program, buffer, sizeof(buffer), copy, 128,
                                                         &words);
            program->secondary_register_count--;
        } else
            status = poulsback_fragment_uniform_snapshot(program, buffer, sizeof(buffer), copy, 128,
                                                         (void *)copy);
        assert(status != SGX535_NIR_OK && words == SIZE_MAX && !memcmp(copy, before, sizeof(copy)));
    }
    ralloc_free(s);
    free(program);
    free(saved);
    free(ir);
    puts(
        "PASS actual NIR explicit DWORD/vec4 uniform units, exact-bit constant buffer snapshots and transactional refusals; no GPU");
    return 0;
}
