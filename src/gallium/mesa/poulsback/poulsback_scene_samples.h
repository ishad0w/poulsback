// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SCENE_SAMPLES_H
#define POULSBACK_SCENE_SAMPLES_H
#include "poulsback_color0_task.h"
#include "sgx535_single_sample.h"

#define POULSBACK_SCENE_SAMPLE_INPUT_VERSION 1u
#define POULSBACK_SCENE_SAMPLE_STATE_VERSION 1u

/* Copied from the actual accepted framebuffer and raster bindings at the same
 * draw boundary as the bound shader/constants. Absence is a missing record,
 * never an all-zero default. These counts are not native admission. */
struct poulsback_scene_sample_input {
    uint32_t version, framebuffer_samples, effective_framebuffer_samples;
    uint32_t min_samples, sample_mask;
    uint32_t color_resource_samples, color_storage_samples, color_surface_samples;
    uint32_t depth_resource_samples, depth_storage_samples, depth_surface_samples;
    uint32_t multisample, force_persample_interp, no_ms_sample_mask_out;
};
/* Scene-owned facts for this exact copied program and window snapshot. The
 * nested source is the unchanged 20-word raw0 consumer interface. IO masks are
 * masks, not the bit indices printed by nir_print_shader. No mutable CSO/NIR,
 * surface or color resource pointer is retained. */
struct poulsback_scene_sample_state {
    uint32_t version;
    struct sgx535_single_sample_source samples;
    uint32_t sample_mask, program_profile, uniform_layout;
    uint32_t nir_inputs_read[2], nir_outputs_written[2];
};

/* The sample source alone (the frame writer's sink): INPUT's bindings, NIR's
 * sample use and the program's INSTRUCTIONS. */
int poulsback_scene_sample_source(const struct poulsback_scene_sample_input *,
                                  const struct nir_shader *, uint32_t instructions,
                                  struct sgx535_single_sample_source *);
#endif
