// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_DEPTH_WINDOW_MESH_SINGLE_SAMPLE_H
#define SGX535_DEPTH_WINDOW_MESH_SINGLE_SAMPLE_H
#include "sgx535_depth_window_mesh_gl_window.h"
#include "sgx535_single_sample.h"

/* Additive complete GL-depth entry, not a relabelled old image. The original
 * request4/BG6, WINDOW1/1/raw minimum0, D24S8, LESS/write/clear+1 and every
 * symbolic payload stay unchanged. The record independently checks all raw
 * attachment/sample facts and derives one covered-fragment invocation.
 * The raw0 route uses the existing bounded scene extents1..2047 on each axis,
 * with exact viewport(width/2,-height/2,1/2)+(width/2,height/2,1/2).
 * Color/PBE, depth allocation and ordered load extents are regenerated together;
 * the old raw1 diagnostic remains64x64. This is CPU construction, not admission
 * of larger images into the native64x64 attachment provider.
 * Old GL-depth entry remains raw1-only. No public packet number or gate changes.
 * Receipt outputs and geometry commit only after complete regeneration; all
 * aliases/header/payload/binding checks remain in force. CPU construction does
 * not establish source authentication, sample/edge effect or retirement. */
struct sgx535_depth_window_mesh_single_sample_request {
    struct sgx535_depth_window_mesh_gl_window_request mesh;
    struct sgx535_single_sample_source samples;
};
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_build(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    struct sgx535_depth_window_mesh_task *, struct sgx535_single_sample_lowering *, uint8_t *,
    uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    const struct sgx535_depth_window_mesh_task *, const struct sgx535_single_sample_lowering *,
    const uint8_t *, uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, struct sgx535_depth_window_mesh_bound *,
    struct sgx535_single_sample_lowering *, struct sgx535_depth_window_mesh_workspace *, uint8_t *,
    uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    const struct sgx535_depth_window_mesh_task *, const uint8_t *, uint32_t,
    const struct sgx535_depth_window_mesh_binding *, const struct sgx535_depth_window_mesh_bound *,
    const struct sgx535_single_sample_lowering *, struct sgx535_depth_window_mesh_workspace *,
    uint8_t *, uint32_t);
/* Separate CPU program grammar. Old entrypoints/binds refuse its mesh profile;
 * the shared emitter preserves all geometry, extent and missing-contract rules. */
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_raw_mask_build_v1(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    struct sgx535_depth_window_mesh_task *, struct sgx535_single_sample_lowering *, uint8_t *,
    uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_raw_mask_validate_v1(
    const struct sgx535_depth_window_mesh_single_sample_request *,
    const struct sgx535_depth_window_mesh_task *, const struct sgx535_single_sample_lowering *,
    const uint8_t *, uint32_t, struct sgx535_depth_window_mesh_workspace *, uint8_t *, uint32_t);
#endif
