// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_pds.h"
#include "sgx535_secondary_upload_internal.h"

#include <string.h>

#define ROW 8u                   /* DS0 and DS1 alternate by rows of this many dwords */
#define MOVS (UINT32_C(7) << 24) /* MOVS, condition always */
#define SELECT_0123 ((0u << 11) | (1u << 9) | (2u << 7) | (3u << 5))
#define OUT_DOUTI 2u
#define OUT_DOUTT 4u
#define OUT_DOUTU 5u
#define HALT UINT32_C(0xaf000000)
/* DOUTU0 waits for the iterators (19); DOUTU1 PUNCHTHROUGH 25:24 = PHASE1,
 * EXE1VALID 0, EXE1 11:1 (EMGD's tasks, c4 `blend` and `alpha`) */
#define DOUTU0_ITERATORS (UINT32_C(1) << 19)
#define DOUTU0_TEXTURE (UINT32_C(1) << 20)
#define TEXLASTISSUE (UINT32_C(1) << 11)
#define DOUTU1_PHASE1 (UINT32_C(1) << 24)
#define USEISSUE_NONE 15u
#define USELASTISSUE (UINT32_C(1) << 25)
#define DOUTU2 UINT32_C(0x20)

/* The data word of DS<BANK> dword I. */
static uint32_t at(uint32_t bank, uint32_t i)
{
    return (2u * (i / ROW) + bank) * ROW + i % ROW;
}

/* Output K: DATA in DS0 dwords 2K, 2K + 1 and DS1 dwords 2K, 2K + 1, and its
 * MOVS to OUT. */
static void output(struct sgx535_pixel_pds *p, uint32_t k, uint32_t out, const uint32_t data[4])
{
    p->words[at(0, 2u * k)] = data[0];
    p->words[at(0, 2u * k + 1u)] = data[1];
    p->words[at(1, 2u * k)] = data[2];
    p->words[at(1, 2u * k + 1u)] = data[3];
    p->words[p->data_words + k] = MOVS | k << 18 | k << 13 | SELECT_0123 | out;
}
bool sgx535_pixel_pds_build(const struct sgx535_pixel_pds_request *r, struct sgx535_pixel_pds *p)
{
    uint32_t outputs, last, entry, k = 0, waits = 0, last_use = UINT32_MAX,
                                   last_texture = UINT32_MAX;
    if (!r || !p || r->temps > 127u || (r->phase2 & 1u) ||
        r->iteration_count > SGX535_PIXEL_PDS_MAX_ITERATIONS ||
        r->texture_mask >> r->iteration_count)
        return false;
    entry = (r->link >> 8) & 0x7ffu;
    if (r->phase2 && entry + r->phase2 / 2u > 0x7ffu)
        return false;
    for (uint32_t i = 0; i < r->iteration_count; i++) {
        if ((r->iteration[i] >> 12 & 15u) != USEISSUE_NONE) {
            waits |= DOUTU0_ITERATORS;
            last_use = i;
        }
        if (r->texture_mask >> i & 1u) {
            waits |= DOUTU0_TEXTURE;
            last_texture = i;
        }
    }
    outputs = 1u + r->iteration_count + (uint32_t)__builtin_popcount(r->texture_mask);
    last = at(1, 2u * outputs - 1u);
    memset(p, 0, sizeof(*p));
    p->data_words = (last + 4u) & ~3u;
    if (p->data_words + outputs + 1u > SGX535_PIXEL_PDS_MAX_WORDS)
        return false;
    output(p, k++, OUT_DOUTU,
           (const uint32_t[4]){
               r->link | waits,
               (r->temps & 31u) << 27 |
                   (r->phase2 ? DOUTU1_PHASE1 | (entry + r->phase2 / 2u) << 1 | 1u : 0u),
               DOUTU2 | r->temps >> 5, 0});
    for (uint32_t i = 0; i < r->iteration_count; i++) {
        p->iteration_at[i] = (uint8_t)at(0, 2u * k);
        output(p, k++, OUT_DOUTI,
               (const uint32_t[4]){r->iteration[i] | (i == last_use ? USELASTISSUE : 0) |
                                       (i == last_texture ? TEXLASTISSUE : 0),
                                   0, 0, 0});
        /* a texture read's words follow its DOUTI (EMGD's c3 draws) */
        if (r->texture_mask >> i & 1u)
            output(p, k++, OUT_DOUTT,
                   (const uint32_t[4]){r->texture[i][0], r->texture[i][1], r->texture[i][2], 0});
    }
    p->words[p->data_words + outputs] = HALT;
    p->word_count = p->data_words + outputs + 1u;
    return true;
}
bool sgx535_pixel_pds_dms(uint32_t primary, uint32_t temps, uint32_t secondary, uint32_t *word)
{
    struct sgx535_ordinary_secondary_dms dms;
    uint32_t instances, pixels;
    if (!word || primary > 128u || temps > 128u || secondary > 128u)
        return false;
    if (secondary) {
        if (!sgx535_ordinary_secondary_dms(primary, temps ? temps : 1u, secondary, &dms))
            return false;
        *word = dms.word;
        return true;
    }
    instances = 192u / ((temps * 4u + 15u) & ~15u ? (temps * 4u + 15u) & ~15u : 16u);
    instances = instances > 4u ? 4u : instances ? instances : 1u;
    pixels = 24u + 3u > instances * 8u ? instances * 8u - 3u : 24u;
    *word = (primary > 4u ? primary : 4u) | (instances - 1u) << 16 | (pixels & 31u) << 25;
    return true;
}
