// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_dma.h"

enum sgx535_vertex_dma_status sgx535_vertex_dma_build(uint32_t input_words,
                                                      struct sgx535_vertex_dma_plan *out)
{
    struct sgx535_vertex_dma_plan plan = {0};
    uint32_t columns, rows = 0, prefix, tail;
    if (!out)
        return SGX535_VERTEX_DMA_INVALID;
    if (!input_words || input_words > SGX535_VERTEX_DMA_MAX_WORDS)
        return SGX535_VERTEX_DMA_RANGE;
    plan.input_words = input_words;
    if (input_words <= 16u) {
        columns = input_words;
        rows = 1;
    } else {
        /* The retained producer chooses the largest divisor below sixteen.
         * A smaller divisor only increases a quotient already over sixteen. */
        for (columns = 15; columns > 1u; columns--) {
            if (!(input_words % columns)) {
                rows = input_words / columns;
                if (rows > 16u)
                    rows = 0;
                break;
            }
        }
    }
    if (!rows) {
        columns = 16;
        rows = input_words / columns;
    }
    prefix = rows * columns;
    tail = input_words - prefix;
    plan.transfer_count = tail ? 2u : 1u;
    plan.transfers[0].word_count = prefix;
    plan.transfers[0].descriptor_word = (rows - 1u) << 4 | (columns - 1u) << 21 | (columns - 1u);
    if (tail) {
        plan.transfers[1].source_byte_offset = prefix * 4u;
        plan.transfers[1].preceding_words = prefix;
        plan.transfers[1].word_count = tail;
        plan.transfers[1].descriptor_word =
            UINT32_C(0x80000000) | (tail - 1u) << 21 | prefix << 8 | (tail - 1u);
    } else {
        plan.transfers[0].descriptor_word |= UINT32_C(0x80000000);
    }
    *out = plan;
    return SGX535_VERTEX_DMA_OK;
}
