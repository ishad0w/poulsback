// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_CLIP_VIEWPORT_GENERIC_F32_H
#define POULSBACK_SGX535_CLIP_VIEWPORT_GENERIC_F32_H
#include "sgx535_clip_viewport_mesh.h"

#define SGX535_GENERIC_F32_CLIP_VERSION 1u
#define SGX535_GENERIC_F32_CLIP_MISSING_STORAGE 1u
#define SGX535_GENERIC_F32_CLIP_MISSING_PRECISION 2u
#define SGX535_GENERIC_F32_CLIP_MISSING_NUMERICS 4u
#define SGX535_GENERIC_F32_CLIP_MISSING_NATIVE 8u
#define SGX535_GENERIC_F32_CLIP_MISSING_ALL 15u

/* An additive, source-bound physical linkage for the existing six-vertex
 * CLIP/W1 source contract. Source COLOR0, flags1/1/min1, negative-Y viewport,
 * copied WINDOW witness and entire IR remain unchanged. Only this named front
 * selects dense generic F32 coordinate0: position4+RGBA4, BASE absent,
 * TEXSIZE UVST, matching DOUTI and PA0/1/2/3. There are no independent toggles.
 * Old primary-color APIs cannot validate this image or its bound packet.
 *
 * The nested packet structures share byte/section/relocation mechanics only;
 * this outer type is not an old task/source/owner conversion or admission.
 * The foreground explicitly emits TEXSIZE7 then TEXFLOAT0, following the
 * matched Linux EMGD state producer. The shared fixed24-word state USE derives
 * MOV16+8 and state EMIT24. Foreground/mask/terminate DMA inputs are explicitly
 * zero-padded to24 words; the logical records contain17/11/2 words. Padding is
 * our bounded construction policy, not claimed byte identity with EMGD's
 * variable DMA inputs or a hardware ignored-padding guarantee.
 * Internal TSP storage (including prospective five-DWORD records), coefficient
 * precision/evaluation, general W and native execution remain UNKNOWN. Numerical accuracy,
 * visibility, retirement and reuse remain unqualified; HOLD restrictions are
 * unchanged. No depth owner.
 * A separately reviewed native consumer must retain this new class explicitly.
 */
struct sgx535_generic_f32_clip_request {
    uint32_t version, packed_background_argb, reserved[2];
    struct sgx535_clip_viewport_mesh_request source;
};
struct sgx535_generic_f32_clip_image {
    uint32_t version, missing_linkage_contracts;
    struct sgx535_clip_viewport_mesh_background_image packet;
};
struct sgx535_generic_f32_clip_bound {
    uint32_t version, missing_linkage_contracts;
    struct sgx535_clip_viewport_mesh_background_bound packet;
};
struct sgx535_generic_f32_clip_workspace {
    struct sgx535_clip_viewport_mesh_workspace native;
    struct sgx535_generic_f32_clip_image result, original;
    struct sgx535_generic_f32_clip_bound bound;
    uint8_t original_geometry[280];
};
/* Complete pairwise span/overflow checks. Stable source and copied geometry/IR
 * are rechecked after compiler allocations and binding. Errors preserve caller
 * image, bound output and geometry; workspace/scratch are unpublished. */
enum sgx535_task_status
sgx535_generic_f32_clip_build(const struct sgx535_generic_f32_clip_request *,
                              struct sgx535_generic_f32_clip_image *, uint8_t *, uint32_t,
                              struct sgx535_generic_f32_clip_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_generic_f32_clip_validate(
    const struct sgx535_generic_f32_clip_request *, const struct sgx535_generic_f32_clip_image *,
    const uint8_t *, uint32_t, struct sgx535_generic_f32_clip_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_generic_f32_clip_bind(
    const struct sgx535_generic_f32_clip_request *, const struct sgx535_generic_f32_clip_image *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, struct sgx535_generic_f32_clip_bound *,
    struct sgx535_generic_f32_clip_workspace *, uint8_t *, uint32_t);
enum sgx535_task_status sgx535_generic_f32_clip_bound_validate(
    const struct sgx535_generic_f32_clip_request *, const struct sgx535_generic_f32_clip_image *,
    const uint8_t *, uint32_t, const struct sgx535_window_mesh_binding *,
    const struct sgx535_background_binding *, const struct sgx535_generic_f32_clip_bound *,
    struct sgx535_generic_f32_clip_workspace *, uint8_t *, uint32_t);
/* Always refuses, including forged missing/qualification fields. */
enum sgx535_task_status
sgx535_generic_f32_clip_native_admission(const struct sgx535_generic_f32_clip_image *);
#endif
