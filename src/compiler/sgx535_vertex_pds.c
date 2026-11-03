// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_pds.h"
enum sgx535_vertex_dma_status sgx535_vertex_pds_project(uint32_t input_words, uint32_t stride_words,
                                                        struct sgx535_vertex_pds_program *out)
{
    struct sgx535_vertex_pds_program p = {0};
    struct sgx535_vertex_dma_plan dma;
    uint32_t code = 12;
    if (!out)
        return SGX535_VERTEX_DMA_INVALID;
    if (stride_words != input_words ||
        sgx535_vertex_dma_build(input_words, &dma) != SGX535_VERTEX_DMA_OK)
        return SGX535_VERTEX_DMA_RANGE;
    p.data_words = 12;
    p.dma_count = dma.transfer_count;
    p.words[1] = dma.transfers[0].descriptor_word;
    if (dma.transfer_count == 2)
        p.words[3] = dma.transfers[1].descriptor_word;
    p.words[4] = UINT32_C(0x00200000);
    p.words[8] = stride_words * 4u;
    p.words[code++] = UINT32_C(0x67800070);
    p.words[code++] = UINT32_C(0x2f030343);
    if (dma.transfer_count == 2)
        p.words[code++] = UINT32_C(0x2f070343);
    p.words[code++] = UINT32_C(0x030803e5);
    p.words[code++] = UINT32_C(0xaf000000);
    p.word_count = code;
    *out = p;
    return SGX535_VERTEX_DMA_OK;
}
