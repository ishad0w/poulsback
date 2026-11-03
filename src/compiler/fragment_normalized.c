// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment_normalized.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_internal.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_generic_f32.h"
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/string.h>
#else
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#endif

struct normalized_work {
    struct sgx535_fragment_instruction lowered[SGX535_NIR_MAX_INSTRUCTIONS];
    union {
        struct sgx535_fragment_dri_color0_program color0;
        struct sgx535_fragment_program no_input;
    } compiled;
    struct sgx535_fragment_normalized_candidate result;
    uint32_t logical_uniform_words_used[4], logical_uniform_extent;
};

static unsigned source_count(enum sgx535_fragment_opcode opcode)
{
    switch (opcode) {
    case SGX535_FRAGMENT_MOV:
    case SGX535_FRAGMENT_RCP:
    case SGX535_FRAGMENT_RSQ:
    case SGX535_FRAGMENT_FRACT:
    case SGX535_FRAGMENT_RAW_USHR_IMM:
    case SGX535_FRAGMENT_OUTPUT:
        return 1;
    case SGX535_FRAGMENT_MIN:
    case SGX535_FRAGMENT_MAX:
    case SGX535_FRAGMENT_RAW_XOR:
    case SGX535_FRAGMENT_RAW_AND:
    case SGX535_FRAGMENT_RAW_IEQ:
        return 2;
    case SGX535_FRAGMENT_MAD:
        return 3;
    default:
        return 0;
    }
}

/* Called only after complete scalar validation. The special raw mask is not
 * an F32 literal: every real edge must use it as the canonical AND operand.
 * Inspect arity rather than zero-filled unused source fields (node0 exists). */
static bool only_absolute_mask(const struct sgx535_fragment_instruction *ir, size_t count,
                               size_t index)
{
    size_t i;
    unsigned j;
    bool used = false;
    if (ir[index].immediate != UINT32_C(0x7fffffff))
        return false;
    for (i = index + 1u; i < count; i++) {
        for (j = 0; j < source_count(ir[i].opcode); j++) {
            if (ir[i].source[j] != index)
                continue;
            if (ir[i].opcode != SGX535_FRAGMENT_RAW_AND || j != 1u)
                return false;
            used = true;
        }
    }
    return used;
}

/* A nonfinite word can be an integer mask/comparison operand, never an F32
 * literal. Check all actual edges, including dead consumers, before liveness.
 * MOV is intentionally not a new implicit type/provenance conversion. */
static bool only_raw_word_uses(const struct sgx535_fragment_instruction *ir, size_t count,
                               size_t index)
{
    bool used = false;
    for (size_t i = index + 1u; i < count; i++)
        for (unsigned j = 0; j < source_count(ir[i].opcode); j++) {
            if (ir[i].source[j] != index)
                continue;
            if (ir[i].opcode != SGX535_FRAGMENT_RAW_IEQ &&
                ir[i].opcode != SGX535_FRAGMENT_RAW_AND &&
                ir[i].opcode != SGX535_FRAGMENT_RAW_XOR &&
                ir[i].opcode != SGX535_FRAGMENT_RAW_USHR_IMM)
                return false;
            used = true;
        }
    return used;
}

static enum sgx535_nir_status
normalized_compile_source(const struct sgx535_fragment_instruction *input, size_t count,
                          const struct sgx535_fragment_linkage *binding,
                          enum sgx535_fragment_output_profile profile, bool raw_sign,
                          bool generic_f32, bool raw_mask, bool no_input, struct normalized_work *w)
{
    enum sgx535_nir_status status;
    unsigned i;
    if (no_input) {
        const struct sgx535_fragment_program *p = &w->compiled.no_input;
        status = sgx535_fragment_compile(input, count, profile, &w->compiled.no_input);
        if (status != SGX535_NIR_OK)
            return status;
        /* The generic compiler validates every node before this narrower
         * linkage check, including an unused INPUT. No iterator is invented. */
        if (p->input_count || p->primary_register_count)
            return SGX535_NIR_ERR_UNSUPPORTED;
        w->result.encoding = p->encoding;
        w->result.secondary_register_count = p->secondary_register_count;
        memcpy(w->result.uniform_words_used, p->uniform_words_used,
               sizeof(w->result.uniform_words_used));
    } else {
        const struct sgx535_fragment_dri_color0_program *p = &w->compiled.color0;
        status = raw_mask ? sgx535_fragment_dri_raw_mask_profile(input, count, binding, profile,
                                                                 &w->compiled.color0)
                 : generic_f32 ? sgx535_fragment_dri_generic_f32_profile(
                                     input, count, binding, profile, &w->compiled.color0)
                 : raw_sign ? sgx535_fragment_dri_raw_sign_profile(input, count, binding, profile,
                                                                   &w->compiled.color0)
                            : sgx535_fragment_compile_dri_color0(input, count, binding, profile,
                                                                 &w->compiled.color0);
        if (status != SGX535_NIR_OK)
            return status;
        w->result.encoding = p->encoding;
        w->result.logical_components = p->logical_components;
        w->result.primary_register_count = p->primary_register_count;
        w->result.secondary_register_count = p->secondary_register_count;
        for (i = 0; i < 4; i++) {
            w->result.primary_component[i] = p->primary_component[i];
            w->result.uniform_words_used[i] = p->uniform_words_used[i];
        }
    }
    return SGX535_NIR_OK;
}

static enum sgx535_nir_status
compile_normalized(const struct sgx535_fragment_instruction *input, size_t count,
                   const struct sgx535_fragment_linkage *linkage, bool raw_sign, bool generic_f32,
                   bool raw_mask, bool no_input,
                   struct sgx535_fragment_normalized_candidate *output,
                   struct sgx535_fragment_generic_f32_secondary_candidate *secondary)
{
    struct sgx535_fragment_linkage binding;
    struct normalized_work *w;
    enum sgx535_nir_status status;
    uint32_t sources[4], base, i;
    if (!input || !linkage || (!output && !secondary))
        return SGX535_NIR_ERR_ARGUMENT;
    if (count < 5 || count > SGX535_NIR_MAX_INSTRUCTIONS - SGX535_NORMALIZED_EXTRA_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    binding = *linkage;
#ifdef __KERNEL__
    w = kzalloc(sizeof(*w), GFP_KERNEL);
#else
    w = calloc(1, sizeof(*w));
#endif
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    memcpy(w->lowered, input, count * sizeof(*input));
    /* Validate before replacing final OUTPUT positions: an invalid forward
     * source must not become a valid reference to an inserted constant. */
    status = normalized_compile_source(w->lowered, count, &binding, SGX535_FRAGMENT_RAW_F32_WORDS,
                                       raw_sign, generic_f32, raw_mask, no_input, w);
    if (status != SGX535_NIR_OK)
        goto done;
    if (secondary) {
        /* Keep source indices separate from the selected ordinary SA layout.
         * The complete original stream was validated, including dead loads. */
        w->logical_uniform_extent = w->result.secondary_register_count;
        memcpy(w->logical_uniform_words_used, w->result.uniform_words_used,
               sizeof(w->logical_uniform_words_used));
        if (w->logical_uniform_extent >= SGX535_FRAGMENT_UNIFORM_WORDS) {
            status = SGX535_NIR_ERR_LIMIT;
            goto done;
        }
        for (i = 0; i < count; i++)
            if (w->lowered[i].opcode == SGX535_FRAGMENT_UNIFORM)
                w->lowered[i].index++;
    }
    for (i = 0; i < count; i++) {
        if (w->lowered[i].opcode == SGX535_FRAGMENT_CONSTANT &&
            (w->lowered[i].immediate & 0x7f800000u) == 0x7f800000u &&
            !(raw_mask ? only_raw_word_uses(w->lowered, count, i)
                       : raw_sign && only_absolute_mask(w->lowered, count, i))) {
            status = SGX535_NIR_ERR_UNSUPPORTED;
            goto done;
        }
        if (raw_sign && !raw_mask &&
            (w->lowered[i].opcode == SGX535_FRAGMENT_RAW_XOR ||
             w->lowered[i].opcode == SGX535_FRAGMENT_RAW_AND)) {
            const struct sgx535_fragment_instruction *mask = &w->lowered[w->lowered[i].source[1]];
            uint32_t expected = w->lowered[i].opcode == SGX535_FRAGMENT_RAW_XOR
                                    ? UINT32_C(0x80000000)
                                    : UINT32_C(0x7fffffff);
            if (mask->opcode != SGX535_FRAGMENT_CONSTANT || mask->immediate != expected) {
                status = SGX535_NIR_ERR_UNSUPPORTED;
                goto done;
            }
        }
    }
    base = (uint32_t)count - 4u;
    for (i = 0; i < 4; i++)
        sources[i] = w->lowered[base + i].source[0];
    w->lowered[base] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT};
    w->lowered[base + 1u] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                                 .immediate = 0x3f800000u};
    for (i = 0; i < 4; i++) {
        uint32_t clamp = base + 2u + i * 2u;
        w->lowered[clamp] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAX,
                                                                 .source = {sources[i], base, 0}};
        w->lowered[clamp + 1u] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_MIN, .source = {clamp, base + 1u, 0}};
        w->lowered[base + 10u + i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {clamp + 1u, 0, 0}};
    }
    status = normalized_compile_source(w->lowered, count + SGX535_NORMALIZED_EXTRA_INSTRUCTIONS,
                                       &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, raw_sign,
                                       generic_f32, raw_mask, no_input, w);
    if (status != SGX535_NIR_OK)
        goto done;
    for (i = 0; i < 4; i++) {
        static const uint32_t lane[4] = {2, 1, 0, 3};
        w->result.output_byte_lane[i] = lane[i];
    }
    w->result.source_instruction_count = (uint32_t)count;
    w->result.lowered_instruction_count = (uint32_t)count + SGX535_NORMALIZED_EXTRA_INSTRUCTIONS;
    w->result.reference_policy = SGX535_NORMALIZED_FINITE_CLAMP_RTE;
    w->result.missing_contracts = SGX535_NORMALIZED_MISSING_ALL;
    if (secondary) {
        memset(secondary, 0, sizeof(*secondary));
        secondary->program = w->result;
        memcpy(secondary->logical_uniform_words_used, w->logical_uniform_words_used,
               sizeof(secondary->logical_uniform_words_used));
        secondary->logical_uniform_extent = w->logical_uniform_extent;
        secondary->reserved_sa_words = w->logical_uniform_extent ? 1u : 0u;
        secondary->uniform_sa_words = w->logical_uniform_extent;
        secondary->physical_sa_words = w->result.secondary_register_count;
    } else
        *output = w->result;
done:
#ifdef __KERNEL__
    kfree(w);
#else
    free(w);
#endif
    return status;
}

enum sgx535_nir_status sgx535_fragment_compile_dri_color0_normalized_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_normalized_candidate *output)
{
    return compile_normalized(input, count, linkage, false, false, false, false, output, NULL);
}

enum sgx535_nir_status sgx535_fragment_compile_no_input_normalized_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    struct sgx535_fragment_normalized_candidate *output)
{
    const struct sgx535_fragment_linkage linkage = {0};
    return compile_normalized(input, count, &linkage, false, false, false, true, output, NULL);
}

enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_normalized_candidate *output)
{
    return compile_normalized(input, count, linkage, true, false, false, false, output, NULL);
}

enum sgx535_nir_status sgx535_fragment_compile_generic_f32_normalized_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_normalized_candidate *output)
{
    return compile_normalized(input, count, linkage, true, true, false, false, output, NULL);
}

enum sgx535_nir_status sgx535_fragment_compile_generic_f32_secondary_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_generic_f32_secondary_candidate *output)
{
    return compile_normalized(input, count, linkage, true, true, false, false, NULL, output);
}

enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_normalized_candidate *output)
{
    return compile_normalized(input, count, linkage, true, false, true, false, output, NULL);
}

enum sgx535_nir_status sgx535_fragment_normalized_native_admission(
    const struct sgx535_fragment_normalized_candidate *program)
{
    return program ? SGX535_NIR_ERR_UNSUPPORTED : SGX535_NIR_ERR_ARGUMENT;
}
