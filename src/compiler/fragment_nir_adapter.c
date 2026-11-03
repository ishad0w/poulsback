// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa NIR adapter for the bounded raw F32 fragment transport profile.
 * The caller explicitly lowers I/O (vec4 slots) and uniforms (32-bit words).
 * No input mutation, scene recognition or native capability promotion. */
#include "nir.h"
#include "sgx535_fragment.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_raw_sign.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_internal.h"
#include <stdlib.h>
#include <string.h>

struct definition_map {
    const nir_def *def;
    uint32_t scalar[4];
    bool barycentric;
    unsigned interpolation;
};
struct adapter_work {
    struct sgx535_fragment_instruction stream[SGX535_NIR_MAX_INSTRUCTIONS];
    struct definition_map map[SGX535_NIR_MAX_INSTRUCTIONS];
    unsigned count, map_count;
    bool linked, raw_sign, raw_sign_inf, bool32, bool32_ieq, int32_uniforms, const_ushr;
    bool sampled, sampled_seen;
    struct sgx535_fragment_sampled_input sample;
    enum sgx535_fragment_uniform_layout uniform_layout;
    enum sgx535_fragment_io_precision_policy io_precision;
    struct sgx535_fragment_linkage linkage;
};
static const nir_block *linear_entry(const nir_shader *shader, bool lowered_declarations)
{
    nir_function *f;
    nir_cf_node *node;
    nir_block *block;
    if (shader->info.stage != MESA_SHADER_FRAGMENT || shader->info.float_controls_execution_mode ||
        (!lowered_declarations && !exec_list_is_empty(&shader->variables)) ||
        !exec_list_is_singular(&shader->functions))
        return NULL;
    if (lowered_declarations) {
        /* Direct I/O and tex instructions below carry the complete access
         * contract. Their retained GLSL declarations are metadata, not storage
         * reads. Dereferences and other storage modes still have no projection. */
        nir_foreach_variable_in_list(
            variable, &shader->variables) if (variable->data.mode != nir_var_shader_in &&
                                              variable->data.mode != nir_var_shader_out &&
                                              variable->data.mode != nir_var_uniform) return NULL;
    }
    f = exec_node_data(nir_function, shader->functions.head_sentinel.next, node);
    if (!f->is_entrypoint || f->num_params || !f->impl || f->shader != shader ||
        f->impl->function != f || f->impl->preamble || !f->impl->structured ||
        !exec_list_is_empty(&f->impl->locals) || !exec_list_is_singular(&f->impl->body))
        return NULL;
    node = exec_node_data(nir_cf_node, f->impl->body.head_sentinel.next, node);
    if (node->type != nir_cf_node_block)
        return NULL;
    block = nir_cf_node_as_block(node);
    if (block->impl != f->impl || !f->impl->end_block ||
        block->successors[0] != f->impl->end_block || block->successors[1] ||
        !exec_list_is_empty(&f->impl->end_block->instr_list))
        return NULL;
    return block;
}
static const nir_block *entry(const nir_shader *shader)
{
    return linear_entry(shader, false);
}
static bool width32(const nir_def *def)
{
    return def && def->bit_size == 32 && def->num_components >= 1 && def->num_components <= 4;
}
const nir_block *sgx535_nir_fragment_linear_block(const nir_shader *shader)
{
    return entry(shader);
}
static struct definition_map *lookup(struct adapter_work *w, const nir_def *def)
{
    unsigned i;
    for (i = 0; i < w->map_count; i++)
        if (w->map[i].def == def)
            return &w->map[i];
    return NULL;
}
static enum sgx535_nir_status
append(struct adapter_work *w, struct sgx535_fragment_instruction instruction, uint32_t *index)
{
    if (w->count >= SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    if (index)
        *index = w->count;
    w->stream[w->count++] = instruction;
    return SGX535_NIR_OK;
}
static enum sgx535_nir_status scalar_source(struct adapter_work *w, const nir_def *def,
                                            unsigned component, uint32_t *index)
{
    struct definition_map *map;
    if (!width32(def) || component >= def->num_components)
        return SGX535_NIR_ERR_UNSUPPORTED;
    map = lookup(w, def);
    if (!map)
        return SGX535_NIR_ERR_SSA;
    if (map->barycentric)
        return SGX535_NIR_ERR_UNSUPPORTED;
    *index = map->scalar[component];
    return SGX535_NIR_OK;
}
static bool offset(struct adapter_work *w, const nir_def *def, uint32_t *value)
{
    uint32_t index;
    if (!def || def->num_components != 1 || scalar_source(w, def, 0, &index) != SGX535_NIR_OK ||
        w->stream[index].opcode != SGX535_FRAGMENT_CONSTANT)
        return false;
    *value = w->stream[index].immediate;
    return true;
}
static bool indices_equal(const nir_intrinsic_instr *actual, const nir_intrinsic_instr *expected)
{
    return !memcmp(actual->const_index, expected->const_index, sizeof(expected->const_index));
}
static bool input_contract(struct adapter_work *w, const nir_intrinsic_instr *in,
                           enum sgx535_fragment_interpolation interpolation, uint32_t *slot)
{
    nir_intrinsic_instr expected = {0};
    nir_io_semantics semantics = {0};
    int signed_base = nir_intrinsic_base(in);
    unsigned base, component = nir_intrinsic_component(in);
    nir_io_semantics actual = nir_intrinsic_io_semantics(in);
    enum sgx535_fragment_semantic semantic = SGX535_FRAGMENT_GENERIC;
    uint32_t index;
    unsigned i;
    if (signed_base < 0 || (unsigned)signed_base >= SGX535_FRAGMENT_INPUT_SLOTS ||
        component + in->num_components > 4)
        return false;
    base = (unsigned)signed_base;
    index = base;
    expected.intrinsic = in->intrinsic;
    if (w->linked) {
        if (actual.location == VARYING_SLOT_COL0 || actual.location == VARYING_SLOT_COL1) {
            semantic = SGX535_FRAGMENT_COLOR;
            index = actual.location == VARYING_SLOT_COL0 ? 0 : 1;
        } else if (actual.location >= VARYING_SLOT_VAR0 && actual.location <= VARYING_SLOT_VAR31)
            index = actual.location - VARYING_SLOT_VAR0;
        else
            return false;
        semantics.location = actual.location;
    } else {
        _Static_assert(VARYING_SLOT_VAR0 + SGX535_FRAGMENT_INPUT_SLOTS <= 128,
                       "direct input locations fit Mesa's seven-bit field");
        /* The base bound above proves this mask cannot discard location bits. */
        semantics.location = (VARYING_SLOT_VAR0 + base) & 0x7fu;
    }
    semantics.num_slots = 1;
    if (w->io_precision == SGX535_FRAGMENT_IO_F32)
        semantics.medium_precision = actual.medium_precision;
    nir_intrinsic_set_base(&expected, signed_base);
    nir_intrinsic_set_component(&expected, component);
    nir_intrinsic_set_dest_type(&expected, nir_type_float32);
    nir_intrinsic_set_io_semantics(&expected, semantics);
    /* Mesa RANGE bounds an indirect offset. For these direct accesses the
     * caller already proved offset == 0, and the complete slot/component/type
     * contract below independently bounds the access. Mesa FF lowering leaves
     * RANGE zero; keep the existing explicit one-slot spelling as well. */
    if (in->intrinsic == nir_intrinsic_load_input) {
        unsigned range = nir_intrinsic_range(in);
        if (range > 1)
            return false;
        nir_intrinsic_set_range(&expected, range);
    }
    if (!indices_equal(in, &expected))
        return false;
    if (!w->linked) {
        *slot = base;
        return true;
    }
    for (i = 0; i < w->linkage.count; i++) {
        struct sgx535_fragment_varying *v = &w->linkage.varying[i];
        bool same = v->semantic == semantic && v->index == index;
        if ((v->driver_base == base) != same)
            return false;
        if (same) {
            if (v->interpolation != interpolation)
                return false;
            break;
        }
    }
    if (i == w->linkage.count) {
        if (i == SGX535_FRAGMENT_INPUT_SLOTS)
            return false;
        w->linkage.varying[i] =
            (struct sgx535_fragment_varying){semantic, index, base, 0, interpolation};
        w->linkage.count++;
    }
    if (w->linkage.varying[i].components < component + in->num_components)
        w->linkage.varying[i].components = component + in->num_components;
    *slot = i;
    return true;
}
static bool uniform_contract(const struct adapter_work *w, const nir_intrinsic_instr *in,
                             uint32_t off, uint32_t *first_word)
{
    nir_intrinsic_instr expected = {0};
    int signed_base = nir_intrinsic_base(in);
    uint32_t base, range = nir_intrinsic_range(in);
    uint32_t stride = w->uniform_layout == SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS ? 4u : 1u;
    uint32_t limit = SGX535_FRAGMENT_UNIFORM_WORDS / stride;
    nir_alu_type type = nir_intrinsic_dest_type(in);
    if (type != nir_type_float32 && !(w->int32_uniforms && type == nir_type_int32))
        return false;
    if (signed_base < 0)
        return false;
    base = (uint32_t)signed_base;
    if (!range || base >= limit || range > limit - base || off >= range ||
        in->num_components > (range - off) * stride)
        return false;
    expected.intrinsic = nir_intrinsic_load_uniform;
    nir_intrinsic_set_base(&expected, signed_base);
    nir_intrinsic_set_range(&expected, range);
    /* UNIFORM loads unchanged SECONDARY words. Only the comparison entry
     * admits signed32 metadata; it neither converts nor rewrites that type. */
    nir_intrinsic_set_dest_type(&expected, type);
    if (!indices_equal(in, &expected))
        return false;
    *first_word = (base + off) * stride;
    return true;
}
static bool output_contract(const struct adapter_work *w, const nir_intrinsic_instr *out)
{
    nir_intrinsic_instr expected = {0};
    nir_io_semantics semantics = {0};
    unsigned mask = nir_intrinsic_write_mask(out);
    if (!mask || (mask & ~15u) || (w->io_precision != SGX535_FRAGMENT_IO_F32 && mask != 15u))
        return false;
    expected.intrinsic = nir_intrinsic_store_output;
    semantics.location = nir_intrinsic_io_semantics(out).location;
    if (semantics.location != FRAG_RESULT_DATA0 && semantics.location != FRAG_RESULT_COLOR)
        return false;
    semantics.num_slots = 1;
    if (w->io_precision == SGX535_FRAGMENT_IO_F32)
        semantics.medium_precision = nir_intrinsic_io_semantics(out).medium_precision;
    /* Offset zero and one complete output slot are checked independently;
     * accept the omitted direct-I/O range used by Mesa FF lowering. */
    if (nir_intrinsic_range(out) > 1)
        return false;
    nir_intrinsic_set_range(&expected, nir_intrinsic_range(out));
    nir_intrinsic_set_write_mask(&expected, mask);
    nir_intrinsic_set_src_type(&expected, nir_type_float32);
    nir_intrinsic_set_io_semantics(&expected, semantics);
    return out->num_components == 4 && width32(out->src[0].ssa) &&
           out->src[0].ssa->num_components == 4 && indices_equal(out, &expected);
}
static enum sgx535_nir_status constant(struct adapter_work *w, uint32_t value, uint32_t *index)
{
    struct sgx535_fragment_instruction op = {0};
    op.opcode = SGX535_FRAGMENT_CONSTANT;
    op.immediate = value;
    return append(w, op, index);
}
static enum sgx535_nir_status alu(struct adapter_work *w, const nir_alu_instr *a,
                                  struct definition_map *map)
{
    unsigned c, arity;
    enum sgx535_fragment_opcode opcode;
    uint32_t allowed_controls =
        w->raw_sign_inf && (a->op == nir_op_fneg || a->op == nir_op_fabs) ? nir_fp_preserve_inf : 0;
    if (w->bool32 && a->op == nir_op_b2f32)
        allowed_controls = nir_fp_preserve_signed_zero;
    if (a->no_signed_wrap || a->no_unsigned_wrap || (a->fp_math_ctrl & ~allowed_controls))
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (w->sampled && a->op == nir_op_mov) {
        /* A component-preserving SSA alias adds no dependent coordinate
         * calculation. Keep the original checked INPUT as the IR2 source. */
        for (c = 0; c < a->def.num_components; c++) {
            enum sgx535_nir_status status =
                scalar_source(w, a->src[0].src.ssa, a->src[0].swizzle[c], &map->scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        return SGX535_NIR_OK;
    }
    if (a->op == nir_op_vec2 || a->op == nir_op_vec3 || a->op == nir_op_vec4) {
        if (nir_op_infos[a->op].num_inputs != a->def.num_components)
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (c = 0; c < a->def.num_components; c++) {
            enum sgx535_nir_status status =
                scalar_source(w, a->src[c].src.ssa, a->src[c].swizzle[0], &map->scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        return SGX535_NIR_OK;
    }
    switch (a->op) {
    case nir_op_ushr:
        if (!w->const_ushr || !a->src[0].src.ssa || a->src[0].src.ssa->bit_size != 32 ||
            !a->src[1].src.ssa || a->src[1].src.ssa->bit_size != 32 ||
            nir_def_instr(a->src[1].src.ssa)->type != nir_instr_type_load_const)
            return SGX535_NIR_ERR_UNSUPPORTED;
        opcode = SGX535_FRAGMENT_RAW_USHR_IMM;
        arity = 1;
        break;
    case nir_op_ieq32:
        if (!w->bool32_ieq)
            return SGX535_NIR_ERR_UNSUPPORTED;
        opcode = SGX535_FRAGMENT_RAW_IEQ;
        arity = 2;
        break;
    case nir_op_iand:
        if (!w->bool32)
            return SGX535_NIR_ERR_UNSUPPORTED;
        opcode = SGX535_FRAGMENT_RAW_AND;
        arity = 2;
        break;
    case nir_op_inot:
        if (!w->bool32)
            return SGX535_NIR_ERR_UNSUPPORTED;
        opcode = SGX535_FRAGMENT_RAW_XOR;
        arity = 1;
        break;
    case nir_op_b2f32:
    case nir_op_b2i32:
        if (!w->bool32)
            return SGX535_NIR_ERR_UNSUPPORTED;
        opcode = SGX535_FRAGMENT_RAW_AND;
        arity = 1;
        break;
    case nir_op_mov:
        opcode = SGX535_FRAGMENT_MOV;
        arity = 1;
        break;
    case nir_op_ffma:
    case nir_op_flrp:
        opcode = SGX535_FRAGMENT_MAD;
        arity = 3;
        break;
    case nir_op_fmin:
        opcode = SGX535_FRAGMENT_MIN;
        arity = 2;
        break;
    case nir_op_fmax:
        opcode = SGX535_FRAGMENT_MAX;
        arity = 2;
        break;
    case nir_op_fsat:
        opcode = SGX535_FRAGMENT_MAX;
        arity = 1;
        break;
    case nir_op_frcp:
        opcode = SGX535_FRAGMENT_RCP;
        arity = 1;
        break;
    case nir_op_frsq:
        opcode = SGX535_FRAGMENT_RSQ;
        arity = 1;
        break;
    case nir_op_ffract:
        opcode = SGX535_FRAGMENT_FRACT;
        arity = 1;
        break;
    case nir_op_fadd:
    case nir_op_fmul:
    case nir_op_fsub:
    case nir_op_fdiv:
        opcode = SGX535_FRAGMENT_MAD;
        arity = 2;
        break;
    case nir_op_fneg:
    case nir_op_fabs:
        opcode = w->raw_sign
                     ? (a->op == nir_op_fneg ? SGX535_FRAGMENT_RAW_XOR : SGX535_FRAGMENT_RAW_AND)
                     : SGX535_FRAGMENT_MAD;
        arity = 1;
        break;
    default:
        return SGX535_NIR_ERR_UNSUPPORTED;
    }
    for (c = 0; c < a->def.num_components; c++) {
        struct sgx535_fragment_instruction op = {0};
        enum sgx535_nir_status status;
        uint32_t sources[3] = {0};
        unsigned j;
        for (j = 0; j < arity; j++) {
            status = scalar_source(w, a->src[j].src.ssa, a->src[j].swizzle[c], &sources[j]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        op.opcode = opcode;
        memcpy(op.source, sources, sizeof(sources));
        /* These are explicit instruction projections, not a numerical proof:
         * arithmetic qualification remains zero (NaN/zero/rounding/fusion).
         * Never silently accept constrained fp_math_ctrl operations. */
        if (a->op == nir_op_ushr) {
            uint32_t count_source;
            status = scalar_source(w, a->src[1].src.ssa, a->src[1].swizzle[c], &count_source);
            if (status != SGX535_NIR_OK)
                return status;
            if (w->stream[count_source].opcode != SGX535_FRAGMENT_CONSTANT)
                return SGX535_NIR_ERR_UNSUPPORTED;
            op.immediate = w->stream[count_source].immediate;
            if (op.immediate != 1u && op.immediate != 2u && op.immediate != 4u &&
                op.immediate != 8u && op.immediate != 16u && op.immediate != 31u)
                return SGX535_NIR_ERR_UNSUPPORTED;
        } else if (w->bool32 && a->op == nir_op_inot) {
            status = constant(w, UINT32_MAX, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (w->bool32 && (a->op == nir_op_b2f32 || a->op == nir_op_b2i32)) {
            status = constant(w, a->op == nir_op_b2f32 ? UINT32_C(0x3f800000) : 1u, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fsat) {
            /* Mesa's fsat is MIN(MAX(x, +0), +1). The reverse order has
             * different NaN behavior. Keep this explicit order; constrained
             * operations still refuse above and SGX arithmetic is unqualified. */
            status = constant(w, 0, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_flrp) {
            /* a*(1-t)+b*t keeps the explicit source expression. The shortcut
             * a+t*(b-a) loses b for large cancellation at t=1. These existing
             * MAD forms still carry no native floating-semantics claim. */
            struct sgx535_fragment_instruction complement = {0}, product = {0};
            uint32_t inverse, first;
            complement.opcode = SGX535_FRAGMENT_MAD;
            complement.source[0] = sources[2];
            status = constant(w, 0xbf800000u, &complement.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
            status = constant(w, 0x3f800000u, &complement.source[2]);
            if (status != SGX535_NIR_OK)
                return status;
            status = append(w, complement, &inverse);
            if (status != SGX535_NIR_OK)
                return status;
            product.opcode = SGX535_FRAGMENT_MAD;
            product.source[0] = sources[0];
            product.source[1] = inverse;
            status = constant(w, 0, &product.source[2]);
            if (status != SGX535_NIR_OK)
                return status;
            status = append(w, product, &first);
            if (status != SGX535_NIR_OK)
                return status;
            op.source[0] = sources[1];
            op.source[1] = sources[2];
            op.source[2] = first;
        } else if (w->raw_sign && (a->op == nir_op_fneg || a->op == nir_op_fabs)) {
            status = constant(w, a->op == nir_op_fneg ? 0x80000000u : 0x7fffffffu, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fadd) {
            op.source[2] = sources[1];
            status = constant(w, 0x3f800000u, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fmul) {
            status = constant(w, 0, &op.source[2]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fneg || a->op == nir_op_fabs) {
            status = constant(w, 0xbf800000u, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
            status = constant(w, 0, &op.source[2]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fsub) {
            /* b * -1 + a: keep the operand order explicit. */
            op.source[0] = sources[1];
            op.source[2] = sources[0];
            status = constant(w, 0xbf800000u, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
        } else if (a->op == nir_op_fdiv) {
            struct sgx535_fragment_instruction reciprocal = {0};
            reciprocal.opcode = SGX535_FRAGMENT_RCP;
            reciprocal.source[0] = sources[1];
            status = append(w, reciprocal, &op.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
            status = constant(w, 0, &op.source[2]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        status = append(w, op, &map->scalar[c]);
        if (status != SGX535_NIR_OK)
            return status;
        if (a->op == nir_op_fsat) {
            struct sgx535_fragment_instruction upper = {0};
            upper.opcode = SGX535_FRAGMENT_MIN;
            upper.source[0] = map->scalar[c];
            status = constant(w, 0x3f800000u, &upper.source[1]);
            if (status != SGX535_NIR_OK)
                return status;
            status = append(w, upper, &map->scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        if (a->op == nir_op_fabs && !w->raw_sign) {
            /* MAX(x, -x) is an explicit unconstrained word projection.
             * NaNs, signed zero and the SFU/ALU numerical contract remain
             * unqualified; constrained FABS is rejected above. */
            struct sgx535_fragment_instruction absolute = {0};
            absolute.opcode = SGX535_FRAGMENT_MAX;
            absolute.source[0] = sources[0];
            absolute.source[1] = map->scalar[c];
            status = append(w, absolute, &map->scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
    }
    return SGX535_NIR_OK;
}
static enum sgx535_nir_status intrinsic(struct adapter_work *w, const nir_intrinsic_instr *in,
                                        struct definition_map *map)
{
    uint32_t off, slot = 0, uniform_word = 0;
    unsigned c;
    if (in->intrinsic == nir_intrinsic_load_barycentric_pixel) {
        nir_intrinsic_instr expected = {0};
        expected.intrinsic = in->intrinsic;
        if (w->linked && nir_intrinsic_interp_mode(in) == INTERP_MODE_NONE)
            nir_intrinsic_set_interp_mode(&expected, INTERP_MODE_NONE);
        else
            nir_intrinsic_set_interp_mode(&expected, INTERP_MODE_SMOOTH);
        if (in->num_components || in->def.num_components != 2 || !indices_equal(in, &expected))
            return SGX535_NIR_ERR_UNSUPPORTED;
        map->barycentric = true;
        map->interpolation = nir_intrinsic_interp_mode(in);
        return SGX535_NIR_OK;
    }
    if (in->intrinsic == nir_intrinsic_store_output) {
        uint32_t zero = 0;
        unsigned mask = nir_intrinsic_write_mask(in);
        if (!nir_instr_is_last(&in->instr) || !output_contract(w, in) ||
            !offset(w, in->src[1].ssa, &off) || off)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (mask != 15u) {
            /* This is the only output store: earlier stores refuse above.
             * Unwritten shader lanes are undefined, independently of any
             * framebuffer color mask. Complete them before the final four
             * OUTPUTs; every original source instruction was still checked. */
            enum sgx535_nir_status status = constant(w, 0, &zero);
            if (status != SGX535_NIR_OK)
                return status;
        }
        for (c = 0; c < 4; c++) {
            struct sgx535_fragment_instruction op = {0};
            enum sgx535_nir_status status;
            op.opcode = SGX535_FRAGMENT_OUTPUT;
            op.index = c;
            op.source[0] = zero;
            status = mask & (1u << c) ? scalar_source(w, in->src[0].ssa, c, &op.source[0])
                                      : SGX535_NIR_OK;
            if (status != SGX535_NIR_OK)
                return status;
            status = append(w, op, NULL);
            if (status != SGX535_NIR_OK)
                return status;
        }
        return SGX535_NIR_OK;
    }
    if (in->intrinsic != nir_intrinsic_load_uniform && in->intrinsic != nir_intrinsic_load_input &&
        in->intrinsic != nir_intrinsic_load_interpolated_input)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (w->sampled && in->intrinsic != nir_intrinsic_load_interpolated_input)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (!offset(w, in->src[in->intrinsic == nir_intrinsic_load_interpolated_input ? 1 : 0].ssa,
                &off))
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (in->intrinsic == nir_intrinsic_load_uniform) {
        if (!uniform_contract(w, in, off, &uniform_word))
            return SGX535_NIR_ERR_UNSUPPORTED;
    } else {
        enum sgx535_fragment_interpolation interpolation = SGX535_FRAGMENT_FLAT;
        if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
            struct definition_map *bary = lookup(w, in->src[0].ssa);
            if (!bary || !bary->barycentric)
                return SGX535_NIR_ERR_UNSUPPORTED;
            interpolation = bary->interpolation == INTERP_MODE_NONE
                                ? SGX535_FRAGMENT_COLOR_INTERPOLATION
                                : SGX535_FRAGMENT_SMOOTH;
        }
        if (off || !input_contract(w, in, interpolation, &slot))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
            struct definition_map *bary = lookup(w, in->src[0].ssa);
            /* Mesa compatibility COLOR follows raster flatshade when its
             * qualifier is NONE. Never reinterpret that default for GENERIC. */
            if (bary->interpolation == INTERP_MODE_NONE &&
                (!w->linked || w->linkage.varying[slot].semantic != SGX535_FRAGMENT_COLOR))
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
    }
    for (c = 0; c < in->num_components; c++) {
        struct sgx535_fragment_instruction op = {0};
        enum sgx535_nir_status status;
        if (in->intrinsic == nir_intrinsic_load_uniform) {
            op.opcode = SGX535_FRAGMENT_UNIFORM;
            op.index = uniform_word + c;
        } else {
            op.opcode = SGX535_FRAGMENT_INPUT;
            op.index = slot;
            op.component = nir_intrinsic_component(in) + c;
        }
        status = append(w, op, &map->scalar[c]);
        if (status != SGX535_NIR_OK)
            return status;
    }
    return SGX535_NIR_OK;
}
static enum sgx535_nir_status project_instruction(struct adapter_work *w, const nir_instr *instr)
{
    struct definition_map map = {0};
    enum sgx535_nir_status status;
    if (w->map_count == SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    switch (instr->type) {
    case nir_instr_type_tex: {
        const nir_tex_instr *texture = nir_instr_as_tex(instr);
        uint32_t coordinates[2];
        if (!w->sampled || w->sampled_seen || texture->op != nir_texop_tex ||
            texture->sampler_dim != GLSL_SAMPLER_DIM_2D || texture->dest_type != nir_type_float32 ||
            texture->def.num_components != 4 || texture->def.bit_size != 32 ||
            texture->coord_components != 2 || texture->num_srcs != 1 || !texture->src ||
            texture->src[0].src_type != nir_tex_src_coord || texture->is_array ||
            texture->is_shadow || texture->is_sparse || texture->array_is_lowered_cube ||
            texture->texture_non_uniform || texture->sampler_non_uniform ||
            texture->embedded_sampler || texture->offset_non_uniform || texture->backend_flags ||
            texture->skip_helpers || list_is_empty(&texture->def.uses) ||
            texture->texture_index >= SGX535_FRAGMENT_SAMPLED_SLOTS ||
            texture->sampler_index >= SGX535_FRAGMENT_SAMPLED_SLOTS || !texture->src[0].src.ssa ||
            texture->src[0].src.ssa->num_components != 2)
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (unsigned c = 0; c < 2; c++) {
            status = scalar_source(w, texture->src[0].src.ssa, c, &coordinates[c]);
            if (status != SGX535_NIR_OK)
                return status;
            const struct sgx535_fragment_instruction *input = &w->stream[coordinates[c]];
            if (input->opcode != SGX535_FRAGMENT_INPUT || input->component != c ||
                (c && input->index != w->stream[coordinates[0]].index))
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
        /* can_speculate permits a reordering; this projection preserves the
         * original linear instruction order and makes no speculative issue. */
        w->sample = (struct sgx535_fragment_sampled_input){
            .texture_slot = texture->texture_index,
            .sampler_slot = texture->sampler_index,
            .coordinate_slot = w->stream[coordinates[0]].index,
            .coordinate_components = 2,
            .interpolation = SGX535_FRAGMENT_SMOOTH,
            .result_format = SGX535_FRAGMENT_SAMPLED_BGRA8_U8,
            .result_dwords = 1};
        map.def = &texture->def;
        for (unsigned c = 0; c < 4; c++) {
            struct sgx535_fragment_instruction sample = {
                .opcode = SGX535_FRAGMENT_SAMPLE2D_COMPONENT,
                .component = c,
                .source = {coordinates[0], coordinates[1], 0}};
            status = append(w, sample, &map.scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        w->sampled_seen = true;
        break;
    }
    case nir_instr_type_undef: {
        unsigned c;
        map.def = &nir_instr_as_undef(instr)->def;
        if (!width32(map.def))
            return SGX535_NIR_ERR_UNSUPPORTED;
        /* Choose zero for an undefined component, as Mesa's explicit
         * undef-to-zero lowering permits. Defined values and constraints
         * stay untouched, and the caller's NIR graph is not rewritten. */
        for (c = 0; c < map.def->num_components; c++) {
            status = constant(w, 0, &map.scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        break;
    }
    case nir_instr_type_load_const: {
        const nir_load_const_instr *load = nir_instr_as_load_const(instr);
        unsigned c;
        map.def = &load->def;
        if (!width32(map.def))
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (c = 0; c < map.def->num_components; c++) {
            status = constant(w, load->value[c].u32, &map.scalar[c]);
            if (status != SGX535_NIR_OK)
                return status;
        }
        break;
    }
    case nir_instr_type_alu: {
        const nir_alu_instr *a = nir_instr_as_alu(instr);
        map.def = &a->def;
        if (!width32(map.def))
            return SGX535_NIR_ERR_UNSUPPORTED;
        status = alu(w, a, &map);
        if (status != SGX535_NIR_OK)
            return status;
        break;
    }
    case nir_instr_type_intrinsic: {
        const nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
        if (in->intrinsic != nir_intrinsic_store_output) {
            map.def = &in->def;
            if (!width32(map.def) || (in->intrinsic == nir_intrinsic_load_barycentric_pixel
                                          ? map.def->num_components != 2
                                          : map.def->num_components != in->num_components))
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
        status = intrinsic(w, in, &map);
        if (status != SGX535_NIR_OK)
            return status;
        break;
    }
    default:
        return SGX535_NIR_ERR_UNSUPPORTED;
    }
    if (map.def) {
        if (lookup(w, map.def))
            return SGX535_NIR_ERR_SSA;
        w->map[w->map_count++] = map;
    }
    return SGX535_NIR_OK;
}
static enum sgx535_nir_status project(struct adapter_work *w, const nir_block *block)
{
    nir_foreach_instr(instr, block)
    {
        enum sgx535_nir_status status = project_instruction(w, instr);
        if (status != SGX535_NIR_OK)
            return status;
    }
    return SGX535_NIR_OK;
}

enum sgx535_nir_status sgx535_nir_fragment_sampled_ir(const nir_shader *shader,
                                                      struct sgx535_fragment_instruction *output,
                                                      size_t capacity, size_t *count,
                                                      struct sgx535_fragment_sampled_input *sample)
{
    struct adapter_work *w;
    const nir_block *block;
    enum sgx535_nir_status status;
    uintptr_t a = (uintptr_t)output, b = (uintptr_t)count, c = (uintptr_t)sample;
    size_t bytes;
    if (!shader || !output || !count || !sample)
        return SGX535_NIR_ERR_ARGUMENT;
    if (!capacity || capacity > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    bytes = capacity * sizeof(*output);
    if (bytes > UINTPTR_MAX - a || sizeof(*count) > UINTPTR_MAX - b ||
        sizeof(*sample) > UINTPTR_MAX - c || (a < b + sizeof(*count) && b < a + bytes) ||
        (a < c + sizeof(*sample) && c < a + bytes) ||
        (b < c + sizeof(*sample) && c < b + sizeof(*count)))
        return SGX535_NIR_ERR_ARGUMENT;
    block = linear_entry(shader, true);
    if (!block)
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    w->sampled = true;
    w->io_precision = SGX535_FRAGMENT_IO_F32;
    status = project(w, block);
    if (status == SGX535_NIR_OK)
        status = w->sampled_seen ? sgx535_fragment_sampled_validate(w->stream, w->count, &w->sample)
                                 : SGX535_NIR_ERR_UNSUPPORTED;
    if (status == SGX535_NIR_OK && w->count > capacity)
        status = SGX535_NIR_ERR_LIMIT;
    if (status == SGX535_NIR_OK) {
        memcpy(output, w->stream, w->count * sizeof(*output));
        *count = w->count;
        *sample = w->sample;
    }
    free(w);
    return status;
}

/* Validate the selected original diamond's non-Boolean instructions with the
 * same ALU/I/O/uniform consumers as linear compilation. Boolean/phi entries
 * were proved by the caller and receive opaque MOV placeholders: they must
 * never masquerade as a direct constant uniform offset. This temporary IR is
 * validation state only and is neither returned nor executed. */
enum sgx535_nir_status sgx535_nir_fragment_pure_original_options_contract(
    const nir_instr *const *instructions, const bool *opaque, size_t count,
    const struct sgx535_fragment_nir_options *options)
{
    struct adapter_work *w;
    enum sgx535_nir_status status = SGX535_NIR_OK;
    if (!instructions || !opaque || !options)
        return SGX535_NIR_ERR_ARGUMENT;
    if (!count || count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    if ((options->uniform_layout != SGX535_FRAGMENT_UNIFORM_DWORDS &&
         options->uniform_layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS) ||
        (options->io_precision != SGX535_FRAGMENT_IO_STRICT &&
         options->io_precision != SGX535_FRAGMENT_IO_F32))
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    w->linked = w->raw_sign = w->raw_sign_inf = w->bool32 = w->bool32_ieq = true;
    w->int32_uniforms = true;
    w->uniform_layout = options->uniform_layout;
    w->io_precision = options->io_precision;
    for (size_t i = 0; i < count && status == SGX535_NIR_OK; i++) {
        const nir_instr *instr = instructions[i];
        if (!opaque[i]) {
            status = project_instruction(w, instr);
        } else {
            struct definition_map map = {0};
            uint32_t zero;
            map.def = nir_instr_def((nir_instr *)instr);
            if (!map.def || w->map_count == SGX535_NIR_MAX_INSTRUCTIONS) {
                status = SGX535_NIR_ERR_LIMIT;
                break;
            }
            status = constant(w, 0, &zero);
            for (unsigned c = 0; c < map.def->num_components && status == SGX535_NIR_OK; c++) {
                struct sgx535_fragment_instruction move = {0};
                move.opcode = SGX535_FRAGMENT_MOV;
                move.source[0] = zero;
                status = append(w, move, &map.scalar[c]);
            }
            if (status == SGX535_NIR_OK)
                w->map[w->map_count++] = map;
        }
    }
    free(w);
    return status;
}
static void dense_linkage(struct adapter_work *w)
{
    struct sgx535_fragment_linkage ordered = {0};
    uint32_t remap[SGX535_FRAGMENT_INPUT_SLOTS] = {0};
    unsigned i, j;
    ordered.count = w->linkage.count;
    for (i = 0; i < w->linkage.count; i++) {
        const struct sgx535_fragment_varying *v = &w->linkage.varying[i];
        unsigned rank = 0;
        for (j = 0; j < w->linkage.count; j++) {
            const struct sgx535_fragment_varying *other = &w->linkage.varying[j];
            if (other->semantic < v->semantic ||
                (other->semantic == v->semantic && other->index < v->index))
                rank++;
        }
        ordered.varying[rank] = *v;
        remap[i] = rank;
    }
    for (i = 0; i < w->count; i++)
        if (w->stream[i].opcode == SGX535_FRAGMENT_INPUT)
            w->stream[i].index = remap[w->stream[i].index];
    w->linkage = ordered;
}
enum sgx535_nir_status sgx535_nir_project_fragment(const nir_shader *shader,
                                                   enum sgx535_fragment_output_profile profile,
                                                   struct sgx535_fragment_program *output)
{
    const nir_block *block;
    struct adapter_work *w;
    enum sgx535_nir_status status;
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    if (profile != SGX535_FRAGMENT_RAW_F32_WORDS && profile != SGX535_FRAGMENT_U8_WORD_PROJECTION)
        return SGX535_NIR_ERR_UNSUPPORTED;
    block = entry(shader);
    if (!block)
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    status = project(w, block);
    if (status == SGX535_NIR_OK)
        status = sgx535_fragment_compile(w->stream, w->count, profile, output);
    free(w);
    return status;
}

static enum sgx535_nir_status
fragment_ir(const nir_shader *shader, struct sgx535_fragment_instruction *output, size_t capacity,
            size_t *count, struct sgx535_fragment_linkage *linkage,
            enum sgx535_fragment_uniform_layout uniform_layout, bool raw_sign, bool raw_sign_inf,
            bool bool32, bool bool32_ieq, enum sgx535_fragment_io_precision_policy io_precision,
            bool int32_uniforms, bool const_ushr)
{
    const nir_block *block;
    struct adapter_work *w;
    struct sgx535_fragment_program *checked;
    enum sgx535_nir_status status;
    uintptr_t a = (uintptr_t)output, b = (uintptr_t)count, c = (uintptr_t)linkage;
    size_t bytes;
    if (!shader || !output || !count)
        return SGX535_NIR_ERR_ARGUMENT;
    if (uniform_layout != SGX535_FRAGMENT_UNIFORM_DWORDS &&
        uniform_layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (io_precision != SGX535_FRAGMENT_IO_STRICT && io_precision != SGX535_FRAGMENT_IO_F32)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (!capacity || capacity > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    bytes = capacity * sizeof(*output);
    if (bytes > UINTPTR_MAX - a || sizeof(*count) > UINTPTR_MAX - b ||
        (a < b + sizeof(*count) && b < a + bytes))
        return SGX535_NIR_ERR_ARGUMENT;
    if (linkage &&
        (sizeof(*linkage) > UINTPTR_MAX - c || (a < c + sizeof(*linkage) && c < a + bytes) ||
         (b < c + sizeof(*linkage) && c < b + sizeof(*count))))
        return SGX535_NIR_ERR_ARGUMENT;
    block = entry(shader);
    if (!block)
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    checked = calloc(1, sizeof(*checked));
    if (!w || !checked) {
        free(w);
        free(checked);
        return SGX535_NIR_ERR_NOMEM;
    }
    w->linked = linkage != NULL;
    w->uniform_layout = uniform_layout;
    w->io_precision = io_precision;
    w->raw_sign = raw_sign;
    w->raw_sign_inf = raw_sign_inf;
    w->bool32 = bool32;
    w->bool32_ieq = bool32_ieq;
    w->int32_uniforms = int32_uniforms;
    w->const_ushr = const_ushr;
    status = project(w, block);
    if (status == SGX535_NIR_OK && w->linked)
        dense_linkage(w);
    if (status == SGX535_NIR_OK) {
        if (bool32_ieq || const_ushr)
            status = sgx535_fragment_compile_raw_ieq_candidate(w->stream, w->count, checked);
        else if (raw_sign)
            status = sgx535_fragment_compile_raw_sign_candidate(w->stream, w->count, checked);
        else
            status = sgx535_fragment_compile(w->stream, w->count, SGX535_FRAGMENT_RAW_F32_WORDS,
                                             checked);
    }
    if (status == SGX535_NIR_OK && w->count > capacity)
        status = SGX535_NIR_ERR_LIMIT;
    if (status == SGX535_NIR_OK) {
        memcpy(output, w->stream, w->count * sizeof(*output));
        *count = w->count;
        if (linkage)
            *linkage = w->linkage;
    }
    free(checked);
    free(w);
    return status;
}

/* A linked, strict-IO entry with the given admission flags. */
static enum sgx535_nir_status strict_linked(const nir_shader *shader,
                                            enum sgx535_fragment_uniform_layout layout,
                                            struct sgx535_fragment_instruction *output,
                                            size_t capacity, size_t *count,
                                            struct sgx535_fragment_linkage *linkage, bool raw_sign,
                                            bool inf, bool bool32, bool ieq)
{
    if (!linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    return fragment_ir(shader, output, capacity, count, linkage, layout, raw_sign, inf, bool32, ieq,
                       SGX535_FRAGMENT_IO_STRICT, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_ir(const nir_shader *shader,
                                              struct sgx535_fragment_instruction *output,
                                              size_t capacity, size_t *count)
{
    return fragment_ir(shader, output, capacity, count, NULL, SGX535_FRAGMENT_UNIFORM_DWORDS, false,
                       false, false, false, SGX535_FRAGMENT_IO_STRICT, false, false);
}

enum sgx535_nir_status
sgx535_nir_fragment_options_ir(const nir_shader *shader,
                               const struct sgx535_fragment_nir_options *options,
                               struct sgx535_fragment_instruction *output, size_t capacity,
                               size_t *count, struct sgx535_fragment_linkage *linkage)
{
    struct sgx535_fragment_nir_options selected;
    if (!options || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return fragment_ir(shader, output, capacity, count, linkage, selected.uniform_layout, false,
                       false, false, false, selected.io_precision, false, false);
}

enum sgx535_nir_status sgx535_nir_fragment_const_ushr_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage)
{
    struct sgx535_fragment_nir_options selected;
    if (!options || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return fragment_ir(shader, output, capacity, count, linkage, selected.uniform_layout, false,
                       false, false, false, selected.io_precision, false, true);
}

enum sgx535_nir_status sgx535_nir_fragment_raw_sign_ir(const nir_shader *shader,
                                                       enum sgx535_fragment_uniform_layout layout,
                                                       struct sgx535_fragment_instruction *output,
                                                       size_t capacity, size_t *count,
                                                       struct sgx535_fragment_linkage *linkage)
{
    return strict_linked(shader, layout, output, capacity, count, linkage, true, false, false,
                         false);
}

enum sgx535_nir_status
sgx535_nir_fragment_raw_sign_inf_ir(const nir_shader *shader,
                                    enum sgx535_fragment_uniform_layout layout,
                                    struct sgx535_fragment_instruction *output, size_t capacity,
                                    size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return strict_linked(shader, layout, output, capacity, count, linkage, true, true, false,
                         false);
}

enum sgx535_nir_status
sgx535_nir_fragment_bool32_lowered_ir(const nir_shader *shader,
                                      enum sgx535_fragment_uniform_layout layout,
                                      struct sgx535_fragment_instruction *output, size_t capacity,
                                      size_t *count, struct sgx535_fragment_linkage *linkage)
{
    return strict_linked(shader, layout, output, capacity, count, linkage, true, true, true, false);
}

enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_lowered_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage, bool int32_uniforms)
{
    struct sgx535_fragment_nir_options selected;
    if (!options || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return fragment_ir(shader, output, capacity, count, linkage, selected.uniform_layout, true,
                       true, true, true, selected.io_precision, int32_uniforms, false);
}

enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_const_ushr_lowered_options_ir(
    const nir_shader *shader, const struct sgx535_fragment_nir_options *options,
    struct sgx535_fragment_instruction *output, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage, bool int32_uniforms)
{
    struct sgx535_fragment_nir_options selected;
    if (!options || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    selected = *options;
    return fragment_ir(shader, output, capacity, count, linkage, selected.uniform_layout, true,
                       true, true, true, selected.io_precision, int32_uniforms, true);
}

/* Only the new producer bridge normalizes validated scalar SSA. The ordinary
 * adapter APIs retain their original instruction streams. There are no
 * side-effecting scalar operations: the final four OUTPUTs are the roots.
 * Check arity and backward edges independently before computing liveness;
 * future opcodes must not silently become dead, accepted instructions.
 * Scalar MOV has no modifiers in this validated IR, so an identity MOV maps
 * to its already defined source rather than allocating another instruction. */
static int dri_ir_arity(enum sgx535_fragment_opcode opcode, bool raw_sign, bool raw_mask)
{
    switch (opcode) {
    case SGX535_FRAGMENT_CONSTANT:
    case SGX535_FRAGMENT_INPUT:
    case SGX535_FRAGMENT_UNIFORM:
        return 0;
    case SGX535_FRAGMENT_MOV:
    case SGX535_FRAGMENT_RCP:
    case SGX535_FRAGMENT_RSQ:
    case SGX535_FRAGMENT_FRACT:
    case SGX535_FRAGMENT_OUTPUT:
        return 1;
    case SGX535_FRAGMENT_MIN:
    case SGX535_FRAGMENT_MAX:
        return 2;
    case SGX535_FRAGMENT_RAW_AND:
    case SGX535_FRAGMENT_RAW_XOR:
        return raw_sign ? 2 : -1;
    case SGX535_FRAGMENT_RAW_IEQ:
        return raw_mask ? 2 : -1;
    case SGX535_FRAGMENT_RAW_USHR_IMM:
        return raw_mask ? 1 : -1;
    case SGX535_FRAGMENT_MAD:
        return 3;
    default:
        return -1;
    }
}
static enum sgx535_nir_status fragment_dri_live_ir(struct sgx535_fragment_instruction *ir,
                                                   size_t *count, bool *live, uint32_t *remap,
                                                   bool raw_sign, bool raw_mask)
{
    size_t i, written = 0, n = *count;
    unsigned j;
    if (n < 5 || n > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    for (i = 0; i < n; i++) {
        int arity = dri_ir_arity(ir[i].opcode, raw_sign, raw_mask);
        if (arity < 0 || (ir[i].opcode == SGX535_FRAGMENT_OUTPUT) != (i >= n - 4))
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (j = 0; j < 3; j++) {
            if (j < (unsigned)arity) {
                if (ir[i].source[j] >= i || ir[ir[i].source[j]].opcode == SGX535_FRAGMENT_OUTPUT)
                    return SGX535_NIR_ERR_SSA;
            } else if (ir[i].source[j])
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
        live[i] = i >= n - 4;
        remap[i] = UINT32_MAX;
    }
    for (i = n; i-- > 0;) {
        int arity = dri_ir_arity(ir[i].opcode, raw_sign, raw_mask);
        if (!live[i])
            continue;
        for (j = 0; j < (unsigned)arity; j++)
            live[ir[i].source[j]] = true;
    }
    for (i = 0; i < n; i++) {
        struct sgx535_fragment_instruction instruction;
        int arity;
        if (!live[i])
            continue;
        instruction = ir[i];
        arity = dri_ir_arity(instruction.opcode, raw_sign, raw_mask);
        for (j = 0; j < (unsigned)arity; j++) {
            uint32_t source = remap[instruction.source[j]];
            if (source >= written)
                return SGX535_NIR_ERR_SSA;
            instruction.source[j] = source;
        }
        if (instruction.opcode == SGX535_FRAGMENT_MOV) {
            remap[i] = instruction.source[0];
            continue;
        }
        remap[i] = (uint32_t)written;
        ir[written++] = instruction;
    }
    *count = written;
    return SGX535_NIR_OK;
}

enum sgx535_nir_status sgx535_fragment_dri_live_ir(struct sgx535_fragment_instruction *ir,
                                                   size_t *count, bool *live, uint32_t *remap,
                                                   bool raw_sign)
{
    return fragment_dri_live_ir(ir, count, live, remap, raw_sign, false);
}

enum sgx535_nir_status sgx535_fragment_dri_raw_mask_live_ir(struct sgx535_fragment_instruction *ir,
                                                            size_t *count, bool *live,
                                                            uint32_t *remap)
{
    if (!ir || !count || !live || !remap)
        return SGX535_NIR_ERR_ARGUMENT;
    return fragment_dri_live_ir(ir, count, live, remap, true, true);
}

struct dri_nir_work {
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    bool live[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t remap[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_dri_nir_program result;
    size_t count;
};
static enum sgx535_nir_status dri_nir_extract(const nir_shader *shader,
                                              enum sgx535_fragment_uniform_layout layout,
                                              enum sgx535_fragment_interpolation color_mode,
                                              enum sgx535_fragment_output_profile profile,
                                              struct dri_nir_work *w)
{
    struct sgx535_fragment_linkage canonical;
    enum sgx535_nir_status status;
    if ((color_mode != SGX535_FRAGMENT_SMOOTH && color_mode != SGX535_FRAGMENT_FLAT) ||
        (layout != SGX535_FRAGMENT_UNIFORM_DWORDS &&
         layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS) ||
        (profile != SGX535_FRAGMENT_RAW_F32_WORDS && profile != SGX535_FRAGMENT_U8_WORD_PROJECTION))
        return SGX535_NIR_ERR_UNSUPPORTED;
    status = fragment_ir(shader, w->ir, SGX535_NIR_MAX_INSTRUCTIONS, &w->count,
                         &w->result.source_linkage, layout, false, false, false, false,
                         SGX535_FRAGMENT_IO_STRICT, false, false);
    if (status == SGX535_NIR_OK) {
        canonical = w->result.source_linkage;
        /* The adapter just produced both dense IR indices and this sidecar
         * from the same immutable graph. Only after that checked association
         * can the matched producer bind logical slot0 independently of its
         * original NIR driver base. Keep the original sidecar as provenance. */
        if (canonical.count != 1 ||
            (canonical.varying[0].interpolation != SGX535_FRAGMENT_COLOR_INTERPOLATION &&
             canonical.varying[0].interpolation != color_mode)) {
            status = SGX535_NIR_ERR_UNSUPPORTED;
        } else {
            canonical.varying[0].driver_base = 0;
            /* Flat attributes are replicated by Mesa draw before transport.
             * This local word producer still consumes the same COLOR0 input;
             * keep source and resolved interpolation in the result sidecar.
             * Only a checked backend may admit the replicated geometry. */
            canonical.varying[0].interpolation = SGX535_FRAGMENT_SMOOTH;
            status = sgx535_fragment_dri_live_ir(w->ir, &w->count, w->live, w->remap, false);
            if (status == SGX535_NIR_OK)
                status = sgx535_fragment_compile_dri_color0(w->ir, w->count, &canonical, profile,
                                                            &w->result.program);
        }
    }
    if (status == SGX535_NIR_OK)
        w->result.resolved_interpolation = color_mode;
    return status;
}
enum sgx535_nir_status sgx535_nir_project_dri_color0(const nir_shader *shader,
                                                     enum sgx535_fragment_uniform_layout layout,
                                                     enum sgx535_fragment_interpolation color_mode,
                                                     enum sgx535_fragment_output_profile profile,
                                                     struct sgx535_fragment_dri_nir_program *output)
{
    struct dri_nir_work *w;
    enum sgx535_nir_status status;
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    /* Keep old argument/profile precedence ahead of allocation. */
    if ((color_mode != SGX535_FRAGMENT_SMOOTH && color_mode != SGX535_FRAGMENT_FLAT) ||
        (layout != SGX535_FRAGMENT_UNIFORM_DWORDS &&
         layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS) ||
        (profile != SGX535_FRAGMENT_RAW_F32_WORDS && profile != SGX535_FRAGMENT_U8_WORD_PROJECTION))
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    status = dri_nir_extract(shader, layout, color_mode, profile, w);
    if (status == SGX535_NIR_OK)
        *output = w->result;
    free(w);
    return status;
}
enum sgx535_nir_status sgx535_nir_dri_color0_ir(const nir_shader *shader,
                                                enum sgx535_fragment_uniform_layout layout,
                                                enum sgx535_fragment_interpolation color_mode,
                                                struct sgx535_fragment_dri_nir_ir *output)
{
    struct dri_nir_work *w;
    enum sgx535_nir_status status;
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    if ((color_mode != SGX535_FRAGMENT_SMOOTH && color_mode != SGX535_FRAGMENT_FLAT) ||
        (layout != SGX535_FRAGMENT_UNIFORM_DWORDS && layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS))
        return SGX535_NIR_ERR_UNSUPPORTED;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    status = dri_nir_extract(shader, layout, color_mode, SGX535_FRAGMENT_U8_WORD_PROJECTION, w);
    if (status == SGX535_NIR_OK) {
        memset(output, 0, sizeof(*output));
        memcpy(output->instruction, w->ir, w->count * sizeof(*w->ir));
        output->instruction_count = (uint32_t)w->count;
        output->source_linkage = w->result.source_linkage;
        output->resolved_interpolation = w->result.resolved_interpolation;
    }
    free(w);
    return status;
}
