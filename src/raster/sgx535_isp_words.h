// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_ISP_WORDS_H
#define SGX535_ISP_WORDS_H

#include <stdbool.h>
#include <stdint.h>

/* A draw's ISP state words and MTE cull mode from GL's depth, stencil,
 * polygon offset, face and colour state (docs/native-3d.md#pixel-pds-and-isp-words).
 * GL's orders: compare functions NEVER (0) to ALWAYS (7); stencil ops KEEP 0,
 * ZERO 1, REPLACE 2, INCR 3, DECR 4, INVERT 5, INCR_WRAP 6, DECR_WRAP 7.
 * CPU only. */

struct sgx535_isp_stencil {
    uint8_t func, ref, value_mask, write_mask;
    uint8_t fail, zfail, zpass;
};

enum sgx535_isp_cull { SGX535_CULL_NONE, SGX535_CULL_FRONT, SGX535_CULL_BACK, SGX535_CULL_BOTH };

struct sgx535_isp_state {
    uint8_t depth_test, depth_func, depth_write;
    uint8_t stencil_test;
    struct sgx535_isp_stencil front, back; /* GL's faces */
    uint8_t offset;                        /* polygon offset fill */
    int8_t offset_factor, offset_units;    /* whole numbers, -16..15 */
    uint8_t cull, front_ccw;
    uint8_t bottom_up; /* a bottom-up FBO; else the window (y flipped) */
    uint8_t pass_type; /* the epilogue variant's (sgx535_fs_variant) */
    uint8_t colormask; /* bit 0 R, 1 G, 2 B, 3 A */
    uint8_t no_color;  /* the variant writes no colour (or alpha NEVER) */
};

struct sgx535_isp_words {
    uint32_t ispa, ispb, ispc;          /* ISPC valid when has_ispc */
    uint32_t bf_ispa, bf_ispb, bf_ispc; /* valid when two_sided */
    uint8_t has_ispc, two_sided;
    uint8_t cull; /* MTE control 1:0: 0 none, 1 cw, 2 ccw */
};

/* The words for STATE. false for what has no form: a polygon offset outside
 * -16..15, a function or op above 7, culling both faces (the draw rasterises
 * nothing: the caller drops it). */
bool sgx535_isp_words(const struct sgx535_isp_state *state, struct sgx535_isp_words *words);

#endif
