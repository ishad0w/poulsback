// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Fragment NIR -> backend IR (sgx535_fs_nir.h). */
#include "sgx535_fs_nir.h"
#include "nir.h"
#include "nir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_VALUE 0xffffu

/* NIR's options for the private clone (docs/compiler.md#fragment-backend): everything SGX535 has no
 * instruction for is lowered by NIR, except sin and cos (lower_sincos misses
 * the 2^-10 accuracy target) which lower_sincos() below lowers. */
static const nir_shader_compiler_options options = {
    .lower_fdiv = true,
    .lower_flrp32 = true,
    .lower_fpow = true,
    .lower_fsat = true,
    .lower_fsqrt = true,
    .lower_fmod = true,
    .lower_fsign = true,
    .lower_ffloor = true,
    .lower_fceil = true,
    .lower_ftrunc = true,
    .lower_fround_even = true,
    .lower_fdph = true,
    .lower_bitops = true,
    .float_mul_add32 = (nir_float_muladd_support)(nir_float_muladd_support_has_ffma |
                                                  nir_float_muladd_support_fuse),
    .max_unroll_iterations = 16,
    .io_options = nir_io_has_intrinsics,
};

/* An IR value with a modifier: what a NIR def's component stands for. */
struct ref {
    uint16_t v;
    uint8_t mod, set;
};

struct adapter {
    struct sgx535_fs_nir *out;
    enum sgx535_fs_uniforms uniforms;
    struct ref *def; /* [index * 4 + component] */
    unsigned defs;
    uint16_t phi_var[1024]; /* (phi def index, component) -> VAR, by search */
    unsigned phi_def[1024], phis;
    uint16_t location_slot[VARYING_SLOT_MAX];
    bool failed;
};

static bool refuse(struct adapter *a, const char *what)
{
    if (!a->failed)
        snprintf(a->out->reason, sizeof(a->out->reason), "%s", what);
    a->failed = true;
    return false;
}

static uint16_t emit(struct adapter *a, struct sgx535_fs_instruction in)
{
    if (a->out->count >= SGX535_FS_MAX_INSTRUCTIONS) {
        refuse(a, "instructions");
        return 0;
    }
    a->out->ir[a->out->count] = in;
    return (uint16_t)a->out->count++;
}

static struct ref *slot(struct adapter *a, const nir_def *d, unsigned c)
{
    return &a->def[d->index * 4u + c];
}
static void set(struct adapter *a, const nir_def *d, unsigned c, uint16_t v, uint8_t mod)
{
    *slot(a, d, c) = (struct ref){v, mod, 1};
}
/* Emit IN and let component C of D stand for it. */
static bool bind(struct adapter *a, const nir_def *d, unsigned c, struct sgx535_fs_instruction in)
{
    set(a, d, c, emit(a, in), 0);
    return !a->failed;
}
static struct ref get(struct adapter *a, const nir_def *d, unsigned c)
{
    struct ref r = *slot(a, d, c);
    if (!r.set)
        refuse(a, "value not translated");
    return r;
}

static uint16_t constant(struct adapter *a, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_CONST, .imm = bits});
}

/* ---- sin and cos: half-angle polynomials ------------------------------------------- */

/* x -> half angle h in [-pi/2, pi/2): turns t = x / 2pi + 1/2, h = pi frac(t) - pi/2.
 * sin x = 2 sin h cos h, cos x = cos^2 h - sin^2 h, with sin h and cos h by
 * their Taylor terms to h^9 and h^8 (docs/compiler.md#fragment-backend). */
/* sum of k[i] x^i, by Horner's rule. */
static nir_def *poly(nir_builder *b, nir_def *x, const float *k, unsigned n)
{
    nir_def *r = nir_imm_float(b, k[n - 1]);
    while (n-- > 1)
        r = nir_ffma(b, x, r, nir_imm_float(b, k[n - 1]));
    return r;
}

static bool lower_sincos(nir_builder *b, nir_alu_instr *alu, void *data)
{
    /* sin h = h (1 - h²/3! + h⁴/5! - h⁶/7! + h⁸/9!), cos h = 1 - h²/2! + ... + h⁸/8! */
    static const float sin_k[] = {1.0f, -1.0f / 6.0f, 1.0f / 120.0f, -1.0f / 5040.0f,
                                  1.0f / 362880.0f};
    static const float cos_k[] = {1.0f, -0.5f, 1.0f / 24.0f, -1.0f / 720.0f, 1.0f / 40320.0f};
    nir_def *x, *t, *h, *h2, *s, *c;
    (void)data;
    if (alu->op != nir_op_fsin && alu->op != nir_op_fcos)
        return false;
    b->cursor = nir_before_instr(&alu->instr);
    x = nir_ssa_for_alu_src(b, alu, 0);
    t = nir_ffract(b, nir_ffma(b, x, nir_imm_float(b, 0.15915494f), nir_imm_float(b, 0.5f)));
    h = nir_ffma(b, t, nir_imm_float(b, 3.14159265f), nir_imm_float(b, -1.57079633f));
    h2 = nir_fmul(b, h, h);
    s = nir_fmul(b, h, poly(b, h2, sin_k, 5));
    c = poly(b, h2, cos_k, 5);
    nir_def_replace(&alu->def, alu->op == nir_op_fsin
                                   ? nir_fmul(b, nir_fmul_imm(b, s, 2.0), c)
                                   : nir_ffma(b, c, c, nir_fneg(b, nir_fmul(b, s, s))));
    return true;
}

static void optimize(nir_shader *s)
{
    static bool (*const passes[])(nir_shader *) = {
        nir_lower_vars_to_ssa,    nir_opt_copy_prop,  nir_opt_dce,
        nir_opt_dead_cf,          nir_opt_cse,        nir_opt_algebraic,
        nir_opt_constant_folding, nir_opt_loop_unroll};
    const nir_opt_peephole_select_options flatten = {
        .limit = 1000, .indirect_load_ok = true, .expensive_alu_ok = true, .discard_ok = true};
    bool progress;
    unsigned rounds = 0;
    do {
        progress = false;
        for (unsigned k = 0; k < sizeof(passes) / sizeof(passes[0]); k++)
            NIR_PASS(progress, s, passes[k]);
        NIR_PASS(progress, s, nir_opt_if, 0);
        NIR_PASS(progress, s, nir_opt_peephole_select, &flatten);
        NIR_PASS(progress, s, nir_lower_alu_to_scalar, NULL, NULL);
        NIR_PASS(progress, s, nir_lower_phis_to_scalar, NULL, NULL);
    } while (progress && ++rounds < 64);
}

/* Constant uniform offsets into the base, before nir_lower_int_to_float turns
 * the offsets' integer constants into floats. */
static bool fold_uniform_offsets(nir_shader *s)
{
    bool progress = false;
    nir_foreach_function_impl(impl, s)
    {
        nir_builder b = nir_builder_create(impl);
        nir_foreach_block(block, impl) nir_foreach_instr(instr, block)
        {
            nir_intrinsic_instr *in;
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(in->src[0]) ||
                !nir_src_as_uint(in->src[0]))
                continue;
            nir_intrinsic_set_base(in, nir_intrinsic_base(in) + (int)nir_src_as_uint(in->src[0]));
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&in->src[0], nir_imm_int(&b, 0));
            progress = true;
        }
        nir_progress(progress, impl, nir_metadata_control_flow);
    }
    return progress;
}

/* Sign-bit idioms of NIR's own lowerings (asin, atan: copysign) as float ops:
 * ior(iand(x, 0x80000000), iand(y, 0x7fffffff)) is y's magnitude with x's
 * sign, iand(x, 0x7fffffff) is |x|. */
/* An iand with a constant mask: the mask, and the other source. */
static uint32_t mask_of(nir_alu_instr *m, unsigned *other)
{
    if (!m || m->op != nir_op_iand || m->def.bit_size != 32)
        return 0;
    for (unsigned j = 0; j < 2; j++)
        if (nir_src_is_const(m->src[j].src)) {
            *other = 1u - j;
            return (uint32_t)nir_src_comp_as_uint(m->src[j].src, m->src[j].swizzle[0]);
        }
    return 0;
}
static bool lower_sign_bits(nir_builder *b, nir_alu_instr *alu, void *data)
{
    nir_def *r;
    unsigned o0 = 0, o1 = 0;
    (void)data;
    if (alu->def.bit_size != 32 || alu->def.num_components != 1)
        return false;
    b->cursor = nir_before_instr(&alu->instr);
    if (alu->op == nir_op_ior) {
        nir_alu_instr *a0 = nir_src_as_alu(alu->src[0].src), *a1 = nir_src_as_alu(alu->src[1].src);
        uint32_t m0 = mask_of(a0, &o0), m1 = mask_of(a1, &o1);
        nir_alu_instr *sign, *magnitude;
        unsigned os, om;
        nir_def *x, *y;
        if (m0 == 0x80000000u)
            sign = a0, os = o0, magnitude = a1, om = o1;
        else if (m1 == 0x80000000u)
            sign = a1, os = o1, magnitude = a0, om = o0;
        else
            return false;
        /* The magnitude: iand(y, 0x7fffffff), or fabs(y) as opt_algebraic left it. */
        if (magnitude && magnitude->op == nir_op_fabs)
            om = 0;
        else if (mask_of(magnitude, &om) != 0x7fffffffu)
            return false;
        x = nir_mov_alu(b, sign->src[os], 1), y = nir_mov_alu(b, magnitude->src[om], 1);
        r = nir_bcsel(b, nir_flt(b, x, nir_imm_float(b, 0.0f)), nir_fneg(b, nir_fabs(b, y)),
                      nir_fabs(b, y));
    } else if (mask_of(alu, &o0) == 0x7fffffffu) {
        r = nir_fabs(b, nir_mov_alu(b, alu->src[o0], 1));
    } else
        return false;
    nir_def_replace(&alu->def, r);
    return true;
}

/* nir_lower_int_to_float handles these integer-valued ops; any other one
 * (shifts, bit operations, remainders) is refused by name before it runs. */
static bool integer_ops_supported(nir_shader *s, char *name, size_t size)
{
    static const nir_op handled[] = {
        nir_op_mov,   nir_op_vec2,  nir_op_vec3,  nir_op_vec4,       nir_op_bcsel,
        nir_op_b2i32, nir_op_f2i32, nir_op_f2u32, nir_op_iadd,       nir_op_isub,
        nir_op_imul,  nir_op_idiv,  nir_op_iabs,  nir_op_ineg,       nir_op_imax,
        nir_op_imin,  nir_op_umax,  nir_op_umin,  nir_op_i32csel_gt, nir_op_i32csel_ge};
    nir_foreach_function_impl(impl, s) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block)
    {
        nir_alu_instr *alu;
        nir_alu_type type;
        bool ok = false;
        if (instr->type != nir_instr_type_alu)
            continue;
        alu = nir_instr_as_alu(instr);
        type = nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type);
        if ((type != nir_type_int && type != nir_type_uint) || alu->def.bit_size == 1)
            continue;
        for (unsigned k = 0; k < sizeof(handled) / sizeof(handled[0]); k++)
            ok |= handled[k] == alu->op;
        if (!ok) {
            snprintf(name, size, "integer %s", nir_op_infos[alu->op].name);
            return false;
        }
    }
    return true;
}

static nir_shader *lowered(const nir_shader *source, struct adapter *a)
{
    nir_shader *s = nir_shader_clone(NULL, source);
    nir_lower_tex_options tex = {.lower_txp = ~0u, .lower_rect = true};
    if (!s)
        return NULL;
    s->options = &options;
    NIR_PASS(_, s, nir_lower_vars_to_ssa);
    NIR_PASS(_, s, nir_lower_continue_constructs);
    NIR_PASS(_, s, nir_lower_tex, &tex);
    NIR_PASS(_, s, nir_shader_alu_pass, lower_sincos, nir_metadata_control_flow, NULL);
    NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);
    NIR_PASS(_, s, nir_lower_load_const_to_scalar);
    optimize(s);
    NIR_PASS(_, s, fold_uniform_offsets);
    NIR_PASS(_, s, nir_shader_alu_pass, lower_sign_bits, nir_metadata_control_flow, NULL);
    NIR_PASS(_, s, nir_opt_dce);
    NIR_PASS(_, s, nir_lower_flrp, 32, false);
    if (!integer_ops_supported(s, a->out->reason, sizeof(a->out->reason))) {
        a->failed = true;
        return s;
    }
    NIR_PASS(_, s, nir_lower_int_to_float);
    NIR_PASS(_, s, nir_lower_bool_to_float, true);
    optimize(s);
    NIR_PASS(_, s, nir_lower_load_const_to_scalar);
    NIR_PASS(_, s, nir_opt_dce);
    nir_divergence_analysis(s);
    nir_foreach_function_impl(impl, s) nir_index_ssa_defs(impl);
    return s;
}

/* ---- inputs ------------------------------------------------------------------------- */

static bool is_input(const nir_intrinsic_instr *in)
{
    return in->intrinsic == nir_intrinsic_load_interpolated_input ||
           in->intrinsic == nir_intrinsic_load_input;
}

/* TC slots in location order; components and interpolation per slot. */
static bool varyings(struct adapter *a, nir_function_impl *impl)
{
    uint8_t used[VARYING_SLOT_MAX] = {0}, flat[VARYING_SLOT_MAX] = {0},
            smooth[VARYING_SLOT_MAX] = {0};
    unsigned slots = 0;
    nir_foreach_block(block, impl) nir_foreach_instr(instr, block)
    {
        nir_intrinsic_instr *in;
        unsigned location, end;
        if (instr->type != nir_instr_type_intrinsic)
            continue;
        in = nir_instr_as_intrinsic(instr);
        if (!is_input(in))
            continue;
        location = nir_intrinsic_io_semantics(in).location;
        if (location >= VARYING_SLOT_MAX || nir_intrinsic_io_semantics(in).num_slots != 1 ||
            !nir_src_is_const(in->src[in->intrinsic == nir_intrinsic_load_interpolated_input]) ||
            nir_src_as_uint(in->src[in->intrinsic == nir_intrinsic_load_interpolated_input]) ||
            in->def.bit_size != 32)
            return refuse(a, "input form");
        if (location == VARYING_SLOT_POS || location == VARYING_SLOT_FACE)
            continue;
        if (location == VARYING_SLOT_PNTC)
            return refuse(a, "gl_PointCoord");
        end = nir_intrinsic_component(in) + in->def.num_components;
        if (end > used[location])
            used[location] = (uint8_t)end;
        if (in->intrinsic == nir_intrinsic_load_input)
            flat[location] = 1;
        else {
            nir_intrinsic_instr *bary = nir_src_as_intrinsic(in->src[0]);
            if (!bary || (bary->intrinsic != nir_intrinsic_load_barycentric_pixel &&
                          bary->intrinsic != nir_intrinsic_load_barycentric_centroid))
                return refuse(a, "interpolation qualifier");
            if (nir_intrinsic_interp_mode(bary) == INTERP_MODE_NOPERSPECTIVE)
                return refuse(a, "noperspective");
            if (nir_intrinsic_interp_mode(bary) == INTERP_MODE_FLAT)
                flat[location] = 1;
            else
                smooth[location] = 1;
        }
    }
    for (unsigned l = 0; l < VARYING_SLOT_MAX; l++) {
        a->location_slot[l] = NO_VALUE;
        if (!used[l])
            continue;
        if (flat[l] && smooth[l])
            return refuse(a, "flat and smooth reads of one varying");
        if (slots == SGX535_FS_MAX_VARYINGS)
            return refuse(a, "more than 10 varyings");
        a->location_slot[l] = (uint16_t)slots;
        a->out->varying_location[slots] = (uint16_t)l;
        a->out->key.varying[slots] =
            (struct sgx535_fs_varying){flat[l] ? SGX535_FS_FLAT : SGX535_FS_SMOOTH, used[l], 0};
        slots++;
    }
    return true;
}

/* ---- instructions ---------------------------------------------------------------- */

static struct ref alu_src(struct adapter *a, nir_alu_instr *alu, unsigned i)
{
    return get(a, alu->src[i].src.ssa, alu->src[i].swizzle[0]);
}

static bool alu(struct adapter *a, nir_alu_instr *alu)
{
    /* The NIR op of each scalar IR op the adapter emits for an ALU. */
    static const nir_op from[] = {
        [SGX535_FS_ADD] = nir_op_fadd,   [SGX535_FS_MUL] = nir_op_fmul,
        [SGX535_FS_MAD] = nir_op_ffma,   [SGX535_FS_MIN] = nir_op_fmin,
        [SGX535_FS_MAX] = nir_op_fmax,   [SGX535_FS_RCP] = nir_op_frcp,
        [SGX535_FS_RSQ] = nir_op_frsq,   [SGX535_FS_LOG2] = nir_op_flog2,
        [SGX535_FS_EXP2] = nir_op_fexp2, [SGX535_FS_FRACT] = nir_op_ffract,
        [SGX535_FS_SLT] = nir_op_slt,    [SGX535_FS_SGE] = nir_op_sge,
        [SGX535_FS_SEQ] = nir_op_seq,    [SGX535_FS_SNE] = nir_op_sne,
        [SGX535_FS_SEL] = nir_op_fcsel};
    nir_def *d = &alu->def;
    struct ref r;
    if (d->bit_size != 32)
        return refuse(a, "not 32-bit");
    switch (alu->op) {
    case nir_op_mov:
    case nir_op_vec2:
    case nir_op_vec3:
    case nir_op_vec4:
        for (unsigned c = 0; c < d->num_components; c++) {
            r = alu->op == nir_op_mov ? get(a, alu->src[0].src.ssa, alu->src[0].swizzle[c])
                                      : alu_src(a, alu, c);
            set(a, d, c, r.v, r.mod);
        }
        return !a->failed;
    case nir_op_fneg:
        r = alu_src(a, alu, 0);
        set(a, d, 0, r.v, (uint8_t)(r.mod ^ SGX535_USE_NEG));
        return !a->failed;
    case nir_op_fabs:
        r = alu_src(a, alu, 0);
        set(a, d, 0, r.v, SGX535_USE_ABS);
        return !a->failed;
    default:
        break;
    }
    if (d->num_components != 1)
        return refuse(a, "vector ALU");
    for (uint8_t op = SGX535_FS_ADD; op <= SGX535_FS_SEL; op++) {
        struct sgx535_fs_instruction in = {.op = op};
        if (!from[op] || from[op] != alu->op)
            continue;
        for (unsigned s = 0; s < nir_op_infos[alu->op].num_inputs; s++) {
            r = alu_src(a, alu, s);
            in.src[s] = r.v, in.mod[s] = r.mod;
        }
        return !a->failed && bind(a, d, 0, in);
    }
    return refuse(a, nir_op_infos[alu->op].name);
}

static bool tex(struct adapter *a, nir_tex_instr *t)
{
    struct sgx535_fs_instruction in = {.op = SGX535_FS_TEX};
    int coord = nir_tex_instr_src_index(t, nir_tex_src_coord);
    int lod =
        nir_tex_instr_src_index(t, t->op == nir_texop_txb ? nir_tex_src_bias : nir_tex_src_lod);
    uint16_t v;
    if (t->op == nir_texop_txs) {
        /* The size of level 0, as nir_lower_tex's rectangle scaling asks. */
        int l = nir_tex_instr_src_index(t, nir_tex_src_lod);
        if (l < 0 || !nir_src_is_const(t->src[l].src) || nir_src_as_uint(t->src[l].src) ||
            t->is_array || t->def.num_components > 3 || t->sampler_index >= SGX535_FS_MAX_SAMPLERS)
            return refuse(a, "texture size query");
        for (unsigned c = 0; c < t->def.num_components; c++)
            bind(a, &t->def, c,
                 (struct sgx535_fs_instruction){.op = SGX535_FS_TEXSIZE,
                                                .index = (uint16_t)t->texture_index,
                                                .comp = (uint8_t)c});
        return !a->failed;
    }
    if (t->op != nir_texop_tex && t->op != nir_texop_txb && t->op != nir_texop_txl)
        return refuse(a, "texture op");
    if (t->is_array || coord < 0 || t->def.bit_size != 32 ||
        t->sampler_index >= SGX535_FS_MAX_SAMPLERS)
        return refuse(a, "texture form");
    for (unsigned s = 0; s < t->num_srcs; s++) {
        nir_tex_src_type type = t->src[s].src_type;
        if (type != nir_tex_src_coord && type != nir_tex_src_bias && type != nir_tex_src_lod &&
            type != nir_tex_src_comparator && type != nir_tex_src_texture_deref &&
            type != nir_tex_src_sampler_deref)
            return refuse(a, "texture source");
    }
    switch (t->sampler_dim) {
    case GLSL_SAMPLER_DIM_1D:
        in.dim = 1;
        break;
    case GLSL_SAMPLER_DIM_2D:
        in.dim = 2;
        break;
    case GLSL_SAMPLER_DIM_3D:
    case GLSL_SAMPLER_DIM_CUBE:
        in.dim = 3;
        break;
    default:
        return refuse(a, "sampler dimension");
    }
    for (unsigned c = 0; c < in.dim; c++) {
        struct ref r = get(a, t->src[coord].src.ssa, c);
        in.src[c] = r.v, in.mod[c] = r.mod;
    }
    if (t->op != nir_texop_tex) {
        struct ref r;
        if (lod < 0)
            return refuse(a, "texture LOD");
        r = get(a, t->src[lod].src.ssa, 0);
        in.src[in.dim] = r.v, in.mod[in.dim] = r.mod;
        in.comp = t->op == nir_texop_txb ? SGX535_FS_TEX_BIAS : SGX535_FS_TEX_LOD;
    }
    if (t->is_shadow) {
        /* the reference after the coordinates and LOD; the variant's key
         * holds the function (sgx535_fs_sampler.compare) */
        int ref = nir_tex_instr_src_index(t, nir_tex_src_comparator);
        unsigned k = in.dim + (in.comp != SGX535_FS_TEX_IMPLICIT ? 1u : 0u);
        struct ref r;
        if (ref < 0 || k > 3)
            return refuse(a, "shadow texture form");
        r = get(a, t->src[ref].src.ssa, 0);
        in.src[k] = r.v, in.mod[k] = r.mod, in.imm = 1;
    }
    in.index = (uint16_t)t->sampler_index;
    a->out->sampler_mask |= 1u << t->sampler_index;
    if (a->failed)
        return false;
    v = emit(a, in);
    for (unsigned c = 0; c < t->def.num_components && c < 4; c++)
        bind(a, &t->def, c,
             (struct sgx535_fs_instruction){.op = SGX535_FS_TEXC, .comp = (uint8_t)c, .src = {v}});
    return !a->failed;
}

static bool intrinsic(struct adapter *a, nir_intrinsic_instr *in)
{
    nir_def *d = &in->def;
    switch (in->intrinsic) {
    case nir_intrinsic_load_barycentric_pixel:
    case nir_intrinsic_load_barycentric_centroid:
        return true;
    case nir_intrinsic_load_interpolated_input:
    case nir_intrinsic_load_input: {
        unsigned location = nir_intrinsic_io_semantics(in).location,
                 first = nir_intrinsic_component(in);
        for (unsigned c = 0; c < d->num_components; c++) {
            uint16_t v;
            if (location == VARYING_SLOT_POS)
                v = emit(a, (struct sgx535_fs_instruction){
                                .op = SGX535_FS_SYSTEM,
                                .index = (uint16_t)(SGX535_FS_FRAG_X + first + c)});
            else if (location == VARYING_SLOT_FACE &&
                     nir_alu_type_get_base_type(nir_intrinsic_dest_type(in)) == nir_type_bool)
                v = emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_SYSTEM,
                                                           .index = SGX535_FS_FRONT_FACING});
            else if (location == VARYING_SLOT_FACE) {
                /* A float FACE input: 1.0 front, -1.0 back. */
                uint16_t f = emit(a, (struct sgx535_fs_instruction){
                                         .op = SGX535_FS_SYSTEM, .index = SGX535_FS_FRONT_FACING});
                uint16_t two = constant(a, 2.0f), one = constant(a, 1.0f);
                v = emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_MAD,
                                                           .src = {f, two, one},
                                                           .mod = {0, 0, SGX535_USE_NEG}});
            } else
                v = emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_INPUT,
                                                           .index = a->location_slot[location],
                                                           .comp = (uint8_t)(first + c)});
            set(a, d, c, v, 0);
        }
        return !a->failed;
    }
    case nir_intrinsic_load_frag_coord:
        for (unsigned c = 0; c < d->num_components; c++)
            bind(a, d, c,
                 (struct sgx535_fs_instruction){.op = SGX535_FS_SYSTEM,
                                                .index = (uint16_t)(SGX535_FS_FRAG_X + c)});
        return !a->failed;
    case nir_intrinsic_load_front_face:
        bind(a, d, 0,
             (struct sgx535_fs_instruction){.op = SGX535_FS_SYSTEM,
                                            .index = SGX535_FS_FRONT_FACING});
        return !a->failed;
    case nir_intrinsic_load_uniform: {
        unsigned first;
        if (!nir_src_is_const(in->src[0]) || d->bit_size != 32)
            return refuse(a, "indexed uniform (ldad, later)");
        first = nir_intrinsic_base(in) + nir_src_as_uint(in->src[0]);
        if (a->uniforms == SGX535_FS_UNIFORMS_VEC4)
            first *= 4u;
        /* Integer and boolean uniforms hold int bits; the upload converts them
         * to F32, as nir_lower_int_to_float made every use a float, and a bool
         * to 1.0 or 0.0, as nir_lower_bool_to_float (Mesa's true is ~0). */
        nir_alu_type type = nir_alu_type_get_base_type(nir_intrinsic_dest_type(in));
        uint8_t is_int = type == nir_type_bool                           ? 2
                         : type == nir_type_int || type == nir_type_uint ? 1
                                                                         : 0;
        for (unsigned c = 0; c < d->num_components; c++) {
            if (first + c > 255)
                return refuse(a, "uniform dword beyond 255");
            bind(a, d, c,
                 (struct sgx535_fs_instruction){
                     .op = SGX535_FS_UNIFORM, .comp = is_int, .index = (uint16_t)(first + c)});
        }
        return !a->failed;
    }
    case nir_intrinsic_store_output: {
        nir_io_semantics io = nir_intrinsic_io_semantics(in);
        unsigned mask = nir_intrinsic_write_mask(in), first = nir_intrinsic_component(in);
        nir_def *value = in->src[0].ssa;
        if (!nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]) ||
            io.dual_source_blend_index || value->bit_size != 32)
            return refuse(a, "output form");
        for (unsigned c = 0; c < value->num_components; c++) {
            struct ref r;
            uint16_t index;
            if (!(mask >> c & 1u))
                continue;
            if (io.location == FRAG_RESULT_DEPTH)
                index = SGX535_FS_DEPTH;
            else if (io.location == FRAG_RESULT_COLOR || io.location == FRAG_RESULT_DATA0)
                index = (uint16_t)(first + c);
            else
                return refuse(a, "output other than COLOR0 and depth");
            r = get(a, value, c);
            emit(a, (struct sgx535_fs_instruction){
                        .op = SGX535_FS_OUTPUT, .index = index, .src = {r.v}, .mod = {r.mod}});
        }
        return !a->failed;
    }
    case nir_intrinsic_ddx:
    case nir_intrinsic_ddx_fine:
    case nir_intrinsic_ddx_coarse:
    case nir_intrinsic_ddy:
    case nir_intrinsic_ddy_fine:
    case nir_intrinsic_ddy_coarse: {
        bool y = in->intrinsic == nir_intrinsic_ddy || in->intrinsic == nir_intrinsic_ddy_fine ||
                 in->intrinsic == nir_intrinsic_ddy_coarse;
        if (d->bit_size != 32)
            return refuse(a, "derivative size");
        for (unsigned c = 0; c < d->num_components; c++) {
            struct ref r = get(a, in->src[0].ssa, c);
            bind(a, d, c,
                 (struct sgx535_fs_instruction){
                     .op = y ? SGX535_FS_DDY : SGX535_FS_DDX, .src = {r.v}, .mod = {r.mod}});
        }
        return !a->failed;
    }
    case nir_intrinsic_terminate:
    case nir_intrinsic_demote:
        emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_KILL, .src = {constant(a, 1.0f)}});
        return !a->failed;
    case nir_intrinsic_terminate_if:
    case nir_intrinsic_demote_if: {
        struct ref r = get(a, in->src[0].ssa, 0);
        emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_KILL, .src = {r.v}, .mod = {r.mod}});
        return !a->failed;
    }
    default:
        return refuse(a, nir_intrinsic_infos[in->intrinsic].name);
    }
}

static uint16_t phi_var(struct adapter *a, const nir_def *d, unsigned c)
{
    unsigned key = d->index * 4u + c;
    for (unsigned k = 0; k < a->phis; k++)
        if (a->phi_def[k] == key)
            return a->phi_var[k];
    if (a->phis == SGX535_FS_MAX_VARS) {
        refuse(a, "phis");
        return 0;
    }
    a->phi_def[a->phis] = key;
    a->phi_var[a->phis] = (uint16_t)a->phis;
    return a->phi_var[a->phis++];
}

/* STOREs for the phis of the block's successors, from this block. */
static void phi_stores(struct adapter *a, nir_block *block)
{
    for (unsigned s = 0; s < 2; s++) {
        nir_block *succ = block->successors[s];
        if (!succ)
            continue;
        nir_foreach_phi(phi, succ)
        {
            nir_foreach_phi_src(src, phi)
            {
                if (src->pred != block)
                    continue;
                for (unsigned c = 0; c < phi->def.num_components; c++) {
                    struct ref r = get(a, src->src.ssa, c);
                    emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_STORE,
                                                           .index = phi_var(a, &phi->def, c),
                                                           .src = {r.v},
                                                           .mod = {r.mod}});
                }
            }
        }
    }
}

static bool block(struct adapter *a, nir_block *b)
{
    nir_jump_instr *jump = NULL;
    nir_foreach_phi(phi, b)
    {
        for (unsigned c = 0; c < phi->def.num_components; c++)
            bind(a, &phi->def, c,
                 (struct sgx535_fs_instruction){.op = SGX535_FS_LOAD,
                                                .index = phi_var(a, &phi->def, c)});
    }
    nir_foreach_instr(instr, b)
    {
        switch (instr->type) {
        case nir_instr_type_phi:
            break;
        case nir_instr_type_alu:
            alu(a, nir_instr_as_alu(instr));
            break;
        case nir_instr_type_load_const: {
            nir_load_const_instr *k = nir_instr_as_load_const(instr);
            /* Scalar after nir_lower_load_const_to_scalar. */
            if (k->def.bit_size != 32 || k->def.num_components != 1)
                return refuse(a, "constant size");
            bind(a, &k->def, 0,
                 (struct sgx535_fs_instruction){.op = SGX535_FS_CONST, .imm = k->value[0].u32});
            break;
        }
        case nir_instr_type_undef: {
            nir_undef_instr *u = nir_instr_as_undef(instr);
            for (unsigned c = 0; c < u->def.num_components; c++)
                set(a, &u->def, c, constant(a, 0.0f), 0);
            break;
        }
        case nir_instr_type_intrinsic:
            intrinsic(a, nir_instr_as_intrinsic(instr));
            break;
        case nir_instr_type_tex:
            tex(a, nir_instr_as_tex(instr));
            break;
        case nir_instr_type_jump:
            jump = nir_instr_as_jump(instr);
            break;
        default:
            return refuse(a, "instruction type");
        }
        if (a->failed)
            return false;
    }
    phi_stores(a, b);
    if (jump) {
        if (jump->type == nir_jump_break)
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_BREAK});
        else if (jump->type == nir_jump_continue)
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_CONTINUE});
        else
            return refuse(a, "return");
    }
    return !a->failed;
}

static bool cf_list(struct adapter *a, struct exec_list *list)
{
    foreach_list_typed(nir_cf_node, node, node, list)
    {
        switch (node->type) {
        case nir_cf_node_block:
            if (!block(a, nir_cf_node_as_block(node)))
                return false;
            break;
        case nir_cf_node_if: {
            nir_if *nif = nir_cf_node_as_if(node);
            struct ref cond;
            uint32_t mark;
            if (nir_src_is_divergent(&nif->condition))
                return refuse(a, "per-pixel branch (not yet qualified on the device)");
            cond = get(a, nif->condition.ssa, 0);
            emit(a, (struct sgx535_fs_instruction){
                        .op = SGX535_FS_IF, .src = {cond.v}, .mod = {cond.mod}});
            if (!cf_list(a, &nif->then_list))
                return false;
            mark = a->out->count;
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_ELSE});
            if (!cf_list(a, &nif->else_list))
                return false;
            if (a->out->count == mark + 1u)
                a->out->count = mark; /* an empty ELSE */
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_ENDIF});
            break;
        }
        case nir_cf_node_loop: {
            nir_loop *loop = nir_cf_node_as_loop(node);
            if (nir_loop_is_divergent(loop))
                return refuse(a, "loop with a per-pixel exit (not yet qualified on the device)");
            if (nir_loop_has_continue_construct(loop))
                return refuse(a, "continue construct");
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_LOOP});
            if (!cf_list(a, &loop->body))
                return false;
            emit(a, (struct sgx535_fs_instruction){.op = SGX535_FS_ENDLOOP});
            break;
        }
        default:
            return refuse(a, "control flow");
        }
        if (a->failed)
            return false;
    }
    return true;
}

enum sgx535_fs_status sgx535_fs_from_nir(const nir_shader *source, enum sgx535_fs_uniforms uniforms,
                                         struct sgx535_fs_nir *out)
{
    struct adapter a = {.out = out, .uniforms = uniforms};
    nir_shader *s;
    nir_function_impl *impl;
    if (!source || !out || source->info.stage != MESA_SHADER_FRAGMENT)
        return SGX535_FS_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    s = lowered(source, &a);
    if (!s)
        return SGX535_FS_ERR_LIMIT;
    if (a.failed) {
        ralloc_free(s);
        return SGX535_FS_ERR_UNSUPPORTED;
    }
    impl = nir_shader_get_entrypoint(s);
    a.defs = impl->ssa_alloc;
    a.def = calloc((size_t)a.defs * 4u + 4u, sizeof(*a.def));
    if (!a.def) {
        ralloc_free(s);
        return SGX535_FS_ERR_LIMIT;
    }
    if (varyings(&a, impl))
        cf_list(&a, &impl->body);
    free(a.def);
    ralloc_free(s);
    if (a.failed)
        return !strcmp(out->reason, "instructions") ? SGX535_FS_ERR_LIMIT
                                                    : SGX535_FS_ERR_UNSUPPORTED;
    return SGX535_FS_OK;
}
