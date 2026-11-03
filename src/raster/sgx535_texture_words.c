// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_texture_words.h"

/* DOUTT0 address modes (U 8:6, V 5:3, S 2:0). */
enum {
    MODE_REPEAT = 0,
    MODE_FLIP = 1,
    MODE_CLAMP = 2,
    MODE_REPEAT_BDRMEM = 4, /* repeat beside a border axis */
    MODE_CLAMP_BDR = 5,
    MODE_CLAMP_BDRMEM = 6, /* clamp to edge beside a border axis */
    MODE_OGLCLAMP = 7
};

static uint32_t log2_of(uint32_t v)
{
    uint32_t n = 0;
    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}

static uint32_t level_size(uint32_t size, uint32_t level)
{
    size >>= level;
    return size ? size : 1u;
}

/* One axis's mode; BORDER: another axis clamps to the border. -1: no form. */
static int address_mode(uint8_t wrap, bool border)
{
    switch (wrap) {
    case SGX535_WRAP_REPEAT:
        return border ? MODE_REPEAT_BDRMEM : MODE_REPEAT;
    case SGX535_WRAP_MIRRORED_REPEAT:
        return border ? -1 : MODE_FLIP;
    case SGX535_WRAP_CLAMP_TO_EDGE:
        return border ? MODE_CLAMP_BDRMEM : MODE_CLAMP;
    case SGX535_WRAP_CLAMP:
        return MODE_OGLCLAMP;
    case SGX535_WRAP_CLAMP_TO_BORDER:
        return MODE_CLAMP_BDR;
    default:
        return -1;
    }
}

/* MIPMAPCLAMP, the highest LOD sampled counted from the first level: 15 for a
 * texture of one level (and a stride surface), 0 without a mip filter. A
 * chain sampled to its end takes its level count from the first level, as
 * EMGD writes it (64x64: 7); a MAX_LEVEL below the end, the last level. */
static uint32_t lod_clamp(const struct sgx535_texture_layout *layout,
                          const struct sgx535_texture_sampler *sampler, uint32_t first,
                          uint32_t last)
{
    if (layout->levels == 1u || layout->type == SGX535_TEXTYPE_STRIDE)
        return 15u;
    if (sampler->mip_filter == SGX535_MIP_NONE)
        return 0;
    return last == layout->levels - 1u ? layout->levels - first : last - first;
}

enum sgx535_texture_status sgx535_texture_words(const struct sgx535_texture_layout *layout,
                                                const struct sgx535_texture_sampler *sampler,
                                                const struct sgx535_texture_view *view,
                                                uint32_t address, uint32_t words[3])
{
    uint32_t first, last, w, h, d, adjust, aniso = 0, modes = 0, size_bits, rebase;
    bool border, three_d;
    float eighths;
    int bias;
    if (!layout || !sampler || !view || !words || (address & 3u))
        return SGX535_TEXTURE_INVALID;
    three_d = layout->target == SGX535_TEXTURE_3D;
    last = view->last_level < layout->levels ? view->last_level : layout->levels - 1u;
    /* MIN_LOD's whole part, bounded before the conversion (a float past the
     * integer's range is undefined) and the sum before it can wrap: past the
     * levels either way, the last level is the first */
    rebase = sampler->min_lod >= (float)SGX535_TEXTURE_MAX_LEVELS ? SGX535_TEXTURE_MAX_LEVELS
             : sampler->min_lod > 0.0f                            ? (uint32_t)sampler->min_lod
                                                                  : 0u;
    first = view->first_level < SGX535_TEXTURE_MAX_LEVELS ? view->first_level + rebase
                                                          : view->first_level;
    if (view->first_level > last || view->plane >= layout->planes ||
        sampler->max_anisotropy > 16u || sampler->min_filter > SGX535_FILTER_LINEAR ||
        sampler->mag_filter > SGX535_FILTER_LINEAR || sampler->mip_filter > SGX535_MIP_LINEAR)
        return SGX535_TEXTURE_INVALID;
    if (first > last)
        first = last;
    /* A cube's faces are a whole chain apart: the hardware's step from a later
     * level would not match; a stride surface is one level. */
    if ((layout->target == SGX535_TEXTURE_CUBE && first) ||
        (layout->type == SGX535_TEXTYPE_STRIDE && last != first && layout->levels > 1u))
        return SGX535_TEXTURE_INVALID;
    border = sampler->wrap[0] == SGX535_WRAP_CLAMP_TO_BORDER ||
             sampler->wrap[1] == SGX535_WRAP_CLAMP_TO_BORDER ||
             (three_d && sampler->wrap[2] == SGX535_WRAP_CLAMP_TO_BORDER);
    /* the border area in front of the map (sgx535_texture_layout_border) */
    if (border && !layout->border_bytes)
        return SGX535_TEXTURE_INVALID;
    for (unsigned axis = 0; axis < 3; axis++) {
        int mode = axis < 2 || three_d ? address_mode(sampler->wrap[axis], border) : MODE_REPEAT;
        if (mode < 0)
            return SGX535_TEXTURE_INVALID;
        modes |= (uint32_t)mode << (6u - 3u * axis);
    }
    /* the bias in eighths, clamped to the field before the conversion (NaN:
     * none) */
    eighths = sampler->lod_bias * 8.0f;
    eighths = eighths > 32.0f      ? 32.0f
              : eighths < -31.0f   ? -31.0f
              : eighths == eighths ? eighths
                                   : 0;
    bias = (int)(eighths + (eighths < 0 ? -0.5f : 0.5f));
    adjust = (uint32_t)(31 + (bias < -31 ? -31 : bias > 32 ? 32 : bias));
    if (sampler->max_anisotropy > 1u)
        aniso = log2_of(sampler->max_anisotropy);
    words[0] = (uint32_t)layout->chan_replicate << 30 | (uint32_t)(sampler->srgb != 0) << 27 |
               adjust << 21 | lod_clamp(layout, sampler, first, last) << 17 | aniso << 14 |
               (uint32_t)(aniso && sampler->mag_filter ? 2u : sampler->mag_filter) << 12 |
               (uint32_t)(aniso && sampler->min_filter ? 2u : sampler->min_filter) << 10 |
               (uint32_t)(sampler->mip_filter == SGX535_MIP_LINEAR) << 9 | modes;
    w = level_size(layout->width, first);
    h = level_size(layout->height, first);
    d = three_d ? level_size(layout->depth, first) : 1u;
    if (layout->type == SGX535_TEXTYPE_STRIDE)
        size_bits = (w - 1u) << 12 | (h - 1u);
    else
        size_bits = log2_of(w) << 16 | log2_of(d) << 12 | log2_of(h);
    words[1] = (uint32_t)layout->type << 29 | (uint32_t)layout->hw_format << 24 | size_bits;
    words[2] = address + sgx535_texture_offset(layout, view->plane, 0, first) -
               (border ? layout->border_bytes : 0u);
    return SGX535_TEXTURE_OK;
}
