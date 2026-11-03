// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nir.h"
#include "nir_builder.h"
#include "sgx535_fragment_linkage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The adapter's options entry with DWORD uniforms and strict F32 IO. */
static const struct sgx535_fragment_nir_options dwords_strict = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                                 SGX535_FRAGMENT_IO_STRICT};

static nir_shader *shader(unsigned variant)
{
    static const nir_shader_compiler_options options = {0};
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "semantic-linkage");
    const unsigned locations[] = {VARYING_SLOT_VAR0, VARYING_SLOT_COL1, VARYING_SLOT_COL0};
    const unsigned bases[] = {3, 2, 1};
    nir_def *sum = NULL,
            *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_SMOOTH);
    unsigned i;
    for (i = 0; i < 3; i++) {
        unsigned location = locations[i], base = bases[i];
        if (variant == 1 && i == 2)
            location = VARYING_SLOT_POS;
        if (variant == 2 && i == 2)
            base = bases[0];
        if (variant == 3 && i == 2)
            location = VARYING_SLOT_COL1;
        if (variant == 4 && i == 2)
            base = SGX535_FRAGMENT_INPUT_SLOTS;
        nir_def *v = nir_load_interpolated_input(
            &b, 4, 32, bary, nir_imm_int(&b, 0), .base = base, .component = 0,
            .dest_type = nir_type_float32, .io_semantics = {.location = location, .num_slots = 1});
        sum = sum ? nir_fadd(&b, sum, v) : v;
    }
    if (variant >= 5) {
        nir_def *v;
        if (variant == 5)
            v = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = 1, .range = 1,
                               .dest_type = nir_type_float32,
                               .io_semantics = {.location = VARYING_SLOT_COL0, .num_slots = 1});
        else {
            nir_def *color_bary =
                nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_NONE);
            v = nir_load_interpolated_input(
                &b, 4, 32, color_bary, nir_imm_int(&b, 0), .base = 1, .dest_type = nir_type_float32,
                .io_semantics = {.location = VARYING_SLOT_COL0, .num_slots = 1});
        }
        sum = nir_fadd(&b, sum, v);
    }
    nir_store_output(&b, sum, nir_imm_int(&b, 0), .base = 0, .range = 1, .write_mask = 15,
                     .src_type = nir_type_float32,
                     .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
    nir_validate_shader(b.shader, "linkage source fixture");
    return b.shader;
}
static char *printed(nir_shader *s)
{
    char *raw = NULL;
    size_t bytes = 0;
    FILE *f = open_memstream(&raw, &bytes);
    assert(f);
    nir_print_shader(s, f);
    assert(!fclose(f));
    return raw;
}
static nir_shader *single_color(unsigned base, unsigned location, enum glsl_interp_mode mode)
{
    static const nir_shader_compiler_options options = {0};
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "matched-dri-color0");
    nir_def *v;
    if (mode == INTERP_MODE_FLAT)
        v = nir_load_input(&b, 4, 32, nir_imm_int(&b, 0), .base = base, .range = 1,
                           .dest_type = nir_type_float32,
                           .io_semantics = {.location = location, .num_slots = 1});
    else {
        nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = mode);
        v = nir_load_interpolated_input(&b, 4, 32, bary, nir_imm_int(&b, 0), .base = base,
                                        .dest_type = nir_type_float32,
                                        .io_semantics = {.location = location, .num_slots = 1});
    }
    nir_store_output(&b, v, nir_imm_int(&b, 0), .base = 0, .range = 1, .write_mask = 15,
                     .src_type = nir_type_float32,
                     .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
    nir_validate_shader(b.shader, "matched DRI source fixture");
    return b.shader;
}
static nir_shader *live_color(unsigned variant)
{
    static const nir_shader_compiler_options options = {0};
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "matched-dri-liveness");
    nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_SMOOTH);
    nir_def *color = nir_load_interpolated_input(
        &b, 4, 32, bary, nir_imm_int(&b, 0), .base = 7, .dest_type = nir_type_float32,
        .io_semantics = {.location = VARYING_SLOT_COL0, .num_slots = 1});
    nir_def *u, *constant, *value;
    /* Deliberately retain real dead NIR, including intrinsic offset constants.
     * The adapter must validate it before its new bridge can prune scalar SSA. */
    (void)nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = variant == 2 ? 128 : 31,
                           .range = 1, .dest_type = nir_type_float32);
    (void)nir_imm_int(&b, 0x5aa5dead);
    if (variant >= 3) {
        nir_def *dead = variant == 3 ? nir_fsin(&b, color) : nir_ffma(&b, color, color, color);
        if (variant == 4)
            nir_instr_as_alu(nir_def_instr(dead))->fp_math_ctrl = 1;
    }
    u = nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = 3, .range = 1,
                         .dest_type = nir_type_float32);
    constant = nir_imm_int(&b, 0x7fc12345); /* Raw NaN payload, never a host float. */
    /* Real scalar/vector MOV chains share their sources without evaluating
     * any bits. Later arithmetic, outputs and repeated users must all remap. */
    u = nir_mov(&b, nir_mov(&b, u));
    constant = nir_mov(&b, constant);
    color = nir_mov(&b, color);
    if (variant == 1) {
        nir_def *x = nir_ffma(&b, nir_channel(&b, color, 0), u, constant);
        nir_def *y = nir_fmin(&b, nir_fmax(&b, nir_channel(&b, color, 1), u),
                              nir_frcp(&b, nir_channel(&b, color, 2)));
        value = nir_vec4(&b, x, y, nir_frsq(&b, constant),
                         nir_mov(&b, nir_mov(&b, nir_channel(&b, color, 3))));
    } else
        value = nir_vec4(&b, nir_channel(&b, color, 0), u, constant, nir_channel(&b, color, 3));
    nir_store_output(&b, value, nir_imm_int(&b, 0), .base = 0, .range = 1, .write_mask = 15,
                     .src_type = nir_type_float32,
                     .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
    nir_validate_shader(b.shader, "matched DRI liveness fixture");
    return b.shader;
}
static uint64_t source_token(unsigned kind, uint32_t value)
{
    return ((uint64_t)kind << 32) | value;
}
static uint64_t expression_token(unsigned op, uint64_t a, uint64_t b, uint64_t c)
{
    /* Symbolic source identity, not floating-point execution or packing. */
    uint64_t expression = UINT64_C(1469598103934665603);
    expression = (expression ^ op) * UINT64_C(1099511628211);
    expression = (expression ^ a) * UINT64_C(1099511628211);
    expression = (expression ^ b) * UINT64_C(1099511628211);
    return (expression ^ c) * UINT64_C(1099511628211);
}
static void live_word_oracle(const struct sgx535_fragment_dri_color0_program *p,
                             unsigned uniform_word, unsigned variant)
{
    uint64_t reg[124] = {0}, expected[4], output[4] = {0};
    bool defined[124] = {false};
    uint32_t bitmap[4] = {0};
    unsigned i, outputs = 0, constants = 0, uniforms = 0;
    uint64_t uniform = source_token(2, uniform_word), constant = source_token(3, 0x7fc12345u);
    expected[0] = source_token(1, 2);
    expected[1] = uniform;
    expected[2] = constant;
    expected[3] = source_token(1, 3);
    if (variant == 1) {
        expected[0] = expression_token(1, source_token(1, 2), uniform, constant);
        expected[1] = expression_token(2, expression_token(3, source_token(1, 1), uniform, 0),
                                       expression_token(4, source_token(1, 0), 0, 0), 0);
        expected[2] = expression_token(5, constant, 0, 0);
    }
    bitmap[uniform_word / 32] = 1u << (uniform_word % 32);
    assert(!memcmp(p->uniform_words_used, bitmap, sizeof(bitmap)) &&
           p->secondary_register_count == uniform_word + 1);
    assert(!p->arithmetic_qualified && !p->linkage_qualified && !p->output_qualified);
    assert(p->encoding.word_count % 2 == 0);
    for (i = 0; i < p->encoding.word_count; i += 2) {
        uint32_t low = p->encoding.words[i], high = p->encoding.words[i + 1];
        unsigned dst = (low >> 21) & 127u, source = (low >> 7) & 127u;
        uint64_t value = 0;
        if ((high & 0xfffc0e0fu) == 0xfca00000u) {
            uint32_t raw =
                (low & 0x1fffffu) | ((high & 0x1f0u) << 17) | (((high >> 12) & 63u) << 26);
            assert(raw == 0x7fc12345u);
            value = source_token(3, raw);
            constants++;
        } else if (high == 0x28a10000u) {
            unsigned bank = low >> 30;
            assert(low == (0x20000000u | (bank << 30) | (dst << 21) | (source << 7)));
            if (bank == 2) {
                assert(source < 4);
                value = source_token(1, source);
            } else if (bank == 3) {
                assert(source == uniform_word);
                value = source_token(2, source);
                uniforms++;
            } else {
                assert(bank == 0 && source < 124 && defined[source]);
                value = reg[source];
            }
        } else if ((high & ~0x40000u) == 0x28a10001u || (high & ~0x4003cu) == 0x40a10c01u) {
            static const unsigned masks[4] = {4, 2, 1, 8};
            assert(outputs < 4 && source < 124 && defined[source]);
            if (p->output_profile == SGX535_FRAGMENT_U8_WORD_PROJECTION)
                assert(low == (0x20040000u | (source << 7)) &&
                       high ==
                           (0x40a10c01u | (masks[outputs] << 2) | (outputs == 3 ? 0x40000u : 0)));
            else
                assert(low == (0x20000000u | (outputs << 21) | (source << 7)) &&
                       high == (0x28a10001u | (outputs == 3 ? 0x40000u : 0)));
            output[outputs++] = reg[source];
            continue;
        } else {
            uint64_t arguments[3] = {0};
            unsigned arity, op, j, sources[3] = {source, low & 127u, 0};
            if (high == 0x00a00000u) {
                op = 1;
                arity = 3;
                sources[0] = (low >> 14) & 127u;
                sources[1] = source;
                sources[2] = low & 127u;
            } else if (high == 0x18a00000u) {
                op = 2;
                arity = 2;
            } else if (high == 0x18a00200u) {
                op = 3;
                arity = 2;
            } else if (high == 0x08a00000u) {
                op = 4;
                arity = 1;
            } else {
                assert(high == 0x08a00200u);
                op = 5;
                arity = 1;
            }
            for (j = 0; j < arity; j++) {
                assert(sources[j] < 124 && defined[sources[j]]);
                arguments[j] = reg[sources[j]];
            }
            value = expression_token(op, arguments[0], arguments[1], arguments[2]);
        }
        assert(dst < 124);
        reg[dst] = value;
        defined[dst] = true;
    }
    assert(outputs == 4 && constants == 1 && uniforms == 1 &&
           !memcmp(output, expected, sizeof(output)));
}
static void dri_liveness(void)
{
    struct sgx535_fragment_dri_nir_program *out = malloc(sizeof(*out)),
                                           *saved = malloc(sizeof(*saved));
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_linkage linkage;
    unsigned variant, layout, profile;
    size_t count, i;
    bool dead_uniform, dead_constant;
    assert(out && saved && ir);
    for (variant = 0; variant < 2; variant++)
        for (layout = 0; layout < 2; layout++) {
            nir_shader *s = live_color(variant);
            char *before = printed(s), *after;
            assert(sgx535_nir_fragment_options_ir(
                       s,
                       &(struct sgx535_fragment_nir_options){
                           (enum sgx535_fragment_uniform_layout)layout, SGX535_FRAGMENT_IO_STRICT},
                       ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage) == SGX535_NIR_OK);
            dead_uniform = false;
            dead_constant = false;
            for (i = 0; i < count; i++) {
                if (ir[i].opcode == SGX535_FRAGMENT_UNIFORM && ir[i].index == (layout ? 124u : 31u))
                    dead_uniform = true;
                if (ir[i].opcode == SGX535_FRAGMENT_CONSTANT && ir[i].immediate == 0x5aa5deadu)
                    dead_constant = true;
            }
            assert(dead_uniform && dead_constant);
            for (profile = 0; profile < 2; profile++) {
                assert(sgx535_nir_project_dri_color0(s, (enum sgx535_fragment_uniform_layout)layout,
                                                     SGX535_FRAGMENT_SMOOTH,
                                                     profile ? SGX535_FRAGMENT_U8_WORD_PROJECTION
                                                             : SGX535_FRAGMENT_RAW_F32_WORDS,
                                                     out) == SGX535_NIR_OK);
                assert(out->source_linkage.count == 1 &&
                       out->source_linkage.varying[0].driver_base == 7);
                assert(out->program.encoding.word_count < count * 2);
                live_word_oracle(&out->program, layout ? 12 : 3, variant);
            }
            after = printed(s);
            assert(!strcmp(before, after));
            free(before);
            free(after);
            ralloc_free(s);
        }
    memset(out, 0xa5, sizeof(*out));
    *saved = *out;
    for (variant = 2; variant <= 4; variant++)
        for (layout = 0; layout < 2; layout++) {
            nir_shader *s = live_color(variant);
            char *before = printed(s), *after;
            assert(sgx535_nir_project_dri_color0(
                       s, (enum sgx535_fragment_uniform_layout)layout, SGX535_FRAGMENT_SMOOTH,
                       SGX535_FRAGMENT_U8_WORD_PROJECTION, out) == SGX535_NIR_ERR_UNSUPPORTED);
            assert(!memcmp(out, saved, sizeof(*out)));
            after = printed(s);
            assert(!strcmp(before, after));
            free(before);
            free(after);
            ralloc_free(s);
        }
    free(ir);
    free(saved);
    free(out);
}
static void dri_bridge(void)
{
    static const uint32_t literal[16] = {0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u,
                                         0xa0400000u, 0x28a10000u, 0xa0600180u, 0x28a10000u,
                                         0x20040000u, 0x40a10c11u, 0x20040080u, 0x40a10c09u,
                                         0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
    struct sgx535_fragment_dri_nir_program *out = malloc(sizeof(*out)),
                                           *saved = malloc(sizeof(*saved));
    struct sgx535_fragment_dri_nir_ir *exported = malloc(sizeof(*exported)),
                                      *export_saved = malloc(sizeof(*export_saved));
    unsigned base, mode, layout, variant;
    nir_shader *s;
    char *before, *after;
    assert(out && saved && exported && export_saved);
    for (base = 0; base <= 7; base += 7)
        for (mode = 0; mode < 3; mode++) {
            const enum glsl_interp_mode source_modes[] = {INTERP_MODE_SMOOTH, INTERP_MODE_NONE,
                                                          INTERP_MODE_FLAT};
            const enum sgx535_fragment_interpolation source_interp[] = {
                SGX535_FRAGMENT_SMOOTH, SGX535_FRAGMENT_COLOR_INTERPOLATION, SGX535_FRAGMENT_FLAT};
            unsigned resolved;
            s = single_color(base, VARYING_SLOT_COL0, source_modes[mode]);
            before = printed(s);
            for (layout = 0; layout < 2; layout++)
                for (resolved = 0; resolved < 2; resolved++) {
                    enum sgx535_fragment_interpolation color =
                        resolved ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_SMOOTH;
                    bool valid = mode == 1 || source_interp[mode] == color;
                    memset(out, 0xa5, sizeof(*out));
                    *saved = *out;
                    memset(exported, 0xa5, sizeof(*exported));
                    *export_saved = *exported;
                    assert(sgx535_nir_project_dri_color0(
                               s, (enum sgx535_fragment_uniform_layout)layout, color,
                               SGX535_FRAGMENT_U8_WORD_PROJECTION,
                               out) == (valid ? SGX535_NIR_OK : SGX535_NIR_ERR_UNSUPPORTED));
                    assert(sgx535_nir_dri_color0_ir(s, (enum sgx535_fragment_uniform_layout)layout,
                                                    color, exported) ==
                           (valid ? SGX535_NIR_OK : SGX535_NIR_ERR_UNSUPPORTED));
                    if (!valid) {
                        assert(!memcmp(out, saved, sizeof(*out)) &&
                               !memcmp(exported, export_saved, sizeof(*exported)));
                        continue;
                    }
                    assert(out->source_linkage.count == 1 &&
                           out->source_linkage.varying[0].driver_base == base &&
                           out->source_linkage.varying[0].interpolation == source_interp[mode]);
                    /* Word producer stays identical; the sidecar retains the source
             * contract. Native admission still needs replicated geometry. */
                    assert(out->program.encoding.word_count == 16 &&
                           !memcmp(out->program.encoding.words, literal, sizeof(literal)));
                    assert(exported->instruction_count == 8 &&
                           !memcmp(&exported->source_linkage, &out->source_linkage,
                                   sizeof(out->source_linkage)));
                    assert(exported->resolved_interpolation == color);
                    for (variant = 8; variant < SGX535_NIR_MAX_INSTRUCTIONS; variant++) {
                        static const struct sgx535_fragment_instruction zero = {0};
                        assert(!memcmp(&exported->instruction[variant], &zero, sizeof(zero)));
                    }
                    assert(out->resolved_interpolation == color &&
                           out->program.primary_register_count == 4);
                    assert(sgx535_fragment_dri_color0_native_admission(&out->program) ==
                           SGX535_NIR_ERR_UNSUPPORTED);
                }
            memset(out, 0xa5, sizeof(*out));
            *saved = *out;
            memset(exported, 0xa5, sizeof(*exported));
            *export_saved = *exported;
            for (variant = 0; variant < 4; variant++) {
                enum sgx535_fragment_uniform_layout uniform = SGX535_FRAGMENT_UNIFORM_DWORDS;
                enum sgx535_fragment_interpolation color = (enum sgx535_fragment_interpolation)99;
                enum sgx535_fragment_output_profile profile = SGX535_FRAGMENT_U8_WORD_PROJECTION;
                if (variant == 1)
                    color = SGX535_FRAGMENT_COLOR_INTERPOLATION;
                if (variant == 2) {
                    color = SGX535_FRAGMENT_SMOOTH;
                    uniform = (enum sgx535_fragment_uniform_layout)99;
                }
                if (variant == 3) {
                    color = SGX535_FRAGMENT_SMOOTH;
                    profile = SGX535_FRAGMENT_NORMALIZED_U8;
                }
                assert(sgx535_nir_project_dri_color0(s, uniform, color, profile, out) ==
                       SGX535_NIR_ERR_UNSUPPORTED);
                assert(!memcmp(out, saved, sizeof(*out)));
                if (variant != 3) {
                    assert(sgx535_nir_dri_color0_ir(s, uniform, color, exported) ==
                           SGX535_NIR_ERR_UNSUPPORTED);
                    assert(!memcmp(exported, export_saved, sizeof(*exported)));
                }
            }
            after = printed(s);
            assert(!strcmp(before, after));
            free(before);
            free(after);
            ralloc_free(s);
        }
    for (variant = 0; variant < 4; variant++) {
        s = single_color(7,
                         variant == 0   ? VARYING_SLOT_COL1
                         : variant == 1 ? VARYING_SLOT_VAR0
                                        : VARYING_SLOT_COL0,
                         variant == 2 ? INTERP_MODE_FLAT : INTERP_MODE_SMOOTH);
        /* Variant3 supplies mixed semantic linkage from the original fixture. */
        if (variant == 3) {
            ralloc_free(s);
            s = shader(0);
        }
        before = printed(s);
        assert(sgx535_nir_project_dri_color0(
                   s, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH,
                   SGX535_FRAGMENT_U8_WORD_PROJECTION, out) == SGX535_NIR_ERR_UNSUPPORTED);
        assert(!memcmp(out, saved, sizeof(*out)));
        assert(sgx535_nir_dri_color0_ir(s, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH,
                                        exported) == SGX535_NIR_ERR_UNSUPPORTED);
        assert(!memcmp(exported, export_saved, sizeof(*exported)));
        after = printed(s);
        assert(!strcmp(before, after));
        free(before);
        free(after);
        ralloc_free(s);
    }
    assert(sgx535_nir_project_dri_color0(NULL, SGX535_FRAGMENT_UNIFORM_DWORDS,
                                         SGX535_FRAGMENT_SMOOTH, SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                         out) == SGX535_NIR_ERR_ARGUMENT);
    s = single_color(0, VARYING_SLOT_COL0, INTERP_MODE_SMOOTH);
    assert(sgx535_nir_project_dri_color0(s, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH,
                                         SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                         NULL) == SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_nir_dri_color0_ir(NULL, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH,
                                    exported) == SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_nir_dri_color0_ir(s, SGX535_FRAGMENT_UNIFORM_DWORDS, SGX535_FRAGMENT_SMOOTH,
                                    NULL) == SGX535_NIR_ERR_ARGUMENT);
    ralloc_free(s);
    free(export_saved);
    free(exported);
    free(saved);
    free(out);
}

/* Real NIR direct-I/O shapes emitted by the Mesa FF state tracker. RANGE is
 * optional only here: constants and complete slot/type metadata bound access. */
static nir_shader *direct_io(unsigned flat, unsigned in_range, unsigned out_range, unsigned variant)
{
    static const nir_shader_compiler_options options = {0};
    nir_builder b =
        nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "direct-io-range");
    nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_NONE);
    nir_def *in_offset = nir_imm_int(&b, variant == 3 ? 1 : 0);
    nir_def *value, *out_offset;
    nir_io_semantics input_sem = {0}, output_sem = {0};
    input_sem.location = VARYING_SLOT_COL0;
    input_sem.num_slots = variant == 7 ? 2 : variant == 16 ? 0 : 1;
    output_sem.location = FRAG_RESULT_COLOR;
    output_sem.num_slots = variant == 8 ? 2 : 1;
    if (variant == 5 || variant == 13)
        in_offset = nir_load_uniform(&b, 1, 32, in_offset, .base = 0,
                                     .range = variant == 13 ? 0 : 1, .dest_type = nir_type_float32);
    if (flat)
        value = nir_load_input(&b, 4, 32, in_offset,
                               .base = variant == 17 ? SGX535_FRAGMENT_INPUT_SLOTS : 0,
                               .range = variant == 1    ? 2
                                        : variant == 18 ? UINT32_MAX
                                                        : in_range,
                               .component = variant == 15 ? 1 : 0,
                               .dest_type = variant == 11 ? nir_type_uint32 : nir_type_float32,
                               .io_semantics = input_sem);
    else
        value = nir_load_interpolated_input(
            &b, 4, 32, bary, in_offset, .base = variant == 17 ? SGX535_FRAGMENT_INPUT_SLOTS : 0,
            .component = variant == 15 ? 1 : 0,
            .dest_type = variant == 11 ? nir_type_uint32 : nir_type_float32,
            .io_semantics = input_sem);
    if (variant == 16) {
        /* The builder defaults num_slots=0 to one. Mutate the actual
         * intrinsic afterward so this negative really has zero slots. */
        nir_intrinsic_instr *in = nir_instr_as_intrinsic(nir_def_instr(value));
        nir_io_semantics sem = nir_intrinsic_io_semantics(in);
        sem.num_slots = 0;
        nir_intrinsic_set_io_semantics(in, sem);
    }
    out_offset = nir_imm_int(&b, variant == 4 ? 1 : 0);
    if (variant == 6)
        out_offset = nir_load_uniform(&b, 1, 32, out_offset, .base = 0, .range = 1,
                                      .dest_type = nir_type_float32);
    nir_store_output(&b, value, out_offset, .base = variant == 9 ? 1 : 0,
                     .range = variant == 2    ? 2
                              : variant == 19 ? UINT32_MAX
                                              : out_range,
                     .component = variant == 10 ? 1 : 0, .write_mask = variant == 14 ? 7 : 15,
                     .src_type = variant == 12 ? nir_type_uint32 : nir_type_float32,
                     .io_semantics = output_sem);
    if (!variant)
        nir_validate_shader(b.shader, "Mesa FF direct one-slot IO range fixture");
    return b.shader;
}
static void direct_io_ranges(void)
{
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_instruction *reference =
        calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_dri_nir_program *program = malloc(sizeof(*program)),
                                           *saved = malloc(sizeof(*saved));
    struct sgx535_fragment_linkage link, reference_link;
    unsigned flat, in_range, out_range, variant, positive = 0, refused = 0;
    size_t count, reference_count;
    assert(ir && reference && program && saved);
    for (flat = 0; flat < 2; flat++) {
        nir_shader *s = direct_io(flat, 1, 1, 0);
        assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, reference,
                                              SGX535_NIR_MAX_INSTRUCTIONS, &reference_count,
                                              &reference_link) == SGX535_NIR_OK);
        ralloc_free(s);
        for (in_range = 0; in_range < 2; in_range++)
            for (out_range = 0; out_range < 2; out_range++) {
                char *before, *after;
                s = direct_io(flat, in_range, out_range, 0);
                before = printed(s);
                assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir,
                                                      SGX535_NIR_MAX_INSTRUCTIONS, &count,
                                                      &link) == SGX535_NIR_OK);
                assert(count == reference_count && !memcmp(ir, reference, count * sizeof(*ir)) &&
                       !memcmp(&link, &reference_link, sizeof(link)));
                assert(link.count == 1 && link.varying[0].driver_base == 0 &&
                       link.varying[0].components == 4 &&
                       link.varying[0].interpolation ==
                           (flat ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_COLOR_INTERPOLATION));
                assert(sgx535_nir_project_dri_color0(
                           s, SGX535_FRAGMENT_UNIFORM_DWORDS,
                           flat ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_SMOOTH,
                           SGX535_FRAGMENT_RAW_F32_WORDS, program) == SGX535_NIR_OK);
                assert(!program->program.arithmetic_qualified &&
                       !program->program.linkage_qualified && !program->program.output_qualified);
                assert(sgx535_fragment_dri_color0_native_admission(&program->program) ==
                       SGX535_NIR_ERR_UNSUPPORTED);
                after = printed(s);
                assert(!strcmp(before, after));
                free(before);
                free(after);
                ralloc_free(s);
                positive++;
            }
        for (variant = 1; variant <= 19; variant++) {
            char *before, *after;
            if (!flat && (variant == 1 || variant == 18))
                continue; /* interpolated input has no RANGE */
            s = direct_io(flat, 0, 0, variant);
            before = printed(s);
            memset(ir, 0xa5, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
            memcpy(reference, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
            memset(&link, 0xa5, sizeof(link));
            reference_link = link;
            count = SIZE_MAX;
            enum sgx535_nir_status status = sgx535_nir_fragment_options_ir(
                s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &link);
            if (status != SGX535_NIR_ERR_UNSUPPORTED) {
                fprintf(stderr, "unexpected IO rejection result: flat=%u variant=%u status=%d\n",
                        flat, variant, status);
                nir_print_shader(s, stderr);
            }
            assert(status == SGX535_NIR_ERR_UNSUPPORTED);
            assert(count == SIZE_MAX &&
                   !memcmp(ir, reference, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)) &&
                   !memcmp(&link, &reference_link, sizeof(link)));
            memset(program, 0xa5, sizeof(*program));
            *saved = *program;
            assert(sgx535_nir_project_dri_color0(
                       s, SGX535_FRAGMENT_UNIFORM_DWORDS,
                       flat ? SGX535_FRAGMENT_FLAT : SGX535_FRAGMENT_SMOOTH,
                       SGX535_FRAGMENT_RAW_F32_WORDS, program) == SGX535_NIR_ERR_UNSUPPORTED);
            assert(!memcmp(program, saved, sizeof(*program)));
            after = printed(s);
            assert(!strcmp(before, after));
            free(before);
            free(after);
            ralloc_free(s);
            refused++;
        }
    }
    printf(
        "PASS direct IO RANGE 0/1: %u actual NIR positives, %u bounded/type/offset/uniform transactional refusals\n",
        positive, refused);
    free(saved);
    free(program);
    free(reference);
    free(ir);
}

int main(void)
{
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_instruction *before = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_program *program = calloc(1, sizeof(*program));
    struct sgx535_fragment_linkage link, old;
    nir_shader *s;
    size_t count, i;
    unsigned seen[3] = {0}, variant;
    char *a, *b;
    assert(ir && before && program);
    s = shader(0);
    a = printed(s);
    count = SIZE_MAX;
    memset(&link, 0x55, sizeof(link));
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                          &count, &link) == SGX535_NIR_OK);
    assert(link.count == 3);
    assert(link.varying[0].semantic == SGX535_FRAGMENT_COLOR && link.varying[0].index == 0 &&
           link.varying[0].driver_base == 1);
    assert(link.varying[1].semantic == SGX535_FRAGMENT_COLOR && link.varying[1].index == 1 &&
           link.varying[1].driver_base == 2);
    assert(link.varying[2].semantic == SGX535_FRAGMENT_GENERIC && link.varying[2].index == 0 &&
           link.varying[2].driver_base == 3);
    for (i = 0; i < count; i++)
        if (ir[i].opcode == SGX535_FRAGMENT_INPUT) {
            assert(ir[i].index < 3);
            seen[ir[i].index]++;
        }
    for (i = 0; i < 3; i++)
        assert(seen[i] == 4 && link.varying[i].components == 4 &&
               link.varying[i].interpolation == SGX535_FRAGMENT_SMOOTH);
    assert(sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
           SGX535_NIR_OK);
    assert(program->input_count == 3 && program->primary_register_count == 12);
    assert(!program->linkage_qualified && !program->arithmetic_qualified &&
           !program->output_qualified);
    assert(sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED);
    b = printed(s);
    assert(!strcmp(a, b));
    free(a);
    free(b);
    memcpy(before, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    old = link;
    count = SIZE_MAX;
    assert(sgx535_nir_fragment_ir(s, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count) ==
           SGX535_NIR_ERR_UNSUPPORTED);
    assert(count == SIZE_MAX && !memcmp(ir, before, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)));
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, 1, &count, &link) ==
           SGX535_NIR_ERR_LIMIT);
    assert(count == SIZE_MAX && !memcmp(&link, &old, sizeof(link)) &&
           !memcmp(ir, before, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)));
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                          (size_t *)ir, &link) == SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                          &count, (void *)ir) == SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                          (void *)&link, &link) == SGX535_NIR_ERR_ARGUMENT);
    assert(!memcmp(ir, before, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)) &&
           !memcmp(&link, &old, sizeof(link)));
    ralloc_free(s);
    for (variant = 1; variant <= 6; variant++) {
        s = shader(variant);
        a = printed(s);
        count = SIZE_MAX;
        assert(sgx535_nir_fragment_options_ir(s, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                              &count, &link) == SGX535_NIR_ERR_UNSUPPORTED);
        assert(count == SIZE_MAX && !memcmp(&link, &old, sizeof(link)) &&
               !memcmp(ir, before, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)));
        b = printed(s);
        assert(!strcmp(a, b));
        free(a);
        free(b);
        ralloc_free(s);
    }
    dri_bridge();
    dri_liveness();
    direct_io_ranges();
    free(program);
    free(before);
    free(ir);
    puts(
        "PASS actual Mesa NIR COLOR0/1+GENERIC0 dense semantic identity, immutable input, transactional alias/refusal; no native admission");
    return 0;
}
