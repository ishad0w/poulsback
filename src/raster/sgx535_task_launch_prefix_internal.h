// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TASK_LAUNCH_PREFIX_INTERNAL_H
#define SGX535_TASK_LAUNCH_PREFIX_INTERNAL_H
/* Shared source-defined prefix only. Complete typed constructors authenticate
 * inputs and regenerate every image before this private helper is reached. */
static void sgx535_launch_prefix_write(struct sgx535_task_launch_stage_plan *s, uint32_t offset,
                                       uint32_t value, uint32_t unknown)
{
    uint32_t i = s->count++;
    s->actions[i] = (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, offset, value, 0};
    s->unknown_value_masks[i] = unknown;
}
enum sgx535_launch_prefix_convention {
    SGX535_LAUNCH_PREFIX_LEGACY = 0,
    SGX535_LAUNCH_PREFIX_WIN7_GL_WINDOW = 1,
    /* The GL window with clip-space draws: the MTE clamps W, as EMGD's 0xa2. */
    SGX535_LAUNCH_PREFIX_WIN7_GL_CLIP = 2
};
static int sgx535_launch_prefix_build_convention(struct sgx535_task_launch_stage_plan *ta,
                                                 struct sgx535_task_launch_stage_plan *r,
                                                 uint32_t ta_stream, uint32_t event_pds, int dri,
                                                 uint32_t background_policy,
                                                 const struct sgx535_background_bound *background,
                                                 enum sgx535_launch_prefix_convention convention)
{
    if (convention != SGX535_LAUNCH_PREFIX_LEGACY &&
        convention != SGX535_LAUNCH_PREFIX_WIN7_GL_WINDOW &&
        convention != SGX535_LAUNCH_PREFIX_WIN7_GL_CLIP)
        return 0;
    /* The selected complete TA register-pair producer. Stream relocation is
     * full byte OFFSET, unlike pointers inside the TA stream itself. */
    sgx535_launch_prefix_write(ta, 0x204, 0, 0);
    sgx535_launch_prefix_write(ta, 0x218, 0x400000, 0);
    sgx535_launch_prefix_write(ta, 0x23c,
                               convention == SGX535_LAUNCH_PREFIX_WIN7_GL_CLIP ? 0xa2u : 0x82u, 0);
    sgx535_launch_prefix_write(ta, 0x240, 0x358637bd, 0);
    sgx535_launch_prefix_write(ta, 0x244, 0x358637bd, 0);
    sgx535_launch_prefix_write(ta, 0x250, 0x88, 0);
    sgx535_launch_prefix_write(ta, 0x238, ta_stream, 0);
    /* No-depth branch, no background load program. The matched DRI v3 profile
     * retains a fresh context's clear-state defaults without a GL clear call:
     * tracked pointers bind the actual depth/stencil attribute groups, whose
     * initializers write binary64 1.0 and int32 0. The caller narrows those to
     * binary32 and uint8 respectively. This is state, not an emitted clear.
     * v2 keeps its original unresolved caller inputs and exact descriptor. */
    sgx535_launch_prefix_write(r, 0x400, 0xffff80, 0);
    sgx535_launch_prefix_write(r, 0x404, 0, 0);
    sgx535_launch_prefix_write(r, 0x40c, 0, 0);
    /* RENDBOX1/2 stay 0 with ISP_RENDER type 0, as EMGD runs
     * (docs/re/platform/runtime-render-state.md#render-state-and-memory). */
    sgx535_launch_prefix_write(r, 0x410, 0, 0);
    sgx535_launch_prefix_write(r, 0x414, 0x100, 0);
    sgx535_launch_prefix_write(r, 0x418, 1, 0);
    sgx535_launch_prefix_write(r, 0x41c, 0x0da24260, 0);
    sgx535_launch_prefix_write(r, 0x420, 0, 0);
    sgx535_launch_prefix_write(r, 0x424, 0xfbf4, 0);
    sgx535_launch_prefix_write(r, 0x42c, 0, 0);
    sgx535_launch_prefix_write(r, 0x480, 0xf0000, 0);
    sgx535_launch_prefix_write(r, 0xcb0, 0, 0);
    sgx535_launch_prefix_write(r, 0x484, 0, 0);
    sgx535_launch_prefix_write(r, 0x488, 0, 0);
    sgx535_launch_prefix_write(r, 0x48c, 0, 0);
    sgx535_launch_prefix_write(r, 0x490, 0, 0);
    sgx535_launch_prefix_write(r, 0x494, 0xff, 0);
    sgx535_launch_prefix_write(r, 0x4c4, background ? background->bgobjtag_word : 0, 0);
    sgx535_launch_prefix_write(r, 0x4bc,
                               background ? background->bgobj_word
                               : background_policy == SGX535_PAIR_BACKGROUND_DISABLED_FIXED16
                                   ? 0u
                                   : 0x200u,
                               dri ? 0u : 0xffu);
    sgx535_launch_prefix_write(r, 0x4b8,
                               background ? background->bgdepth_word
                               : dri      ? 0x3f800000u
                                          : 0u,
                               dri ? 0u : 0xffffffffu);
    sgx535_launch_prefix_write(r, 0x4c8, 0x88, 0);
    sgx535_launch_prefix_write(r, 0x4dc, 0, 0);
    /* Matched Win7 allocation0x100 -> retained flag8 -> normal single-sample
     * tuple1/88/1. TSP bit0's edge equation is not established. This is a
     * constructor input policy, never an edit of a validated launch plan. */
    sgx535_launch_prefix_write(r, 0x800, convention != SGX535_LAUNCH_PREFIX_LEGACY ? 1u : 0u, 0);
    sgx535_launch_prefix_write(r, 0xa5c, event_pds, 0);
    /* Matched DRI record[3]=16 DWORDs; the register consumes groups of four.
     * v2's separate record/producer must not inherit this profile join. */
    sgx535_launch_prefix_write(r, 0xa60, dri ? 4u : 0u, dri ? 0u : 0x3fu);
    sgx535_launch_prefix_write(r, 0xa64, 0x4fff, 0);
    return 1;
}
static inline void sgx535_launch_prefix_build(struct sgx535_task_launch_stage_plan *ta,
                                              struct sgx535_task_launch_stage_plan *r,
                                              uint32_t ta_stream, uint32_t event_pds, int dri,
                                              uint32_t background_policy,
                                              const struct sgx535_background_bound *background)
{
    (void)sgx535_launch_prefix_build_convention(ta, r, ta_stream, event_pds, dri, background_policy,
                                                background, SGX535_LAUNCH_PREFIX_LEGACY);
}
#endif
