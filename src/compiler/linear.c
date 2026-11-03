// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_linear.h"
#include "sgx535_usse.h"
#include <stdbool.h>
#include <stdlib.h>

struct linear_work {
    struct sgx535_nir_program result;
    uint32_t last_use[SGX535_NIR_MAX_INSTRUCTIONS];
    unsigned registers[SGX535_NIR_MAX_INSTRUCTIONS];
    bool occupied[SGX535_NIR_TEMP_COUNT];
    unsigned live;
};

static int source_count(enum sgx535_linear_opcode opcode)
{
    switch (opcode) {
    case SGX535_LINEAR_CONSTANT:
        return 0;
    case SGX535_LINEAR_MOV:
    case SGX535_LINEAR_RCP:
    case SGX535_LINEAR_OUTPUT:
        return 1;
    case SGX535_LINEAR_MIN:
    case SGX535_LINEAR_MAX:
        return 2;
    case SGX535_LINEAR_MAD:
        return 3;
    default:
        return -1;
    }
}

static enum sgx535_nir_status validate(const struct sgx535_linear_instruction *input, size_t count,
                                       enum sgx535_linear_profile profile, struct linear_work *work)
{
    size_t i;
    unsigned j;
    if (!count || count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    if (profile != SGX535_LINEAR_INTEGER && profile != SGX535_LINEAR_F32_WORD_PROJECTION)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (input[count - 1u].opcode != SGX535_LINEAR_OUTPUT)
        return SGX535_NIR_ERR_UNSUPPORTED;
    for (i = 0; i < count; ++i) {
        int sources = source_count(input[i].opcode);
        if (sources < 0 || (input[i].opcode == SGX535_LINEAR_OUTPUT && i != count - 1u))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (profile == SGX535_LINEAR_INTEGER && input[i].opcode >= SGX535_LINEAR_MAD &&
            input[i].opcode <= SGX535_LINEAR_RCP)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (input[i].opcode != SGX535_LINEAR_CONSTANT && input[i].immediate)
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (j = 0; j < 3u; ++j) {
            if (j < (unsigned)sources) {
                uint32_t source = input[i].source[j];
                if (source >= i || input[source].opcode == SGX535_LINEAR_OUTPUT)
                    return SGX535_NIR_ERR_SSA;
                work->last_use[source] = (uint32_t)i;
            } else if (input[i].source[j])
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
        work->registers[i] = SGX535_NIR_TEMP_COUNT;
    }
    return SGX535_NIR_OK;
}

static int reserve(struct linear_work *work)
{
    unsigned reg;
    for (reg = 0; reg < SGX535_NIR_TEMP_COUNT; ++reg) {
        if (work->occupied[reg])
            continue;
        work->occupied[reg] = true;
        ++work->live;
        if (work->live > work->result.peak_live_temps)
            work->result.peak_live_temps = work->live;
        if (reg + 1u > work->result.temp_register_count)
            work->result.temp_register_count = reg + 1u;
        return (int)reg;
    }
    return -1;
}

static void release_dead(struct linear_work *work, size_t position)
{
    size_t i;
    for (i = 0; i <= position; ++i) {
        unsigned reg = work->registers[i];
        if (reg < SGX535_NIR_TEMP_COUNT && work->last_use[i] <= position) {
            work->occupied[reg] = false;
            work->registers[i] = SGX535_NIR_TEMP_COUNT;
            --work->live;
        }
    }
}

static enum sgx535_nir_status translate(const struct sgx535_linear_instruction *input, size_t count,
                                        struct linear_work *work)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        const struct sgx535_linear_instruction *op = &input[i];
        uint32_t words[2];
        int destination = op->opcode == SGX535_LINEAR_OUTPUT ? 0 : reserve(work);
        if (destination < 0)
            return SGX535_NIR_ERR_PRESSURE;
        if (op->opcode != SGX535_LINEAR_OUTPUT)
            work->registers[i] = (unsigned)destination;
        if (op->opcode == SGX535_LINEAR_CONSTANT) {
            struct sgx535_limm instruction = {SGX535_LIMM_TEMP, (uint32_t)destination,
                                              op->immediate, 0};
            if (sgx535_limm_encode(&instruction, words) != SGX535_LIMM_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_LINEAR_MOV || op->opcode == SGX535_LINEAR_OUTPUT) {
            struct sgx535_mov instruction = {
                {op->opcode == SGX535_LINEAR_OUTPUT ? SGX535_MOV_OUTPUT : SGX535_MOV_TEMP,
                 (uint32_t)destination},
                {SGX535_MOV_TEMP, work->registers[op->source[0]]},
                op->opcode == SGX535_LINEAR_OUTPUT ? 1u : 0u};
            if (sgx535_mov_encode(&instruction, words) != SGX535_MOV_OK)
                return SGX535_NIR_ERR_SSA;
        } else {
            static const enum sgx535_f32_opcode mapping[] = {SGX535_F32_MAD, SGX535_F32_MIN,
                                                             SGX535_F32_MAX, SGX535_F32_RCP};
            struct sgx535_f32 instruction = {0};
            unsigned j, sources = (unsigned)source_count(op->opcode);
            instruction.opcode = mapping[op->opcode - SGX535_LINEAR_MAD];
            instruction.destination =
                (struct sgx535_f32_operand){SGX535_F32_TEMP, (uint32_t)destination};
            for (j = 0; j < sources; ++j)
                instruction.source[j].number = work->registers[op->source[j]];
            if (sgx535_f32_encode(&instruction, words) != SGX535_F32_OK)
                return SGX535_NIR_ERR_SSA;
        }
        work->result.words[work->result.word_count++] = words[0];
        work->result.words[work->result.word_count++] = words[1];
        release_dead(work, i);
    }
    return SGX535_NIR_OK;
}

enum sgx535_nir_status sgx535_linear_compile(const struct sgx535_linear_instruction *input,
                                             size_t count, enum sgx535_linear_profile profile,
                                             struct sgx535_nir_program *output)
{
    struct linear_work *work;
    enum sgx535_nir_status status;
    if (!input || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    if (!count || count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    work = calloc(1, sizeof(*work));
    if (!work)
        return SGX535_NIR_ERR_NOMEM;
    status = validate(input, count, profile, work);
    if (status == SGX535_NIR_OK)
        status = translate(input, count, work);
    if (status == SGX535_NIR_OK)
        *output = work->result;
    free(work);
    return status;
}
