// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_WINDOW_MESH_PRESERVE_H
#define SGX535_DEPTH_WINDOW_MESH_PRESERVE_H
#include "sgx535_depth_window_mesh.h"

#define SGX535_DEPTH_WINDOW_MESH_PRESERVE_VERSION 3u
/* Complete64x64 D24S8 candidate: mask -> ordered LESS/write mesh -> unchanged
 * termination/event, without the explicit depth-clear object. Background and
 * its depth+1 remain unchanged. PREVIOUS_SCENE is source-derived state2 intent;
 * LOAD-versus-background initialization, raw storage, visibility and retirement
 * are hypotheses. This produces no physical readiness or ordinary admission.
 * Old request layout/version1 and old build/state entry points stay unchanged.
 * Output image/bound version3 cannot be relabelled as an old fresh image.
 * The ordinary full regeneration/alias/output transaction rules apply. */
enum sgx535_task_status sgx535_depth_window_mesh_preserve_build(
    const struct sgx535_depth_window_mesh_request *, struct sgx535_depth_window_mesh_task *,
    uint8_t *, uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_preserve_validate(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *, uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_preserve_bind(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *, uint32_t, const struct sgx535_depth_window_mesh_binding *,
    struct sgx535_depth_window_mesh_bound *, struct sgx535_depth_window_mesh_workspace *, uint8_t *,
    uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_preserve_bound_validate(
    const struct sgx535_depth_window_mesh_request *, const struct sgx535_depth_window_mesh_task *,
    const uint8_t *, uint32_t, const struct sgx535_depth_window_mesh_binding *,
    const struct sgx535_depth_window_mesh_bound *, struct sgx535_depth_window_mesh_workspace *,
    uint8_t *, uint32_t);
#endif
