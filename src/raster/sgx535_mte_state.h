// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_MTE_STATE_H
#define SGX535_MTE_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* The TA state record a state program emits (docs/native-3d.md#ta-state-records): a header whose
 * bit n announces group n, then the
 * announced groups in bit order. EMGD sends a group only when it changed
 * (docs/re/emgd-linux/vertex-path.md#state-and-output-convention). CPU only. */

enum sgx535_mte_group {
    SGX535_MTE_ISPA,
    SGX535_MTE_ISPB,
    SGX535_MTE_ISPC,
    SGX535_MTE_BF_ISPA,
    SGX535_MTE_BF_ISPB,
    SGX535_MTE_BF_ISPC,
    SGX535_MTE_PDS,      /* SECONDARY program, a middle word, pixel PDS program */
    SGX535_MTE_RGNCLIP,  /* sgx535_region_clip */
    SGX535_MTE_VIEWPORT, /* Tx, Sx, Ty, Sy, Tz, Sz */
    SGX535_MTE_WRAP,
    SGX535_MTE_OUTSELECTS,
    SGX535_MTE_WCLAMP,
    SGX535_MTE_MTECTRL,
    SGX535_MTE_TERMINATE,
    SGX535_MTE_TEXSIZE,
    SGX535_MTE_TEXFLOAT,
    SGX535_MTE_GROUPS
};

#define SGX535_MTE_STATE_WORDS 25u /* the header and every group */

struct sgx535_mte_state {
    uint32_t present;                            /* bit n: group n has a value */
    uint32_t words[SGX535_MTE_STATE_WORDS - 1u]; /* each group at its offset, in bit order */
};

/* Group G's first word in a state's WORDS, and its size in words. */
uint32_t sgx535_mte_group_offset(enum sgx535_mte_group g);
uint32_t sgx535_mte_group_size(enum sgx535_mte_group g);

/* Set group G of STATE to VALUE (its size in words). */
void sgx535_mte_set(struct sgx535_mte_state *state, enum sgx535_mte_group g, const uint32_t *value);

/* The record of STATE's groups in MASK (and present): the header, then the
 * groups; its word count, 0 when MASK names no present group. */
uint32_t sgx535_mte_record(const struct sgx535_mte_state *state, uint32_t mask,
                           uint32_t words[SGX535_MTE_STATE_WORDS]);

/* The groups present in NOW that are absent from or differ in BEFORE (all
 * of NOW's with no BEFORE). */
uint32_t sgx535_mte_changed(const struct sgx535_mte_state *before,
                            const struct sgx535_mte_state *now);

/* A record's groups into STATE (others unchanged): false when the header
 * announces more words than COUNT holds. */
bool sgx535_mte_parse(const uint32_t *words, uint32_t count, struct sgx535_mte_state *state);

/* The state task that delivers a record (docs/native-3d.md#ta-state-records): its PDS program
 * DMAs COUNT record dwords from ADDRESS into the USE's attributes and starts
 * the emit program (LINK, sgx535_pds_use_link_project's fields) once the DMA
 * is done; the emit program moves the attributes to its outputs and emits
 * them as TA state. A TA STATE entry names the PDS program (word 0: the
 * address >> 4 under 0x40000000) with STATE_W1. */
#define SGX535_MTE_TASK_WORDS 15u /* 12 data words, then code */
#define SGX535_MTE_TASK_DATA_WORDS 12u
#define SGX535_MTE_EMIT_COUNT 16u /* dwords the emit program emits */
#define SGX535_MTE_STATE_W1 /* data 48 B, MTE emit, pipe 1, one partition, 64 B attributes */      \
    ((SGX535_MTE_TASK_DATA_WORDS / 4u) << 26 | 1u << 17 | 1u << 15 | 1u << 13 | 1u << 9 |          \
     SGX535_MTE_EMIT_COUNT / 4u)
/* false: COUNT 0 or past SGX535_MTE_EMIT_COUNT. */
bool sgx535_mte_task(uint32_t address, uint32_t count, uint32_t link,
                     uint32_t words[SGX535_MTE_TASK_WORDS]);
/* The emit program's two instructions (four words). */
bool sgx535_mte_emit_program(uint32_t words[4]);

#endif
