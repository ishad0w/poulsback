// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_ZLS_H
#define SGX535_ZLS_H

#include <stdbool.h>
#include <stdint.h>

/* A render's depth and stencil load, store and clear words
 * (docs/native-3d.md#typed-depth-preparation-and-clear-prefix):
 * ISP_ZLSCTL and the background object's depth and stencil (ISP_BGOBJDEPTH,
 * ISP_BGOBJ), fields named after EUR_CR_ISP_*.
 * Without a load or store, ZLSCTL is the tile extent alone, as EMGD's window
 * renders (ceil(width / 16) - 1). A load or store takes the bits the window
 * depth path stores and reloads D24S8 with on the Dell (sgx535_depth_state.c):
 * depth and stencil together, forced, both formats, the extent of the stored
 * surface's 32-pixel pitch. The clear is the background object's: depth F32,
 * stencil, the draw mask set and the background tag on (EMGD's 0x300). CPU
 * only. */

enum sgx535_zls_format { SGX535_ZLS_NONE, SGX535_ZLS_D16, SGX535_ZLS_D24S8 };

struct sgx535_zls_request {
    uint8_t format;
    uint32_t width;      /* target pixels */
    uint8_t load, store; /* the depth (and stencil) plane from memory, to memory */
    float clear_depth;
    uint8_t clear_stencil;
};

struct sgx535_zls_words {
    uint32_t zlsctl, bgobjdepth, bgobj;
};

/* false: a load or store without a format, a width of 0 or over 4096. */
bool sgx535_zls_words(const struct sgx535_zls_request *request, struct sgx535_zls_words *words);

#endif
