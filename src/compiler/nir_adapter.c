// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa NIR is an optional external dependency. Translation preserves
 * the source graph and passes a validated flat profile to the CPU compiler.
 */
#include "nir.h"
#include "sgx535_linear.h"
#include <string.h>

static const nir_block *linear_entry(const nir_shader *shader, enum sgx535_linear_profile profile)
{
    nir_function *entry;
    nir_cf_node *node;
    nir_block *block;
    if (shader->info.stage != MESA_SHADER_FRAGMENT || !exec_list_is_empty(&shader->variables) ||
        !exec_list_is_singular(&shader->functions))
        return NULL;
    if (profile == SGX535_LINEAR_F32_WORD_PROJECTION && shader->info.float_controls_execution_mode)
        return NULL;
    entry = exec_node_data(nir_function, shader->functions.head_sentinel.next, node);
    if (!entry->is_entrypoint || entry->num_params || !entry->impl || entry->shader != shader ||
        entry->impl->function != entry || entry->impl->preamble || !entry->impl->structured ||
        !exec_list_is_empty(&entry->impl->locals) || !exec_list_is_singular(&entry->impl->body))
        return NULL;
    node = exec_node_data(nir_cf_node, entry->impl->body.head_sentinel.next, node);
    if (node->type != nir_cf_node_block)
        return NULL;
    block = nir_cf_node_as_block(node);
    if (block->impl != entry->impl || !entry->impl->end_block ||
        block->successors[0] != entry->impl->end_block || block->successors[1] ||
        !exec_list_is_empty(&entry->impl->end_block->instr_list))
        return NULL;
    return block;
}

static bool scalar(const nir_def *definition)
{
    return definition && definition->num_components == 1 && definition->bit_size == 32;
}

static int definition_index(const nir_def *definition, const nir_def *const *definitions,
                            unsigned count)
{
    unsigned i;
    if (!definition)
        return -1;
    for (i = 0; i < count; ++i)
        if (definition == definitions[i])
            return (int)i;
    return -1;
}

static bool output_contract(const nir_intrinsic_instr *instruction,
                            enum sgx535_linear_profile profile)
{
    nir_intrinsic_instr expected = {0};
    nir_io_semantics semantics = {0};
    expected.intrinsic = nir_intrinsic_store_output;
    semantics.location = FRAG_RESULT_DATA0;
    semantics.num_slots = 1;
    nir_intrinsic_set_range(&expected, 1);
    nir_intrinsic_set_write_mask(&expected, 1);
    nir_intrinsic_set_src_type(&expected, profile == SGX535_LINEAR_INTEGER ? nir_type_uint32
                                                                           : nir_type_float32);
    nir_intrinsic_set_io_semantics(&expected, semantics);
    return instruction->num_components == 1 &&
           !memcmp(instruction->const_index, expected.const_index, sizeof(expected.const_index));
}

static int alu_profile(nir_op op, enum sgx535_linear_profile profile,
                       enum sgx535_linear_opcode *opcode)
{
    if (op == nir_op_mov) {
        *opcode = SGX535_LINEAR_MOV;
        return 1;
    }
    if (profile != SGX535_LINEAR_F32_WORD_PROJECTION)
        return -1;
    switch (op) {
    case nir_op_ffma:
        *opcode = SGX535_LINEAR_MAD;
        return 3;
    case nir_op_fmin:
        *opcode = SGX535_LINEAR_MIN;
        return 2;
    case nir_op_fmax:
        *opcode = SGX535_LINEAR_MAX;
        return 2;
    case nir_op_frcp:
        *opcode = SGX535_LINEAR_RCP;
        return 1;
    default:
        return -1;
    }
}

static enum sgx535_nir_status project(const nir_shader *shader, enum sgx535_linear_profile profile,
                                      struct sgx535_nir_program *output)
{
    struct sgx535_linear_instruction stream[SGX535_NIR_MAX_INSTRUCTIONS] = {0};
    const nir_def *definitions[SGX535_NIR_MAX_INSTRUCTIONS] = {0};
    const nir_block *block;
    unsigned count = 0;
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    block = linear_entry(shader, profile);
    if (!block)
        return SGX535_NIR_ERR_UNSUPPORTED;
    nir_foreach_instr(instruction, block)
    {
        const nir_def *definition = NULL, *sources[3] = {0};
        struct sgx535_linear_instruction *target;
        unsigned i, source_count = 0;
        if (count == SGX535_NIR_MAX_INSTRUCTIONS)
            return SGX535_NIR_ERR_LIMIT;
        target = &stream[count];
        switch (instruction->type) {
        case nir_instr_type_load_const: {
            const nir_load_const_instr *load = nir_instr_as_load_const(instruction);
            definition = &load->def;
            if (!scalar(definition))
                return SGX535_NIR_ERR_UNSUPPORTED;
            target->opcode = SGX535_LINEAR_CONSTANT;
            target->immediate = load->value[0].u32;
            break;
        }
        case nir_instr_type_alu: {
            const nir_alu_instr *alu = nir_instr_as_alu(instruction);
            int arity = alu_profile(alu->op, profile, &target->opcode);
            if (arity < 0 || alu->no_signed_wrap || alu->no_unsigned_wrap || alu->fp_math_ctrl)
                return SGX535_NIR_ERR_UNSUPPORTED;
            definition = &alu->def;
            source_count = (unsigned)arity;
            for (i = 0; i < source_count; ++i) {
                if (alu->src[i].swizzle[0])
                    return SGX535_NIR_ERR_UNSUPPORTED;
                sources[i] = alu->src[i].src.ssa;
            }
            break;
        }
        case nir_instr_type_intrinsic: {
            const nir_intrinsic_instr *store = nir_instr_as_intrinsic(instruction);
            int offset;
            if (store->intrinsic != nir_intrinsic_store_output || !nir_instr_is_last(instruction) ||
                !output_contract(store, profile))
                return SGX535_NIR_ERR_UNSUPPORTED;
            offset = definition_index(store->src[1].ssa, definitions, count);
            if (offset < 0)
                return SGX535_NIR_ERR_SSA;
            if (stream[offset].opcode != SGX535_LINEAR_CONSTANT || stream[offset].immediate)
                return SGX535_NIR_ERR_UNSUPPORTED;
            target->opcode = SGX535_LINEAR_OUTPUT;
            sources[0] = store->src[0].ssa;
            source_count = 1;
            break;
        }
        default:
            return SGX535_NIR_ERR_UNSUPPORTED;
        }
        if (definition && !scalar(definition))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (definition && definition_index(definition, definitions, count) >= 0)
            return SGX535_NIR_ERR_SSA;
        for (i = 0; i < source_count; ++i) {
            int source;
            if (!sources[i])
                return SGX535_NIR_ERR_SSA;
            if (!scalar(sources[i]))
                return SGX535_NIR_ERR_UNSUPPORTED;
            source = definition_index(sources[i], definitions, count);
            if (source < 0)
                return SGX535_NIR_ERR_SSA;
            target->source[i] = (uint32_t)source;
        }
        definitions[count++] = definition;
    }
    if (!count)
        return SGX535_NIR_ERR_UNSUPPORTED;
    return sgx535_linear_compile(stream, count, profile, output);
}

enum sgx535_nir_status sgx535_nir_compile(const nir_shader *shader,
                                          struct sgx535_nir_program *output)
{
    return project(shader, SGX535_LINEAR_INTEGER, output);
}

enum sgx535_nir_status sgx535_nir_project_f32(const nir_shader *shader,
                                              struct sgx535_nir_f32_projection *output)
{
    struct sgx535_nir_f32_projection result = {0};
    enum sgx535_nir_status status;
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    status = project(shader, SGX535_LINEAR_F32_WORD_PROJECTION, &result.encoding);
    if (status == SGX535_NIR_OK)
        *output = result;
    return status;
}
