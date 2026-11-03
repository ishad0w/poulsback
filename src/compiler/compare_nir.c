// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "nir.h"
#include "nir_builder.h"
#include "sgx535_fragment_compare.h"
#include "sgx535_fragment_internal.h"

bool sgx535_nir_ige_const_contract(const nir_alu_instr *alu)
{
    unsigned lane, source;
    bool constant = false;
    if (!alu || alu->op != nir_op_ige || alu->def.bit_size != 1 || alu->def.num_components < 1 ||
        alu->def.num_components > 4 || alu->fp_math_ctrl || alu->no_signed_wrap ||
        alu->no_unsigned_wrap)
        return false;
    for (source = 0; source < 2; source++) {
        const nir_alu_src *src = &alu->src[source];
        const nir_def *def = src->src.ssa;
        if (!def || def->bit_size != 32 || def->num_components < 1 || def->num_components > 4)
            return false;
        for (lane = 0; lane < alu->def.num_components; lane++)
            if (src->swizzle[lane] >= def->num_components)
                return false;
        constant |= nir_def_instr(def)->type == nir_instr_type_load_const;
    }
    return constant;
}

static nir_def *signed_ge_constant(nir_builder *b, nir_def *value, uint32_t threshold)
{
    nir_def *key, *result = NULL;
    unsigned first = 0;
    threshold ^= UINT32_C(0x80000000);
    if (!threshold)
        return nir_imm_bool(b, true);
    key = nir_ior(b, nir_iand(b, value, nir_imm_intN_t(b, UINT32_C(0x7fffffff), 32)),
                  nir_iand(b, nir_inot(b, value), nir_imm_intN_t(b, UINT32_C(0x80000000), 32)));
    /* For unsigned keys, each higher 1-bit requires both that bit and the
     * lower comparison; a higher 0-bit permits either. Consecutive equal
     * threshold bits form an all-bits/any-bit test. Low zero bits impose no
     * condition. This avoids signed overflow and a guessed TEST ordering mode. */
    while (!(threshold & (UINT32_C(1) << first)))
        first++;
    while (first < 32) {
        unsigned next = first + 1;
        bool one = (threshold & (UINT32_C(1) << first)) != 0;
        uint32_t mask;
        nir_def *bits, *test;
        while (next < 32 && ((threshold >> next) & 1u) == (unsigned)one)
            next++;
        mask = UINT32_MAX >> (32 - (next - first));
        mask <<= first;
        bits = nir_iand(b, key, nir_imm_intN_t(b, mask, 32));
        test = nir_ieq(b, bits, nir_imm_intN_t(b, one ? mask : 0, 32));
        if (!one)
            test = nir_inot(b, test);
        result = !result ? test : one ? nir_iand(b, test, result) : nir_ior(b, test, result);
        first = next;
    }
    return result;
}

static bool lower_ige_const(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *alu;
    nir_def *values[4];
    unsigned lane, constant_source, variable_source;
    const nir_load_const_instr *constant;
    (void)unused;
    if (instr->type != nir_instr_type_alu || nir_instr_as_alu(instr)->op != nir_op_ige)
        return false;
    alu = nir_instr_as_alu(instr);
    constant_source = nir_def_instr(alu->src[1].src.ssa)->type == nir_instr_type_load_const ? 1 : 0;
    variable_source = 1 - constant_source;
    constant = nir_instr_as_load_const(nir_def_instr(alu->src[constant_source].src.ssa));
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    for (lane = 0; lane < alu->def.num_components; lane++) {
        uint32_t threshold = constant->value[alu->src[constant_source].swizzle[lane]].u32;
        nir_def *value = nir_channel(b, alu->src[variable_source].src.ssa,
                                     alu->src[variable_source].swizzle[lane]);
        /* C >= x is !(x >= C+1), except signed MAX which covers every x.
         * The increment is unsigned; no signed C overflow is performed. */
        if (!constant_source && threshold == UINT32_C(0x7fffffff))
            values[lane] = nir_imm_bool(b, true);
        else {
            values[lane] = signed_ge_constant(b, value, threshold + (constant_source ? 0u : 1u));
            if (!constant_source)
                values[lane] = nir_inot(b, values[lane]);
        }
    }
    nir_def_replace(&alu->def, nir_vec(b, values, alu->def.num_components));
    return true;
}

void sgx535_nir_lower_ige_const(nir_shader *shader)
{
    nir_shader_instructions_pass(shader, lower_ige_const, nir_metadata_control_flow, NULL);
}

enum sgx535_nir_status
sgx535_nir_fragment_compare_options_ir(const nir_shader *shader,
                                       const struct sgx535_fragment_nir_options *options,
                                       struct sgx535_fragment_instruction *output, size_t capacity,
                                       size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return sgx535_nir_fragment_bool32_compare_options_ir(shader, options, output, capacity, count,
                                                         linkage);
}
