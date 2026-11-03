// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_nir.h"
#include "sgx535_fragment_internal.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "pipe/p_screen.h"
#include "nir/nir_to_tgsi.h"
#include "nir.h"
#include "nir_builder.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PB_UNIFORM_ADDRESS_NODES 32u
struct uniform_address {
    nir_alu_instr *node[PB_UNIFORM_ADDRESS_NODES];
    uint32_t value[PB_UNIFORM_ADDRESS_NODES];
    unsigned count;
};
static bool uniform_address_value(nir_def *def, struct uniform_address *tree, unsigned depth,
                                  uint32_t *value)
{
    nir_instr *instr;
    nir_alu_instr *alu;
    uint32_t a, b;
    uint64_t result;
    unsigned i;
    if (!def || def->bit_size != 32 || def->num_components != 1 ||
        depth >= PB_UNIFORM_ADDRESS_NODES)
        return false;
    instr = nir_def_instr(def);
    if (instr->type == nir_instr_type_load_const) {
        *value = nir_instr_as_load_const(instr)->value[0].u32;
        return true;
    }
    if (instr->type != nir_instr_type_alu)
        return false;
    alu = nir_instr_as_alu(instr);
    for (i = 0; i < tree->count; i++)
        if (tree->node[i] == alu) {
            *value = tree->value[i];
            return true;
        }
    if (tree->count == PB_UNIFORM_ADDRESS_NODES || alu->no_signed_wrap || alu->no_unsigned_wrap ||
        alu->fp_math_ctrl ||
        (alu->op != nir_op_iadd && alu->op != nir_op_imul && alu->op != nir_op_ishl) ||
        alu->src[0].swizzle[0] || alu->src[1].swizzle[0])
        return false;
    if (!uniform_address_value(alu->src[0].src.ssa, tree, depth + 1, &a) ||
        !uniform_address_value(alu->src[1].src.ssa, tree, depth + 1, &b) ||
        tree->count == PB_UNIFORM_ADDRESS_NODES)
        return false;
    /* This is a compile-time unsigned32 address model, not fragment math.
     * Conservatively retain wrapping/overshift/flagged expressions for the
     * strict adapter to refuse, even where unconstrained NIR defines wrap. */
    if (alu->op == nir_op_iadd)
        result = (uint64_t)a + b;
    else if (alu->op == nir_op_imul)
        result = (uint64_t)a * b;
    else {
        if (b >= 32)
            return false;
        result = (uint64_t)a << b;
    }
    if (result > UINT32_MAX)
        return false;
    tree->node[tree->count] = alu;
    tree->value[tree->count++] = (uint32_t)result;
    *value = (uint32_t)result;
    return true;
}
static void uniform_addresses(nir_shader *shader)
{
    nir_foreach_function_impl(impl, shader)
    {
        nir_builder b = nir_builder_create(impl);
        bool changed = false;
        nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block)
        {
            struct uniform_address tree = {0};
            uint32_t value;
            unsigned i;
            nir_intrinsic_instr *in;
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_uniform ||
                nir_def_instr(in->src[0].ssa)->type == nir_instr_type_load_const ||
                !uniform_address_value(in->src[0].ssa, &tree, 0, &value))
                continue;
            b.cursor = nir_before_instr(instr);
            nir_src_rewrite(&in->src[0], nir_imm_intN_t(&b, value, 32));
            /* Postorder collection lets us remove a parent before its inputs.
             * Shared uses keep their original instructions and constraints;
             * no unrelated dead instruction is pruned by this pass. */
            for (i = tree.count; i-- > 0;)
                if (nir_def_is_unused(&tree.node[i]->def)) {
                    nir_instr_remove(&tree.node[i]->instr);
                    nir_instr_free(&tree.node[i]->instr);
                }
            changed = true;
        }
        nir_progress(changed, impl, nir_metadata_control_flow);
    }
}

static const nir_shader_compiler_options fragment_options = {
    .io_options = nir_io_has_intrinsics,
};

void poulsback_nir_finalize_fragment(nir_shader *nir)
{
    if (!nir || nir->info.stage != MESA_SHADER_FRAGMENT)
        return;
    /* State tracker already owns IO locations and uniform address units.
     * Keep fragment ALU/texture/control-flow instructions for strict validation;
     * remove obsolete variable indirection/declarations, then normalize only
     * the bounded integer uniform addresses below. This is not opt_dce. */
    nir_lower_global_vars_to_local(nir);
    nir_lower_vars_to_ssa(nir);
    nir_remove_dead_derefs(nir);
    nir_remove_dead_variables(nir,
                              nir_var_shader_in | nir_var_shader_out | nir_var_uniform |
                                  nir_var_function_temp | nir_var_shader_temp,
                              NULL);
    /* Mesa's ordinary uniform lowering generates constant integer address
     * trees for aggregate elements. Normalize only that address calculation;
     * floating instructions and their original constraints remain untouched. */
    uniform_addresses(nir);
}

struct fragment_compile {
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_linkage linkage;
    struct sgx535_fragment_program program;
    size_t count;
};

static enum sgx535_nir_status solid_fragment_ir(const nir_shader *source,
                                                enum sgx535_fragment_uniform_layout layout,
                                                bool uniforms,
                                                struct poulsback_nir_solid_fragment *out)
{
    const struct sgx535_fragment_nir_options options = {layout, SGX535_FRAGMENT_IO_F32};
    struct sgx535_fragment_linkage linkage;
    size_t count = 0;
    enum sgx535_nir_status status = sgx535_nir_fragment_options_ir(
        source, &options, out->instruction, SGX535_NIR_MAX_INSTRUCTIONS, &count, &linkage);
    if (status != SGX535_NIR_OK)
        return status;
    if (linkage.count)
        return SGX535_NIR_ERR_UNSUPPORTED;
    for (size_t i = 0; i < count; i++) {
        enum sgx535_fragment_opcode op = out->instruction[i].opcode;
        if (op != SGX535_FRAGMENT_CONSTANT && op != SGX535_FRAGMENT_MOV &&
            op != SGX535_FRAGMENT_OUTPUT &&
            !(uniforms && (op == SGX535_FRAGMENT_UNIFORM || op == SGX535_FRAGMENT_MAD)))
            return SGX535_NIR_ERR_UNSUPPORTED;
    }
    status = sgx535_fragment_compile_no_input_normalized_candidate(out->instruction, count,
                                                                   &out->program);
    if (status == SGX535_NIR_OK)
        out->instruction_count = (uint32_t)count;
    return status;
}

static enum sgx535_nir_status solid_projection(const nir_shader *source,
                                               enum sgx535_fragment_uniform_layout layout,
                                               bool uniforms,
                                               struct poulsback_nir_solid_fragment *out)
{
    struct poulsback_nir_solid_fragment *work;
    enum sgx535_nir_status status;
    if (!source || !out)
        return SGX535_NIR_ERR_ARGUMENT;
    work = calloc(1, sizeof(*work));
    if (!work)
        return SGX535_NIR_ERR_NOMEM;
    status = solid_fragment_ir(source, layout, uniforms, work);
    if (status == SGX535_NIR_OK && uniforms) {
        bool has_uniform = false, has_mad = false;
        for (unsigned i = 0; i < work->instruction_count; i++) {
            has_uniform |= work->instruction[i].opcode == SGX535_FRAGMENT_UNIFORM;
            has_mad |= work->instruction[i].opcode == SGX535_FRAGMENT_MAD;
        }
        /* The context specializes only CSOs with a captured uniform source. */
        if (has_mad && !has_uniform)
            status = SGX535_NIR_ERR_UNSUPPORTED;
    }
    if (status == SGX535_NIR_OK)
        *out = *work;
    free(work);
    return status;
}

enum sgx535_nir_status poulsback_nir_solid_fragment_ir(const nir_shader *source,
                                                       enum sgx535_fragment_uniform_layout layout,
                                                       struct poulsback_nir_solid_fragment *out)
{
    return solid_projection(source, layout, false, out);
}

enum sgx535_nir_status
poulsback_nir_solid_fragment_source_ir(const nir_shader *source,
                                       enum sgx535_fragment_uniform_layout layout,
                                       struct poulsback_nir_solid_fragment *out)
{
    return solid_projection(source, layout, true, out);
}

static int specialize_fragment(const nir_shader *source, enum sgx535_fragment_uniform_layout layout,
                               const void *data, size_t bytes, nir_shader **out)
{
    struct sgx535_fragment_instruction *ir;
    struct sgx535_fragment_linkage linkage;
    uint32_t words[SGX535_FRAGMENT_UNIFORM_WORDS] = {0};
    nir_shader *copy;
    size_t count = 0;
    enum sgx535_nir_status status;
    unsigned stride = layout == SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS ? 4u : 1u;
    if (!source || !out || (bytes && !data) || bytes % sizeof(*words) || bytes > sizeof(words))
        return -EINVAL;
    ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    if (!ir)
        return -ENOMEM;
    status = sgx535_nir_fragment_raw_sign_inf_ir(source, layout, ir, SGX535_NIR_MAX_INSTRUCTIONS,
                                                 &count, &linkage);
    if (status != SGX535_NIR_OK) {
        free(ir);
        return status == SGX535_NIR_ERR_NOMEM ? -ENOMEM : -EOPNOTSUPP;
    }
    /* Use the adapter's checked scalar word addresses, not the shader's
     * declared range as a promise that caller memory exists. Keep dead loads
     * in this check: specializing cannot hide an unavailable source word. */
    for (size_t i = 0; i < count; i++)
        if (ir[i].opcode == SGX535_FRAGMENT_UNIFORM && ir[i].index >= bytes / sizeof(*words)) {
            free(ir);
            return -EOPNOTSUPP;
        }
    free(ir);
    if (bytes)
        memcpy(words, data, bytes);
    copy = nir_shader_clone(NULL, source);
    if (!copy)
        return -ENOMEM;
    nir_foreach_function_impl(impl, copy)
    {
        nir_builder b = nir_builder_create(impl);
        bool changed = false;
        nir_foreach_block(block, impl) nir_foreach_instr_safe(instr, block)
        {
            nir_intrinsic_instr *in;
            nir_def *replacement;
            nir_const_value value[4] = {0};
            uint64_t first;
            if (instr->type != nir_instr_type_intrinsic)
                continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_uniform)
                continue;
            /* Recheck active clone semantics independently. NIR cloning may
             * change representation, never the addressing contract. No byte
             * base is invented: Mesa's cap chooses DWORD or vec4-slot units. */
            if (!nir_src_is_const(in->src[0]) || in->def.bit_size != 32 || !in->num_components ||
                in->num_components > 4)
                goto invalid;
            first = ((uint64_t)nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * stride;
            if (first > bytes / sizeof(*words) ||
                in->num_components > bytes / sizeof(*words) - first)
                goto invalid;
            for (unsigned c = 0; c < in->num_components; c++)
                value[c].u32 = words[first + c];
            b.cursor = nir_before_instr(instr);
            replacement = nir_build_imm(&b, in->num_components, 32, value);
            if (!replacement) {
                ralloc_free(copy);
                return -ENOMEM;
            }
            nir_def_rewrite_uses(&in->def, replacement);
            nir_instr_remove(instr);
            nir_instr_free(instr);
            changed = true;
        }
        nir_progress(changed, impl, nir_metadata_control_flow);
    }
    *out = copy;
    return 0;
invalid:
    ralloc_free(copy);
    return -EOPNOTSUPP;
}

int poulsback_nir_specialize_fragment(const nir_shader *source,
                                      enum sgx535_fragment_uniform_layout layout, const void *data,
                                      size_t bytes, nir_shader **out)
{
    return specialize_fragment(source, layout, data, bytes, out);
}

/* The vertex stage's options: nir_to_tgsi's (the draw module converts the
 * same shaders with them), and loops with a constant bound unrolled at link:
 * the vertex backend has no branches. */
static nir_shader_compiler_options vertex_options;

static void finalize(struct pipe_screen *screen, nir_shader *nir, bool optimize)
{
    (void)screen;
    (void)optimize;
    poulsback_nir_finalize_fragment(nir);
}

void poulsback_nir_screen_init(struct pipe_screen *screen)
{
    vertex_options = nir_to_tgsi_compiler_options;
    vertex_options.max_unroll_iterations = 32;
    screen->nir_options[MESA_SHADER_VERTEX] = &vertex_options;
    screen->nir_options[MESA_SHADER_FRAGMENT] = &fragment_options;
    screen->finalize_nir = finalize;
}
