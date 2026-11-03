// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The TA state record (src/raster/sgx535_mte_state.c) against EMGD's
 * state records (docs/native-3d.md#ta-state-records): each parses
 * into groups and rebuilds word for word; the state the draws leave in force
 * gives the per-draw record EMGD sent (the pds group alone) as its change.
 * CPU only. */
#include "sgx535_mte_state.h"
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

static const struct {
    const char *name;
    uint32_t count, words[16];
} records[] = {
    {"ISP A, B, C", 4, {0x00000007, 0x01c00300, 0x000000f0, 0x0e92ffff}},
    {"a textured draw's program",
     12,
     {0x0000dec0, 0x00008800, 0x22030001, 0x0c008810, 0x0000000f, 0x0000000f, 0x00000000,
      0x06001000, 0x33d6bf95, 0x000f8000, 0x00000001, 0x00000000}},
    {"a constant-colour program",
     16,
     {0x0000d5c0, 0x0c00050c, 0x00078000, 0x0c008818, 0x0000000f, 0x0000000f, 0x43000000,
      0x43000000, 0x43000000, 0xc3000000, 0x3f000000, 0x3f000000, 0x09001100, 0x00088000,
      0x00000007, 0x00000000}},
    {"the next draw's uniforms", 4, {0x00000040, 0x0c000512, 0x00078000, 0x0c008818}},
};

int main(void)
{
    struct sgx535_mte_state s, before;
    uint32_t out[SGX535_MTE_STATE_WORDS], total = 0;
    for (unsigned g = 0; g < SGX535_MTE_GROUPS; g++)
        total += sgx535_mte_group_size((enum sgx535_mte_group)g);
    CHECK(total + 1u == SGX535_MTE_STATE_WORDS);
    CHECK(sgx535_mte_group_offset(SGX535_MTE_PDS) == 6 &&
          sgx535_mte_group_offset(SGX535_MTE_VIEWPORT) == 11 &&
          sgx535_mte_group_offset(SGX535_MTE_TEXFLOAT) == 23);
    memset(&s, 0, sizeof(s));
    for (unsigned r = 0; r < sizeof(records) / sizeof(records[0]); r++) {
        struct sgx535_mte_state one;
        uint32_t n;
        memset(&one, 0, sizeof(one));
        CHECK(sgx535_mte_parse(records[r].words, records[r].count, &one));
        n = sgx535_mte_record(&one, ~0u, out);
        if (n != records[r].count || memcmp(out, records[r].words, n * 4u)) {
            fprintf(stderr, "%s: not rebuilt\n", records[r].name);
            return 1;
        }
        before = s;
        CHECK(sgx535_mte_parse(records[r].words, records[r].count, &s));
        /* what changed is what EMGD sent, or less where it sent a group again */
        CHECK((sgx535_mte_changed(&before, &s) & ~records[r].words[0]) == 0);
    }
    /* the last draw changed its uniforms only: its record is EMGD's */
    CHECK(sgx535_mte_changed(&before, &s) == 1u << SGX535_MTE_PDS);
    CHECK(sgx535_mte_record(&s, sgx535_mte_changed(&before, &s), out) == 4 &&
          !memcmp(out, records[3].words, 16));
    /* a state built group by group gives EMGD's constant-colour record */
    {
        struct sgx535_mte_state b;
        const uint32_t *w = records[2].words;
        memset(&b, 0, sizeof(b));
        sgx535_mte_set(&b, SGX535_MTE_TEXFLOAT, w + 15);
        sgx535_mte_set(&b, SGX535_MTE_PDS, w + 1);
        sgx535_mte_set(&b, SGX535_MTE_RGNCLIP, w + 4);
        sgx535_mte_set(&b, SGX535_MTE_VIEWPORT, w + 6);
        sgx535_mte_set(&b, SGX535_MTE_OUTSELECTS, w + 12);
        sgx535_mte_set(&b, SGX535_MTE_MTECTRL, w + 13);
        sgx535_mte_set(&b, SGX535_MTE_TEXSIZE, w + 14);
        CHECK(sgx535_mte_record(&b, ~0u, out) == 16 && !memcmp(out, w, 64));
    }
    /* nothing changed: no record */
    CHECK(sgx535_mte_changed(&s, &s) == 0 && sgx535_mte_record(&s, 0, out) == 0);
    /* no state before: everything present */
    CHECK(sgx535_mte_changed(NULL, &s) == s.present);
    /* EMGD's state task for its 4-word record at f8480100 (entry 0x11a) */
    {
        uint32_t t[SGX535_MTE_TASK_WORDS], emit[4];
        static const uint32_t emgd[SGX535_MTE_TASK_WORDS] = {
            0xf8480100, 0x00000003, 0x0020011a, 0, 0,          0,          0,         0,
            0,          0,          0,          0, 0x07030223, 0x07040345, 0xaf000000};
        CHECK(sgx535_mte_task(0xf8480100u, 4, 0x11a, t));
        for (unsigned i = 0; i < SGX535_MTE_TASK_WORDS; i++)
            CHECK((i >= 4u && i < 12u) || t[i] == emgd[i]); /* EMGD's filler differs */
        /* EMGD's STATE word 1 but for its 96-byte attributes (it emits 24) */
        CHECK(SGX535_MTE_STATE_W1 == (0x0c02a206u & ~0xffu) + SGX535_MTE_EMIT_COUNT / 4u);
        CHECK(sgx535_mte_emit_program(emit) && emit[2] == (0xa0200000u | 16u << 7) &&
              emit[3] == 0xfb274000u);
        CHECK(!sgx535_mte_task(0, 0, 0x11a, t) && !sgx535_mte_task(0, 17, 0x11a, t));
    }
    CHECK(!sgx535_mte_parse(records[2].words, 15, &s));
    CHECK(!sgx535_mte_parse((const uint32_t[]){0x10000}, 1, &s));
    printf("PASS: sgx535 MTE state (%lu checks, %zu EMGD records)\n", checks,
           sizeof(records) / sizeof(records[0]));
    return 0;
}
