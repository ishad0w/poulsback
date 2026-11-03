// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_PIXEL_EVENT_H
#define SGX535_PIXEL_EVENT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

enum sgx535_pixel_event_variant {
    SGX535_PIXEL_EVENT_PBE_ONLY = 0,
    SGX535_PIXEL_EVENT_PTOFF_PBE = 1
};

struct sgx535_pixel_event_program {
    uint32_t pds_words[37];
    uint32_t use_words[8];
    uint32_t pds_data_words, pds_word_count, use_word_count;
};

/* Internal selected event profiles. Address/link words await owner-derived
 * relocation; successful word construction grants no execution or release.
 * Returns 0 on success, -1 on invalid input, preserving output on error. */
int sgx535_pixel_event_build(enum sgx535_pixel_event_variant, struct sgx535_pixel_event_program *);

#endif
