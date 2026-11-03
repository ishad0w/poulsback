// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_NORMALIZED_COLOR0_BACKGROUND_H
#define SGX535_NORMALIZED_COLOR0_BACKGROUND_H
#include "sgx535_normalized_color0_task.h"
#include "sgx535_background_object.h"

#define SGX535_NORMALIZED_COLOR0_BACKGROUND_VERSION 1u
struct sgx535_normalized_color0_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_normalized_color0_tile16_request foreground;
};
struct sgx535_normalized_color0_background_image {
    uint32_t version, missing_background_contracts;
    struct sgx535_normalized_color0_task foreground;
    struct sgx535_background_image background;
};
struct sgx535_normalized_color0_background_bound {
    uint32_t version, missing_background_contracts;
    struct sgx535_normalized_color0_launch launch;
    struct sgx535_background_bound background;
};
struct sgx535_normalized_color0_background_workspace {
    struct sgx535_normalized_color0_background_image image;
    struct sgx535_normalized_color0_workspace foreground;
    struct sgx535_background_bound background;
    struct sgx535_task_launch_workspace projection;
    struct sgx535_normalized_color0_background_bound result;
};

/* Separate complete fixed16 composition, never UAPI or device admission.
 * Regenerates the whole normalized task from stable symbolic IR, and its own
 * packed constant background from a fixed policy. USE+1536 is a driver-owned
 * placement, not a hardware constant: every foreground/event/other section
 * must be disjoint from the eight BG code bytes. BG primary/secondary PDS
 * stay+768/+832 and the distinct READ RASTGEOM owner is fixed32000000/4KiB.
 * No caller-chosen USE entry, raw task controls, addresses or qualification
 * flags are added. The two existing typed bindings must describe one graph.
 *
 * Inputs, all indirect IR, bindings and whole generated image must remain
 * stable while compilation may allocate/sleep. Output and explicit off-stack
 * workspace must be disjoint from all inputs and each other. No source
 * pointers are retained. All masks/zero qualification fields persist. Errors
 * preserve output; CPU projection does not authenticate physical pages or
 * grant population, publication, cache visibility, IRQ or retirement rights.
 * No mask-initialization pass is added or inherited from a previous HOLD. */
enum sgx535_task_status sgx535_normalized_color0_background_build(
    const struct sgx535_normalized_color0_background_request *,
    struct sgx535_normalized_color0_background_image *,
    struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_background_validate(
    const struct sgx535_normalized_color0_background_request *,
    const struct sgx535_normalized_color0_background_image *,
    struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status
sgx535_normalized_color0_background_bind(const struct sgx535_normalized_color0_background_request *,
                                         const struct sgx535_normalized_color0_background_image *,
                                         const struct sgx535_task_launch_binding *,
                                         const struct sgx535_background_binding *,
                                         struct sgx535_normalized_color0_background_bound *,
                                         struct sgx535_normalized_color0_background_workspace *);
enum sgx535_task_status sgx535_normalized_color0_background_bound_validate(
    const struct sgx535_normalized_color0_background_request *,
    const struct sgx535_normalized_color0_background_image *,
    const struct sgx535_task_launch_binding *, const struct sgx535_background_binding *,
    const struct sgx535_normalized_color0_background_bound *,
    struct sgx535_normalized_color0_background_workspace *);
#endif
