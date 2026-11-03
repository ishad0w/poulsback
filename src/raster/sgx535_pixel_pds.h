// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_PIXEL_PDS_H
#define SGX535_PIXEL_PDS_H

#include <stdbool.h>
#include <stdint.h>

/* The pixel PDS program of a fragment program variant (docs/native-3d.md#pixel-pds-and-isp-words):
 * its data, then
 * code that copies it to the outputs (one MOVS per output) and HALT.
 *
 * The outputs are the USE task (DOUTU: the code address, a wait for the
 * iterators when one issues to the USE, the temporaries, phase 2's entry
 * for punch-through and depth feedback) and one iteration
 * (DOUTI) per word of ITERATION, in order, the last one that issues to the
 * USE marked USELASTISSUE (25); the variant puts ZABS last.
 * Output k's four dwords are DS0 words 2k, 2k + 1 and DS1 words 2k, 2k + 1;
 * the data alternates DS0 and DS1 by rows of 8 dwords. EMGD packs its data
 * tighter; the outputs are the same. CPU only. */

#define SGX535_PIXEL_PDS_MAX_ITERATIONS 12u
#define SGX535_PIXEL_PDS_MAX_WORDS 160u

struct sgx535_pixel_pds_request {
    /* The code address's fields (sgx535_pds_use_link_project): bank, entry
     * high and low, the task's other bits 0 */
    uint32_t link;
    uint32_t temps;  /* temporaries the program uses */
    uint32_t phase2; /* 0, or the instruction where phase 2 starts (even) */
    uint32_t iteration_count;
    uint32_t iteration[SGX535_PIXEL_PDS_MAX_ITERATIONS]; /* DOUTI words */
    /* bit k: iteration k is a non-dependent texture read, its DOUTI followed
     * by a DOUTT of TEXTURE[k] (DOUTT0-2; EMGD's texture draws): the
     * last such DOUTI marked TEXLASTISSUE (11), DOUTU0 waiting for the
     * texture (20) */
    uint32_t texture_mask;
    uint32_t texture[SGX535_PIXEL_PDS_MAX_ITERATIONS][3];
};

struct sgx535_pixel_pds {
    uint32_t words[SGX535_PIXEL_PDS_MAX_WORDS];
    uint32_t word_count; /* data, then code */
    uint32_t data_words; /* a multiple of 4: the program's data size in 16-byte units x 4 */
    uint8_t iteration_at[SGX535_PIXEL_PDS_MAX_ITERATIONS]; /* iteration k's DOUTI data word */
};

/* false: a temporary count above 127, phase 2 odd or past the entry's 2048
 * 16-byte units, more iterations than outputs fit. */
bool sgx535_pixel_pds_build(const struct sgx535_pixel_pds_request *request,
                            struct sgx535_pixel_pds *program);

/* The TA pds group's middle word (DMS info) for a pixel task of PRIMARY
 * iterated dwords, TEMPS temporaries and SECONDARY SA dwords, as today's
 * window programs have it: without SA, primary at least 4, at most four
 * instances of the temporaries' 16-byte allocation in 192 bytes, 24 pixels
 * (fewer when the instances hold less); with SA, the ordinary SECONDARY rule
 * (sgx535_secondary_upload_internal.h), which gives EMGD's word for run
 * 231's constant-colour draws. false: past 128 of any. */
bool sgx535_pixel_pds_dms(uint32_t primary, uint32_t temps, uint32_t secondary, uint32_t *word);

#endif
