// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pbe_state.h"
#include "sgx535_pixel_event.h"
int sgx535_pbe_state_project(const struct sgx535_pbe_surface *s, struct sgx535_pbe_state *out)
{
    struct sgx535_pbe_state p = {0};
    struct sgx535_pixel_event_program event;
    uint64_t right, bottom, extent;
    uint32_t i;
    /* The ten-bit line-stride field counts pairs of four-byte pixels. */
    if (!s || !out || !s->width || !s->height || !s->stride_bytes || (s->stride_bytes & 127u) ||
        s->stride_bytes > 8192u)
        return -1;
    right = (uint64_t)s->origin_x + s->width;
    bottom = (uint64_t)s->origin_y + s->height;
    if (right > 4096 || bottom > 4096 || right * 4u > s->stride_bytes)
        return -1;
    extent = (bottom - 1u) * s->stride_bytes + right * 4u;
    if (extent > s->allocation_bytes || extent > 0xffffffffu)
        return -1;
    p.words[0] = 0x8000u | (((s->stride_bytes / 4u - 1u) / 2u) << 16);
    p.words[2] = s->origin_y << 12 | s->origin_x;
    p.words[3] = (uint32_t)(bottom - 1u) << 12 | (uint32_t)(right - 1u);
    if (sgx535_pixel_event_build(SGX535_PIXEL_EVENT_PBE_ONLY, &event))
        return -1;
    p.required_bytes = (uint32_t)extent;
    p.pixel_pds_data_words = event.pds_data_words;
    p.pixel_pds_word_count = event.pds_word_count;
    for (i = 0; i < 4; i++)
        p.event_use_words[i] = event.use_words[i];
    for (i = 0; i < 32; i++)
        p.pixel_pds_words[i] = event.pds_words[i];
    *out = p;
    return 0;
}
