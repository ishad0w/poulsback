// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_fragment.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_linear.h"
#include "sgx535_texture2d.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define LAST_DEPENDENCY UINT32_C(0x00080000)

enum sgx535_pds_fragment_status
sgx535_pds_fragment_sample_task_project(const struct sgx535_pds_fragment_sample_task *input,
                                        struct sgx535_pds_fragment_sample *output)
{
    struct sgx535_pds_fragment_sample_task task;
    struct sgx535_pds_use_link_projection link;
    struct sgx535_pds_fragment_sample result = {0};
    uint64_t body_end, code_end;

    if (!input || !output)
        return SGX535_PDS_FRAGMENT_ERR_ARGUMENT;
    task = *input;
    if (task.phase_count != 1 || task.texture_issue_count != 1 || task.use_issue_count ||
        task.last_texture_issue)
        return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
    if (!task.word_count || task.word_count > SGX535_NIR_MAX_WORDS || (task.word_count & 1u) ||
        task.temp_register_count < 4 || task.temp_register_count > SGX535_NIR_TEMP_COUNT ||
        task.coordinate_slot > SGX535_ITERATOR_F32_MAX_SLOT || task.code.data_master != 1 ||
        sgx535_pds_use_link_project(&task.code, &link) != SGX535_PDS_USE_LINK_OK)
        return SGX535_PDS_FRAGMENT_ERR_RANGE;
    code_end = (uint64_t)task.code.code_va_bytes + task.code.code_size_bytes;
    body_end = (uint64_t)task.code.entry_va_bytes + (uint64_t)task.word_count * 4u;
    if (body_end > code_end)
        return SGX535_PDS_FRAGMENT_ERR_RANGE;

    result.task_control[0] = link.address_fields | UINT32_C(0x00100000);
    result.task_control[1] = (task.temp_register_count & 31u) << 27;
    result.task_control[2] = UINT32_C(0x20) | (task.temp_register_count >> 5);
    result.coordinate_slot = task.coordinate_slot;
    *output = result;
    return SGX535_PDS_FRAGMENT_OK;
}

struct pds_constant_operand {
    uint32_t bank;
    uint32_t word;
};

/* This profile alternates pairs between DS0 and DS1, starting at DS0. */
static struct pds_constant_operand pds_constant_operand(uint32_t ordinal)
{
    struct pds_constant_operand operand = {
        .bank = (ordinal / 2u) % 2u,
        .word = (ordinal / 4u) * 2u + ordinal % 2u,
    };
    return operand;
}

static uint32_t pds_constant_memory_word(uint32_t ordinal)
{
    struct pds_constant_operand operand = pds_constant_operand(ordinal);
    return (operand.word / 8u) * 16u + operand.bank * 8u + operand.word % 8u;
}

/* Internal calls use bounded ordinals and fixed DOUTI/DOUTU/DOUTT ports. */
static uint32_t pds_movs_constants(uint32_t first_ordinal, uint32_t port)
{
    uint32_t pairs[2] = {0u, 0u};
    uint32_t instruction = (7u << 24) | port;
    for (uint32_t lane = 0u; lane < 4u; ++lane) {
        uint32_t ordinal = first_ordinal + lane;
        /* Repeating lane 2 keeps an odd-start MOVS within one pair per bank. */
        if (lane == 3u && first_ordinal % 2u != 0u)
            --ordinal;
        struct pds_constant_operand operand = pds_constant_operand(ordinal);
        uint32_t selector = operand.bank * 2u + operand.word % 2u;
        pairs[operand.bank] = operand.word / 2u;
        instruction |= selector << (11u - lane * 2u);
    }
    return instruction | (pairs[0] << 18) | (pairs[1] << 13);
}

enum sgx535_pds_fragment_status
sgx535_pds_fragment_sample_encode(const struct sgx535_pds_fragment_sample *input,
                                  const struct sgx535_texture2d_nearest *texture,
                                  const struct sgx535_texture2d_cpu_binding *binding,
                                  struct sgx535_pds_fragment_sample_program *output)
{
    struct sgx535_pds_fragment_sample task;
    struct sgx535_pds_fragment_sample_program result = {0};
    uint32_t constants[7], tuple[3];
    enum sgx535_texture2d_status status;

    if (!input || !texture || !binding || !output)
        return SGX535_PDS_FRAGMENT_ERR_ARGUMENT;
    task = *input;
    if (task.coordinate_slot > SGX535_ITERATOR_F32_MAX_SLOT)
        return SGX535_PDS_FRAGMENT_ERR_RANGE;
    status = sgx535_texture2d_nearest_descriptor(texture, binding, tuple);
    if (status != SGX535_TEXTURE2D_OK)
        return status == SGX535_TEXTURE2D_ERR_RANGE ? SGX535_PDS_FRAGMENT_ERR_RANGE
                                                    : SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
    /* Last USE iteration and last texture issue are distinct. This selected
     * stream has only a texture result, so only the texture dependency is set. */
    constants[0] = (task.task_control[0] & ~UINT32_C(0x00180000)) | UINT32_C(0x00100000);
    constants[1] = task.task_control[1];
    constants[2] = task.task_control[2];
    constants[3] = UINT32_C(0x0c00f900) | task.coordinate_slot;
    memcpy(constants + 4, tuple, sizeof(tuple));
    for (uint32_t i = 0; i < 7; i++)
        result.words[pds_constant_memory_word(i)] = constants[i];
    result.primary_dwords = 1;
    result.data_words = 12;
    result.total_words = SGX535_PDS_FRAGMENT_SAMPLE_WORDS;
    result.words[12] = pds_movs_constants(0, 5);
    result.words[13] = pds_movs_constants(3, 2);
    result.words[14] = pds_movs_constants(4, 4);
    result.words[15] = UINT32_C(0xaf000000);
    *output = result;
    return SGX535_PDS_FRAGMENT_OK;
}

enum sgx535_pds_fragment_status
sgx535_pds_fragment_encode(const struct sgx535_pds_fragment *input,
                           struct sgx535_pds_fragment_program *output)
{
    struct sgx535_pds_fragment task;
    struct sgx535_pds_fragment_program result = {0};
    struct sgx535_iterator_words issue;
    uint32_t extent = 0, used_slots = 0, index, i;
    if (!input || !output)
        return SGX535_PDS_FRAGMENT_ERR_ARGUMENT;
    task = *input;
    if (task.iterator_count > SGX535_PDS_FRAGMENT_MAX_ITERATORS)
        return SGX535_PDS_FRAGMENT_ERR_RANGE;
    for (i = 0; i < task.iterator_count; i++) {
        if (sgx535_iterator_f32_encode(&task.iterator[i], &issue) != SGX535_ITERATOR_OK ||
            task.iterator[i].last != (i + 1U == task.iterator_count ? 1U : 0U))
            return SGX535_PDS_FRAGMENT_ERR_RANGE;
        if (used_slots & (1U << task.iterator[i].slot))
            return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
        used_slots |= 1U << task.iterator[i].slot;
    }
    for (; i < SGX535_PDS_FRAGMENT_MAX_ITERATORS; i++)
        if (task.iterator[i].slot || task.iterator[i].components || task.iterator[i].last)
            return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
    result.iterator_count = task.iterator_count;
    for (i = 0; i < 3U + task.iterator_count; i++) {
        index = pds_constant_memory_word(i);
        if (index + 1U > extent)
            extent = index + 1U;
        if (i < 3U) {
            result.words[index] = task.task_control[i];
        } else {
            /* Every descriptor was checked above; this cannot fail. */
            (void)sgx535_iterator_f32_encode(&task.iterator[i - 3U], &issue);
            result.words[index] = issue.issue | (task.iterator[i - 3U].last ? LAST_DEPENDENCY : 0U);
        }
    }
    result.data_words = (extent + 3U) & ~3U;
    result.total_words = result.data_words + task.iterator_count + 2U;
    result.words[result.data_words] = pds_movs_constants(0U, 5U);
    for (i = 0; i < task.iterator_count; i++)
        result.words[result.data_words + 1U + i] = pds_movs_constants(3U + i, 2U);
    result.words[result.total_words - 1U] = UINT32_C(0xaf000000);
    *output = result;
    return SGX535_PDS_FRAGMENT_OK;
}

enum sgx535_pds_fragment_status
sgx535_pds_fragment_decode(const struct sgx535_pds_fragment_program *input,
                           struct sgx535_pds_fragment *output)
{
    struct sgx535_pds_fragment_program words, canonical;
    struct sgx535_pds_fragment task = {0};
    struct sgx535_iterator_words issue;
    uint32_t index, i;
    if (!input || !output)
        return SGX535_PDS_FRAGMENT_ERR_ARGUMENT;
    words = *input;
    if (words.iterator_count > SGX535_PDS_FRAGMENT_MAX_ITERATORS)
        return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
    task.iterator_count = words.iterator_count;
    for (i = 0; i < 3U + task.iterator_count; i++) {
        index = pds_constant_memory_word(i);
        if (i < 3U) {
            task.task_control[i] = words.words[index];
        } else {
            issue.issue = words.words[index] & ~LAST_DEPENDENCY;
            issue.tag_layer = UINT32_MAX;
            if (sgx535_iterator_f32_decode(&issue, &task.iterator[i - 3U]) != SGX535_ITERATOR_OK)
                return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
        }
    }
    if (sgx535_pds_fragment_encode(&task, &canonical) != SGX535_PDS_FRAGMENT_OK ||
        memcmp(&canonical, &words, sizeof(words)))
        return SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED;
    *output = task;
    return SGX535_PDS_FRAGMENT_OK;
}
