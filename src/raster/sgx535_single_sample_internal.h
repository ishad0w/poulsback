// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_SINGLE_SAMPLE_INTERNAL_H
#define SGX535_SINGLE_SAMPLE_INTERNAL_H
#include "sgx535_single_sample.h"

/* Used only by complete fixed GL-depth construction and its retained owner.
 * The existing compiler separately checks every scalar instruction and only
 * supports COLOR0 inputs/outputs; no sample system-value opcode is admitted.
 * This equality check does not authenticate the frontend producer. */
static inline int sgx535_single_sample_project(const struct sgx535_single_sample_source *s,
                                               const struct sgx535_window_mesh_window *window,
                                               uint32_t instructions,
                                               struct sgx535_single_sample_lowering *out)
{
    struct sgx535_single_sample_lowering p = {SGX535_SINGLE_SAMPLE_LOWERING_VERSION, 0, 1, 1};
    if (!s || !window || !out || s->version != SGX535_SINGLE_SAMPLE_SOURCE_VERSION ||
        s->min_samples || s->framebuffer_samples || s->effective_framebuffer_samples != 1 ||
        s->color_resource_samples || s->color_storage_samples || s->color_surface_samples ||
        s->depth_resource_samples || s->depth_storage_samples || s->depth_surface_samples ||
        s->multisample || s->force_persample_interp || s->no_ms_sample_mask_out ||
        s->fragment_sample_inputs || s->fragment_sample_outputs || s->fragment_per_sample ||
        s->fragment_instructions != instructions || instructions < 5 ||
        instructions > SGX535_NIR_MAX_INSTRUCTIONS || s->reserved[0] || s->reserved[1] ||
        s->reserved[2] || window->min_samples || window->sample_mask != 0xffffffffu)
        return 0;
    *out = p;
    return 1;
}
#endif
