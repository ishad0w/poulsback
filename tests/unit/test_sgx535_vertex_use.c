// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_vertex_use.h"
#include "sgx535_usse.h"
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
    static const uint32_t four[] = {0xa0000000u, 0x28a13001u, 0xa0200000u, 0xfb275000u};
    static const uint32_t seventeen[] = {0xa0000000u, 0x28a1f001u, 0xa2000800u,
                                         0x28a10001u, 0xa0200000u, 0xfb275000u};
    struct {
        uint32_t before;
        struct sgx535_vertex_use_projection projection;
        uint32_t after;
    } guarded = {0};
    struct sgx535_vertex_use_projection sentinel, saved;
    uint32_t n, i;
    guarded.before = 0x31415926u;
    guarded.after = 0x27182818u;
    CHECK(sgx535_vertex_use_project(4, &guarded.projection) == SGX535_VERTEX_USE_OK);
    CHECK(guarded.projection.instruction_count == 2);
    CHECK(!memcmp(guarded.projection.words, four, sizeof(four)));
    CHECK(sgx535_vertex_use_project(17, &guarded.projection) == SGX535_VERTEX_USE_OK);
    CHECK(guarded.projection.instruction_count == 3);
    CHECK(!memcmp(guarded.projection.words, seventeen, sizeof(seventeen)));
    for (n = 1; n <= SGX535_VERTEX_USE_MAX_WORDS; n++) {
        uint32_t covered = 0, terminal;
        CHECK(sgx535_vertex_use_project(n, &guarded.projection) == SGX535_VERTEX_USE_OK);
        CHECK(guarded.projection.input_words == n);
        CHECK(guarded.projection.instruction_count == (n + 15u) / 16u + 1u);
        terminal = guarded.projection.instruction_count - 1u;
        for (i = 0; i < terminal; i++) {
            struct sgx535_mov mov = {.source = {SGX535_MOV_PRIMARY, covered},
                                     .destination = {SGX535_MOV_OUTPUT, covered},
                                     .end = 0};
            uint32_t direct[2];
            uint32_t raw_count;
            CHECK(sgx535_mov_encode(&mov, direct) == SGX535_MOV_OK);
            CHECK(guarded.projection.words[2u * i] == direct[0]);
            CHECK((guarded.projection.words[2u * i + 1u] & ~UINT32_C(0x0000f000)) == direct[1]);
            raw_count = ((guarded.projection.words[2u * i + 1u] >> 12) & 15u) + 1u;
            CHECK(raw_count >= 1u && raw_count <= 16u);
            CHECK(covered + raw_count <= n && covered + raw_count <= 128u);
            if (i + 1u < terminal)
                CHECK(raw_count == 16u);
            covered += raw_count;
        }
        CHECK(covered == n);
        CHECK(guarded.projection.words[2u * terminal] == 0xa0200000u);
        CHECK(guarded.projection.words[2u * terminal + 1u] == 0xfb275000u);
        for (i = 2u * guarded.projection.instruction_count;
             i < 2u * SGX535_VERTEX_USE_MAX_INSTRUCTIONS; i++)
            CHECK(guarded.projection.words[i] == 0u);
        CHECK(guarded.before == 0x31415926u && guarded.after == 0x27182818u);
    }
    memset(&sentinel, 0x5a, sizeof(sentinel));
    saved = sentinel;
    CHECK(sgx535_vertex_use_project(0, &sentinel) == SGX535_VERTEX_USE_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_use_project(129, &sentinel) == SGX535_VERTEX_USE_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_use_project(UINT32_MAX, &sentinel) == SGX535_VERTEX_USE_RANGE);
    CHECK(!memcmp(&sentinel, &saved, sizeof(saved)));
    CHECK(sgx535_vertex_use_project(4, NULL) == SGX535_VERTEX_USE_INVALID);
    printf("PASS %u static vertex USE checks; no GPU execution\n", checks);
    return 0;
}
