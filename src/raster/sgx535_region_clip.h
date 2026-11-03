// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_REGION_CLIP_H
#define SGX535_REGION_CLIP_H

#include <stdbool.h>
#include <stdint.h>

/* The region clip TA state group (two words) for GL's scissor, in EMGD's
 * form (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues): mode
 * 31:30 (2, outside: what lies outside the region is clipped), left 23:16
 * and right 7:0 in word 0, top 23:16 and bottom 7:0 in word 1, all in
 * 16-pixel tiles and inclusive, y counted from the top in the window and
 * from the bottom in a bottom-up FBO. Tiles hold the scissor's edges; the
 * pixel-exact edge is the viewport-pass objects' (docs/native-3d.md#current-draw-state). CPU
 * only. */

struct sgx535_scissor {
    uint8_t enable;
    int32_t x, y; /* GL's: from the bottom left */
    uint32_t width, height;
};

/* The words for SCISSOR on a target of WIDTH x HEIGHT pixels. No scissor is
 * the whole target. EMGD's bounds are not clamped to the target above (a
 * scissor past the right edge keeps its tile); below 0 they are. An empty
 * scissor gives left = right = the target's width in tiles, top = bottom = 0
 * (EMGD; the writer may drop the draw instead). false: a bound past 255
 * tiles or a target wider or taller than 4096 pixels. */
bool sgx535_region_clip(const struct sgx535_scissor *scissor, uint32_t width, uint32_t height,
                        bool bottom_up, uint32_t words[2]);

#endif
