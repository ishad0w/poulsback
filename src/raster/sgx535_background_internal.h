// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_BACKGROUND_INTERNAL_H
#define SGX535_BACKGROUND_INTERNAL_H
#include "sgx535_background_object.h"
/* Trusted scratch only, reached through the complete typed scene constructor.
 * Placements belong to that constructor, never a public address/control input.
 * Old background frontdoors continue to accept version1 and their old policy. */
#define SGX535_BACKGROUND_NORMALIZED_TILE16_VERSION 2u
#define SGX535_BACKGROUND_NORMALIZED_USE_OFFSET 1536u
#define SGX535_BACKGROUND_WINDOW_MESH_VERSION 3u
#define SGX535_BACKGROUND_CLIP_VIEWPORT_VERSION 4u
enum sgx535_task_status
sgx535_background_normalized_tile16_seed(const struct sgx535_background_request *,
                                         struct sgx535_background_image *);
enum sgx535_task_status sgx535_background_normalized_tile16_bind_seed(
    const struct sgx535_background_request *, const struct sgx535_background_image *,
    const struct sgx535_background_binding *, struct sgx535_background_bound *);
enum sgx535_task_status sgx535_background_window_mesh_bind_seed(
    const struct sgx535_background_request *, const struct sgx535_background_image *,
    const struct sgx535_background_binding *, struct sgx535_background_bound *);
/* Counted WINDOW composition may share a larger canonical owner with several
 * programs. Only this internal leaf accepts page-multiple full extents; old
 * seed/public bindings retain their exact4096-byte owner contract. */
enum sgx535_task_status sgx535_background_window_mesh_bind_full_owner_seed(
    const struct sgx535_background_request *, const struct sgx535_background_image *,
    const struct sgx535_background_binding *, struct sgx535_background_bound *);
/* Placement comes from the complete WINDOW compiler layout. Regeneration
 * retains the same program, controls and checked owner/bank projection. */
enum sgx535_task_status
sgx535_background_window_mesh_layout_seed(const struct sgx535_background_request *, uint32_t,
                                          struct sgx535_background_image *);
enum sgx535_task_status sgx535_background_window_mesh_bind_layout_seed(
    const struct sgx535_background_request *, const struct sgx535_background_image *,
    const struct sgx535_background_binding *, uint32_t, int, struct sgx535_background_bound *);
/* New complete fixed placement. Old1/2/3 keep PDS768/832 byte-identical. */
enum sgx535_task_status
sgx535_background_clip_viewport_seed(const struct sgx535_background_request *,
                                     struct sgx535_background_image *);
enum sgx535_task_status sgx535_background_clip_viewport_bind_seed(
    const struct sgx535_background_request *, const struct sgx535_background_image *,
    const struct sgx535_background_binding *, struct sgx535_background_bound *);
#endif
