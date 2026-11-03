// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DRAW_MASK_H
#define SGX535_DRAW_MASK_H

#include <stdbool.h>
#include <stdint.h>

/* The scissor's pixel-exact edges (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues):
 * the ISP keeps a draw
 * mask per pixel, and viewport objects (pass type 3, no tag or depth write)
 * clear it (ISPC 0) or set it (ISPC 1 << 25) over a screen rectangle. A scissor change clears the
 * previous rectangle and sets the new one, as EMGD does. Rectangles are in target pixels from the
 * top left, half-open,
 * drawn as pretransformed quads (x, y), before the scissored draw. CPU only. */

struct sgx535_rect {
    int32_t x0, y0, x1, y1; /* x0 <= x1, y0 <= y1; empty when equal */
};

struct sgx535_mask_object {
    uint32_t ispa, ispb, ispc;
    struct sgx535_rect rect;
};

/* GL's scissor (from the bottom left) as a target rectangle: y flipped in the
 * window, kept in a bottom-up FBO. false for a negative size. */
bool sgx535_scissor_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                         uint32_t target_height, bool bottom_up, struct sgx535_rect *rect);

/* The objects that take the draw mask from FROM to TO, NULL for the whole
 * target (W x H): clear FROM and set TO; the whole target set again when TO
 * is NULL; nothing when they are the same. An empty TO only clears (the
 * scissored draws then draw nothing). Returns the count (0-2), or -1 for a
 * target over 4096 pixels. */
int sgx535_draw_mask_objects(const struct sgx535_rect *from, const struct sgx535_rect *to,
                             uint32_t width, uint32_t height, struct sgx535_mask_object out[2]);

#endif
