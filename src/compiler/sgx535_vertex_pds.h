// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_VERTEX_PDS_H
#define SGX535_VERTEX_PDS_H
#include "sgx535_vertex_dma.h"
#define SGX535_VERTEX_PDS_WORDS 17u
struct sgx535_vertex_pds_program {
    uint32_t data_words, word_count, dma_count;
    uint32_t words[SGX535_VERTEX_PDS_WORDS];
};
/* Matched producer layout, not an executable task certificate. Source byte
 * addresses remain zero for bounded owner-derived relocations; task word4
 * retains only the producer's fixed background 0x200000. No caller controls.
 * DMA read semantics, register count/background, PDS EXEC BASE effects and USE
 * bank ownership are separate contracts. The retained producer writes the
 * input-word count at data word8; this profile therefore requires tightly
 * packed input, stride_words == input_words. Padded strides are refused.
 * Errors leave output unchanged; stable input/output may overlap. */
enum sgx535_vertex_dma_status sgx535_vertex_pds_project(uint32_t input_words, uint32_t stride_words,
                                                        struct sgx535_vertex_pds_program *);
#endif
