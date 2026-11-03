// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_CLIP_VIEWPORT_MESH_INTERNAL_H
#define POULSBACK_SGX535_CLIP_VIEWPORT_MESH_INTERNAL_H
#include "sgx535_clip_viewport_mesh.h"

/* Private mechanics only. The caller independently snapshots, validates and
 * rechecks its typed source contract. These write unpublished workspace and
 * never grant a source-family conversion, physical ownership or admission. */
enum sgx535_task_status
sgx535_clip_viewport_internal_emit(const struct sgx535_clip_viewport_mesh_request *,
                                   struct sgx535_clip_viewport_mesh_workspace *, uint8_t *geometry);
enum sgx535_task_status sgx535_clip_viewport_internal_bind_pair(
    const struct sgx535_background_request *, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, struct sgx535_clip_viewport_mesh_workspace *);
#endif
