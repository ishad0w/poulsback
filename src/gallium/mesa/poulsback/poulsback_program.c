// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_program.h"
#include <stdlib.h>
#include <string.h>
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}

enum sgx535_nir_status
poulsback_fragment_uniform_snapshot(const struct sgx535_fragment_program *program,
                                    const void *buffer, size_t bytes, uint32_t *output,
                                    size_t capacity, size_t *count)
{
    uint32_t words[SGX535_FRAGMENT_UNIFORM_WORDS] = {0};
    size_t extent = 0, i;
    if (!program || !output || !count || (bytes && !buffer))
        return SGX535_NIR_ERR_ARGUMENT;
    if (capacity > SGX535_FRAGMENT_UNIFORM_WORDS ||
        bytes > SGX535_FRAGMENT_UNIFORM_WORDS * sizeof(*words) || (bytes & 3u))
        return SGX535_NIR_ERR_LIMIT;
    if (overlap(output, capacity * sizeof(*output), count, sizeof(*count)) ||
        overlap(output, capacity * sizeof(*output), program, sizeof(*program)) ||
        overlap(count, sizeof(*count), program, sizeof(*program)))
        return SGX535_NIR_ERR_ARGUMENT;
    for (i = 0; i < SGX535_FRAGMENT_UNIFORM_WORDS; i++)
        if (program->uniform_words_used[i / 32] & (1u << (i % 32)))
            extent = i + 1;
    if (program->secondary_register_count != extent)
        return SGX535_NIR_ERR_ARGUMENT;
    if (extent > capacity || extent > bytes / sizeof(*words))
        return SGX535_NIR_ERR_LIMIT;
    if (extent)
        memcpy(words, buffer, extent * sizeof(*words));
    if (extent)
        memcpy(output, words, extent * sizeof(*output));
    *count = extent;
    return SGX535_NIR_OK;
}
struct wire_work {
    struct sgx535_fragment_program validation;
    struct poulsback_render_instruction instruction[POULSBACK_RENDER_MAX_INSTRUCTIONS];
};
_Static_assert(SGX535_NIR_MAX_INSTRUCTIONS == POULSBACK_RENDER_MAX_INSTRUCTIONS,
               "Compiler/wire instruction bound must match explicitly");
