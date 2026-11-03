// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The pixel PDS program builder (src/raster/sgx535_pixel_pds.c) against
 * EMGD-form program vectors (docs/native-3d.md#pixel-pds-and-isp-words): a small PDS
 * interpreter runs both, and the outputs must agree: DOUTU's first three
 * dwords, each DOUTI's iteration word (the other dwords of an iteration that
 * issues to the USE are not read). Then the refusals. CPU only. */
#include "sgx535_pixel_pds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define MAX_OUTPUTS 16u
struct output {
    uint32_t kind; /* 5 DOUTU, 2 DOUTI */
    uint32_t dword[4];
};

/* Run WORDS (DATA_WORDS of data, then code to HALT): the outputs, or -1 for
 * an instruction this interpreter does not know. MOVS: SRC1 22:18 names DS0
 * dwords 2n and 2n + 1, SRC2 17:13 DS1's; selects 12:11, 10:9, 8:7, 6:5 pick
 * one of the four; the output is 3:0. */
static int run(const uint32_t *words, uint32_t word_count, uint32_t data_words,
               struct output out[MAX_OUTPUTS])
{
    int n = 0;
    for (uint32_t pc = data_words; pc < word_count; pc++) {
        uint32_t w = words[pc], src[4];
        if (w == 0xaf000000u)
            return n;
        if ((w >> 24) != 0x07u || (w >> 23 & 1u) || n == (int)MAX_OUTPUTS)
            return -1;
        for (unsigned b = 0; b < 2; b++)
            for (unsigned j = 0; j < 2; j++) {
                uint32_t i = 2u * (w >> (b ? 13 : 18) & 31u) + j;
                src[2u * b + j] = words[(2u * (i / 8u) + b) * 8u + i % 8u];
            }
        out[n].kind = w & 15u;
        for (unsigned d = 0; d < 4; d++)
            out[n].dword[d] = src[w >> (11 - 2 * d) & 3u];
        n++;
    }
    return -1;
}

static const struct {
    const char *name;
    uint32_t link, temps, phase2, iteration_count, iteration[2];
    uint32_t emgd[16]; /* 12 data words, then code */
    uint32_t emgd_words;
} cases[] = {
    {"blend draw 2, a constant colour",
     0x710,
     1,
     0,
     0,
     {0},
     {0x00000710, 0x08000000, 0xff102030, 0xff102030, 0xff102030, 0xff102030, 0xff102030,
      0xff102030, 0x00000020, 0xff102030, 0xff102030, 0xff102030, 0x07000365, 0xaf000000},
     14},
    {"blend draw 1, a smooth colour",
     0x510,
     1,
     0,
     1,
     {0x09c0a00f}, /* the backend's word: the builder marks the last issue */
     {0x00080510, 0x08000000, 0xff102030, 0xff102030, 0xff102030, 0xff102030, 0xff102030,
      0xff102030, 0x00000020, 0x0bc0a00f, 0xff102030, 0xff102030, 0x07000365, 0x070418a2,
      0xaf000000},
     15},
    {"alpha draw 3, punch-through",
     0x910,
     1,
     4,
     2,
     {0x0dc0a00f, 0x0d80e00f},
     {0x00080910, 0x09000017, 0x0f80e00f, 0, 0, 0, 0, 0, 0x00000020, 0x0dc0a00f, 0, 0, 0x07000365,
      0x070418a2, 0x07042362, 0xaf000000},
     16},
};

int main(void)
{
    struct sgx535_pixel_pds p;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct sgx535_pixel_pds_request r = {
            cases[c].link, cases[c].temps, cases[c].phase2, cases[c].iteration_count, {0}, 0,
            {{0}}};
        struct output ours[MAX_OUTPUTS], emgd[MAX_OUTPUTS];
        int n, m;
        memcpy(r.iteration, cases[c].iteration, sizeof(cases[c].iteration));
        CHECK(sgx535_pixel_pds_build(&r, &p));
        CHECK(p.data_words % 4u == 0 && p.word_count <= SGX535_PIXEL_PDS_MAX_WORDS);
        n = run(p.words, p.word_count, p.data_words, ours);
        m = run(cases[c].emgd, cases[c].emgd_words, 12, emgd);
        CHECK(n == (int)cases[c].iteration_count + 1 && m == n);
        for (int k = 0; k < n; k++) {
            unsigned compared = ours[k].kind == 5u ? 3u : 1u;
            CHECK(ours[k].kind == emgd[k].kind);
            for (unsigned d = 0; d < compared; d++)
                if (ours[k].dword[d] != emgd[k].dword[d]) {
                    fprintf(stderr, "%s: output %d dword %u: %08x, EMGD %08x\n", cases[c].name, k,
                            d, ours[k].dword[d], emgd[k].dword[d]);
                    return 1;
                }
        }
    }
    /* EMGD's c3 texture draw 3: a non-dependent read, its DOUTI (no USE
     * issue, TEXLASTISSUE added) and DOUTT; DOUTU0 waits for the texture */
    {
        static const uint32_t emgd_words[16] = {0x00100710, 0x10000000, 0x03fe0090, 0x0c050005,
                                                0,          0,          0,          0,
                                                0x00000020, 0x0c00f900, 0x60345000, 0,
                                                0x07000365, 0x070418a2, 0x07042364, 0xaf000000};
        struct sgx535_pixel_pds_request r = {0x710, 2, 0, 1, {0x0c00f100}, 1u, {{0}}};
        struct output ours[MAX_OUTPUTS], emgd[MAX_OUTPUTS];
        int n, m;
        r.texture[0][0] = 0x03fe0090u, r.texture[0][1] = 0x0c050005u, r.texture[0][2] = 0x60345000u;
        CHECK(sgx535_pixel_pds_build(&r, &p));
        n = run(p.words, p.word_count, p.data_words, ours);
        m = run(emgd_words, 16, 12, emgd);
        CHECK(n == 3 && m == 3);
        for (int k = 0; k < n; k++) {
            CHECK(ours[k].kind == emgd[k].kind);
            for (unsigned d = 0; d < (ours[k].kind == 2u ? 1u : 3u); d++)
                CHECK(ours[k].dword[d] == emgd[k].dword[d]);
        }
        CHECK(p.words[p.iteration_at[0]] == 0x0c00f900u);
        /* a texture mask past the iterations is refused */
        r.texture_mask = 2u;
        CHECK(!sgx535_pixel_pds_build(&r, &p));
    }
    /* EMGD's own one-output program is twelve data words; ours the same */
    CHECK(sgx535_pixel_pds_build(&(struct sgx535_pixel_pds_request){0x710, 1, 0, 0, {0}, 0, {{0}}},
                                 &p) &&
          p.data_words == 12 && p.word_count == 14);
    /* a temporary count past 31 continues in DOUTU2 3:0 */
    CHECK(sgx535_pixel_pds_build(&(struct sgx535_pixel_pds_request){0x710, 40, 0, 0, {0}, 0, {{0}}},
                                 &p) &&
          p.words[1] == 8u << 27 && p.words[8] == 0x21u);
    /* twelve iterations fit */
    {
        struct sgx535_pixel_pds_request r = {0x10, 2, 0,    SGX535_PIXEL_PDS_MAX_ITERATIONS,
                                             {0},  0, {{0}}};
        struct output out[MAX_OUTPUTS];
        for (unsigned i = 0; i < SGX535_PIXEL_PDS_MAX_ITERATIONS; i++)
            r.iteration[i] = 0x0d00000fu | i << 12;
        CHECK(sgx535_pixel_pds_build(&r, &p));
        CHECK(run(p.words, p.word_count, p.data_words, out) == 13);
        for (unsigned i = 0; i < SGX535_PIXEL_PDS_MAX_ITERATIONS; i++)
            CHECK((p.words[p.iteration_at[i]] & ~(1u << 25)) == r.iteration[i]);
        for (unsigned i = 0; i < SGX535_PIXEL_PDS_MAX_ITERATIONS; i++)
            CHECK(
                out[i + 1u].kind == 2u &&
                out[i + 1u].dword[0] ==
                    (r.iteration[i] | (i == SGX535_PIXEL_PDS_MAX_ITERATIONS - 1u ? 1u << 25 : 0)));
    }
    /* a texture-only iteration (USEISSUE none) does not wait for the iterators */
    CHECK(sgx535_pixel_pds_build(
              &(struct sgx535_pixel_pds_request){0x310, 1, 0, 1, {0x0c00f900}, 0, {{0}}}, &p) &&
          p.words[0] == 0x310u);
    CHECK(!sgx535_pixel_pds_build(
        &(struct sgx535_pixel_pds_request){0x710, 128, 0, 0, {0}, 0, {{0}}}, &p));
    CHECK(!sgx535_pixel_pds_build(&(struct sgx535_pixel_pds_request){0x710, 1, 3, 0, {0}, 0, {{0}}},
                                  &p));
    CHECK(!sgx535_pixel_pds_build(
        &(struct sgx535_pixel_pds_request){0x7ff00, 1, 4, 0, {0}, 0, {{0}}}, &p));
    CHECK(!sgx535_pixel_pds_build(
        &(struct sgx535_pixel_pds_request){
            0x710, 1, 0, SGX535_PIXEL_PDS_MAX_ITERATIONS + 1u, {0}, 0, {{0}}},
        &p));
    CHECK(!sgx535_pixel_pds_build(NULL, &p));
    /* DMS info: EMGD's constant-colour draws (sa0-6, one temporary), today's
     * window programs (a colour, one and nine temporaries) */
    {
        uint32_t dms;
        CHECK(sgx535_pixel_pds_dms(0, 1, 7, &dms) && dms == 0x00078000u);
        CHECK(sgx535_pixel_pds_dms(4, 1, 0, &dms) && dms == (4u | 3u << 16 | 24u << 25));
        CHECK(sgx535_pixel_pds_dms(0, 9, 0, &dms) && dms == (4u | 3u << 16 | 24u << 25));
        CHECK(sgx535_pixel_pds_dms(4, 20, 0, &dms) && dms == (4u | 1u << 16 | 13u << 25));
        CHECK(!sgx535_pixel_pds_dms(4, 129, 0, &dms));
    }
    printf("PASS: sgx535 pixel PDS (%lu checks, %zu EMGD programs)\n", checks,
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
