// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Mesa NIR vertex shaders as SGX535 vertex programs (sgx535_vertex_nir.h).
 * The source NIR is not changed: a clone is lowered, read and freed. Booleans
 * are floats, exactly 0.0 or 1.0. */
#include "nir.h"
#include "sgx535_vertex_nir.h"
#include <stdlib.h>
#include <string.h>

struct work {
    struct sgx535_vertex_nir_program *p;
    struct sgx535_vertex_source (*def)[4]; /* each SSA definition's components */
    bool *defined;
    uint32_t position_written;
};

static bool refuse(struct work *w, const char *what)
{
    if (!w->p->refusal)
        w->p->refusal = what;
    return false;
}
static struct sgx535_vertex_source sa(uint32_t index)
{
    return (struct sgx535_vertex_source){SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)index};
}
/* The SA register holding KIND/VALUE, appended once. */
static bool constant(struct work *w, uint32_t kind, uint32_t value, struct sgx535_vertex_source *out)
{
    struct sgx535_vertex_nir_program *p = w->p;
    for (uint32_t k = 0; k < p->constant_count; k++)
        if (p->constant[k].kind == kind && p->constant[k].value == value) {
            *out = sa(k);
            return true;
        }
    if (p->constant_count == SGX535_VERTEX_CONSTANTS)
        return refuse(w, "constant registers");
    p->constant[p->constant_count] = (struct sgx535_vertex_constant){kind, value};
    *out = sa(p->constant_count++);
    return true;
}
static bool emit(struct work *w, uint8_t opcode, struct sgx535_vertex_source a,
                 struct sgx535_vertex_source b, struct sgx535_vertex_source c,
                 struct sgx535_vertex_source *out)
{
    struct sgx535_vertex_nir_program *p = w->p;
    if (p->count == SGX535_VERTEX_MAX_INSTRUCTIONS)
        return refuse(w, "instructions");
    p->instruction[p->count] = (struct sgx535_vertex_instruction){opcode, -1, {a, b, c}};
    *out = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, (uint16_t)p->count++};
    return true;
}
static const struct sgx535_vertex_source none;
static bool unary(struct work *w, uint8_t opcode, struct sgx535_vertex_source a,
                  struct sgx535_vertex_source *out)
{
    return emit(w, opcode, a, none, none, out);
}
static bool binary(struct work *w, uint8_t opcode, struct sgx535_vertex_source a,
                   struct sgx535_vertex_source b, struct sgx535_vertex_source *out)
{
    return emit(w, opcode, a, b, none, out);
}
static bool mul(struct work *w, struct sgx535_vertex_source a, struct sgx535_vertex_source b,
                struct sgx535_vertex_source *out)
{
    return emit(w, SGX535_VERTEX_MAD, a, b, sa(w->p->zero), out);
}
static struct sgx535_vertex_source negated(struct sgx535_vertex_source s)
{
    s.negate ^= 1u;
    return s;
}

/* A 32-bit float value, or a boolean: an exact 0.0 or 1.0 (comparisons). */
static bool source(struct work *w, const nir_alu_instr *alu, unsigned i, unsigned c,
                   struct sgx535_vertex_source *out)
{
    const nir_def *d = alu->src[i].src.ssa;
    if ((d->bit_size != 32 && d->bit_size != 1) || !w->defined[d->index])
        return refuse(w, "source");
    *out = w->def[d->index][alu->src[i].swizzle[c]];
    return true;
}

/* Comparisons without predicates: A < B is 1.0 exactly when B - A is positive
 * (a normal float: a product by 2^126 reaches 1), else 0.0, as
 * clamp((B - A) * 2^126, 0, 1); A != B the same of |B - A|. The others are
 * one minus these. */
static bool step(struct work *w, struct sgx535_vertex_source a, struct sgx535_vertex_source b,
                 bool unequal, bool complement, struct sgx535_vertex_source *out)
{
    struct sgx535_vertex_source big, d, t;
    if (!constant(w, SGX535_VERTEX_LITERAL, 0x7e800000u, &big) ||
        !emit(w, SGX535_VERTEX_MAD, b, sa(w->p->one), negated(a), &d))
        return false;
    if (unequal) {
        d.absolute = 1;
        d.negate = 0;
    }
    if (!mul(w, d, big, &t) || !binary(w, SGX535_VERTEX_MAX, t, sa(w->p->zero), &t) ||
        !binary(w, SGX535_VERTEX_MIN, t, sa(w->p->one), out))
        return false;
    return !complement || emit(w, SGX535_VERTEX_MAD, *out, negated(sa(w->p->one)),
                               sa(w->p->one), out);
}
/* C ? X : Y for a 0.0/1.0 C: C * X + (Y - C * Y), exact for finite X and Y. */
static bool blend(struct work *w, struct sgx535_vertex_source c, struct sgx535_vertex_source x,
                   struct sgx535_vertex_source y, struct sgx535_vertex_source *out)
{
    struct sgx535_vertex_source t;
    return emit(w, SGX535_VERTEX_MAD, negated(c), y, y, &t) &&
           emit(w, SGX535_VERTEX_MAD, c, x, t, out);
}

static bool alu(struct work *w, const nir_alu_instr *x)
{
    const nir_def *d = &x->def;
    unsigned n = d->num_components, inputs = nir_op_infos[x->op].num_inputs;
    struct sgx535_vertex_source s[4], r = none, t;
    struct sgx535_vertex_source *out = w->def[d->index];
    if (d->bit_size != 32 && d->bit_size != 1)
        return refuse(w, "bit size");
    if (x->op == nir_op_mov || x->op == nir_op_vec2 || x->op == nir_op_vec3 ||
        x->op == nir_op_vec4) {
        for (unsigned c = 0; c < n; c++)
            if (!source(w, x, x->op == nir_op_mov ? 0 : c, x->op == nir_op_mov ? c : 0, &out[c]))
                return false;
        w->defined[d->index] = true;
        return true;
    }
    /* A dot product reads vectors; everything else is scalar here. */
    if (x->op == nir_op_fdot2 || x->op == nir_op_fdot3 || x->op == nir_op_fdot4) {
        unsigned size = x->op == nir_op_fdot2 ? 2 : x->op == nir_op_fdot3 ? 3 : 4;
        if (n != 1)
            return refuse(w, "vector dot product");
        for (unsigned c = 0; c < size; c++) {
            if (!source(w, x, 0, c, &s[0]) || !source(w, x, 1, c, &s[1]) ||
                !emit(w, SGX535_VERTEX_MAD, s[0], s[1], c ? r : sa(w->p->zero), &r))
                return false;
        }
        out[0] = r;
        w->defined[d->index] = true;
        return true;
    }
    if (n != 1 || inputs > 3)
        return refuse(w, "vector operation");
    for (unsigned i = 0; i < inputs; i++)
        if (!source(w, x, i, 0, &s[i]))
            return false;
    switch (x->op) {
    case nir_op_fneg:
        r = negated(s[0]);
        break;
    case nir_op_fabs:
        r = s[0];
        r.negate = 0;
        r.absolute = 1;
        break;
    case nir_op_fadd:
        if (!emit(w, SGX535_VERTEX_MAD, s[0], sa(w->p->one), s[1], &r))
            return false;
        break;
    case nir_op_fmul:
        if (!mul(w, s[0], s[1], &r))
            return false;
        break;
    case nir_op_ffma:
        if (!emit(w, SGX535_VERTEX_MAD, s[0], s[1], s[2], &r))
            return false;
        break;
    case nir_op_fmin:
    case nir_op_fmax:
        if (!binary(w, x->op == nir_op_fmin ? SGX535_VERTEX_MIN : SGX535_VERTEX_MAX, s[0], s[1],
                    &r))
            return false;
        break;
    case nir_op_fsat:
        if (!binary(w, SGX535_VERTEX_MAX, s[0], sa(w->p->zero), &t) ||
            !binary(w, SGX535_VERTEX_MIN, t, sa(w->p->one), &r))
            return false;
        break;
    case nir_op_frcp:
    case nir_op_frsq:
    case nir_op_fexp2:
    case nir_op_flog2:
        if (!unary(w,
                   x->op == nir_op_frcp   ? SGX535_VERTEX_RCP
                   : x->op == nir_op_frsq ? SGX535_VERTEX_RSQ
                   : x->op == nir_op_fexp2 ? SGX535_VERTEX_EXP
                                           : SGX535_VERTEX_LOG,
                   s[0], &r))
            return false;
        break;
    case nir_op_fsqrt:
        if (!unary(w, SGX535_VERTEX_RSQ, s[0], &t) || !unary(w, SGX535_VERTEX_RCP, t, &r))
            return false;
        break;
    case nir_op_fdiv:
        if (!unary(w, SGX535_VERTEX_RCP, s[1], &t) || !mul(w, s[0], t, &r))
            return false;
        break;
    case nir_op_fpow:
        if (!unary(w, SGX535_VERTEX_LOG, s[0], &t) || !mul(w, t, s[1], &t) ||
            !unary(w, SGX535_VERTEX_EXP, t, &r))
            return false;
        break;
    case nir_op_flrp: /* s0 + s2 * (s1 - s0) */
        if (!emit(w, SGX535_VERTEX_MAD, s[1], sa(w->p->one), negated(s[0]), &t) ||
            !emit(w, SGX535_VERTEX_MAD, s[2], t, s[0], &r))
            return false;
        break;
    case nir_op_flt:
    case nir_op_slt:
    case nir_op_fge:
    case nir_op_sge:
        if (nir_src_bit_size(x->src[0].src) != 32 ||
            !step(w, s[0], s[1], false, x->op == nir_op_fge || x->op == nir_op_sge, &r))
            return false;
        break;
    case nir_op_fneu:
    case nir_op_sne:
    case nir_op_feq:
    case nir_op_seq:
        if (nir_src_bit_size(x->src[0].src) != 32 ||
            !step(w, s[0], s[1], true, x->op == nir_op_feq || x->op == nir_op_seq, &r))
            return false;
        break;
    case nir_op_b2f32:
        r = s[0];
        break;
    case nir_op_inot:
        if (d->bit_size != 1 || !emit(w, SGX535_VERTEX_MAD, s[0], negated(sa(w->p->one)),
                                      sa(w->p->one), &r))
            return false;
        break;
    case nir_op_iand:
    case nir_op_ior:
        if (d->bit_size != 1 ||
            !binary(w, x->op == nir_op_iand ? SGX535_VERTEX_MIN : SGX535_VERTEX_MAX, s[0], s[1], &r))
            return false;
        break;
    case nir_op_bcsel:
        if (nir_src_bit_size(x->src[0].src) != 1 || d->bit_size != 32 ||
            !blend(w, s[0], s[1], s[2], &r))
            return false;
        break;
    case nir_op_fcsel: /* s0 != 0.0 ? s1 : s2 */
        if (!step(w, s[0], sa(w->p->zero), true, false, &t) || !blend(w, t, s[1], s[2], &r))
            return false;
        break;
    default:
        return refuse(w, nir_op_infos[x->op].name);
    }
    out[0] = r;
    w->defined[d->index] = true;
    return true;
}

static bool constant_offset(const nir_src *src, uint32_t *value)
{
    if (!nir_src_is_const(*src) || src->ssa->num_components != 1)
        return false;
    *value = (uint32_t)nir_src_as_uint(*src);
    return true;
}

static bool intrinsic(struct work *w, const nir_intrinsic_instr *x)
{
    struct sgx535_vertex_nir_program *p = w->p;
    uint32_t base, offset, component, n;
    switch (x->intrinsic) {
    case nir_intrinsic_load_input:
        base = (uint32_t)nir_intrinsic_base(x);
        component = (uint32_t)nir_intrinsic_component(x);
        n = x->def.num_components;
        if (x->def.bit_size != 32 || !constant_offset(&x->src[0], &offset) || offset ||
            base >= SGX535_VERTEX_INPUTS || component + n > 4)
            return refuse(w, "input");
        for (uint32_t c = 0; c < n; c++)
            w->def[x->def.index][c] = (struct sgx535_vertex_source){
                SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4u * base + component + c)};
        p->input_components[base] |= ((1u << n) - 1u) << component;
        if (base + 1u > p->input_count)
            p->input_count = base + 1u;
        break;
    case nir_intrinsic_load_ubo:
    case nir_intrinsic_load_uniform:
        /* CB0 by byte offset (the screen lowers uniforms to UBO 0), or a
         * dword-unit uniform. */
        n = x->def.num_components;
        if (x->def.bit_size != 32)
            return refuse(w, "uniform size");
        if (x->intrinsic == nir_intrinsic_load_ubo) {
            if (!constant_offset(&x->src[0], &base) || base ||
                !constant_offset(&x->src[1], &offset) || (offset & 3u))
                return refuse(w, "indirect uniform");
            offset /= 4u;
        } else {
            if (!constant_offset(&x->src[0], &offset))
                return refuse(w, "indirect uniform");
            offset += (uint32_t)nir_intrinsic_base(x);
        }
        for (uint32_t c = 0; c < n; c++)
            if (!constant(w, SGX535_VERTEX_WORD, offset + c, &w->def[x->def.index][c]))
                return false;
        break;
    case nir_intrinsic_store_output: {
        nir_io_semantics io = nir_intrinsic_io_semantics(x);
        uint32_t mask = nir_intrinsic_write_mask(x), written = 0, k = 0;
        struct sgx535_vertex_source *slot;
        const nir_def *v = x->src[0].ssa;
        component = (uint32_t)nir_intrinsic_component(x);
        if (!constant_offset(&x->src[1], &offset) || offset || v->bit_size != 32 ||
            !w->defined[v->index])
            return refuse(w, "output");
        if (io.location == VARYING_SLOT_POS)
            slot = p->position;
        else {
            /* another output: its entry by location, the first store's place */
            while (k < p->output_count && p->output[k].location != io.location)
                k++;
            if (k == SGX535_VERTEX_NIR_OUTPUTS)
                return refuse(w, "more than 16 outputs");
            if (k == p->output_count++)
                p->output[k] = (struct sgx535_vertex_nir_output){
                    (uint16_t)io.location, 0, io.location == VARYING_SLOT_COL0, {{0}}};
            slot = p->output[k].source;
        }
        for (uint32_t c = 0; c < 4; c++)
            if (mask >> c & 1u) {
                if (component + c >= 4)
                    return refuse(w, "output component");
                slot[component + c] = w->def[v->index][c];
                written |= 1u << (component + c);
            }
        if (io.location == VARYING_SLOT_POS)
            w->position_written |= written;
        else
            p->output[k].written |= (uint8_t)written;
        return true;
    }
    default:
        return refuse(w, nir_intrinsic_infos[x->intrinsic].name);
    }
    w->defined[x->def.index] = true;
    return true;
}

static unsigned type_size(const struct glsl_type *type, bool bindless)
{
    (void)bindless;
    return glsl_count_attribute_slots(type, false);
}

static bool read_block(struct work *w, nir_function_impl *impl)
{
    nir_block *block;
    if (!exec_list_is_singular(&impl->body))
        return refuse(w, "control flow");
    block = nir_start_block(impl);
    nir_foreach_instr(instr, block) {
        switch (instr->type) {
        case nir_instr_type_alu:
            if (!alu(w, nir_instr_as_alu(instr)))
                return false;
            break;
        case nir_instr_type_intrinsic:
            if (!intrinsic(w, nir_instr_as_intrinsic(instr)))
                return false;
            break;
        case nir_instr_type_load_const: {
            nir_load_const_instr *k = nir_instr_as_load_const(instr);
            if (k->def.bit_size != 32 && k->def.bit_size != 1)
                return refuse(w, "constant size");
            /* A boolean is 0.0 or 1.0, as comparisons give it. */
            for (unsigned c = 0; c < k->def.num_components; c++)
                if (!constant(w, SGX535_VERTEX_LITERAL,
                              k->def.bit_size == 1 ? (k->value[c].b ? 0x3f800000u : 0)
                                                   : k->value[c].u32,
                              &w->def[k->def.index][c]))
                    return false;
            w->defined[k->def.index] = true;
            break;
        }
        case nir_instr_type_undef: {
            nir_undef_instr *u = nir_instr_as_undef(instr);
            for (unsigned c = 0; c < u->def.num_components; c++)
                w->def[u->def.index][c] = sa(w->p->zero);
            w->defined[u->def.index] = true;
            break;
        }
        default:
            return refuse(w, "instruction type");
        }
    }
    return true;
}

enum sgx535_vertex_status sgx535_nir_vertex_ir(const struct nir_shader *source,
                                               struct sgx535_vertex_nir_program *out)
{
    struct work w = {out, NULL, NULL, 0};
    nir_function_impl *impl;
    nir_shader *s;
    bool progress, ok;
    if (!source || !out || source->info.stage != MESA_SHADER_VERTEX)
        return SGX535_VERTEX_INVALID;
    memset(out, 0, sizeof(*out));
    s = nir_shader_clone(NULL, source);
    if (!s)
        return SGX535_VERTEX_INVALID;
    NIR_PASS(_, s, nir_lower_global_vars_to_local);
    NIR_PASS(_, s, nir_lower_vars_to_ssa);
    NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size,
             (nir_lower_io_options)0);
    NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);
    do {
        progress = false;
        NIR_PASS(progress, s, nir_opt_copy_prop);
        NIR_PASS(progress, s, nir_opt_dce);
        NIR_PASS(progress, s, nir_opt_cse);
        NIR_PASS(progress, s, nir_opt_algebraic);
        NIR_PASS(progress, s, nir_opt_constant_folding);
        NIR_PASS(progress, s, nir_opt_dead_cf);
        NIR_PASS(progress, s, nir_opt_remove_phis);
        NIR_PASS(progress, s, nir_opt_undef);
        /* No branches in the program: a loop with a constant bound unrolled
         * (up to the stage's max_unroll_iterations), every if whose sides are
         * straight code made selects. A loop with another bound stays: refused
         * as control flow. */
        NIR_PASS(progress, s, nir_opt_loop_unroll);
        NIR_PASS(progress, s, nir_opt_peephole_select,
                 &(nir_opt_peephole_select_options){
                     .limit = ~0u, .indirect_load_ok = true, .expensive_alu_ok = true});
    } while (progress);
    impl = nir_shader_get_entrypoint(s);
    nir_index_ssa_defs(impl);
    w.def = calloc(impl->ssa_alloc + 1u, sizeof(*w.def));
    w.defined = calloc(impl->ssa_alloc + 1u, sizeof(*w.defined));
    out->constant[0] = (struct sgx535_vertex_constant){SGX535_VERTEX_RESERVED, 0};
    out->constant_count = 1;
    {
        struct sgx535_vertex_source scratch;
        ok = w.def && w.defined && constant(&w, SGX535_VERTEX_LITERAL, 0, &scratch) &&
             constant(&w, SGX535_VERTEX_LITERAL, 0x3f800000u, &scratch);
    }
    out->zero = 1;
    out->one = 2;
    ok = ok && read_block(&w, impl);
    free(w.def);
    free(w.defined);
    ralloc_free(s);
    if (ok && w.position_written != 15u)
        ok = refuse(&w, "position not written");
    out->has_position = w.position_written == 15u;
    return ok ? SGX535_VERTEX_OK : SGX535_VERTEX_UNSUPPORTED;
}
