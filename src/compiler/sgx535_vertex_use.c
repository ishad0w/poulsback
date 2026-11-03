// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_use.h"

enum sgx535_vertex_use_status sgx535_vertex_use_project(uint32_t input_words,
                                                        struct sgx535_vertex_use_projection *out)
{
    struct sgx535_vertex_use_projection candidate = {0};
    uint32_t base, count, i = 0;
    if (!out)
        return SGX535_VERTEX_USE_INVALID;
    if (!input_words || input_words > SGX535_VERTEX_USE_MAX_WORDS)
        return SGX535_VERTEX_USE_RANGE;
    candidate.input_words = input_words;
    for (base = 0; base < input_words; base += 16u) {
        count = input_words - base;
        if (count > 16u)
            count = 16;
        candidate.words[2u * i] = UINT32_C(0xa0000000) | base << 21 | base << 7;
        candidate.words[2u * i + 1u] = UINT32_C(0x28a10001) | (count - 1u) << 12;
        i++;
    }
    candidate.words[2u * i] = UINT32_C(0xa0200000);
    candidate.words[2u * i + 1u] = UINT32_C(0xfb275000);
    candidate.instruction_count = i + 1u;
    *out = candidate;
    return SGX535_VERTEX_USE_OK;
}
