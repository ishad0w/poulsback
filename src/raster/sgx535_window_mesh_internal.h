// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_WINDOW_MESH_INTERNAL_H
#define SGX535_WINDOW_MESH_INTERNAL_H
#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "sgx535_normalized_color0_task.h"
/* Unpublished scratch only, never an unchecked packet admission frontdoor. */
enum sgx535_task_status
sgx535_normalized_color0_mask_mesh_template(uint32_t, uint32_t, struct sgx535_task_packet *,
                                            struct sgx535_normalized_color0_workspace *);
enum sgx535_task_status
sgx535_normalized_no_input_mask_mesh_template(const struct sgx535_task_packet *,
                                              struct sgx535_task_packet *,
                                              struct sgx535_normalized_color0_workspace *);
#include "sgx535_window_mesh_gl_window.h"
#include "sgx535_single_sample.h"
enum sgx535_task_status
sgx535_window_mesh_input_storage_plan(const struct sgx535_window_mesh_request *,
                                      struct sgx535_window_mesh_storage *);
struct sgx535_window_mesh_fragment_layout {
    uint32_t code_bytes, event_offset_bytes, foreground_extent_bytes;
    uint32_t background_offset_bytes, clear_offset_bytes;
};
/* Emitted words own code; the legacy reservation is only a placement floor.
 * The compiler bound precedes all byte arithmetic. Output is transactional. */
enum sgx535_task_status
sgx535_window_mesh_fragment_layout(uint32_t, struct sgx535_window_mesh_fragment_layout *);
/* A clip-space request's MTE state: its 16-word foreground record (the
 * largest one 16-byte state USE program emits), then the unchanged mask's.
 * The header's groups follow in bit order: ISP A, pixel PDS, viewport, output
 * selects, W clamp, MTE control, TEXSIZE and, in the generic F32 family,
 * TEXFLOAT. Region clip is the mask's and the clear's (0, 0), which every
 * foreground draw follows; the MTE keeps a group its record leaves out. */
#define SGX535_WINDOW_MESH_CLIP_MTE_BYTES 108u
#define SGX535_WINDOW_MESH_CLIP_MASK_MTE 64u
#define SGX535_WINDOW_MESH_CLIP_MTE_HEADER 0x5d41u
#define SGX535_WINDOW_MESH_CLIP_F32_MTE_HEADER 0xdd41u
/* Physical IO selection is internal and coupled to the complete state/code
 * emitter. It is never accepted from a native request or public packet. */
enum sgx535_window_fragment_io {
    SGX535_WINDOW_PRIMARY_COLOR = 0,
    SGX535_WINDOW_GENERIC0_F32 = 1,
    SGX535_WINDOW_GENERIC0_F32_SECONDARY = 2
};
struct sgx535_window_mesh_uniform_source {
    const uint32_t *words;
    uint32_t word_count, constant_flags, reserved[2];
};
/* Ordinary PSB SA0 reservation and sixteen-DWORD PDS, coupled to generic F32
 * IO. The words are copied before compilation and rechecked afterwards. No
 * sampler/state/memory constants, native publication or visibility claim.
 * The independent source lives in the existing PDS owner after its reserved
 * foreground/background layout. Existing entrypoints do not select it. */
enum sgx535_task_status sgx535_window_mesh_single_sample_background_secondary_build(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, const struct sgx535_window_mesh_uniform_source *,
    struct sgx535_window_mesh_background_image *, uint8_t *, uint32_t,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_secondary_bind(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, const struct sgx535_window_mesh_uniform_source *,
    const struct sgx535_window_mesh_background_image *, const uint8_t *, uint32_t,
    const struct sgx535_window_mesh_binding *, const struct sgx535_background_binding *,
    struct sgx535_window_mesh_background_bound *, struct sgx535_window_mesh_workspace *, uint8_t *,
    uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_f32_build(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, struct sgx535_window_mesh_background_image *,
    uint8_t *, uint32_t, struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_build(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, struct sgx535_window_mesh_background_image *,
    uint8_t *, uint32_t, struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_bind(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, const struct sgx535_window_mesh_background_image *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, struct sgx535_window_mesh_background_bound *,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_bind_full_owner(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, const struct sgx535_window_mesh_background_image *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, struct sgx535_window_mesh_background_bound *,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t, bool raw_mask);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_raw_mask_build_v1(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, struct sgx535_window_mesh_background_image *,
    uint8_t *, uint32_t, struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_window_mesh_single_sample_background_f32_bind_full_owner(
    const struct sgx535_window_mesh_gl_window_background_request *,
    const struct sgx535_single_sample_source *, const struct sgx535_window_mesh_background_image *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, struct sgx535_window_mesh_background_bound *,
    struct sgx535_window_mesh_workspace *, uint8_t *, uint32_t);
#endif
