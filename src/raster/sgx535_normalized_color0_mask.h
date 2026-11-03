// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_MASK_H
#define SGX535_NORMALIZED_COLOR0_MASK_H
#include "sgx535_normalized_color0_background.h"

#define SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION 11u
#define SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE 10u
#define SGX535_NORMALIZED_COLOR0_MASK_BACKGROUND_VERSION 2u

/* Separate pure fixed16 composition: one source-defined fullscreen mask pass,
 * then one arbitrary clip-W1 COLOR0 triangle in the same terminated task.
 * The mask has its own four-word DMA/USE path; the triangle has eight words.
 * Existing request/result/workspace shapes are reused, but old entry points
 * reject these versions. No UAPI, native provider or physical admission.
 *
 * Stable IR may use the separate raw-sign normalized compiler candidate.
 * Numeric interpolation/PACK, nonfinite arithmetic, coverage, visibility and
 * retirement remain unqualified. All missing masks and zero qualifications
 * persist. Compilation may allocate/sleep. Entire inputs and indirect IR must
 * remain stable and disjoint from output and explicit off-stack workspace;
 * errors preserve output. Validation regenerates the complete typed image. */
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_task_build(
    const struct sgx535_normalized_color0_tile16_request *, struct sgx535_normalized_color0_task *,
    struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_task_validate(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_launch_build(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, const struct sgx535_task_launch_binding *,
    struct sgx535_normalized_color0_launch *, struct sgx535_normalized_color0_launch_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_launch_validate(
    const struct sgx535_normalized_color0_tile16_request *,
    const struct sgx535_normalized_color0_task *, const struct sgx535_task_launch_binding *,
    const struct sgx535_normalized_color0_launch *,
    struct sgx535_normalized_color0_launch_workspace *);

/* Outer version2 binds the regenerated v11/profile10 task and a constant BG
 * at USE+1536, PDS+768/+832, with a distinct fixed RASTGEOM owner. It never
 * inherits mask state from a previous job/HOLD. Full foreground/event/BG
 * extents and both complete owner/bank/epoch graphs must agree. */
enum sgx535_task_status sgx535_normalized_color0_mask_background_build(
    const struct sgx535_normalized_color0_background_request *,
    struct sgx535_normalized_color0_background_image *,
    struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_background_validate(
    const struct sgx535_normalized_color0_background_request *,
    const struct sgx535_normalized_color0_background_image *,
    struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_background_bind(
    const struct sgx535_normalized_color0_background_request *,
    const struct sgx535_normalized_color0_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    struct sgx535_normalized_color0_background_bound *,
    struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_mask_background_bound_validate(
    const struct sgx535_normalized_color0_background_request *,
    const struct sgx535_normalized_color0_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    const struct sgx535_normalized_color0_background_bound *,
    struct sgx535_normalized_color0_background_workspace *);
#endif
