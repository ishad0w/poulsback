// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_internal.h"
#include "sgx535_fragment_bool32.h"
#include "nir.h"
#include <stdlib.h>
#include <string.h>

struct dri_raw_normalized_work {
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    bool live[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t remap[SGX535_NIR_MAX_INSTRUCTIONS];
    size_t count;
    struct sgx535_fragment_dri_raw_sign_normalized_nir_program result;
};

static enum sgx535_nir_status sgx535_nir_fragment_raw_mask_no_uniform_v1_ir(
    const struct nir_shader *shader, enum sgx535_fragment_uniform_layout layout,
    struct sgx535_fragment_instruction *ir, size_t capacity, size_t *count,
    struct sgx535_fragment_linkage *linkage)
{
    const struct sgx535_fragment_nir_options options = {layout, SGX535_FRAGMENT_IO_F32};
    enum sgx535_nir_status status;
    if (!shader || !ir || !count || !linkage)
        return SGX535_NIR_ERR_ARGUMENT;
    /* The complete original is checked before lowering. Even a valid dead or
     * unselected uniform cannot disappear into this no-uniform v1 profile. */
    nir_foreach_function_impl(impl, shader) nir_foreach_block(block, impl) nir_foreach_instr(
        instr, block) if (instr->type == nir_instr_type_intrinsic &&
                          nir_instr_as_intrinsic(instr)->intrinsic ==
                              nir_intrinsic_load_uniform) return SGX535_NIR_ERR_UNSUPPORTED;
    status =
        sgx535_nir_fragment_pure_diamond_options_ir(shader, &options, ir, capacity, count, linkage);
    /* The floor entry independently checks the complete original linear
     * source before lowering its private clone. Capacity, allocation and
     * malformed argument failures are final; no uniform disappears here. */
    return status == SGX535_NIR_ERR_UNSUPPORTED
               ? sgx535_nir_fragment_floor_options_ir(shader, &options, ir, capacity, count,
                                                      linkage)
               : status;
}

static enum sgx535_nir_status raw_normalized_extract(const struct nir_shader *shader,
                                                     enum sgx535_fragment_uniform_layout layout,
                                                     enum sgx535_fragment_interpolation color_mode,
                                                     struct dri_raw_normalized_work *w,
                                                     bool raw_mask)
{
    struct sgx535_fragment_linkage canonical;
    enum sgx535_nir_status status;
    if ((color_mode != SGX535_FRAGMENT_SMOOTH && color_mode != SGX535_FRAGMENT_FLAT) ||
        (layout != SGX535_FRAGMENT_UNIFORM_DWORDS && layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS))
        return SGX535_NIR_ERR_UNSUPPORTED;
    status = raw_mask ? sgx535_nir_fragment_raw_mask_no_uniform_v1_ir(
                            shader, layout, w->ir, SGX535_NIR_MAX_INSTRUCTIONS, &w->count,
                            &w->result.source_linkage)
                      : sgx535_nir_fragment_raw_sign_inf_ir(shader, layout, w->ir,
                                                            SGX535_NIR_MAX_INSTRUCTIONS, &w->count,
                                                            &w->result.source_linkage);
    if (status != SGX535_NIR_OK)
        return status;
    canonical = w->result.source_linkage;
    if (canonical.count != 1 ||
        (canonical.varying[0].interpolation != SGX535_FRAGMENT_COLOR_INTERPOLATION &&
         canonical.varying[0].interpolation != color_mode)) {
        status = SGX535_NIR_ERR_UNSUPPORTED;
        return status;
    }
    canonical.varying[0].driver_base = 0;
    canonical.varying[0].interpolation = SGX535_FRAGMENT_SMOOTH;
    /* Validate finite-literal/mask roles on the whole source before pruning;
     * an unused unsupported NaN literal must not disappear into admission. */
    status = raw_mask ? sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(
                            w->ir, w->count, &canonical, &w->result.program)
                      : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                            w->ir, w->count, &canonical, &w->result.program);
    if (status != SGX535_NIR_OK)
        return status;
    status = raw_mask ? sgx535_fragment_dri_raw_mask_live_ir(w->ir, &w->count, w->live, w->remap)
                      : sgx535_fragment_dri_live_ir(w->ir, &w->count, w->live, w->remap, true);
    if (status != SGX535_NIR_OK)
        return status;
    status = raw_mask ? sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(
                            w->ir, w->count, &canonical, &w->result.program)
                      : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                            w->ir, w->count, &canonical, &w->result.program);
    if (status == SGX535_NIR_OK)
        w->result.resolved_interpolation = color_mode;
    return status;
}

static enum sgx535_nir_status
raw_normalized_arguments(const struct nir_shader *shader,
                         enum sgx535_fragment_uniform_layout layout,
                         enum sgx535_fragment_interpolation color_mode, const void *output)
{
    if (!shader || !output)
        return SGX535_NIR_ERR_ARGUMENT;
    if ((color_mode != SGX535_FRAGMENT_SMOOTH && color_mode != SGX535_FRAGMENT_FLAT) ||
        (layout != SGX535_FRAGMENT_UNIFORM_DWORDS && layout != SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS))
        return SGX535_NIR_ERR_UNSUPPORTED;
    return SGX535_NIR_OK;
}
enum sgx535_nir_status sgx535_nir_project_dri_color0_raw_sign_normalized_candidate(
    const struct nir_shader *shader, enum sgx535_fragment_uniform_layout layout,
    enum sgx535_fragment_interpolation color_mode,
    struct sgx535_fragment_dri_raw_sign_normalized_nir_program *output)
{
    struct dri_raw_normalized_work *w;
    enum sgx535_nir_status status = raw_normalized_arguments(shader, layout, color_mode, output);
    if (status != SGX535_NIR_OK)
        return status;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    status = raw_normalized_extract(shader, layout, color_mode, w, false);
    if (status == SGX535_NIR_OK)
        *output = w->result;
    free(w);
    return status;
}
static enum sgx535_nir_status raw_normalized_ir(const struct nir_shader *shader,
                                                enum sgx535_fragment_uniform_layout layout,
                                                enum sgx535_fragment_interpolation color_mode,
                                                struct sgx535_fragment_dri_nir_ir *output,
                                                bool raw_mask)
{
    struct dri_raw_normalized_work *w;
    enum sgx535_nir_status status = raw_normalized_arguments(shader, layout, color_mode, output);
    if (status != SGX535_NIR_OK)
        return status;
    w = calloc(1, sizeof(*w));
    if (!w)
        return SGX535_NIR_ERR_NOMEM;
    status = raw_normalized_extract(shader, layout, color_mode, w, raw_mask);
    if (status == SGX535_NIR_OK) {
        memset(output, 0, sizeof(*output));
        memcpy(output->instruction, w->ir, w->count * sizeof(*w->ir));
        output->instruction_count = (uint32_t)w->count;
        output->source_linkage = w->result.source_linkage;
        output->resolved_interpolation = color_mode;
    }
    free(w);
    return status;
}

enum sgx535_nir_status sgx535_nir_dri_color0_raw_sign_normalized_ir(
    const struct nir_shader *shader, enum sgx535_fragment_uniform_layout layout,
    enum sgx535_fragment_interpolation color_mode, struct sgx535_fragment_dri_nir_ir *output)
{
    return raw_normalized_ir(shader, layout, color_mode, output, false);
}
enum sgx535_nir_status sgx535_nir_dri_color0_raw_mask_normalized_v1_ir(
    const struct nir_shader *shader, enum sgx535_fragment_uniform_layout layout,
    enum sgx535_fragment_interpolation color_mode, struct sgx535_fragment_dri_nir_ir *output)
{
    return raw_normalized_ir(shader, layout, color_mode, output, true);
}
