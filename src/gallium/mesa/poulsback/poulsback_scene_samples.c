// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_scene_samples.h"
#include "poulsback_selfcheck.h"
#include "compiler/nir/nir.h"
#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct sgx535_single_sample_source) == 80, "Unchanged raw20-word source");
_Static_assert(sizeof(struct poulsback_scene_sample_input) == 56, "Copied binding14 words");
_Static_assert(sizeof(struct poulsback_scene_sample_state) == 112, "Copied per-draw28 words");

struct fragment_samples {
    uint32_t inputs, outputs, per_sample;
    uint32_t inputs_read[2], outputs_written[2];
};
static bool fragment_samples(const struct nir_shader *nir, struct fragment_samples *out)
{
    struct fragment_samples r = {0};
    unsigned input_variables = 0, output_variables = 0;
    static const unsigned system[] = {SYSTEM_VALUE_SAMPLE_ID,
                                      SYSTEM_VALUE_SAMPLE_POS,
                                      SYSTEM_VALUE_SAMPLE_POS_OR_CENTER,
                                      SYSTEM_VALUE_SAMPLE_MASK_IN,
                                      SYSTEM_VALUE_BARYCENTRIC_PERSP_SAMPLE,
                                      SYSTEM_VALUE_BARYCENTRIC_LINEAR_SAMPLE};
    if (!nir || nir->info.stage != MESA_SHADER_FRAGMENT)
        return false;
    nir_foreach_variable_with_modes(v, nir, nir_var_shader_in | nir_var_shader_out)
    {
        if (v->data.sample && v->data.mode == nir_var_shader_in)
            input_variables++;
        if (v->data.sample && v->data.mode == nir_var_shader_out)
            output_variables++;
    }
    r.inputs =
        nir->info.fs.uses_sample_qualifier | nir->info.fs.sample_mask_in_declared | input_variables;
    for (unsigned i = 0; i < sizeof(system) / sizeof(system[0]); i++)
        r.inputs |= BITSET_TEST(nir->info.system_values_read, system[i]) != 0;
    r.outputs = ((nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK)) != 0) |
                output_variables;
    r.per_sample = nir->info.fs.uses_sample_shading | nir->info.fs.uses_sample_qualifier |
                   input_variables | nir->info.fs.sample_interlock_ordered |
                   nir->info.fs.sample_interlock_unordered;
    r.inputs_read[0] = (uint32_t)nir->info.inputs_read;
    r.inputs_read[1] = (uint32_t)(nir->info.inputs_read >> 32);
    r.outputs_written[0] = (uint32_t)nir->info.outputs_written;
    r.outputs_written[1] = (uint32_t)(nir->info.outputs_written >> 32);
    *out = r;
    return true;
}
/* The sample source of INPUT's bindings with NIR's sample use and a program
 * of INSTRUCTIONS. */
static bool sample_source(const struct poulsback_scene_sample_input *input,
                          const struct nir_shader *nir, uint32_t instructions,
                          struct sgx535_single_sample_source *out, struct fragment_samples *shader)
{
    /* no shader: the frame objects' template, without sample inputs or
     * outputs */
    if (!nir)
        memset(shader, 0, sizeof(*shader));
    if (input->version != POULSBACK_SCENE_SAMPLE_INPUT_VERSION ||
        (nir && !fragment_samples(nir, shader)))
        return false;
    *out = (struct sgx535_single_sample_source){
        .version = SGX535_SINGLE_SAMPLE_SOURCE_VERSION,
        .min_samples = input->min_samples,
        .framebuffer_samples = input->framebuffer_samples,
        .effective_framebuffer_samples = input->effective_framebuffer_samples,
        .color_resource_samples = input->color_resource_samples,
        .color_storage_samples = input->color_storage_samples,
        .color_surface_samples = input->color_surface_samples,
        .depth_resource_samples = input->depth_resource_samples,
        .depth_storage_samples = input->depth_storage_samples,
        .depth_surface_samples = input->depth_surface_samples,
        .multisample = input->multisample,
        .force_persample_interp = input->force_persample_interp,
        .no_ms_sample_mask_out = input->no_ms_sample_mask_out,
        .fragment_sample_inputs = shader->inputs,
        .fragment_sample_outputs = shader->outputs,
        .fragment_per_sample = shader->per_sample,
        .fragment_instructions = instructions};
    return true;
}
int poulsback_scene_sample_source(const struct poulsback_scene_sample_input *input,
                                  const struct nir_shader *nir, uint32_t instructions,
                                  struct sgx535_single_sample_source *out)
{
    struct fragment_samples shader;
    return input && out && sample_source(input, nir, instructions, out, &shader) ? 0 : -EINVAL;
}
