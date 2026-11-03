// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_H
#define SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_H
#include "sgx535_depth_window_mesh.h"
#include "sgx535_window_mesh_gl_window.h"

#define SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION 4u
/* Separate complete fresh64x64 D24S8 CPU construction. The actual WINDOW1/1,
 * negative-Y viewport/min_samples1 source is retained by typed BG6 and its
 * constructor supplies the matched Win7 raster tuple. The unchanged checked
 * depth composition supplies mask -> clear+1 -> LESS/write mesh -> event/end.
 * No already validated image/action is patched or relabelled as this class.
 *
 * This proves neither the combined silicon raster/depth effect nor depth
 * packing, stencil initialization, write envelope, reset safety, visibility,
 * completion or retirement. Every old missing mask remains set, and native
 * admission refuses. Existing version1/state2/preserve3 APIs remain unchanged.
 * All original transaction/alias/header/payload/binding checks apply to this
 * whole typed outer request. Workspace stays off the kernel stack. */
struct sgx535_depth_window_mesh_gl_window_request {
    uint32_t version, format, no_depth, reserved; /* no_depth as in the depth request */
    struct sgx535_window_mesh_gl_window_background_request scene;
};
enum sgx535_task_status
sgx535_depth_window_mesh_gl_window_build(const struct sgx535_depth_window_mesh_gl_window_request *,
                                         struct sgx535_depth_window_mesh_task *, uint8_t *,
                                         uint32_t, struct sgx535_depth_window_mesh_workspace *,
                                         uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_validate(
    const struct sgx535_depth_window_mesh_gl_window_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_bind(
    const struct sgx535_depth_window_mesh_gl_window_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, struct sgx535_depth_window_mesh_bound *,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_bound_validate(
    const struct sgx535_depth_window_mesh_gl_window_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, const struct sgx535_depth_window_mesh_bound *,
    struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
#endif
