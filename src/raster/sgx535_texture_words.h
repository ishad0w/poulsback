// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TEXTURE_WORDS_H
#define SGX535_TEXTURE_WORDS_H

#include "sgx535_texture_layout.h"

/* The three texture words (DOUTT0-2) a pixel PDS issue or a dependent smp's
 * SA triple carries, for a texture stored as sgx535_texture_layout says and a
 * sampler (docs/native-3d.md#texture-memory). The
 * words follow EMGD's where EMGD follows GL; where it departs, GL:
 * GL_CLAMP is OGLCLAMP, and the LOD clamp counts from the first level
 * sampled. CPU only. */

enum sgx535_texture_wrap {
    SGX535_WRAP_REPEAT,
    SGX535_WRAP_MIRRORED_REPEAT,
    SGX535_WRAP_CLAMP_TO_EDGE,
    SGX535_WRAP_CLAMP, /* GL_CLAMP: the border colour blends in at the edge */
    SGX535_WRAP_CLAMP_TO_BORDER
};
enum sgx535_texture_filter { SGX535_FILTER_NEAREST, SGX535_FILTER_LINEAR };
enum sgx535_texture_mip_filter { SGX535_MIP_NONE, SGX535_MIP_NEAREST, SGX535_MIP_LINEAR };

struct sgx535_texture_sampler {
    uint8_t wrap[3]; /* s, t, r (r only for 3D) */
    uint8_t min_filter, mag_filter, mip_filter;
    uint8_t max_anisotropy; /* 0 or 1: off; 2, 4, 8, 16 */
    uint8_t srgb;           /* the view's format is sRGB: decode on sampling (GAMMA) */
    float lod_bias;         /* the sampler's and the texture unit's, in levels */
    float min_lod;          /* its whole part rebases the first level, as EMGD */
};

/* The levels sampled (Gallium's view: BASE_LEVEL and MAX_LEVEL within the
 * stored chain) and, for RGBA16F/32F, the plane. */
struct sgx535_texture_view {
    uint32_t first_level, last_level;
    uint32_t plane;
};

/* DOUTT0-2 for a texture whose storage starts at ADDRESS (its GPU byte
 * address, 4-byte aligned); with CLAMP_TO_BORDER, DOUTT2 names the border
 * area's start. Refused: a level or plane the layout lacks, a cube from a
 * level above 0, a stride texture over several levels (a stride surface is
 * one level; USE picks it), CLAMP_TO_BORDER on a layout without a border area
 * (sgx535_texture_layout_border), MIRRORED_REPEAT beside a border axis,
 * anisotropy above 16. */
enum sgx535_texture_status sgx535_texture_words(const struct sgx535_texture_layout *layout,
                                                const struct sgx535_texture_sampler *sampler,
                                                const struct sgx535_texture_view *view,
                                                uint32_t address, uint32_t words[3]);

#endif
