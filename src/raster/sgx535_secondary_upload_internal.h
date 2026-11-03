// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_SECONDARY_UPLOAD_INTERNAL_H
#define SGX535_SECONDARY_UPLOAD_INTERNAL_H
#include "sgx535_vertex_dma.h"

/* Ordinary PSB SECONDARY, not the separate twelve-DWORD Boxee program.
 * Addresses and the empty DM1 USE entry remain zero relocation fields. */
#define SGX535_ORDINARY_SECONDARY_CAPACITY 96u
#define SGX535_ORDINARY_SECONDARY_UPLOAD_BYTES (128u * 4u)

struct sgx535_ordinary_secondary_dms {
    uint32_t sa_budget_words, temp_bytes, instances, pixels, word;
};

static inline int sgx535_ordinary_secondary_dms(uint32_t primary, uint32_t temporary,
                                                uint32_t secondary,
                                                struct sgx535_ordinary_secondary_dms *out)
{
    struct sgx535_ordinary_secondary_dms p;
    uint32_t rounded_primary, bound;
    if (!out || primary > 128u || !temporary || temporary > 128u || secondary > 128u)
        return 0;
    rounded_primary = (primary + 3u) & ~3u;
    p.sa_budget_words = ((0x4dfu - ((secondary + 31u) & ~31u)) & 0xffffffa0u) / 3u;
    p.temp_bytes = (temporary * 4u + 15u) & ~15u;
    p.instances = 192u / p.temp_bytes;
    if (p.instances > 4u)
        p.instances = 4u;
    p.pixels = 32u;
    if (rounded_primary) {
        bound = p.sa_budget_words / (rounded_primary * 4u);
        if (p.instances > bound)
            p.instances = bound;
        p.pixels = bound > 32u ? 32u : bound ? bound : 1u;
    }
    if (!p.instances)
        p.instances = 1u;
    bound = p.pixels + 3u < 32u ? p.pixels + 3u : 32u;
    if (bound > p.instances * 8u)
        p.pixels = p.instances * 8u - 3u;
    p.word = primary | (p.instances - 1u) << 16 | (p.pixels & 31u) << 25;
    if (secondary)
        p.word |= ((secondary * 4u + 127u) / 128u) << 18 | 0x8000u;
    *out = p;
    return 1;
}

static inline void sgx535_ordinary_secondary_put(uint8_t *p, uint32_t v)
{
    for (uint32_t i = 0; i < 4u; i++)
        p[i] = (uint8_t)(v >> (i * 8u));
}

static inline int sgx535_ordinary_secondary_plan(uint32_t words, struct sgx535_vertex_dma_plan *dma)
{
    /* Reserved-only SA0 needs a separate memory/state producer. */
    return words >= 2u && words <= 128u &&
           sgx535_vertex_dma_build(words, dma) == SGX535_VERTEX_DMA_OK;
}

static inline int sgx535_ordinary_secondary_emit(uint32_t words, const uint32_t *uniforms,
                                                 uint8_t *bytes, uint32_t capacity)
{
    struct sgx535_vertex_dma_plan dma;
    uint32_t code = 16u, size = SGX535_ORDINARY_SECONDARY_CAPACITY + words * 4u;
    if (!uniforms || !bytes || !sgx535_ordinary_secondary_plan(words, &dma) || capacity < size)
        return 0;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = 0;
    sgx535_ordinary_secondary_put(bytes + 4u, words <= 16u ? 0x80000000u | (words - 1u)
                                                           : dma.transfers[0].descriptor_word);
    if (dma.transfer_count == 2u)
        sgx535_ordinary_secondary_put(bytes + 36u, dma.transfers[1].descriptor_word);
    sgx535_ordinary_secondary_put(bytes + code++ * 4u, 0x07030223u);
    if (dma.transfer_count == 2u)
        sgx535_ordinary_secondary_put(bytes + code++ * 4u, 0x07601763u);
    sgx535_ordinary_secondary_put(bytes + code++ * 4u, 0x07042345u);
    sgx535_ordinary_secondary_put(bytes + code++ * 4u, 0xaf000000u);
    for (uint32_t i = 1; i < words; i++)
        sgx535_ordinary_secondary_put(bytes + SGX535_ORDINARY_SECONDARY_CAPACITY + i * 4u,
                                      uniforms[i - 1u]);
    return 1;
}
#endif
