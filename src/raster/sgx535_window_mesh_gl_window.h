// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_WINDOW_MESH_GL_WINDOW_H
#define SGX535_WINDOW_MESH_GL_WINDOW_H
#include "sgx535_window_mesh.h"

#define SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION 6u
/* Separate diagnostic constructor, not generic GL admission. Inputs retain the
 * actual copied task14/profile13 WINDOW1/1 source; only64x64, negative-Y
 * viewport(32,-32,1/2)+(32,32,1/2), sample0/min1/all-mask are selected.
 * The matched Win7 normal single-sample allocation policy supplies raster
 * tuple418=1/4c8=88/800=1. SGX535 core/rev121 identity checks are unchanged.
 * TSP[0]'s edge equation, chip applicability beyond the selected source,
 * numerical/API/visibility/retirement and reuse remain unqualified.
 * Old constructors remain0/0 only and cannot validate these1/1 images.
 * Build/bind regenerate before publication, recheck source after compiler
 * sleeps, require disjoint complete source/output spans and preserve outputs
 * on failure. These CPU constructors grant no execution or release rights. */
struct sgx535_window_mesh_gl_window_background_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_window_mesh_request foreground;
};
enum sgx535_task_status
sgx535_window_mesh_gl_window_build(const struct sgx535_window_mesh_request *,
                                   struct sgx535_window_mesh_task *, uint8_t *, uint32_t,
                                   struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_validate(
    const struct sgx535_window_mesh_request *, const struct sgx535_window_mesh_task *,
    const uint8_t *, uint32_t, struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_bind(
    const struct sgx535_window_mesh_request *, const struct sgx535_window_mesh_task *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    struct sgx535_window_mesh_bound *, struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_bound_validate(
    const struct sgx535_window_mesh_request *, const struct sgx535_window_mesh_task *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_window_mesh_bound *, struct sgx535_window_mesh_workspace *, uint8_t *,
    uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_background_build(
    const struct sgx535_window_mesh_gl_window_background_request *,
    struct sgx535_window_mesh_background_image *, uint8_t *, uint32_t,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_background_validate(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *, uint32_t,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_background_bind(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *, uint32_t,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    struct sgx535_window_mesh_background_bound *, struct sgx535_window_mesh_workspace *, uint8_t *,
    uint32_t);
enum sgx535_task_status sgx535_window_mesh_gl_window_background_bound_validate(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *, uint32_t,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    const struct sgx535_window_mesh_background_bound *, struct sgx535_window_mesh_workspace *,
    uint8_t *, uint32_t);
#endif
