// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "nir.h"
#include "nir_builder.h"
#include "sgx535_fragment_compare.h"
#include "sgx535_fragment_internal.h"
#include <stdlib.h>

struct bool_definition {
    const nir_def *def;
    bool canonical[4];
};
struct bool_work {
    struct bool_definition map[SGX535_NIR_MAX_INSTRUCTIONS];
    unsigned count;
};

static bool shape(const nir_def *def)
{
    return def && (def->bit_size == 1 || def->bit_size == 32) && def->num_components >= 1 &&
           def->num_components <= 4;
}
static bool canonical_source(const struct bool_work *w, const nir_alu_src *src, unsigned lane)
{
    const nir_def *def = src->src.ssa;
    unsigned component = src->swizzle[lane], i;
    if (!shape(def) || component >= def->num_components)
        return false;
    for (i = 0; i < w->count; i++)
        if (w->map[i].def == def)
            return w->map[i].canonical[component];
    return false;
}
static bool not_bool1_source(nir_src *src, void *unused)
{
    (void)unused;
    return src->ssa && src->ssa->bit_size != 1;
}
static bool flt_const_contract(const nir_alu_instr *a)
{
    bool constant = false;
    if (a->def.bit_size != 1)
        return false;
    for (unsigned i = 0; i < 2; i++) {
        const nir_alu_src *src = &a->src[i];
        if (!shape(src->src.ssa) || src->src.ssa->bit_size != 32)
            return false;
        for (unsigned lane = 0; lane < a->def.num_components; lane++)
            if (src->swizzle[lane] >= src->src.ssa->num_components)
                return false;
        constant |= nir_def_instr(src->src.ssa)->type == nir_instr_type_load_const;
    }
    return constant;
}
static bool bool_alu(struct bool_work *w, const nir_alu_instr *a, struct bool_definition *map,
                     bool allow_inot, bool allow_ieq, bool allow_ior, bool allow_fneu,
                     bool allow_bcsel, bool allow_ige_const, bool allow_flt_const,
                     bool allow_inverse_equality, bool allow_floor)
{
    unsigned c, i;
    bool boolean_op = false;
    if (!shape(&a->def))
        return false;
    if (a->op == nir_op_bcsel && allow_bcsel) {
        if (a->def.bit_size != 32 || !shape(a->src[0].src.ssa) || a->src[0].src.ssa->bit_size != 1)
            return false;
        for (c = 0; c < a->def.num_components; c++) {
            if (!canonical_source(w, &a->src[0], c))
                return false;
            for (i = 1; i < 3; i++) {
                const nir_alu_src *src = &a->src[i];
                if (!shape(src->src.ssa) || src->src.ssa->bit_size != 32 ||
                    src->swizzle[c] >= src->src.ssa->num_components)
                    return false;
            }
            /* An unselected branch is still part of the original contract.
             * Never infer canonicality by folding a constant condition. */
            map->canonical[c] =
                canonical_source(w, &a->src[1], c) && canonical_source(w, &a->src[2], c);
        }
        boolean_op = true;
    } else if (allow_bcsel && a->op == nir_op_b32csel) {
        /* Only the checked bool1 condition may be widened by this pass. */
        return false;
    } else if (a->op == nir_op_ige && allow_ige_const) {
        if (!sgx535_nir_ige_const_contract(a))
            return false;
        for (c = 0; c < a->def.num_components; c++)
            map->canonical[c] = true;
        boolean_op = true;
    } else if ((a->op == nir_op_flt || a->op == nir_op_fge) && allow_flt_const) {
        if (!flt_const_contract(a))
            return false;
        for (c = 0; c < a->def.num_components; c++)
            map->canonical[c] = true;
        boolean_op = true;
    } else if ((a->op == nir_op_ieq && allow_ieq) || (a->op == nir_op_fneu && allow_fneu) ||
               (allow_inverse_equality && (a->op == nir_op_feq || a->op == nir_op_ine))) {
        if (a->def.bit_size != 1)
            return false;
        for (c = 0; c < a->def.num_components; c++) {
            for (i = 0; i < 2; i++) {
                const nir_alu_src *src = &a->src[i];
                if (!shape(src->src.ssa) || src->src.ssa->bit_size != 32 ||
                    src->swizzle[c] >= src->src.ssa->num_components)
                    return false;
            }
            map->canonical[c] = true;
        }
        boolean_op = true;
    } else if ((allow_ieq && a->op == nir_op_ieq32) || (allow_fneu && a->op == nir_op_fneu32) ||
               (allow_inverse_equality && (a->op == nir_op_feq32 || a->op == nir_op_ine32))) {
        /* Only this checked pass may introduce the lowered compare. An
         * already-lowered original must not bypass the bool1 producer gate. */
        return false;
    } else if (a->op == nir_op_ffloor && allow_floor) {
        if (a->def.bit_size != 32 || !shape(a->src[0].src.ssa) ||
            a->src[0].src.ssa->bit_size != 32 || a->no_signed_wrap || a->no_unsigned_wrap ||
            (a->fp_math_ctrl &
             ~(nir_fp_preserve_signed_zero | nir_fp_preserve_inf | nir_fp_preserve_nan)))
            return false;
        for (c = 0; c < a->def.num_components; c++)
            if (a->src[0].swizzle[c] >= a->src[0].src.ssa->num_components)
                return false;
        return true;
    } else if (a->op == nir_op_ushr) {
        /* Only the checked floor rewrite introduces its constant shifts. */
        return false;
    } else if (a->op == nir_op_vec2 || a->op == nir_op_vec3 || a->op == nir_op_vec4) {
        if (nir_op_infos[a->op].num_inputs != a->def.num_components)
            return false;
        for (c = 0; c < a->def.num_components; c++) {
            map->canonical[c] = canonical_source(w, &a->src[c], 0);
            if (a->def.bit_size == 1 && !map->canonical[c])
                return false;
        }
        boolean_op = a->def.bit_size == 1;
    } else if (a->op == nir_op_mov) {
        for (c = 0; c < a->def.num_components; c++) {
            map->canonical[c] = canonical_source(w, &a->src[0], c);
            if (a->def.bit_size == 1 && !map->canonical[c])
                return false;
        }
        boolean_op = a->def.bit_size == 1;
    } else if (a->op == nir_op_inot) {
        if (!allow_inot)
            return false;
        for (c = 0; c < a->def.num_components; c++) {
            if (!canonical_source(w, &a->src[0], c))
                return false;
            map->canonical[c] = true;
        }
        boolean_op = true;
    } else if (a->op == nir_op_iand || a->op == nir_op_ior || a->op == nir_op_b2b1 ||
               a->op == nir_op_b2b32 || a->op == nir_op_b2f32 || a->op == nir_op_b2i32) {
        unsigned sources = a->op == nir_op_iand || a->op == nir_op_ior ? 2u : 1u;
        if (a->op == nir_op_ior && !allow_ior)
            return false;
        if ((a->op == nir_op_b2b1 && a->def.bit_size != 1) ||
            (a->op != nir_op_iand && a->op != nir_op_ior && a->op != nir_op_b2b1 &&
             a->def.bit_size != 32))
            return false;
        for (c = 0; c < a->def.num_components; c++) {
            for (i = 0; i < sources; i++)
                if (!canonical_source(w, &a->src[i], c))
                    return false;
            map->canonical[c] = a->op != nir_op_b2f32 && a->op != nir_op_b2i32;
        }
        boolean_op = true;
    } else {
        if (a->def.bit_size == 1)
            return false;
        /* The real lowerer rewrites additional boolean-producing operations.
         * Do not let that rewrite hide an unsupported original operation. */
        for (i = 0; i < nir_op_infos[a->op].num_inputs; i++)
            if (!a->src[i].src.ssa || a->src[i].src.ssa->bit_size == 1)
                return false;
    }
    if (boolean_op) {
        unsigned controls = a->op == nir_op_b2f32 ? nir_fp_preserve_signed_zero : 0;
        if ((a->op == nir_op_fneu && allow_fneu) ||
            ((a->op == nir_op_flt || a->op == nir_op_fge) && allow_flt_const) ||
            (a->op == nir_op_feq && allow_inverse_equality))
            controls = nir_fp_preserve_inf | nir_fp_preserve_nan;
        if (a->no_signed_wrap || a->no_unsigned_wrap || (a->fp_math_ctrl & ~controls))
            return false;
    }
    return true;
}

static enum sgx535_nir_status original_contract(const nir_shader *shader, bool allow_inot,
                                                bool allow_ieq, bool allow_ior, bool allow_fneu,
                                                bool allow_bcsel, bool allow_ige_const,
                                                bool allow_flt_const, bool allow_inverse_equality,
                                                bool allow_floor)
{
    const nir_block *block = sgx535_nir_fragment_linear_block(shader);
    struct bool_work *w;
    enum sgx535_nir_status status = SGX535_NIR_OK;
    if (!block)
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    nir_foreach_instr(instr, block)
    {
        struct bool_definition map = {0};
        if (w->count == SGX535_NIR_MAX_INSTRUCTIONS) {
            status = SGX535_NIR_ERR_LIMIT;
            break;
        }
        if (instr->type == nir_instr_type_load_const) {
            const nir_load_const_instr *load = nir_instr_as_load_const(instr);
            unsigned c;
            map.def = &load->def;
            if (!shape(map.def))
                goto unsupported;
            for (c = 0; c < map.def->num_components; c++)
                map.canonical[c] = map.def->bit_size == 1 || load->value[c].u32 == 0 ||
                                   load->value[c].u32 == UINT32_MAX;
        } else if (instr->type == nir_instr_type_alu) {
            const nir_alu_instr *a = nir_instr_as_alu(instr);
            map.def = &a->def;
            if (!bool_alu(w, a, &map, allow_inot, allow_ieq, allow_ior, allow_fneu, allow_bcsel,
                          allow_ige_const, allow_flt_const, allow_inverse_equality, allow_floor))
                goto unsupported;
        } else if (instr->type == nir_instr_type_undef || instr->type == nir_instr_type_intrinsic) {
            map.def = nir_instr_def(instr);
            if ((map.def && (!shape(map.def) || map.def->bit_size == 1)) ||
                !nir_foreach_src(instr, not_bool1_source, NULL))
                goto unsupported;
        } else {
        unsupported:
            status = SGX535_NIR_ERR_UNSUPPORTED;
            break;
        }
        if (map.def)
            w->map[w->count++] = map;
    }
    free(w);
    return status;
}

static nir_def *raw32_nan(nir_builder *b, nir_def *value)
{
    nir_def *exponent = nir_iand(b, value, nir_imm_intN_t(b, UINT32_C(0x7f800000), 32));
    nir_def *mantissa = nir_iand(b, value, nir_imm_intN_t(b, UINT32_C(0x007fffff), 32));
    nir_def *all_exponent = nir_ieq(b, exponent, nir_imm_intN_t(b, UINT32_C(0x7f800000), 32));
    nir_def *zero_mantissa = nir_ieq(b, mantissa, nir_imm_intN_t(b, 0, 32));
    return nir_iand(b, all_exponent, nir_inot(b, zero_mantissa));
}

/* For 127 <= E < 150, (E + 1) & 31 is exactly E - 127.
 * A five-bit ripple increment selects checked constant shifts of the fraction
 * mask. Other exponents are gated below; no variable native shift is used. */
static bool lower_raw32_floor(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_def *values[4];
    (void)unused;
    if (instr->type != nir_instr_type_alu || nir_instr_as_alu(instr)->op != nir_op_ffloor)
        return false;
    nir_alu_instr *a = nir_instr_as_alu(instr);
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    nir_def *zero = nir_imm_int(b, 0);
    nir_def *initial_mask = nir_imm_intN_t(b, UINT32_C(0x007fffff), 32);
    nir_def *sign_mask = nir_imm_intN_t(b, UINT32_C(0x80000000), 32);
    nir_def *all_bits = nir_imm_intN_t(b, UINT32_MAX, 32);
    nir_def *minus_one = nir_imm_float(b, -1.0f);
    nir_def *high_mask = nir_imm_intN_t(b, UINT32_C(0x40000000), 32);
    nir_def *upper_mask = nir_imm_intN_t(b, UINT32_C(0x30000000), 32);
    nir_def *low_seven = nir_imm_intN_t(b, UINT32_C(0x3f800000), 32);
    nir_def *pair_mask = nir_imm_intN_t(b, UINT32_C(0x03000000), 32);
    nir_def *bit_mask[5], *shift[5];
    for (unsigned bit = 0; bit < 5; bit++) {
        bit_mask[bit] = nir_imm_intN_t(b, UINT32_C(1) << (23u + bit), 32);
        shift[bit] = nir_imm_int(b, 1u << bit);
    }
    for (unsigned lane = 0; lane < a->def.num_components; lane++) {
        nir_def *raw = nir_channel(b, a->src[0].src.ssa, a->src[0].swizzle[lane]);
        nir_def *fraction_mask = initial_mask;
        nir_def *carry = NULL;
        for (unsigned bit = 0; bit < 5; bit++) {
            nir_def *clear = nir_ieq(b, nir_iand(b, raw, bit_mask[bit]), zero);
            nir_def *set = nir_inot(b, clear);
            nir_def *incremented = bit ? nir_inot(b, nir_ieq(b, set, carry)) : clear;
            if (bit < 4)
                carry = bit ? nir_iand(b, set, carry) : set;
            fraction_mask =
                nir_bcsel(b, incremented, nir_ushr(b, fraction_mask, shift[bit]), fraction_mask);
        }
        /* Exact eight-bit exponent gates, without a general integer compare:
         * E>=127 iff E7 or all E6..E0; E>=150 iff E7 and
         * (E6 or E5 or (E4 and (E3 or (E2 and E1)))). */
        nir_def *high = nir_inot(b, nir_ieq(b, nir_iand(b, raw, high_mask), zero));
        nir_def *small =
            nir_inot(b, nir_ior(b, high, nir_ieq(b, nir_iand(b, raw, low_seven), low_seven)));
        nir_def *upper = nir_inot(b, nir_ieq(b, nir_iand(b, raw, upper_mask), zero));
        nir_def *middle = nir_inot(b, nir_ieq(b, nir_iand(b, raw, bit_mask[4]), zero));
        nir_def *next = nir_inot(b, nir_ieq(b, nir_iand(b, raw, bit_mask[3]), zero));
        nir_def *pair = nir_ieq(b, nir_iand(b, raw, pair_mask), pair_mask);
        nir_def *large =
            nir_iand(b, high, nir_ior(b, upper, nir_iand(b, middle, nir_ior(b, next, pair))));
        nir_def *mask = nir_bcsel(b, large, all_bits, nir_inot(b, fraction_mask));
        nir_def *sign = nir_iand(b, raw, sign_mask);
        nir_def *truncated = nir_bcsel(b, small, sign, nir_iand(b, raw, mask));
        nir_def *fraction = nir_inot(b, nir_ieq(b, raw, truncated));
        nir_def *negative = nir_ieq(b, sign, sign_mask);
        /* Only a negative nonintegral finite value selects this exact integer
         * subtraction. Zeros, infinities and every NaN retain their raw word. */
        nir_def *adjusted = nir_fadd(b, truncated, minus_one);
        values[lane] = nir_bcsel(b, nir_iand(b, fraction, negative), adjusted, truncated);
    }
    nir_def_replace(&a->def, nir_vec(b, values, a->def.num_components));
    return true;
}

static uint32_t flt_constant_key(uint32_t word)
{
    return word & UINT32_C(0x80000000) ? ~word | UINT32_C(0x80000000) : word;
}

static nir_def *flt_variable_key(nir_builder *b, nir_def *value)
{
    nir_def *sign = nir_imm_intN_t(b, UINT32_C(0x80000000), 32);
    nir_def *negative = nir_ieq(b, nir_iand(b, value, sign), sign);
    return nir_bcsel(b, negative, nir_ior(b, nir_inot(b, value), sign), value);
}

static bool lower_raw32_flt_const(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *a;
    nir_def *values[4];
    unsigned constant_source, variable_source;
    const nir_load_const_instr *constant;
    (void)unused;
    if (instr->type != nir_instr_type_alu ||
        (nir_instr_as_alu(instr)->op != nir_op_flt && nir_instr_as_alu(instr)->op != nir_op_fge))
        return false;
    a = nir_instr_as_alu(instr);
    constant_source = nir_def_instr(a->src[1].src.ssa)->type == nir_instr_type_load_const ? 1 : 0;
    variable_source = 1 - constant_source;
    constant = nir_instr_as_load_const(nir_def_instr(a->src[constant_source].src.ssa));
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    for (unsigned lane = 0; lane < a->def.num_components; lane++) {
        uint32_t word = constant->value[a->src[constant_source].swizzle[lane]].u32;
        /* A constant NaN makes an ordered comparison false. The complete original graph
         * has already been checked, including dead unsupported operations. */
        if ((word & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000)) {
            values[lane] = nir_imm_bool(b, false);
            continue;
        }
        nir_def *variable =
            nir_channel(b, a->src[variable_source].src.ssa, a->src[variable_source].swizzle[lane]);
        nir_def *key = flt_variable_key(b, variable);
        nir_def *threshold = nir_imm_intN_t(b, flt_constant_key(word), 32);
        /* Signed keys order every non-NaN word, including subnormals and
         * infinities. Reuse the existing constant IGE lowering; no native
         * floating TEST ordering or subtraction precision is assumed. */
        nir_def *ge = constant_source ? nir_ige(b, key, threshold) : nir_ige(b, threshold, key);
        nir_def *ordered = nir_inot(b, raw32_nan(b, variable));
        if (a->op == nir_op_fge) {
            /* Inclusive ordering admits both signs of zero. A NaN still
             * makes the ordered comparison false, never !FLT alone. */
            if (!(word & UINT32_C(0x7fffffff))) {
                nir_def *absolute =
                    nir_iand(b, variable, nir_imm_intN_t(b, UINT32_C(0x7fffffff), 32));
                ge = nir_ior(b, ge, nir_ieq(b, absolute, nir_imm_int(b, 0)));
            }
            values[lane] = nir_iand(b, ge, ordered);
        } else {
            if (!(word & UINT32_C(0x7fffffff))) {
                nir_def *absolute =
                    nir_iand(b, variable, nir_imm_intN_t(b, UINT32_C(0x7fffffff), 32));
                ordered =
                    nir_iand(b, ordered, nir_inot(b, nir_ieq(b, absolute, nir_imm_int(b, 0))));
            }
            values[lane] = nir_iand(b, nir_inot(b, ge), ordered);
        }
    }
    nir_def_replace(&a->def, nir_vec(b, values, a->def.num_components));
    return true;
}

static bool lower_inverse_equality(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *a;
    nir_def *left, *right, *value;
    (void)unused;
    if (instr->type != nir_instr_type_alu)
        return false;
    a = nir_instr_as_alu(instr);
    if (a->op != nir_op_feq && a->op != nir_op_ine)
        return false;
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    left = nir_ssa_for_alu_src(b, a, 0);
    right = nir_ssa_for_alu_src(b, a, 1);
    /* Ordered FEQ is the complement of unordered FNEU, including NaNs and
     * signed zero. Raw integer INE is the complement of full-word IEQ.
     * The existing checked consumers implement both without float math. */
    value = a->op == nir_op_feq ? nir_fneu(b, left, right) : nir_ieq(b, left, right);
    nir_def_as_alu(value)->fp_math_ctrl = a->fp_math_ctrl;
    nir_def_replace(&a->def, nir_inot(b, value));
    return true;
}

static bool lower_raw32_fneu(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *a;
    nir_def *left, *right, *left_abs, *right_abs, *same, *left_zero, *right_zero;
    nir_def *both_zero, *equal_numbers, *different, *left_nan, *right_nan, *value;
    (void)unused;
    if (instr->type != nir_instr_type_alu)
        return false;
    a = nir_instr_as_alu(instr);
    if (a->op != nir_op_fneu)
        return false;
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    left = nir_ssa_for_alu_src(b, a, 0);
    right = nir_ssa_for_alu_src(b, a, 1);
    same = nir_ieq(b, left, right);
    left_abs = nir_iand(b, left, nir_imm_intN_t(b, UINT32_C(0x7fffffff), 32));
    right_abs = nir_iand(b, right, nir_imm_intN_t(b, UINT32_C(0x7fffffff), 32));
    left_zero = nir_ieq(b, left_abs, nir_imm_intN_t(b, 0, 32));
    right_zero = nir_ieq(b, right_abs, nir_imm_intN_t(b, 0, 32));
    both_zero = nir_iand(b, left_zero, right_zero);
    equal_numbers = nir_ior(b, same, both_zero);
    different = nir_inot(b, equal_numbers);
    left_nan = raw32_nan(b, left);
    right_nan = raw32_nan(b, right);
    value = nir_ior(b, nir_ior(b, different, left_nan), right_nan);
    nir_def_replace(&a->def, value);
    return true;
}

static bool lower_raw32_bcsel(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *a;
    nir_def *condition, *mask, *left, *right, *selected_left, *selected_right, *value;
    (void)unused;
    if (instr->type != nir_instr_type_alu)
        return false;
    a = nir_instr_as_alu(instr);
    if (a->op != nir_op_bcsel)
        return false;
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    condition = nir_ssa_for_alu_src(b, a, 0);
    left = nir_ssa_for_alu_src(b, a, 1);
    right = nir_ssa_for_alu_src(b, a, 2);
    mask = nir_b2b32(b, condition);
    selected_left = nir_iand(b, mask, left);
    selected_right = nir_iand(b, nir_inot(b, mask), right);
    value = nir_ior(b, selected_left, selected_right);
    nir_def_replace(&a->def, value);
    return true;
}

static bool lower_word_ior(nir_builder *b, nir_instr *instr, void *unused)
{
    nir_alu_instr *a;
    nir_def *left, *right, *not_left, *not_right, *both, *value;
    (void)unused;
    if (instr->type != nir_instr_type_alu)
        return false;
    a = nir_instr_as_alu(instr);
    if (a->op != nir_op_ior)
        return false;
    b->cursor = nir_before_instr(instr);
    b->constant_fold_alu = false;
    left = nir_ssa_for_alu_src(b, a, 0);
    right = nir_ssa_for_alu_src(b, a, 1);
    not_left = nir_inot(b, left);
    not_right = nir_inot(b, right);
    both = nir_iand(b, not_left, not_right);
    value = nir_inot(b, both);
    nir_def_replace(&a->def, value);
    return true;
}

static enum sgx535_nir_status
lower_checked_clone(nir_shader *clone, const struct sgx535_fragment_nir_options *options,
                    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
                    struct sgx535_fragment_linkage *linkage, bool allow_ieq, bool allow_ior,
                    bool allow_fneu, bool allow_bcsel, bool allow_ige_const, bool allow_flt_const,
                    bool allow_inverse_equality, bool allow_floor)
{
    if (allow_floor)
        nir_shader_instructions_pass(clone, lower_raw32_floor, nir_metadata_control_flow, NULL);
    if (allow_inverse_equality)
        nir_shader_instructions_pass(clone, lower_inverse_equality, nir_metadata_control_flow,
                                     NULL);
    if (allow_flt_const)
        nir_shader_instructions_pass(clone, lower_raw32_flt_const, nir_metadata_control_flow, NULL);
    if (allow_ige_const)
        sgx535_nir_lower_ige_const(clone);
    /* Only the validated private clone is rewritten. Resolve original source
     * swizzles before applying Boolean De Morgan; never fold away unchecked
     * original instructions or infer canonicality from the lowered graph. */
    /* Only this validated rewrite may introduce raw noncanonical bit masks.
     * IEEE32 zero/NaN classification uses integer words; it emits no floating
     * arithmetic and preserves both NaN and infinity comparison requirements. */
    if (allow_bcsel)
        nir_shader_instructions_pass(clone, lower_raw32_bcsel, nir_metadata_control_flow, NULL);
    if (allow_fneu)
        nir_shader_instructions_pass(clone, lower_raw32_fneu, nir_metadata_control_flow, NULL);
    if (allow_ior)
        /* De Morgan is exact for both the original canonical Boolean IOR and
         * payload words introduced only by the checked BCSEL rewrite. */
        nir_shader_instructions_pass(clone, lower_word_ior, nir_metadata_control_flow, NULL);
    /* This actual Mesa pass preserves the graph: no DCE or source folding. */
    nir_lower_bool_to_int32(clone);
    if (allow_floor)
        return sgx535_nir_fragment_bool32_ieq_const_ushr_lowered_options_ir(
            clone, options, output, capacity, count, linkage, allow_ige_const);
    return allow_ieq ? sgx535_nir_fragment_bool32_ieq_lowered_options_ir(
                           clone, options, output, capacity, count, linkage, allow_ige_const)
                     : sgx535_nir_fragment_bool32_lowered_ir(clone, options->uniform_layout, output,
                                                             capacity, count, linkage);
}

static enum sgx535_nir_status
bool32_ir(const nir_shader *shader, enum sgx535_fragment_uniform_layout layout,
          struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
          struct sgx535_fragment_linkage *linkage, bool allow_inot, bool allow_ieq, bool allow_ior,
          bool allow_fneu, bool allow_bcsel, enum sgx535_fragment_io_precision_policy io_precision,
          bool allow_ige_const, bool allow_flt_const, bool allow_inverse_equality)
{
    enum sgx535_nir_status status;
    nir_shader *clone;
    const struct sgx535_fragment_nir_options options = {layout, io_precision};
    if (!shader || !output || !count || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    status = original_contract(shader, allow_inot, allow_ieq, allow_ior, allow_fneu, allow_bcsel,
                               allow_ige_const, allow_flt_const, allow_inverse_equality, false);
    if (status != SGX535_NIR_OK)
        return status;
    clone = nir_shader_clone(NULL, shader);
    if (!clone)
        return SGX535_NIR_ERR_NOMEM;
    status = lower_checked_clone(clone, &options, output, capacity, count, linkage, allow_ieq,
                                 allow_ior, allow_fneu, allow_bcsel, allow_ige_const,
                                 allow_flt_const, allow_inverse_equality, false);
    ralloc_free(clone);
    return status;
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_iand_ir(const nir_shader *shader,
                                   enum sgx535_fragment_uniform_layout layout,
                                   struct sgx535_fragment_instruction *output, size_t capacity,
                                   size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, false, false, false, false,
                     false, SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_inot_ir(const nir_shader *shader,
                                   enum sgx535_fragment_uniform_layout layout,
                                   struct sgx535_fragment_instruction *output, size_t capacity,
                                   size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, true, false, false, false,
                     false, SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_ir(const nir_shader *shader,
                                                         enum sgx535_fragment_uniform_layout layout,
                                                         struct sgx535_fragment_instruction *output,
                                                         size_t capacity, size_t *count,
                                                         struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, true, true, false, false,
                     false, SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_bool32_ior_ir(const nir_shader *shader,
                                                         enum sgx535_fragment_uniform_layout layout,
                                                         struct sgx535_fragment_instruction *output,
                                                         size_t capacity, size_t *count,
                                                         struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, true, true, true, false,
                     false, SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_fneu_ir(const nir_shader *shader,
                                   enum sgx535_fragment_uniform_layout layout,
                                   struct sgx535_fragment_instruction *output, size_t capacity,
                                   size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, true, true, true, true,
                     false, SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_bcsel_ir(const nir_shader *shader,
                                    enum sgx535_fragment_uniform_layout layout,
                                    struct sgx535_fragment_instruction *output, size_t capacity,
                                    size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return bool32_ir(shader, layout, output, capacity, count, linkage, true, true, true, true, true,
                     SGX535_FRAGMENT_IO_STRICT, false, false, false);
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_options_ir(const nir_shader *shader,
                                      const struct sgx535_fragment_nir_options *options,
                                      struct sgx535_fragment_instruction *output, size_t capacity,
                                      size_t *count, struct sgx535_fragment_linkage *linkage)
{
    struct sgx535_fragment_nir_options selected;
    if (!options)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return bool32_ir(shader, selected.uniform_layout, output, capacity, count, linkage, true, true,
                     true, true, true, selected.io_precision, false, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_bool32_compare_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage)
{
    struct sgx535_fragment_nir_options selected;
    if (!options)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return bool32_ir(shader, selected.uniform_layout, output, capacity, count, linkage, true, true,
                     true, true, true, selected.io_precision, true, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_flt_const_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage)
{
    struct sgx535_fragment_nir_options selected;
    if (!options)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return bool32_ir(shader, selected.uniform_layout, output, capacity, count, linkage, true, true,
                     true, true, true, selected.io_precision, true, true, true);
}

enum sgx535_nir_status
sgx535_nir_fragment_floor_options_ir(const nir_shader *shader,
                                     const struct sgx535_fragment_nir_options *options,
                                     struct sgx535_fragment_instruction *output, size_t capacity,
                                     size_t *count, struct sgx535_fragment_linkage *linkage)
{
    enum sgx535_nir_status status;
    nir_shader *clone;
    if (!shader || !options || !output || !count || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    status = original_contract(shader, true, true, true, true, true, true, true, true, true);
    if (status != SGX535_NIR_OK)
        return status;
    clone = nir_shader_clone(NULL, shader);
    if (!clone)
        return SGX535_NIR_ERR_NOMEM;
    status = lower_checked_clone(clone, options, output, capacity, count, linkage, true, true, true,
                                 true, true, true, true, true);
    ralloc_free(clone);
    return status;
}

/* One original pure diamond, never a general control-flow flattener. */
struct diamond_work {
    struct bool_work values;
    const nir_instr *instructions[SGX535_NIR_MAX_INSTRUCTIONS];
    bool opaque[SGX535_NIR_MAX_INSTRUCTIONS];
    const nir_block *block[4]; /* prefix, true arm, false arm, merge */
    unsigned count, outputs, phis;
};

static const struct bool_definition *diamond_value(const struct diamond_work *w, const nir_def *def)
{
    for (unsigned i = 0; i < w->values.count; i++)
        if (w->values.map[i].def == def)
            return &w->values.map[i];
    return NULL;
}

static bool diamond_has_predecessor(const nir_block *block, const nir_block *wanted)
{
    nir_foreach_pred(pred, block) if (pred == wanted) return true;
    return false;
}

static bool diamond_shape(const nir_shader *shader, struct diamond_work *w, const nir_if **branch)
{
    nir_function *f;
    nir_cf_node *first, *middle, *last;
    nir_if *selected;
    if (shader->info.stage != MESA_SHADER_FRAGMENT || shader->info.float_controls_execution_mode ||
        !exec_list_is_empty(&shader->variables) || !exec_list_is_singular(&shader->functions))
        return false;
    f = exec_node_data(nir_function, shader->functions.head_sentinel.next, node);
    if (!f->is_entrypoint || f->num_params || !f->impl || f->shader != shader ||
        f->impl->function != f || f->impl->preamble || !f->impl->structured ||
        !exec_list_is_empty(&f->impl->locals) || exec_list_length(&f->impl->body) != 3 ||
        !f->impl->end_block || !exec_list_is_empty(&f->impl->end_block->instr_list))
        return false;
    first = exec_node_data(nir_cf_node, f->impl->body.head_sentinel.next, node);
    middle = nir_cf_node_next(first);
    last = nir_cf_node_next(middle);
    if (first->type != nir_cf_node_block || middle->type != nir_cf_node_if ||
        last->type != nir_cf_node_block)
        return false;
    selected = nir_cf_node_as_if(middle);
    if (!exec_list_is_singular(&selected->then_list) ||
        !exec_list_is_singular(&selected->else_list))
        return false;
    nir_cf_node *then_node =
        exec_node_data(nir_cf_node, selected->then_list.head_sentinel.next, node);
    nir_cf_node *else_node =
        exec_node_data(nir_cf_node, selected->else_list.head_sentinel.next, node);
    if (then_node->type != nir_cf_node_block || else_node->type != nir_cf_node_block)
        return false;
    w->block[0] = nir_cf_node_as_block(first);
    w->block[1] = nir_cf_node_as_block(then_node);
    w->block[2] = nir_cf_node_as_block(else_node);
    w->block[3] = nir_cf_node_as_block(last);
    for (unsigned i = 0; i < 4; i++)
        if (w->block[i]->impl != f->impl)
            return false;
    if (nir_block_num_preds(w->block[0]) || w->block[0]->successors[0] != w->block[1] ||
        w->block[0]->successors[1] != w->block[2] || nir_block_num_preds(w->block[3]) != 2 ||
        w->block[3]->successors[0] != f->impl->end_block || w->block[3]->successors[1])
        return false;
    for (unsigned i = 1; i <= 2; i++)
        if (nir_block_num_preds(w->block[i]) != 1 ||
            !diamond_has_predecessor(w->block[i], w->block[0]) ||
            w->block[i]->successors[0] != w->block[3] || w->block[i]->successors[1] ||
            !diamond_has_predecessor(w->block[3], w->block[i]))
            return false;
    *branch = selected;
    return true;
}

struct diamond_source_check {
    const struct diamond_work *w;
    unsigned stage;
};

static bool diamond_dominating_source(nir_src *src, void *data)
{
    const struct diamond_source_check *check = data;
    const nir_block *producer;
    if (!src->ssa || !diamond_value(check->w, src->ssa))
        return false;
    producer = nir_def_instr(src->ssa)->block;
    /* Lexical order proves instruction dominance for this exact acyclic
     * shape. A sibling arm cannot supply a value except through its phi. */
    return producer == check->w->block[0] || producer == check->w->block[check->stage];
}

static bool diamond_boolean_alu(const nir_alu_instr *a)
{
    switch (a->op) {
    case nir_op_bcsel:
    case nir_op_ige:
    case nir_op_flt:
    case nir_op_ieq:
    case nir_op_fneu:
    case nir_op_ine:
    case nir_op_feq:
    case nir_op_iand:
    case nir_op_inot:
    case nir_op_ior:
    case nir_op_b2b1:
    case nir_op_b2b32:
    case nir_op_b2f32:
    case nir_op_b2i32:
        return true;
    default:
        return a->def.bit_size == 1;
    }
}

static bool diamond_phi(struct diamond_work *w, const nir_phi_instr *phi,
                        struct bool_definition *map)
{
    bool predecessors[2] = {false, false};
    unsigned count = 0;
    if (!shape(&phi->def) || phi->def.bit_size != 32)
        return false;
    map->def = &phi->def;
    for (unsigned c = 0; c < phi->def.num_components; c++)
        map->canonical[c] = true;
    nir_foreach_phi_src(src, phi)
    {
        const struct bool_definition *value = diamond_value(w, src->src.ssa);
        unsigned arm;
        if (src->pred == w->block[1])
            arm = 0;
        else if (src->pred == w->block[2])
            arm = 1;
        else
            return false;
        if (predecessors[arm] || !value || value->def->bit_size != 32 ||
            value->def->num_components != phi->def.num_components ||
            (nir_def_instr(value->def)->block != w->block[0] &&
             nir_def_instr(value->def)->block != src->pred))
            return false;
        predecessors[arm] = true;
        count++;
        for (unsigned c = 0; c < phi->def.num_components; c++)
            map->canonical[c] &= value->canonical[c];
    }
    return count == 2 && predecessors[0] && predecessors[1];
}

static enum sgx535_nir_status diamond_block_contract(struct diamond_work *w, unsigned stage)
{
    bool passed_phi = false;
    nir_foreach_instr(instr, w->block[stage])
    {
        struct bool_definition map = {0};
        struct diamond_source_check check = {w, stage};
        bool opaque = false;
        if (w->count == SGX535_NIR_MAX_INSTRUCTIONS ||
            w->values.count == SGX535_NIR_MAX_INSTRUCTIONS)
            return SGX535_NIR_ERR_LIMIT;
        if (instr->type == nir_instr_type_phi) {
            if (stage != 3 || passed_phi || !diamond_phi(w, nir_instr_as_phi(instr), &map))
                return SGX535_NIR_ERR_UNSUPPORTED;
            w->phis++;
            opaque = true;
        } else {
            passed_phi = true;
            if (!nir_foreach_src(instr, diamond_dominating_source, &check))
                return SGX535_NIR_ERR_SSA;
            if (instr->type == nir_instr_type_load_const) {
                const nir_load_const_instr *load = nir_instr_as_load_const(instr);
                map.def = &load->def;
                if (!shape(map.def))
                    return SGX535_NIR_ERR_UNSUPPORTED;
                for (unsigned c = 0; c < map.def->num_components; c++)
                    map.canonical[c] = map.def->bit_size == 1 || load->value[c].u32 == 0 ||
                                       load->value[c].u32 == UINT32_MAX;
                opaque = map.def->bit_size == 1;
            } else if (instr->type == nir_instr_type_alu) {
                const nir_alu_instr *a = nir_instr_as_alu(instr);
                map.def = &a->def;
                if (!bool_alu(&w->values, a, &map, true, true, true, true, true, true, true, true,
                              false))
                    return SGX535_NIR_ERR_UNSUPPORTED;
                opaque = diamond_boolean_alu(a);
            } else if (instr->type == nir_instr_type_intrinsic) {
                const nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
                /* Branch-local reads are restricted to the fully bounded
                 * uniform consumer. No derivative/helper or effect can hoist. */
                if ((stage == 1 || stage == 2) && in->intrinsic != nir_intrinsic_load_uniform)
                    return SGX535_NIR_ERR_UNSUPPORTED;
                if (in->intrinsic == nir_intrinsic_store_output) {
                    if (stage != 3 || !nir_instr_is_last(instr) || ++w->outputs != 1)
                        return SGX535_NIR_ERR_UNSUPPORTED;
                } else {
                    map.def = &in->def;
                    if (!shape(map.def) || map.def->bit_size != 32)
                        return SGX535_NIR_ERR_UNSUPPORTED;
                }
            } else if (instr->type == nir_instr_type_undef) {
                map.def = &nir_instr_as_undef(instr)->def;
                if (!shape(map.def) || map.def->bit_size != 32)
                    return SGX535_NIR_ERR_UNSUPPORTED;
            } else {
                return SGX535_NIR_ERR_UNSUPPORTED;
            }
        }
        if (map.def) {
            if (diamond_value(w, map.def))
                return SGX535_NIR_ERR_SSA;
            w->values.map[w->values.count++] = map;
        }
        w->instructions[w->count] = instr;
        w->opaque[w->count++] = opaque;
    }
    return SGX535_NIR_OK;
}

static enum sgx535_nir_status
original_diamond_contract(const nir_shader *shader,
                          const struct sgx535_fragment_nir_options *options)
{
    struct diamond_work *w = calloc(1, sizeof(*w));
    const nir_if *branch;
    const struct bool_definition *condition;
    enum sgx535_nir_status status = SGX535_NIR_ERR_UNSUPPORTED;
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    if (!diamond_shape(shader, w, &branch))
        goto finish;
    status = diamond_block_contract(w, 0);
    condition = diamond_value(w, branch->condition.ssa);
    if (status != SGX535_NIR_OK)
        goto finish;
    if (!condition || condition->def->bit_size != 1 || condition->def->num_components != 1 ||
        !condition->canonical[0]) {
        status = SGX535_NIR_ERR_UNSUPPORTED;
        goto finish;
    }
    for (unsigned stage = 1; stage < 4 && status == SGX535_NIR_OK; stage++)
        status = diamond_block_contract(w, stage);
    if (status == SGX535_NIR_OK && (!w->phis || w->outputs != 1))
        status = SGX535_NIR_ERR_UNSUPPORTED;
    if (status == SGX535_NIR_OK)
        status = sgx535_nir_fragment_pure_original_options_contract(w->instructions, w->opaque,
                                                                    w->count, options);
finish:
    free(w);
    return status;
}

enum sgx535_nir_status sgx535_nir_fragment_pure_diamond_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage)
{
    const nir_opt_peephole_select_options select_options = {
        .limit = SGX535_NIR_MAX_INSTRUCTIONS,
        .indirect_load_ok = false,
        .expensive_alu_ok = false,
        .discard_ok = false,
    };
    struct sgx535_fragment_nir_options selected;
    enum sgx535_nir_status status;
    nir_shader *clone;
    if (!shader || !options || !output || !count || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    status = original_diamond_contract(shader, &selected);
    if (status != SGX535_NIR_OK)
        return status;
    clone = nir_shader_clone(NULL, shader);
    if (!clone)
        return SGX535_NIR_ERR_NOMEM;
    /* Only this fully checked clone may enter Mesa's finite safe hoister.
     * Neither finalize_nir nor any ordinary entrypoint changes its grammar. */
    nir_opt_peephole_select(clone, &select_options);
    status = original_contract(clone, true, true, true, true, true, true, true, true, false);
    if (status == SGX535_NIR_OK)
        status = lower_checked_clone(clone, &selected, output, capacity, count, linkage, true, true,
                                     true, true, true, true, true, false);
    ralloc_free(clone);
    return status;
}
