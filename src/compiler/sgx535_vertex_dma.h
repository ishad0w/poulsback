// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_VERTEX_DMA_H
#define SGX535_VERTEX_DMA_H
#ifdef __KERNEL__
#include <linux/types.h>
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#else
#include <stdint.h>
#endif

/* Static word projection only. 256 is this constructor's bounded policy,
 * not an architectural limit. No device address, task or cache permission. */
#define SGX535_VERTEX_DMA_MAX_WORDS 256u
enum sgx535_vertex_dma_status {
    SGX535_VERTEX_DMA_OK,
    SGX535_VERTEX_DMA_INVALID,
    SGX535_VERTEX_DMA_RANGE
};
struct sgx535_vertex_dma_transfer {
    uint32_t source_byte_offset;
    uint32_t preceding_words;
    uint32_t word_count;
    uint32_t descriptor_word;
};
struct sgx535_vertex_dma_plan {
    uint32_t input_words;
    uint32_t transfer_count;
    struct sgx535_vertex_dma_transfer transfers[2];
};
/* Source offsets are within the input object. Absolute address/domain,
 * destination interpretation and physical DMA completion are not established.
 * Refusal leaves the complete output unchanged. */
enum sgx535_vertex_dma_status sgx535_vertex_dma_build(uint32_t input_words,
                                                      struct sgx535_vertex_dma_plan *out);
#endif
