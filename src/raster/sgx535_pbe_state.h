// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_PBE_STATE_H
#define SGX535_PBE_STATE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif
struct sgx535_pbe_surface {
    uint32_t width, height, stride_bytes;
    uint32_t origin_x, origin_y;
    uint64_t allocation_bytes;
};
struct sgx535_pbe_state {
    uint32_t words[4]; /* word1 awaits target VA, mask0xfffffffc. */
    uint32_t event_use_words[4];
    uint32_t required_bytes;
    /* Complete retained producer's pixel-event PDS layout. Address data at
     * word0 (state) and word2 (USE bank/entry) awaits owner-derived relocation.
     * All originally unwritten data words are explicitly zeroed. */
    uint32_t pixel_pds_data_words, pixel_pds_word_count;
    uint32_t pixel_pds_words[32];
};
/* Selected four-byte, pitch-based producer, with checked LINEAR extent.
 * Pitch is 128-byte aligned and at most 8192 bytes for this SGX535 field layout.
 * RGB channel order, tile/sample layout and PBE instruction effects remain
 * unqualified. No address/control caller input, allocation, upload or launch.
 * Unwritten word1 is zero, never interpreted as a usable address. Error=-1
 * preserves output; success0 is word construction only. Stable overlap works. */
int sgx535_pbe_state_project(const struct sgx535_pbe_surface *, struct sgx535_pbe_state *);
#endif
