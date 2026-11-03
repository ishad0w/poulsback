// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_mte_state.h"
#include "sgx535_vertex_use.h"

#include <string.h>

/* Words per group, in header bit order (docs/native-3d.md#ta-state-records). */
static const uint8_t size[SGX535_MTE_GROUPS] = {1, 1, 1, 1, 1, 1, 3, 2, 6, 1, 1, 1, 1, 1, 1, 1};

uint32_t sgx535_mte_group_size(enum sgx535_mte_group g)
{
    return (unsigned)g < SGX535_MTE_GROUPS ? size[g] : 0;
}

uint32_t sgx535_mte_group_offset(enum sgx535_mte_group g)
{
    uint32_t at = 0;
    for (unsigned i = 0; i < (unsigned)g && i < SGX535_MTE_GROUPS; i++)
        at += size[i];
    return at;
}

void sgx535_mte_set(struct sgx535_mte_state *s, enum sgx535_mte_group g, const uint32_t *value)
{
    if (!s || !value || (unsigned)g >= SGX535_MTE_GROUPS)
        return;
    memcpy(s->words + sgx535_mte_group_offset(g), value, size[g] * sizeof(uint32_t));
    s->present |= 1u << g;
}

uint32_t sgx535_mte_record(const struct sgx535_mte_state *s, uint32_t mask,
                           uint32_t words[SGX535_MTE_STATE_WORDS])
{
    uint32_t n = 1, header = 0, at = 0;
    if (!s || !words)
        return 0;
    for (unsigned g = 0; g < SGX535_MTE_GROUPS; at += size[g], g++) {
        if (!((mask & s->present) >> g & 1u))
            continue;
        header |= 1u << g;
        memcpy(words + n, s->words + at, size[g] * sizeof(uint32_t));
        n += size[g];
    }
    words[0] = header;
    return header ? n : 0;
}

uint32_t sgx535_mte_changed(const struct sgx535_mte_state *before,
                            const struct sgx535_mte_state *now)
{
    uint32_t changed = 0, at = 0;
    if (!now)
        return 0;
    for (unsigned g = 0; g < SGX535_MTE_GROUPS; at += size[g], g++) {
        if (!(now->present >> g & 1u))
            continue;
        if (!before || !(before->present >> g & 1u) ||
            memcmp(before->words + at, now->words + at, size[g] * sizeof(uint32_t)))
            changed |= 1u << g;
    }
    return changed;
}

bool sgx535_mte_parse(const uint32_t *words, uint32_t count, struct sgx535_mte_state *s)
{
    uint32_t n = 1;
    if (!words || !count || !s || words[0] >> SGX535_MTE_GROUPS)
        return false;
    for (unsigned g = 0; g < SGX535_MTE_GROUPS; g++) {
        if (!(words[0] >> g & 1u))
            continue;
        if (n + size[g] > count)
            return false;
        sgx535_mte_set(s, (enum sgx535_mte_group)g, words + n);
        n += size[g];
    }
    return true;
}

/* MOVS DOUTD of DS0 0 and 1; MOVS DOUTU of DS0 2, 3 and DS1 0; HALT
 * (docs/native-3d.md#ta-state-records); DOUTU0 waits for the PDS DMA (21). */
#define TASK_DOUTD UINT32_C(0x07030223)
#define TASK_DOUTU UINT32_C(0x07040345)
#define TASK_HALT UINT32_C(0xaf000000)
#define DOUTU0_DMA (UINT32_C(1) << 21)

bool sgx535_mte_task(uint32_t address, uint32_t count, uint32_t link,
                     uint32_t words[SGX535_MTE_TASK_WORDS])
{
    if (!words || !count || count > SGX535_MTE_EMIT_COUNT)
        return false;
    memset(words, 0, SGX535_MTE_TASK_WORDS * sizeof(uint32_t));
    words[0] = address;
    words[1] = count - 1u;
    words[2] = link | DOUTU0_DMA;
    words[SGX535_MTE_TASK_DATA_WORDS] = TASK_DOUTD;
    words[SGX535_MTE_TASK_DATA_WORDS + 1u] = TASK_DOUTU;
    words[SGX535_MTE_TASK_DATA_WORDS + 2u] = TASK_HALT;
    return true;
}

bool sgx535_mte_emit_program(uint32_t words[4])
{
    struct sgx535_vertex_use_projection move;
    if (!words || sgx535_vertex_use_project(SGX535_MTE_EMIT_COUNT, &move) != SGX535_VERTEX_USE_OK ||
        move.instruction_count != 2)
        return false;
    words[0] = move.words[0];
    words[1] = move.words[1];
    words[2] = UINT32_C(0xa0200000) | SGX535_MTE_EMIT_COUNT << 7; /* emitst.end.freep */
    words[3] = UINT32_C(0xfb274000);
    return true;
}
