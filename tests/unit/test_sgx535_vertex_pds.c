// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_pds.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct sgx535_vertex_pds_program p, before;
    uint32_t count, stride, i;
    unsigned checks = 0;
    for (count = 1; count <= 256; count++) {
        for (stride = 1; stride <= 256; stride++) {
            uint32_t total = 0;
            if (stride != count) {
                memset(&p, 0xa5, sizeof(p));
                before = p;
                assert(sgx535_vertex_pds_project(count, stride, &p) == SGX535_VERTEX_DMA_RANGE);
                assert(!memcmp(&p, &before, sizeof(p)));
                continue;
            }
            assert(sgx535_vertex_pds_project(count, stride, &p) == SGX535_VERTEX_DMA_OK);
            checks++;
            assert(p.data_words == 12 && p.dma_count >= 1 && p.dma_count <= 2);
            assert(p.word_count == 15 + p.dma_count && p.words[8] == stride * 4);
            assert(p.words[4] == 0x200000 && p.words[0] == 0 && p.words[2] == 0);
            for (i = 0; i < p.dma_count; i++) {
                uint32_t d = p.words[2 * i + 1];
                uint32_t columns = ((d >> 21) & 15u) + 1;
                uint32_t rows = ((d >> 4) & 15u) + 1;
                assert((d & 15u) + 1 == columns);
                assert(!!(d & 0x80000000u) == (i + 1 == p.dma_count));
                if (i)
                    assert(((d >> 8) & 255u) == total && rows == 1);
                total += columns * rows;
            }
            assert(total == count);
            assert(p.words[12] == 0x67800070 && p.words[13] == 0x2f030343);
            if (p.dma_count == 2)
                assert(p.words[14] == 0x2f070343);
            assert(p.words[p.word_count - 2] == 0x030803e5);
            assert(p.words[p.word_count - 1] == 0xaf000000);
            for (i = 0; i < 12; i++)
                if (i != 1 && i != 3 && i != 4 && i != 8)
                    assert(!p.words[i]);
            if (p.dma_count == 1)
                assert(!p.words[3] && !p.words[16]);
        }
    }
    before = p;
    assert(sgx535_vertex_pds_project(0, 4, &p) == SGX535_VERTEX_DMA_RANGE);
    assert(!memcmp(&before, &p, sizeof(p)));
    assert(sgx535_vertex_pds_project(257, 257, &p) == SGX535_VERTEX_DMA_RANGE);
    assert(!memcmp(&before, &p, sizeof(p)));
    assert(sgx535_vertex_pds_project(4, 3, &p) == SGX535_VERTEX_DMA_RANGE);
    assert(!memcmp(&before, &p, sizeof(p)));
    assert(sgx535_vertex_pds_project(4, UINT32_MAX, &p) == SGX535_VERTEX_DMA_RANGE);
    assert(!memcmp(&before, &p, sizeof(p)));
    assert(sgx535_vertex_pds_project(4, 4, NULL) == SGX535_VERTEX_DMA_INVALID);
    assert(sgx535_vertex_pds_project(4, 4, &p) == SGX535_VERTEX_DMA_OK);
    assert(p.word_count == 16 && p.words[1] == 0x80600003 && p.words[8] == 16);
    printf(
        "PASS %u vertex PDS layouts: full data/code, DMA spans, zero unused words; execution unqualified\n",
        checks);
    return 0;
}
