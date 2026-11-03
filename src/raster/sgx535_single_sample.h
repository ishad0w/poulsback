// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_SINGLE_SAMPLE_H
#define SGX535_SINGLE_SAMPLE_H
#include "sgx535_window_mesh.h"

#define SGX535_SINGLE_SAMPLE_SOURCE_VERSION 1u
#define SGX535_SINGLE_SAMPLE_LOWERING_VERSION 1u

/* Original copied frontend state, not a permit or an effective-count alias.
 * The first lowering requires every raw attachment count independently0.
 * Shader dependency fields belong to the same copied program as the IR. */
struct sgx535_single_sample_source {
    uint32_t version, min_samples, framebuffer_samples, effective_framebuffer_samples;
    uint32_t color_resource_samples, color_storage_samples, color_surface_samples;
    uint32_t depth_resource_samples, depth_storage_samples, depth_surface_samples;
    uint32_t multisample, force_persample_interp, no_ms_sample_mask_out;
    uint32_t fragment_sample_inputs, fragment_sample_outputs, fragment_per_sample;
    uint32_t fragment_instructions, reserved[3];
};
/* Recomputed output only. It is never accepted as source/MMIO/lifetime authority. */
struct sgx535_single_sample_lowering {
    uint32_t version, source_min_samples, coverage_samples, fragment_invocations;
};
#endif
