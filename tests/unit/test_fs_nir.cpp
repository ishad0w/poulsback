// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The fragment backend against NIR: one GLSL pair is
 * compiled by Mesa's standalone frontend and lowered as st/mesa lowers I/O
 * (dword uniforms, I/O intrinsics, sampler indices). Then
 *  - the adapter and backend compile it (sgx535_fs_from_nir, sgx535_fs_compile);
 *  - a NIR interpreter (Mesa's own constant evaluator, four lanes of a quad,
 *    control flow by lane masks on an out-of-SSA clone) and the USSE model
 *    (tests/unit/sgx535_use_sim.c) run the original NIR and our words on
 *    the same random inputs, and COLOR0, depth and discard must agree (not
 *    in a lane where NIR computed a NaN: GLSL's undefined results).
 * usage: poulsback-fs-nir [--words] [--rounds N] [--seed S] VERT FRAG
 * Prints one SHADER line; --words prints every word with the model's text
 * for independent decoding. Exit 0: agreed or refused (named site);
 * 1: a difference or a word the model cannot decode. CPU only. */
#ifdef NDEBUG
#error "Actual NIR validation must remain enabled"
#endif
#include "compiler/glsl/standalone.h"
#include "compiler/glsl/gl_nir_linker.h"
#include "compiler/glsl/linker_util.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_constant_expressions.h"
#include "compiler/glsl/gl_nir.h"
#include "main/mtypes.h"
#include "main/shader_types.h"
#include "main/shaderobj.h"
extern "C" {
#include "sgx535_fs_nir.h"
#include "sgx535_use_sim.h"
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static unsigned rng = 1;
static unsigned rnd()
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 16 & 0x7fffu;
}
static float rndf(float lo, float hi)
{
    return lo + (hi - lo) * (float)rnd() / 32767.0f;
}
static uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
static float f32(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

/* ---- the texture both sides read ------------------------------------------------------- */
static uint32_t texel(void *, unsigned sampler, unsigned, const float c[3], unsigned dim)
{
    float u = c[0], v = dim > 1 ? c[1] : 0.0f, w = dim > 2 ? c[2] : 0.0f;
    float r = u * 3.7f + (float)sampler * 0.13f, g = v * 2.3f, b = (u + v + w) * 1.9f;
    unsigned R = (unsigned)lrintf(255.0f * (r - floorf(r))),
             G = (unsigned)lrintf(255.0f * (g - floorf(g)));
    unsigned B = (unsigned)lrintf(255.0f * (b - floorf(b))), A = 64u + sampler * 7u;
    return B | G << 8 | R << 16 | A << 24;
}
static const float texture_size[3] = {64.0f, 32.0f, 16.0f};

/* ---- st/mesa's I/O lowering ------------------------------------------------------------------ */
static unsigned io_slots(const glsl_type *type, bool)
{
    return glsl_count_attribute_slots(type, false);
}
static unsigned dwords(const glsl_type *type, bool)
{
    return glsl_count_dword_slots(type, false);
}

/* Sampler uniforms in declaration order are units 0, 1, ...; arrays take one per element. */
static bool lower_samplers(nir_shader *nir)
{
    std::vector<nir_variable *> vars;
    std::vector<unsigned> base;
    unsigned units = 0;
    nir_foreach_variable_with_modes(var, nir, nir_var_uniform)
    {
        if (!glsl_type_is_sampler(glsl_without_array(var->type)))
            continue;
        vars.push_back(var);
        base.push_back(units);
        units += glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;
    }
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block)
    {
        if (instr->type != nir_instr_type_tex)
            continue;
        nir_tex_instr *t = nir_instr_as_tex(instr);
        int k = nir_tex_instr_src_index(t, nir_tex_src_texture_deref);
        int s = nir_tex_instr_src_index(t, nir_tex_src_sampler_deref);
        if (k < 0)
            continue;
        nir_deref_instr *d = nir_src_as_deref(t->src[k].src);
        unsigned element = 0;
        if (d->deref_type == nir_deref_type_array) {
            if (!nir_src_is_const(d->arr.index))
                return false;
            element = (unsigned)nir_src_as_uint(d->arr.index);
        }
        nir_variable *var = nir_deref_instr_get_variable(d);
        for (size_t v = 0; v < vars.size(); v++)
            if (vars[v] == var)
                t->texture_index = t->sampler_index = base[v] + element;
        if (s >= 0)
            nir_tex_instr_remove_src(t, s);
        nir_tex_instr_remove_src(t, nir_tex_instr_src_index(t, nir_tex_src_texture_deref));
    }
    return true;
}

static bool lower_io(nir_shader *nir)
{
    unsigned uniforms = 0;
    nir_lower_global_vars_to_local(nir);
    nir_lower_vars_to_ssa(nir);
    /* st_link_glsl_to_nir for a screen without indirect temporary or constant
     * addressing (poulsback_screen.c's fragment caps): if-else trees of direct
     * accesses; then the dead indirect derefs go and the arrays become SSA */
    nir_lower_indirect_derefs_to_if_else_trees(
        nir, (nir_variable_mode)(nir_var_function_temp | nir_var_uniform), UINT32_MAX);
    nir_opt_dce(nir);
    nir_lower_vars_to_ssa(nir);
    nir_opt_dce(nir);
    nir_lower_io_array_vars_to_elements_no_indirects(nir, false);
    if (!lower_samplers(nir))
        return false;
    nir_foreach_variable_with_modes(var, nir, nir_var_uniform)
    {
        if (glsl_type_is_sampler(glsl_without_array(var->type)))
            continue;
        var->data.driver_location = uniforms;
        uniforms += glsl_count_dword_slots(var->type, false);
    }
    nir_foreach_variable_with_modes(var, nir, nir_var_shader_in | nir_var_shader_out)
        var->data.driver_location = (unsigned)var->data.location;
    nir_lower_io(nir, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), io_slots,
                 nir_lower_io_use_interpolated_input_intrinsics);
    nir_lower_io(nir, nir_var_uniform, dwords, (nir_lower_io_options)0);
    nir_opt_dce(nir);
    nir_remove_dead_variables(nir,
                              (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out |
                                                  nir_var_uniform | nir_var_function_temp |
                                                  nir_var_shader_temp),
                              nullptr);
    nir_opt_constant_folding(nir);
    nir_opt_dce(nir);
    nir_validate_shader(nir, "fs-nir fixture: st-like I/O");
    return true;
}

/* ---- inputs ----------------------------------------------------------------------------------- */
struct Inputs {
    float varying[4][VARYING_SLOT_MAX][4];
    uint32_t uniform[256];
    float window[4][4]; /* x, y (down), z, w */
    float height, ysign;
    unsigned back[4], sense;
};
static float frag(const Inputs &in, unsigned lane, unsigned c)
{
    return c == 1 ? in.ysign * in.window[lane][1] + in.height : in.window[lane][c];
}
static bool front(const Inputs &in, unsigned lane)
{
    return !((in.back[lane] ^ in.sense) & 1u);
}

/* ---- the NIR interpreter ----------------------------------------------------------------------- */
struct Lanes {
    nir_const_value c[4][NIR_MAX_VEC_COMPONENTS];
};
struct Result {
    float color[4][4];
    float depth[4];
    bool killed[4], wrote_depth;
};
struct Interp {
    nir_shader *s;
    const Inputs &in;
    std::vector<Lanes> v;
    Result out;
    unsigned steps = 0;
    bool error = false, runaway = false; /* runaway: the step limit (a loop that never ends) */
    /* A lane that computed a NaN: GLSL 1.20 leaves those inputs' results
     * undefined (pow of a negative base, log of a non-positive value, ...),
     * and the inputs here are never NaN. */
    bool undefined[4] = {false, false, false, false};
    struct Loop {
        unsigned broken, continued;
    };
    std::vector<Loop> loops;

    Interp(nir_shader *shader, const Inputs &inputs) : s(shader), in(inputs)
    {
        nir_function_impl *impl = nir_shader_get_entrypoint(s);
        v.resize(impl->ssa_alloc);
        std::memset(&out, 0, sizeof(out));
    }
    nir_const_value &at(nir_def *d, unsigned lane, unsigned c)
    {
        return v[d->index].c[lane][c];
    }
    float scalar(nir_src *src, unsigned lane, unsigned c)
    {
        return f32(at(src->ssa, lane, c).u32);
    }
    void alu(nir_alu_instr *alu, unsigned mask)
    {
        const nir_op_info *info = &nir_op_infos[alu->op];
        unsigned bit_size = 0;
        if (!nir_alu_type_get_type_size(info->output_type))
            bit_size = alu->def.bit_size;
        for (unsigned i = 0; i < info->num_inputs; i++)
            if (!bit_size && !nir_alu_type_get_type_size(info->input_types[i]))
                bit_size = alu->src[i].src.ssa->bit_size;
        if (!bit_size)
            bit_size = 32;
        for (unsigned lane = 0; lane < 4; lane++) {
            nir_const_value src[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS],
                *ptr[NIR_ALU_MAX_INPUTS];
            nir_const_value dst[NIR_MAX_VEC_COMPONENTS];
            std::memset(src, 0, sizeof(src));
            std::memset(dst, 0, sizeof(dst));
            for (unsigned i = 0; i < info->num_inputs; i++) {
                for (unsigned j = 0; j < nir_ssa_alu_instr_src_components(alu, i); j++)
                    src[i][j] = at(alu->src[i].src.ssa, lane, alu->src[i].swizzle[j]);
                ptr[i] = src[i];
            }
            nir_eval_const_opcode(alu->op, dst, NULL, alu->def.num_components, bit_size, ptr,
                                  s->info.float_controls_execution_mode);
            for (unsigned c = 0; c < alu->def.num_components; c++) {
                at(&alu->def, lane, c) = dst[c];
                if (nir_alu_type_get_base_type(info->output_type) == nir_type_float &&
                    bit_size == 32 && std::isnan(f32(dst[c].u32)))
                    undefined[lane] = true;
            }
        }
        (void)mask;
    }
    void tex(nir_tex_instr *t)
    {
        int coord = nir_tex_instr_src_index(t, nir_tex_src_coord);
        int proj = nir_tex_instr_src_index(t, nir_tex_src_projector);
        for (unsigned lane = 0; lane < 4; lane++) {
            if (t->op == nir_texop_txs) {
                for (unsigned c = 0; c < t->def.num_components; c++)
                    at(&t->def, lane, c).i32 = (int32_t)texture_size[c];
                continue;
            }
            float c[3] = {0, 0, 0};
            unsigned n = t->coord_components;
            for (unsigned k = 0; k < n && k < 3; k++)
                c[k] = scalar(&t->src[coord].src, lane, k);
            if (proj >= 0)
                for (unsigned k = 0; k < n && k < 3; k++)
                    c[k] /= scalar(&t->src[proj].src, lane, 0);
            if (t->sampler_dim == GLSL_SAMPLER_DIM_RECT)
                for (unsigned k = 0; k < n && k < 2; k++)
                    c[k] /= texture_size[k];
            uint32_t w = texel(nullptr, t->sampler_index, 0, c,
                               t->sampler_dim == GLSL_SAMPLER_DIM_1D ? 1u
                               : n > 2                               ? 3u
                                                                     : 2u);
            static const unsigned byte[4] = {2, 1, 0, 3};
            if (t->is_shadow) {
                /* GL's default: LEQUAL against R, LUMINANCE (r r r 1) */
                int ref = nir_tex_instr_src_index(t, nir_tex_src_comparator);
                float r = scalar(&t->src[ref].src, lane, 0), d = (float)(w >> 16 & 0xffu) / 255.0f;
                if (proj >= 0)
                    r /= scalar(&t->src[proj].src, lane, 0);
                for (unsigned k = 0; k < t->def.num_components && k < 4; k++)
                    at(&t->def, lane, k).u32 = bits(k == 3 ? 1.0f : r <= d ? 1.0f : 0.0f);
                continue;
            }
            for (unsigned k = 0; k < t->def.num_components && k < 4; k++)
                at(&t->def, lane, k).u32 = bits((float)(w >> (8 * byte[k]) & 0xffu) / 255.0f);
        }
    }
    void intrinsic(nir_intrinsic_instr *i, unsigned mask)
    {
        nir_def *d = nir_intrinsic_infos[i->intrinsic].has_dest ? &i->def : nullptr;
        switch (i->intrinsic) {
        case nir_intrinsic_load_barycentric_pixel:
        case nir_intrinsic_load_barycentric_centroid:
        case nir_intrinsic_decl_reg:
            return;
        case nir_intrinsic_load_interpolated_input:
        case nir_intrinsic_load_input: {
            unsigned loc = nir_intrinsic_io_semantics(i).location,
                     first = nir_intrinsic_component(i);
            for (unsigned lane = 0; lane < 4; lane++)
                for (unsigned c = 0; c < d->num_components; c++) {
                    if (loc == VARYING_SLOT_POS)
                        at(d, lane, c).u32 = bits(frag(in, lane, first + c));
                    else if (loc == VARYING_SLOT_FACE) {
                        if (d->bit_size == 1)
                            at(d, lane, c).b = front(in, lane);
                        else if (nir_alu_type_get_base_type(nir_intrinsic_dest_type(i)) ==
                                 nir_type_bool)
                            at(d, lane, c).u32 = front(in, lane) ? ~0u : 0u;
                        else
                            at(d, lane, c).u32 = bits(front(in, lane) ? 1.0f : -1.0f);
                    } else
                        at(d, lane, c).u32 = bits(in.varying[lane][loc][first + c]);
                }
            return;
        }
        case nir_intrinsic_load_frag_coord:
            for (unsigned lane = 0; lane < 4; lane++)
                for (unsigned c = 0; c < d->num_components; c++)
                    at(d, lane, c).u32 = bits(frag(in, lane, c));
            return;
        case nir_intrinsic_load_front_face:
            for (unsigned lane = 0; lane < 4; lane++)
                if (d->bit_size == 1)
                    at(d, lane, 0).b = front(in, lane);
                else
                    at(d, lane, 0).u32 = front(in, lane) ? ~0u : 0u;
            return;
        case nir_intrinsic_load_uniform:
            /* the offset per lane: an indexed uniform array keeps its index here */
            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned first = nir_intrinsic_base(i) + at(i->src[0].ssa, lane, 0).u32;
                for (unsigned c = 0; c < d->num_components; c++)
                    at(d, lane, c).u32 = in.uniform[(first + c) & 255u];
            }
            return;
        case nir_intrinsic_store_output: {
            nir_io_semantics io = nir_intrinsic_io_semantics(i);
            unsigned wm = nir_intrinsic_write_mask(i), first = nir_intrinsic_component(i);
            for (unsigned lane = 0; lane < 4; lane++) {
                if (!(mask >> lane & 1u))
                    continue;
                for (unsigned c = 0; c < i->src[0].ssa->num_components; c++) {
                    if (!(wm >> c & 1u))
                        continue;
                    float f = scalar(&i->src[0], lane, c);
                    if (io.location == FRAG_RESULT_DEPTH)
                        out.depth[lane] = f, out.wrote_depth = true;
                    else if (first + c < 4)
                        out.color[lane][first + c] = f;
                }
            }
            return;
        }
        case nir_intrinsic_ddx:
        case nir_intrinsic_ddx_fine:
        case nir_intrinsic_ddx_coarse:
        case nir_intrinsic_ddy:
        case nir_intrinsic_ddy_fine:
        case nir_intrinsic_ddy_coarse: {
            bool y = i->intrinsic == nir_intrinsic_ddy || i->intrinsic == nir_intrinsic_ddy_fine ||
                     i->intrinsic == nir_intrinsic_ddy_coarse;
            for (unsigned lane = 0; lane < 4; lane++)
                for (unsigned c = 0; c < d->num_components; c++) {
                    unsigned a = y ? (lane & 1u) : (lane & 2u), b = y ? a + 2u : a + 1u;
                    float g = f32(at(i->src[0].ssa, b, c).u32) - f32(at(i->src[0].ssa, a, c).u32);
                    at(d, lane, c).u32 = bits(y ? in.ysign * g : g);
                }
            return;
        }
        case nir_intrinsic_terminate:
        case nir_intrinsic_demote:
            for (unsigned lane = 0; lane < 4; lane++)
                if (mask >> lane & 1u)
                    out.killed[lane] = true;
            return;
        case nir_intrinsic_terminate_if:
        case nir_intrinsic_demote_if:
            for (unsigned lane = 0; lane < 4; lane++)
                if ((mask >> lane & 1u) && at(i->src[0].ssa, lane, 0).b)
                    out.killed[lane] = true;
            return;
        case nir_intrinsic_load_reg:
            for (unsigned lane = 0; lane < 4; lane++)
                for (unsigned c = 0; c < d->num_components; c++)
                    at(d, lane, c) = at(i->src[0].ssa, lane, c);
            return;
        case nir_intrinsic_store_reg: {
            unsigned wm = nir_intrinsic_write_mask(i);
            for (unsigned lane = 0; lane < 4; lane++)
                if (mask >> lane & 1u)
                    for (unsigned c = 0; c < i->src[0].ssa->num_components; c++)
                        if (wm >> c & 1u)
                            at(i->src[1].ssa, lane, c) = at(i->src[0].ssa, lane, c);
            return;
        }
        default:
            std::fprintf(stderr, "interpreter: %s\n", nir_intrinsic_infos[i->intrinsic].name);
            error = true;
        }
    }
    unsigned block(nir_block *b, unsigned mask)
    {
        nir_foreach_instr(instr, b)
        {
            if (++steps > 200000) {
                error = runaway = true;
                return 0;
            }
            switch (instr->type) {
            case nir_instr_type_alu:
                alu(nir_instr_as_alu(instr), mask);
                break;
            case nir_instr_type_load_const: {
                nir_load_const_instr *k = nir_instr_as_load_const(instr);
                for (unsigned lane = 0; lane < 4; lane++)
                    for (unsigned c = 0; c < k->def.num_components; c++)
                        at(&k->def, lane, c) = k->value[c];
                break;
            }
            case nir_instr_type_undef:
                break;
            case nir_instr_type_intrinsic:
                intrinsic(nir_instr_as_intrinsic(instr), mask);
                break;
            case nir_instr_type_tex:
                tex(nir_instr_as_tex(instr));
                break;
            case nir_instr_type_jump: {
                nir_jump_instr *j = nir_instr_as_jump(instr);
                if (j->type == nir_jump_break)
                    loops.back().broken |= mask;
                else if (j->type == nir_jump_continue)
                    loops.back().continued |= mask;
                else {
                    std::fprintf(stderr, "interpreter: jump %d\n", (int)j->type);
                    error = true;
                }
                return 0;
            }
            default:
                std::fprintf(stderr, "interpreter: instruction type %d\n", (int)instr->type);
                error = true;
                return 0;
            }
        }
        return mask;
    }
    unsigned list(struct exec_list *l, unsigned mask)
    {
        foreach_list_typed(nir_cf_node, node, node, l)
        {
            if (error)
                return 0;
            switch (node->type) {
            case nir_cf_node_block:
                mask = block(nir_cf_node_as_block(node), mask);
                break;
            case nir_cf_node_if: {
                nir_if *nif = nir_cf_node_as_if(node);
                unsigned yes = 0;
                for (unsigned lane = 0; lane < 4; lane++) {
                    nir_const_value cv = at(nif->condition.ssa, lane, 0);
                    if (nif->condition.ssa->bit_size == 1 ? cv.b : cv.u32 != 0)
                        yes |= 1u << lane;
                }
                unsigned t = list(&nif->then_list, mask & yes);
                unsigned e = list(&nif->else_list, mask & ~yes);
                mask = t | e;
                break;
            }
            case nir_cf_node_loop: {
                nir_loop *loop = nir_cf_node_as_loop(node);
                unsigned active = mask;
                loops.push_back({0, 0});
                while (active && !error) {
                    unsigned rest = list(&loop->body, active);
                    active = (rest | loops.back().continued) & ~loops.back().broken;
                    loops.back().continued = 0;
                    if (++steps > 200000)
                        error = runaway = true;
                }
                mask = loops.back().broken & mask;
                loops.pop_back();
                break;
            }
            default:
                error = true;
            }
        }
        return mask;
    }
    bool run()
    {
        list(&nir_shader_get_entrypoint(s)->body, 0xfu);
        return !error;
    }
};

/* ---- the words on the model -------------------------------------------------------------------- */
static float operand(const struct sgx535_sim *sim, unsigned lane, struct sgx535_use_operand o)
{
    uint32_t w = 0;
    switch (o.bank) {
    case SGX535_USE_R:
        w = sim->pixel[lane].r[o.number];
        break;
    case SGX535_USE_PA:
        w = sim->pixel[lane].pa[o.number];
        break;
    case SGX535_USE_SA:
        w = sim->sa[o.number];
        break;
    case SGX535_USE_C:
        w = o.number == SGX535_USE_C_ONE ? 0x3f800000u : o.number == 15 ? 0x3e800000u : 0u;
        break;
    default:
        break;
    }
    if (o.mod & SGX535_USE_ABS)
        w &= 0x7fffffffu;
    if (o.mod & SGX535_USE_NEG)
        w ^= 0x80000000u;
    return f32(w);
}

static void load(struct sgx535_sim *sim, const struct sgx535_fs_nir *ir,
                 const struct sgx535_fs_program *p, const Inputs &in)
{
    std::memset(sim, 0, sizeof(*sim));
    sim->sample = texel;
    for (uint32_t k = 1; k < p->constant_count; k++) {
        const struct sgx535_fs_constant *c = &p->constant[k];
        uint32_t w = 0;
        switch (c->kind) {
        case SGX535_FS_CONSTANT_UNIFORM:
            w = in.uniform[c->value];
            break;
        case SGX535_FS_CONSTANT_UNIFORM_INT:
            w = bits((float)(int32_t)in.uniform[c->value]);
            break;
        case SGX535_FS_CONSTANT_UNIFORM_BOOL:
            w = bits(in.uniform[c->value] ? 1.0f : 0.0f);
            break;
        case SGX535_FS_CONSTANT_LITERAL:
            w = c->value;
            break;
        case SGX535_FS_CONSTANT_TEXTURE:
            w = 0x7e000000u | (c->value >> 2) << 2;
            break;
        case SGX535_FS_CONSTANT_TEXSIZE:
            w = bits(texture_size[c->value & 3u]);
            break;
        case SGX535_FS_CONSTANT_FRAG_Y:
            w = bits(in.height);
            break;
        case SGX535_FS_CONSTANT_FRAG_YSIGN:
            w = bits(in.ysign);
            break;
        case SGX535_FS_CONSTANT_FACING:
            w = in.sense;
            break;
        }
        sim->sa[k] = w;
    }
    for (unsigned lane = 0; lane < 4; lane++) {
        sim->pixel[lane].g16 = in.back[lane];
        for (uint32_t t = 0; t < p->iteration_count; t++) {
            const struct sgx535_fs_iteration *it = &p->iteration[t];
            unsigned issue = it->douti >> 12 & 15u;
            for (unsigned c = 0; c < it->dwords; c++) {
                uint32_t value;
                if (it->texture) {
                    unsigned loc = ir->varying_location[it->douti & 15u];
                    float coord[3] = {in.varying[lane][loc][0], in.varying[lane][loc][1], 0};
                    value = texel(nullptr, it->sampler, 0, coord, 2);
                } else if (issue == 13)
                    value = bits(in.window[lane][c]);
                else
                    value = bits(in.varying[lane][ir->varying_location[issue]][c]);
                sim->pixel[lane].pa[it->pa + c] = value;
            }
        }
    }
}

static bool close_enough(float got, float want, float *error)
{
    float d;
    if (std::isnan(want) || std::isnan(got))
        return std::isnan(want) && std::isnan(got);
    if (std::isinf(want) || std::isinf(got))
        return got == want || std::fabs(want) > 1e30f || std::fabs(got) > 1e30f;
    d = std::fabs(got - want) / std::fmax(1.0f, std::fabs(want));
    if (d > *error)
        *error = d;
    return d <= 2e-3f;
}

int main(int argc, char **argv)
{
    static gl_context context;
    standalone_options options = {};
    gl_shader_program *program;
    bool words = false;
    unsigned rounds = 64;
    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-' && argv[arg][1] == '-'; arg++) {
        if (!std::strcmp(argv[arg], "--words"))
            words = true;
        else if (!std::strcmp(argv[arg], "--rounds") && arg + 1 < argc)
            rounds = (unsigned)std::atoi(argv[++arg]);
        else if (!std::strcmp(argv[arg], "--seed") && arg + 1 < argc)
            rng = (unsigned)std::atoi(argv[++arg]);
        else
            return 2;
    }
    if (argc - arg != 2)
        return 2;
    const char *name = argv[arg + 1];
    options.glsl_version = 120;
    options.do_link = 0;
    options.just_log = 1;
    program = standalone_compile_shader(&options, 2, argv + arg, &context);
    if (!program || program->NumShaders != 2 ||
        program->Shaders[0]->CompileStatus != COMPILE_SUCCESS ||
        program->Shaders[1]->CompileStatus != COMPILE_SUCCESS) {
        std::printf("SHADER %s: GLSL compile failed\n", name);
        return 0;
    }
    _mesa_clear_shader_program_data(&context, program);
    /* the screen's fragment uniform space (poulsback_screen.c: 128 words), not
     * the standalone frontend's 64 components */
    context.Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 128;
    context.Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 128;
    program->data->LinkStatus = LINKING_SUCCESS;
    link_shaders_init(&context, program);
    gl_nir_link_glsl(&context, program);
    gl_linked_shader *fs = program->_LinkedShaders[MESA_SHADER_FRAGMENT];
    if (program->data->LinkStatus != LINKING_SUCCESS || !fs || !fs->Program || !fs->Program->nir) {
        std::printf("SHADER %s: GLSL link failed: %s\n", name,
                    program->data->InfoLog ? program->data->InfoLog : "");
        return 0;
    }
    nir_shader *nir = fs->Program->nir;
    if (!lower_io(nir)) {
        std::printf("SHADER %s: fixture lowering refused (indexed sampler)\n", name);
        return 0;
    }

    if (std::getenv("POULSBACK_FS_NIR_PRINT"))
        nir_print_shader(nir, stderr);
    static struct sgx535_fs_nir ir;
    static struct sgx535_fs_program body;
    enum sgx535_fs_status status = sgx535_fs_from_nir(nir, SGX535_FS_UNIFORMS_DWORDS, &ir);
    if (status != SGX535_FS_OK) {
        std::printf("SHADER %s: REFUSED adapter: %s\n", name, ir.reason);
        return 0;
    }
    /* a shadow sampler's variant: GL's defaults, LEQUAL and LUMINANCE */
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl) nir_foreach_instr(
        instr, block) if (instr->type == nir_instr_type_tex && nir_instr_as_tex(instr)->is_shadow)
    {
        struct sgx535_fs_sampler *s = &ir.key.sampler[nir_instr_as_tex(instr)->sampler_index];
        s->compare = 3 + 1;
        s->swizzle[0] = s->swizzle[1] = s->swizzle[2] = SGX535_FS_SWIZZLE_R;
        s->swizzle[3] = SGX535_FS_SWIZZLE_ONE;
    }
    status = sgx535_fs_compile(ir.ir, ir.count, &ir.key, &body);
    if (status != SGX535_FS_OK) {
        std::printf("SHADER %s: REFUSED backend at %u: %s\n", name, body.failed_at,
                    body.reason ? body.reason : "");
        return 0;
    }
    /* The body and nop.end: the epilogue is the variant's. */
    std::vector<uint32_t> code(body.words, body.words + 2 * body.instruction_count);
    code.push_back(0);
    code.push_back(0xf8040140u);
    if (words) {
        for (size_t k = 0; k < code.size(); k += 2) {
            char text[160];
            if (!sgx535_sim_text(code[k], code[k + 1], text, sizeof(text)))
                std::snprintf(text, sizeof(text), "?");
            std::printf("%08x %08x %s\n", code[k], code[k + 1], text);
        }
        return 0;
    }

    /* The interpreter runs the I/O-lowered NIR out of SSA. */
    nir_shader *ref = nir_shader_clone(nullptr, nir);
    nir_convert_from_ssa(ref, true, false);
    nir_foreach_function_impl(impl, ref) nir_index_ssa_defs(impl);
    /* Integer uniforms get small values (loop counts), boolean ones Mesa's
     * 0 or ~0 (native integers). */
    static uint8_t int_uniform[256];
    nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block)
    {
        if (instr->type != nir_instr_type_intrinsic)
            continue;
        nir_intrinsic_instr *u = nir_instr_as_intrinsic(instr);
        if (u->intrinsic != nir_intrinsic_load_uniform)
            continue;
        nir_alu_type type = nir_alu_type_get_base_type(nir_intrinsic_dest_type(u));
        if (type != nir_type_int && type != nir_type_uint && type != nir_type_bool)
            continue;
        /* an indexed load: the whole range */
        unsigned first = nir_intrinsic_base(u) +
                         (nir_src_is_const(u->src[0]) ? (unsigned)nir_src_as_uint(u->src[0]) : 0u),
                 count =
                     nir_src_is_const(u->src[0]) ? u->def.num_components : nir_intrinsic_range(u);
        for (unsigned c = 0; c < count; c++)
            int_uniform[(first + c) & 255u] = type == nir_type_bool ? 2 : 1;
    }
    float worst = 0.0f;
    unsigned compared = 0, undefined = 0, endless = 0;
    for (unsigned r = 0; r < rounds; r++) {
        static Inputs in;
        std::memset(&in, 0, sizeof(in));
        float x0 = 10.5f + (float)(rnd() % 50);
        for (unsigned lane = 0; lane < 4; lane++) {
            for (unsigned l = 0; l < VARYING_SLOT_MAX; l++)
                for (unsigned c = 0; c < 4; c++)
                    in.varying[lane][l][c] = rndf(-1.0f, 1.5f);
            in.window[lane][0] = x0 + (float)(lane & 1u);
            in.window[lane][1] = 20.5f + (float)(lane >> 1);
            in.window[lane][2] = rndf(0.0f, 1.0f);
            in.window[lane][3] = rndf(0.5f, 2.0f);
            in.back[lane] = rnd() & 1u;
        }
        for (unsigned u = 0; u < 256; u++)
            in.uniform[u] = int_uniform[u] == 2   ? ((rnd() & 1u) ? ~0u : 0u)
                            : int_uniform[u] == 1 ? (uint32_t)(rnd() % 6)
                                                  : bits(rndf(-2.0f, 2.0f));
        in.height = 64.0f;
        in.ysign = (rnd() & 1u) ? -1.0f : 1.0f;
        in.sense = rnd() & 1u;
        Interp interp(ref, in);
        if (!interp.run()) {
            if (interp.runaway) { /* these inputs never end (piglit's do-while (f)) */
                endless++;
                continue;
            }
            if (std::getenv("POULSBACK_FS_NIR_PRINT"))
                nir_print_shader(ref, stderr);
            std::printf("SHADER %s: interpreter failed\n", name);
            return 1;
        }
        static struct sgx535_sim sim;
        load(&sim, &ir, &body, in);
        if (!sgx535_sim_run(&sim, code.data(), (uint32_t)code.size() / 2u)) {
            std::printf("SHADER %s: DIFF model: %s\n", name, sim.error);
            return 1;
        }
        for (unsigned lane = 0; lane < 4; lane++) {
            if (interp.undefined[lane]) {
                undefined++;
                continue;
            }
            /* the kill flag's temporary, or p1 holding the pass (EMGD's form) */
            bool killed = (body.kill != 0xffu && f32(sim.pixel[lane].r[body.kill]) != 0.0f) ||
                          (body.kill_pred && !sim.pixel[lane].p[1]);
            if (killed != interp.out.killed[lane]) {
                std::printf("SHADER %s: DIFF round %u lane %u: discard %d, NIR %d\n", name, r, lane,
                            killed, interp.out.killed[lane]);
                return 1;
            }
            if (killed)
                continue;
            for (unsigned c = 0; c < 4; c++) {
                float got = operand(&sim, lane, body.color[c]), want = interp.out.color[lane][c];
                compared++;
                if (!close_enough(got, want, &worst)) {
                    std::printf("SHADER %s: DIFF round %u lane %u comp %u: %g, NIR %g\n", name, r,
                                lane, c, got, want);
                    return 1;
                }
            }
            if (interp.out.wrote_depth) {
                float got = operand(&sim, lane, body.depth);
                if (!close_enough(got, interp.out.depth[lane], &worst)) {
                    std::printf("SHADER %s: DIFF round %u lane %u depth: %g, NIR %g\n", name, r,
                                lane, got, interp.out.depth[lane]);
                    return 1;
                }
            }
        }
    }
    std::printf("SHADER %s: OK ir %u, instructions %u, temps %u, pa %u, sa %u, %u values, "
                "worst relative error %.2e",
                name, ir.count, body.instruction_count, body.temps, body.primary,
                body.constant_count, compared, (double)worst);
    if (undefined)
        std::printf(", %u lanes undefined (a NaN in NIR)", undefined);
    if (endless)
        std::printf(", %u rounds without an end (not run)", endless);
    std::printf("\n");
    return 0;
}
