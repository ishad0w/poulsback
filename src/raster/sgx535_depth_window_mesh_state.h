// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_WINDOW_MESH_STATE_H
#define SGX535_DEPTH_WINDOW_MESH_STATE_H
#include "sgx535_depth_window_mesh.h"

#define SGX535_DEPTH_WINDOW_MESH_STATE_VERSION 2u
struct sgx535_depth_window_mesh_state_request {
    uint32_t version, compare, write_enable, reserved;
    struct sgx535_depth_window_mesh_request mesh;
};

/* Distinct complete-scene construction: fresh depth clear +1, followed by one
 * checked comparison/write state shared by every ordinary triangle. The same
 * whole-regeneration/binding implementation constructs all live sections; no
 * caller packet, action array or already-bound image is patched afterward.
 * Output image/bound versions are2, and old version1 intake rejects them.
 * Old entry points keep their fixed LESS/write version1 bytes and semantics.
 *
 * Inputs, the whole outer request, output, geometry, workspace and scratch must
 * be disjoint. Compilation may sleep; all input headers/payloads and bindings
 * are checked again before output publication. Failure preserves outputs.
 * This CPU-only preparation does not qualify compare effects, depth format,
 * numeric/interpolation behavior, visibility or retirement. All old missing
 * masks remain set, and native admission always refuses both versions. */
enum sgx535_task_status sgx535_depth_window_mesh_state_build(
    const struct sgx535_depth_window_mesh_state_request *, struct sgx535_depth_window_mesh_task *,
    uint8_t *, uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_state_validate(
    const struct sgx535_depth_window_mesh_state_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_state_bind(
    const struct sgx535_depth_window_mesh_state_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, struct sgx535_depth_window_mesh_bound *,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_state_bound_validate(
    const struct sgx535_depth_window_mesh_state_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, const struct sgx535_depth_window_mesh_bound *,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
#endif
