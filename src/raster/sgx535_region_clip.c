// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_region_clip.h"

#define TILE 16
#define MODE_OUTSIDE (UINT32_C(2) << 30) /* clip what lies outside */

/* The tile of pixel P, 0 below the target. */
static uint32_t tile_of(int64_t p)
{
    return p < 0 ? 0u : (uint32_t)(p / TILE);
}

bool sgx535_region_clip(const struct sgx535_scissor *scissor, uint32_t width, uint32_t height,
                        bool bottom_up, uint32_t words[2])
{
    int64_t x0, x1, y0, y1;
    uint32_t left, right, top, bottom;
    if (!scissor || !words || !width || !height || width > 4096u || height > 4096u)
        return false;
    if (!scissor->enable) {
        x0 = 0, x1 = width, y0 = 0, y1 = height;
    } else {
        x0 = scissor->x, x1 = (int64_t)scissor->x + scissor->width;
        y0 = scissor->y, y1 = (int64_t)scissor->y + scissor->height;
    }
    if (x1 <= x0 || y1 <= y0 || x1 <= 0 || y1 <= 0) {
        left = right = (width + TILE - 1u) / TILE;
        top = bottom = 0;
    } else {
        left = tile_of(x0), right = tile_of(x1 - 1);
        /* rows from the top in the window */
        if (bottom_up)
            top = tile_of(y0), bottom = tile_of(y1 - 1);
        else
            top = tile_of((int64_t)height - y1), bottom = tile_of((int64_t)height - y0 - 1);
    }
    if (right > 255u || bottom > 255u)
        return false;
    words[0] = MODE_OUTSIDE | left << 16 | right;
    words[1] = top << 16 | bottom;
    return true;
}
