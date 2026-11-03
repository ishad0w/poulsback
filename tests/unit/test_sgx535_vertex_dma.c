// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_dma.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %u: %s\n", (unsigned)__LINE__, #x);                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    static const struct {
        uint32_t count, transfers, first, second, offset;
    } literals[] = {{1, 1, 0x80000000u, 0, 0},
                    {4, 1, 0x80600003u, 0, 0},
                    {16, 1, 0x81e0000fu, 0, 0},
                    {17, 2, 0x01e0000fu, 0x80001000u, 64},
                    {27, 1, 0x81000028u, 0, 0},
                    {32, 1, 0x80e00037u, 0, 0},
                    {68, 2, 0x01e0003fu, 0x80604003u, 256},
                    {240, 1, 0x81c000feu, 0, 0},
                    {251, 2, 0x01e000efu, 0x8140f00au, 960},
                    {256, 1, 0x81e000ffu, 0, 0}};
    struct {
        uint32_t before;
        struct sgx535_vertex_dma_plan plan;
        uint32_t after;
    } guarded = {0};
    struct sgx535_vertex_dma_plan sentinel, saved;
    uint32_t n, i;
    guarded.before = 0x13792648u;
    guarded.after = 0x98765432u;
    for (i = 0; i < sizeof(literals) / sizeof(literals[0]); i++) {
        CHECK(sgx535_vertex_dma_build(literals[i].count, &guarded.plan) == SGX535_VERTEX_DMA_OK);
        CHECK(guarded.plan.transfer_count == literals[i].transfers);
        CHECK(guarded.plan.transfers[0].descriptor_word == literals[i].first);
        CHECK(guarded.plan.transfers[1].descriptor_word == literals[i].second);
        CHECK(guarded.plan.transfers[1].source_byte_offset == literals[i].offset);
    }
    for (n = 1; n <= SGX535_VERTEX_DMA_MAX_WORDS; n++) {
        uint32_t total = 0;
        CHECK(sgx535_vertex_dma_build(n, &guarded.plan) == SGX535_VERTEX_DMA_OK);
        CHECK(guarded.plan.input_words == n);
        CHECK(guarded.plan.transfer_count == 1u || guarded.plan.transfer_count == 2u);
        for (i = 0; i < guarded.plan.transfer_count; i++) {
            const struct sgx535_vertex_dma_transfer *t = &guarded.plan.transfers[i];
            uint32_t columns = (t->descriptor_word & 15u) + 1u;
            uint32_t rows = ((t->descriptor_word >> 4) & 15u) + 1u;
            CHECK(((t->descriptor_word >> 21) & 15u) + 1u == columns);
            CHECK(t->word_count == columns * rows);
            CHECK(t->preceding_words == total);
            CHECK(t->source_byte_offset == total * 4u);
            CHECK(((t->descriptor_word >> 8) & 255u) == total);
            CHECK((t->descriptor_word >> 31) == (i + 1u == guarded.plan.transfer_count));
            CHECK(!(t->descriptor_word & ~UINT32_C(0x81e0ffff)));
            total += t->word_count;
        }
        CHECK(total == n);
        if (guarded.plan.transfer_count == 1u) {
            static const struct sgx535_vertex_dma_transfer zero = {0};
            CHECK(!memcmp(&guarded.plan.transfers[1], &zero, sizeof(zero)));
        } else {
            CHECK(guarded.plan.transfers[0].word_count % 16u == 0u);
            CHECK(guarded.plan.transfers[1].word_count < 16u);
        }
        CHECK(guarded.before == 0x13792648u && guarded.after == 0x98765432u);
    }
    memset(&sentinel, 0xa5, sizeof(sentinel));
    saved = sentinel;
    CHECK(sgx535_vertex_dma_build(0, &sentinel) == SGX535_VERTEX_DMA_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_dma_build(257, &sentinel) == SGX535_VERTEX_DMA_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_dma_build(UINT32_MAX, &sentinel) == SGX535_VERTEX_DMA_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_dma_build(4, NULL) == SGX535_VERTEX_DMA_INVALID);
    printf("PASS %u static vertex DMA checks; no GPU execution\n", checks);
    return 0;
}
