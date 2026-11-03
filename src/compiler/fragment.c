// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment.h"
#include "sgx535_fragment_internal.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_usse.h"
#include "sgx535_pack.h"
#include "sgx535_texture2d.h"
#include "sgx535_bitwise.h"
#include "sgx535_fragment_raw_sign.h"
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/string.h>
#else
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#endif

static const uint32_t dri_color0_primary[4] = {2, 1, 0, 3};
static const uint32_t generic_f32_primary[4] = {0, 1, 2, 3};

enum sgx535_nir_status
sgx535_fragment_sampled_validate(const struct sgx535_fragment_instruction *ir, size_t count,
                                 const struct sgx535_fragment_sampled_input *sample)
{
    uint32_t components = 0, coordinates[2] = {0};
    if (!ir || !sample)
        return SGX535_NIR_ERR_ARGUMENT;
    if (count < 10 || count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    if (sample->reserved || sample->texture_slot >= SGX535_FRAGMENT_SAMPLED_SLOTS ||
        sample->sampler_slot >= SGX535_FRAGMENT_SAMPLED_SLOTS ||
        sample->coordinate_slot >= SGX535_FRAGMENT_INPUT_SLOTS)
        return SGX535_NIR_ERR_ARGUMENT;
    if (sample->coordinate_components != 2 || sample->interpolation != SGX535_FRAGMENT_SMOOTH ||
        sample->result_format != SGX535_FRAGMENT_SAMPLED_BGRA8_U8 || sample->result_dwords != 1)
        return SGX535_NIR_ERR_UNSUPPORTED;
    for (size_t i = 0; i < count; i++) {
        const struct sgx535_fragment_instruction *in = &ir[i];
        unsigned sources;
        if ((i >= count - 4) != (in->opcode == SGX535_FRAGMENT_OUTPUT) ||
            (in->opcode != SGX535_FRAGMENT_CONSTANT && in->immediate) ||
            (in->opcode != SGX535_FRAGMENT_INPUT &&
             in->opcode != SGX535_FRAGMENT_SAMPLE2D_COMPONENT && in->component) ||
            (in->opcode != SGX535_FRAGMENT_INPUT &&
             in->opcode != SGX535_FRAGMENT_SAMPLE2D_COMPONENT &&
             in->opcode != SGX535_FRAGMENT_OUTPUT && in->index))
            return SGX535_NIR_ERR_ARGUMENT;
        switch (in->opcode) {
        case SGX535_FRAGMENT_CONSTANT:
            sources = 0;
            break;
        case SGX535_FRAGMENT_INPUT:
            if (in->index != sample->coordinate_slot || in->component >= 2)
                return SGX535_NIR_ERR_ARGUMENT;
            sources = 0;
            break;
        case SGX535_FRAGMENT_SAMPLE2D_COMPONENT:
            if (in->index || in->component >= 4 || (components & (1u << in->component)))
                return SGX535_NIR_ERR_ARGUMENT;
            for (unsigned j = 0; j < 2; j++) {
                uint32_t source = in->source[j];
                if (source >= i || ir[source].opcode != SGX535_FRAGMENT_INPUT ||
                    ir[source].index != sample->coordinate_slot || ir[source].component != j)
                    return SGX535_NIR_ERR_SSA;
                if (components && coordinates[j] != source)
                    return SGX535_NIR_ERR_SSA;
                coordinates[j] = source;
            }
            components |= 1u << in->component;
            sources = 2;
            break;
        case SGX535_FRAGMENT_MOV:
        case SGX535_FRAGMENT_RCP:
            sources = 1;
            break;
        case SGX535_FRAGMENT_MIN:
        case SGX535_FRAGMENT_MAX:
            sources = 2;
            break;
        case SGX535_FRAGMENT_MAD:
            sources = 3;
            break;
        case SGX535_FRAGMENT_OUTPUT:
            if (in->index != i - (count - 4))
                return SGX535_NIR_ERR_ARGUMENT;
            sources = 1;
            break;
        default:
            return SGX535_NIR_ERR_UNSUPPORTED;
        }
        for (unsigned j = 0; j < 3; j++) {
            if (j >= sources) {
                if (in->source[j])
                    return SGX535_NIR_ERR_ARGUMENT;
            } else if (in->source[j] >= i || ir[in->source[j]].opcode == SGX535_FRAGMENT_OUTPUT)
                return SGX535_NIR_ERR_SSA;
            else if (in->opcode != SGX535_FRAGMENT_SAMPLE2D_COMPONENT &&
                     ir[in->source[j]].opcode == SGX535_FRAGMENT_INPUT)
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
    }
    return components == 15 ? SGX535_NIR_OK : SGX535_NIR_ERR_UNSUPPORTED;
}

struct fragment_work {
    struct sgx535_fragment_program result;
    /* Keep the transactional typed projection off the kernel stack. */
    struct sgx535_fragment_dri_color0_program dri_result;
    uint32_t last_use[SGX535_NIR_MAX_INSTRUCTIONS];
    unsigned reg[SGX535_NIR_MAX_INSTRUCTIONS];
    bool occupied[SGX535_NIR_TEMP_COUNT];
    unsigned live, components[SGX535_FRAGMENT_INPUT_SLOTS];
    unsigned primary_base[SGX535_FRAGMENT_INPUT_SLOTS];
    bool dri_color0, raw_sign, raw_ieq, sampled;
    uint32_t dri_primary[4];
};
static struct fragment_work *work_allocate(void)
{
#ifdef __KERNEL__
    return kzalloc(sizeof(struct fragment_work), GFP_KERNEL);
#else
    return calloc(1, sizeof(struct fragment_work));
#endif
}
static void work_free(struct fragment_work *work)
{
#ifdef __KERNEL__
    kfree(work);
#else
    free(work);
#endif
}
static int arity(enum sgx535_fragment_opcode op)
{
    switch (op) {
    case SGX535_FRAGMENT_CONSTANT:
    case SGX535_FRAGMENT_INPUT:
    case SGX535_FRAGMENT_UNIFORM:
        return 0;
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
        return -1;
    }
}
static enum sgx535_nir_status validate(const struct sgx535_fragment_instruction *p, size_t n,
                                       struct fragment_work *w)
{
    size_t i;
    unsigned j, base = 0;
    if (n < 5 || n > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    for (i = 0; i < n; i++) {
        const struct sgx535_fragment_instruction *op = &p[i];
        int sources = arity(op->opcode);
        if (!w->raw_sign &&
            (op->opcode == SGX535_FRAGMENT_RAW_XOR || op->opcode == SGX535_FRAGMENT_RAW_AND))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (op->opcode == SGX535_FRAGMENT_RAW_IEQ && !w->raw_ieq)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (op->opcode == SGX535_FRAGMENT_RAW_USHR_IMM &&
            (!w->raw_ieq || (op->immediate != 1u && op->immediate != 2u && op->immediate != 4u &&
                             op->immediate != 8u && op->immediate != 16u && op->immediate != 31u)))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (sources < 0 || (i >= n - 4 && op->opcode != SGX535_FRAGMENT_OUTPUT) ||
            (i < n - 4 && op->opcode == SGX535_FRAGMENT_OUTPUT))
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (op->opcode != SGX535_FRAGMENT_CONSTANT && op->opcode != SGX535_FRAGMENT_RAW_USHR_IMM &&
            op->immediate)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (op->opcode != SGX535_FRAGMENT_INPUT && op->component)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (op->opcode == SGX535_FRAGMENT_INPUT) {
            if (op->index >= SGX535_FRAGMENT_INPUT_SLOTS || op->component >= 4)
                return SGX535_NIR_ERR_LIMIT;
            if (op->component + 1 > w->components[op->index])
                w->components[op->index] = op->component + 1;
        } else if (op->opcode == SGX535_FRAGMENT_UNIFORM) {
            if (op->index >= SGX535_FRAGMENT_UNIFORM_WORDS)
                return SGX535_NIR_ERR_LIMIT;
            w->result.uniform_words_used[op->index / 32] |= 1u << (op->index % 32);
            if (op->index + 1 > w->result.secondary_register_count)
                w->result.secondary_register_count = op->index + 1;
        } else if (op->opcode == SGX535_FRAGMENT_OUTPUT) {
            if (op->index != i - (n - 4))
                return SGX535_NIR_ERR_UNSUPPORTED;
        } else if (op->index)
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (j = 0; j < 3; j++) {
            if (j < (unsigned)sources) {
                if (op->source[j] >= i || p[op->source[j]].opcode == SGX535_FRAGMENT_OUTPUT)
                    return SGX535_NIR_ERR_SSA;
                w->last_use[op->source[j]] = (uint32_t)i;
            } else if (op->source[j])
                return SGX535_NIR_ERR_UNSUPPORTED;
        }
        w->reg[i] = SGX535_NIR_TEMP_COUNT;
    }
    if (w->dri_color0) {
        /* This producer has one complete COLOR0 attribute; it is not the
         * generic prefix iterator ABI. Do not return a Boxee descriptor. */
        if (!w->components[0])
            return SGX535_NIR_ERR_UNSUPPORTED;
        for (j = 1; j < SGX535_FRAGMENT_INPUT_SLOTS; j++)
            if (w->components[j])
                return SGX535_NIR_ERR_UNSUPPORTED;
        w->result.primary_register_count = 4;
        return SGX535_NIR_OK;
    }
    for (j = 0; j < SGX535_FRAGMENT_INPUT_SLOTS; j++) {
        struct sgx535_fragment_input *in;
        struct sgx535_iterator_f32 descriptor;
        if (!w->components[j])
            continue;
        in = &w->result.input[w->result.input_count++];
        in->slot = j;
        in->components = w->components[j];
        in->primary_base = w->primary_base[j] = base;
        base += in->components;
        descriptor = (struct sgx535_iterator_f32){j, in->components, 0};
        if (sgx535_iterator_f32_encode(&descriptor, &in->iterator) != SGX535_ITERATOR_OK)
            return SGX535_NIR_ERR_UNSUPPORTED;
    }
    if (w->result.input_count) {
        struct sgx535_fragment_input *last = &w->result.input[w->result.input_count - 1];
        struct sgx535_iterator_f32 descriptor = {last->slot, last->components, 1};
        if (sgx535_iterator_f32_encode(&descriptor, &last->iterator) != SGX535_ITERATOR_OK)
            return SGX535_NIR_ERR_UNSUPPORTED;
    }
    w->result.primary_register_count = base;
    return SGX535_NIR_OK;
}
static int reserve(struct fragment_work *w)
{
    unsigned i;
    for (i = 0; i < SGX535_NIR_TEMP_COUNT; i++) {
        if (w->occupied[i])
            continue;
        w->occupied[i] = true;
        if (++w->live > w->result.encoding.peak_live_temps)
            w->result.encoding.peak_live_temps = w->live;
        if (i + 1 > w->result.encoding.temp_register_count)
            w->result.encoding.temp_register_count = i + 1;
        return (int)i;
    }
    return -1;
}
static void release(struct fragment_work *w, size_t position)
{
    size_t i;
    for (i = 0; i <= position; i++)
        if (w->reg[i] < SGX535_NIR_TEMP_COUNT && w->last_use[i] <= position) {
            w->occupied[w->reg[i]] = false;
            w->reg[i] = SGX535_NIR_TEMP_COUNT;
            w->live--;
        }
}
static enum sgx535_nir_status emit(const struct sgx535_fragment_instruction *p, size_t n,
                                   struct fragment_work *w)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const struct sgx535_fragment_instruction *op = &p[i];
        uint32_t words[6];
        unsigned word_count = op->opcode == SGX535_FRAGMENT_RAW_IEQ ? 6u : 2u, wi;
        bool output = op->opcode == SGX535_FRAGMENT_OUTPUT;
        if (w->sampled &&
            (op->opcode == SGX535_FRAGMENT_INPUT ||
             op->opcode == SGX535_FRAGMENT_SAMPLE2D_COMPONENT || (!output && !w->last_use[i]))) {
            release(w, i);
            continue;
        }
        if (w->result.encoding.word_count > SGX535_NIR_MAX_WORDS - word_count)
            return SGX535_NIR_ERR_LIMIT;
        int dst = output ? (int)op->index : reserve(w);
        if (dst < 0)
            return SGX535_NIR_ERR_PRESSURE;
        if (!output)
            w->reg[i] = (unsigned)dst;
        if (output && w->result.output_profile == SGX535_FRAGMENT_U8_WORD_PROJECTION) {
            static const unsigned byte[] = {2, 1, 0, 3};
            struct sgx535_pack_f32_u8_channel x = {{SGX535_MOV_OUTPUT, 0},
                                                   {SGX535_MOV_TEMP, w->reg[op->source[0]]},
                                                   byte[op->index],
                                                   1,
                                                   i == n - 1};
            if (sgx535_pack_f32_u8_channel_encode(&x, words) != SGX535_PACK_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_FRAGMENT_CONSTANT) {
            struct sgx535_limm x = {SGX535_LIMM_TEMP, (uint32_t)dst, op->immediate, 0};
            if (sgx535_limm_encode(&x, words) != SGX535_LIMM_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_FRAGMENT_INPUT || op->opcode == SGX535_FRAGMENT_UNIFORM ||
                   op->opcode == SGX535_FRAGMENT_MOV || output) {
            struct sgx535_mov x = {{output ? SGX535_MOV_OUTPUT : SGX535_MOV_TEMP, (uint32_t)dst},
                                   {SGX535_MOV_TEMP, 0},
                                   i == n - 1};
            if (op->opcode == SGX535_FRAGMENT_INPUT) {
                x.source = (struct sgx535_mov_operand){
                    SGX535_MOV_PRIMARY, w->dri_color0 ? w->dri_primary[op->component]
                                                      : w->primary_base[op->index] + op->component};
            } else if (op->opcode == SGX535_FRAGMENT_UNIFORM)
                x.source = (struct sgx535_mov_operand){SGX535_MOV_SECONDARY, op->index};
            else
                x.source.number = w->reg[op->source[0]];
            if (sgx535_mov_encode(&x, words) != SGX535_MOV_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_FRAGMENT_RAW_USHR_IMM) {
            struct sgx535_const_ushr x = {(uint32_t)dst, w->reg[op->source[0]], op->immediate};
            if (sgx535_const_ushr_encode(&x, words) != SGX535_BITWISE_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_FRAGMENT_RAW_IEQ) {
            struct sgx535_raw_ieq x = {(uint32_t)dst,
                                       {w->reg[op->source[0]], w->reg[op->source[1]]}};
            if (sgx535_raw_ieq_encode(&x, words) != SGX535_RAW_IEQ_OK)
                return SGX535_NIR_ERR_SSA;
        } else if (op->opcode == SGX535_FRAGMENT_RAW_XOR || op->opcode == SGX535_FRAGMENT_RAW_AND) {
            struct sgx535_bitwise x = {op->opcode == SGX535_FRAGMENT_RAW_XOR ? SGX535_BITWISE_XOR
                                                                             : SGX535_BITWISE_AND,
                                       (uint32_t)dst,
                                       {w->reg[op->source[0]], w->reg[op->source[1]]},
                                       0};
            if (sgx535_bitwise_encode(&x, words) != SGX535_BITWISE_OK)
                return SGX535_NIR_ERR_SSA;
        } else {
            static const enum sgx535_f32_opcode map[] = {SGX535_F32_MAD, SGX535_F32_MIN,
                                                         SGX535_F32_MAX, SGX535_F32_RCP};
            struct sgx535_f32 x = {0};
            unsigned j;
            x.opcode = op->opcode == SGX535_FRAGMENT_RSQ ? SGX535_F32_RSQ
                       : op->opcode == SGX535_FRAGMENT_FRACT
                           ? SGX535_F32_FRC
                           : map[op->opcode - SGX535_FRAGMENT_MAD];
            x.destination = (struct sgx535_f32_operand){SGX535_F32_TEMP, (uint32_t)dst};
            for (j = 0; j < (unsigned)arity(op->opcode); j++)
                x.source[j].number = w->reg[op->source[j]];
            if (op->opcode == SGX535_FRAGMENT_FRACT)
                x.source[1] = x.source[0];
            if (sgx535_f32_encode(&x, words) != SGX535_F32_OK)
                return SGX535_NIR_ERR_SSA;
        }
        for (wi = 0; wi < word_count; wi++)
            w->result.encoding.words[w->result.encoding.word_count++] = words[wi];
        release(w, i);
    }
    return SGX535_NIR_OK;
}
static enum sgx535_nir_status compile_work(const struct sgx535_fragment_instruction *input,
                                           size_t count,
                                           enum sgx535_fragment_output_profile profile,
                                           const uint32_t *primary_map, bool raw_sign, bool raw_ieq,
                                           struct fragment_work **output)
{
    struct fragment_work *w;
    enum sgx535_nir_status status;
    if (!input || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    if (profile != SGX535_FRAGMENT_RAW_F32_WORDS && profile != SGX535_FRAGMENT_U8_WORD_PROJECTION)
        return SGX535_NIR_ERR_UNSUPPORTED;
    if (count < 5 || count > SGX535_NIR_MAX_INSTRUCTIONS)
        return SGX535_NIR_ERR_LIMIT;
    w = work_allocate();
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    w->result.output_profile = profile;
    w->dri_color0 = primary_map != NULL;
    if (primary_map)
        memcpy(w->dri_primary, primary_map, sizeof(w->dri_primary));
    w->raw_sign = raw_sign;
    w->raw_ieq = raw_ieq;
    status = validate(input, count, w);
    if (status == SGX535_NIR_OK)
        status = emit(input, count, w);
    if (status == SGX535_NIR_OK)
        *output = w;
    else
        work_free(w);
    return status;
}
enum sgx535_nir_status sgx535_fragment_compile(const struct sgx535_fragment_instruction *input,
                                               size_t count,
                                               enum sgx535_fragment_output_profile profile,
                                               struct sgx535_fragment_program *output)
{
    struct fragment_work *w;
    enum sgx535_nir_status status;
    if (!output)
        return SGX535_NIR_ERR_ARGUMENT;
    status = compile_work(input, count, profile, NULL, false, false, &w);
    if (status == SGX535_NIR_OK) {
        *output = w->result;
        work_free(w);
    }
    return status;
}

enum sgx535_nir_status sgx535_fragment_compile_sampled_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_sampled_input *sample,
    const struct sgx535_texture2d_primary_plan *primary, struct sgx535_fragment_program *output)
{
    static const unsigned rgba_byte[] = {2, 1, 0, 3};
    struct sgx535_texture2d_primary_plan packed;
    struct fragment_work *w;
    enum sgx535_nir_status status;
    uint32_t first = 0;
    if (!input || !sample || !primary || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    status = sgx535_fragment_sampled_validate(input, count, sample);
    if (status != SGX535_NIR_OK)
        return status;
    packed = *primary;
    if (packed.result_dwords != 1 || packed.primary_dwords != 1 || packed.record_index ||
        packed.packed_source.bank != SGX535_MOV_PRIMARY || packed.packed_source.number)
        return SGX535_NIR_ERR_UNSUPPORTED;
    for (unsigned c = 0; c < 4; c++) {
        struct sgx535_unpack_u8_f32_component op;
        if (sgx535_unpack_u8_f32_component_decode(&packed.unpack[c * 2], &op) != SGX535_PACK_OK)
            return SGX535_NIR_ERR_UNSUPPORTED;
        if (!c)
            first = op.destination.number;
        if (first > SGX535_NIR_TEMP_COUNT - 4)
            return SGX535_NIR_ERR_LIMIT;
        if (op.destination.bank != SGX535_MOV_TEMP || op.destination.number != first + c ||
            op.source.bank != SGX535_MOV_PRIMARY || op.source.number ||
            op.byte_index != rgba_byte[c] || op.end)
            return SGX535_NIR_ERR_UNSUPPORTED;
    }
    w = work_allocate();
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    w->sampled = true;
    w->result.output_profile = SGX535_FRAGMENT_U8_WORD_PROJECTION;
    w->result.primary_register_count = 1;
    /* The prefix produces all four values before ordinary SSA emission. They
     * survive earlier constants/ALU even if their symbolic defs are later. */
    for (size_t i = 0; i < count; i++) {
        w->reg[i] = SGX535_NIR_TEMP_COUNT;
        if (input[i].opcode == SGX535_FRAGMENT_INPUT ||
            input[i].opcode == SGX535_FRAGMENT_SAMPLE2D_COMPONENT)
            continue;
        for (unsigned j = 0; j < (unsigned)arity(input[i].opcode); j++)
            w->last_use[input[i].source[j]] = (uint32_t)i;
    }
    for (size_t i = 0; i < count; i++) {
        if (input[i].opcode == SGX535_FRAGMENT_SAMPLE2D_COMPONENT) {
            uint32_t temp = first + input[i].component;
            w->reg[i] = temp;
            w->occupied[temp] = true;
        }
    }
    w->live = w->result.encoding.peak_live_temps = 4;
    w->result.encoding.temp_register_count = first + 4;
    w->result.encoding.word_count = SGX535_TEXTURE2D_UNPACK_WORDS;
    memcpy(w->result.encoding.words, packed.unpack, sizeof(packed.unpack));
    status = emit(input, count, w);
    if (status == SGX535_NIR_OK)
        *output = w->result;
    work_free(w);
    return status;
}

enum sgx535_nir_status
sgx535_fragment_compile_raw_sign_candidate(const struct sgx535_fragment_instruction *input,
                                           size_t count, struct sgx535_fragment_program *output)
{
    struct fragment_work *w;
    enum sgx535_nir_status status;
    if (!output)
        return SGX535_NIR_ERR_ARGUMENT;
    status = compile_work(input, count, SGX535_FRAGMENT_RAW_F32_WORDS, NULL, true, false, &w);
    if (status == SGX535_NIR_OK) {
        *output = w->result;
        work_free(w);
    }
    return status;
}

enum sgx535_nir_status
sgx535_fragment_compile_raw_ieq_candidate(const struct sgx535_fragment_instruction *input,
                                          size_t count, struct sgx535_fragment_program *output)
{
    struct fragment_work *w;
    enum sgx535_nir_status status;
    if (!output)
        return SGX535_NIR_ERR_ARGUMENT;
    status = compile_work(input, count, SGX535_FRAGMENT_RAW_F32_WORDS, NULL, true, true, &w);
    if (status == SGX535_NIR_OK) {
        *output = w->result;
        work_free(w);
    }
    return status;
}

static enum sgx535_nir_status compile_dri_color0(const struct sgx535_fragment_instruction *input,
                                                 size_t count,
                                                 const struct sgx535_fragment_linkage *linkage,
                                                 enum sgx535_fragment_output_profile profile,
                                                 bool raw_sign, bool generic_f32, bool raw_mask,
                                                 struct sgx535_fragment_dri_color0_program *output)
{
    struct sgx535_fragment_linkage binding;
    struct sgx535_fragment_dri_color0_program *result;
    struct fragment_work *w;
    enum sgx535_nir_status status;
    unsigned i;
    if (!input || !linkage || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    binding = *linkage;
    if (binding.count != 1 || binding.varying[0].semantic != SGX535_FRAGMENT_COLOR ||
        binding.varying[0].index || binding.varying[0].driver_base ||
        binding.varying[0].components != 4 ||
        binding.varying[0].interpolation != SGX535_FRAGMENT_SMOOTH)
        return SGX535_NIR_ERR_UNSUPPORTED;
    for (i = 1; i < SGX535_FRAGMENT_INPUT_SLOTS; i++) {
        const struct sgx535_fragment_varying *v = &binding.varying[i];
        if (v->semantic || v->index || v->driver_base || v->components || v->interpolation)
            return SGX535_NIR_ERR_UNSUPPORTED;
    }
    status =
        compile_work(input, count, profile, generic_f32 ? generic_f32_primary : dri_color0_primary,
                     raw_sign, raw_mask, &w);
    if (status != SGX535_NIR_OK)
        return status;
    result = &w->dri_result;
    result->encoding = w->result.encoding;
    result->logical_components = w->components[0];
    result->primary_register_count = 4;
    for (i = 0; i < 4; i++) {
        result->primary_component[i] = generic_f32 ? generic_f32_primary[i] : dri_color0_primary[i];
        result->uniform_words_used[i] = w->result.uniform_words_used[i];
    }
    result->secondary_register_count = w->result.secondary_register_count;
    result->output_profile = profile;
    *output = *result;
    work_free(w);
    return SGX535_NIR_OK;
}
enum sgx535_nir_status
sgx535_fragment_compile_dri_color0(const struct sgx535_fragment_instruction *input, size_t count,
                                   const struct sgx535_fragment_linkage *linkage,
                                   enum sgx535_fragment_output_profile profile,
                                   struct sgx535_fragment_dri_color0_program *output)
{
    return compile_dri_color0(input, count, linkage, profile, false, false, false, output);
}
enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_sign_candidate(
    const struct sgx535_fragment_instruction *input, size_t count,
    const struct sgx535_fragment_linkage *linkage,
    struct sgx535_fragment_dri_color0_program *output)
{
    return sgx535_fragment_dri_raw_sign_profile(input, count, linkage,
                                                SGX535_FRAGMENT_RAW_F32_WORDS, output);
}
enum sgx535_nir_status
sgx535_fragment_dri_raw_sign_profile(const struct sgx535_fragment_instruction *input, size_t count,
                                     const struct sgx535_fragment_linkage *linkage,
                                     enum sgx535_fragment_output_profile profile,
                                     struct sgx535_fragment_dri_color0_program *output)
{
    return compile_dri_color0(input, count, linkage, profile, true, false, false, output);
}
enum sgx535_nir_status
sgx535_fragment_dri_generic_f32_profile(const struct sgx535_fragment_instruction *input,
                                        size_t count, const struct sgx535_fragment_linkage *linkage,
                                        enum sgx535_fragment_output_profile profile,
                                        struct sgx535_fragment_dri_color0_program *output)
{
    return compile_dri_color0(input, count, linkage, profile, true, true, false, output);
}
enum sgx535_nir_status
sgx535_fragment_dri_raw_mask_profile(const struct sgx535_fragment_instruction *input, size_t count,
                                     const struct sgx535_fragment_linkage *linkage,
                                     enum sgx535_fragment_output_profile profile,
                                     struct sgx535_fragment_dri_color0_program *output)
{
    return compile_dri_color0(input, count, linkage, profile, true, false, true, output);
}

enum sgx535_nir_status
sgx535_fragment_native_admission(const struct sgx535_fragment_program *program)
{
    return program ? SGX535_NIR_ERR_UNSUPPORTED : SGX535_NIR_ERR_ARGUMENT;
}
enum sgx535_nir_status sgx535_fragment_dri_color0_native_admission(
    const struct sgx535_fragment_dri_color0_program *program)
{
    return program ? SGX535_NIR_ERR_UNSUPPORTED : SGX535_NIR_ERR_ARGUMENT;
}
