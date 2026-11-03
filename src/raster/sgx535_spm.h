// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_SPM_H
#define SGX535_SPM_H

#include "sgx535_engine121.h"

/* Register programs for a TA out of memory
 * (docs/re/firmware/partial-render.md). CPU construction only: an executor must
 * run each program with no raster running and keep the 0x630 and ZLS shadows
 * the inputs name. Every builder checks its inputs
 * first; an error leaves the program untouched. c is the TA's DPM context, !c the
 * other one. */

/* Steps beside engine121's actions, run by the engine as code: */
enum sgx535_spm_step {
    SGX535_SPM_SET_BITS = 16, /* offset |= value (a read-modify-write) */
    SGX535_SPM_CLEAR_BITS,    /* offset &= ~value */
    /* DPMPauseAllocations: 0xa74 |= 1, then 0x700 read until two reads agree. */
    SGX535_SPM_PAUSE,
    /* DPMReloadFreeList: 0x61c = 0x700, 0x648 = 0x710 << 16, 0x638 = 0x724 >> 16 |
     * 0x72c & 0xffff0000, 0x684 = 1, wait 0x118 bit 0, ack it. */
    SGX535_SPM_RELOAD
};

#define SGX535_SPM_ACTIONS 64u
/* Partial-render depth bits (with FORCEZSTORE, as EMGD sets them) and the
 * final raster's after partial renders. */
#define SGX535_SPM_ZLS_PARTIAL_DEPTH UINT32_C(0x00400054)
#define SGX535_SPM_ZLS_LOADMASK UINT32_C(0x00200000)
#define SGX535_SPM_ZLS_STENCIL UINT32_C(0x00000082)
#define SGX535_SPM_ZLS_FINAL_DEPTH UINT32_C(0x00200004)
#define SGX535_SPM_ZLS_FINAL_STENCIL UINT32_C(0x00000002)
/* The dummy pair: two buffers of this size; 0x48c and 0x490 take their offsets
 * from 0xcb0's base with this enable bit. */
#define SGX535_SPM_DUMMY_BYTES UINT32_C(0x40000)
#define SGX535_SPM_DUMMY_ENABLE UINT32_C(0x80000000)
/* 0x630 after the handoff (B1, B8): ALLOC and LS on c, DALLOC on !c. */
#define SGX535_SPM_HANDOFF_DPM_ID(id, c) (((id) & ~UINT32_C(3)) | ((c) ^ 1u) << 1 | (c))

struct sgx535_spm_program {
    struct sgx535_engine121_action actions[SGX535_SPM_ACTIONS];
    uint32_t count;
};

/* T, one TE step: one more page for the TE with the vertex data master left
 * paused. zls is 0x624 now; the step writes zls + 1, never past ceiling. */
enum sgx535_engine121_status sgx535_spm_te_step_build(uint32_t zls, uint32_t ceiling,
                                                      struct sgx535_spm_program *out);

/* A, the MTE was denied: invalidate the OTPM, ABORTALL, pause the vertex data
 * master. TA_TERMINATE follows by IRQ. */
enum sgx535_engine121_status sgx535_spm_abort_build(struct sgx535_spm_program *out);

/* B, the TA's DPM state into !c. dpm_id is the 0x630 shadow (ALLOC and LS on c);
 * state_table is the scene's 16-byte aligned state-table slot. partial: open with
 * PARTIAL_RENDER (B0), not for the whole-scene render after TE steps (E3). */
enum sgx535_engine121_status sgx535_spm_handoff_build(uint32_t dpm_id, uint32_t context,
                                                      uint32_t state_table, int partial,
                                                      struct sgx535_spm_program *out);

enum sgx535_spm_render_kind {
    SGX535_SPM_RENDER_PARTIAL = 1, /* C: a partial render from !c */
    SGX535_SPM_RENDER_WHOLE,       /* E3: the scene from !c after TE steps */
    SGX535_SPM_RENDER_FINAL        /* F: the job's own raster after partial renders */
};

#define SGX535_SPM_DEPTH 1u
#define SGX535_SPM_STENCIL 2u

struct sgx535_spm_dummy {
    uint32_t base;        /* 0xcb0: 1 MiB aligned */
    uint32_t load, store; /* the two buffers' GPU addresses above base */
};

struct sgx535_spm_render {
    uint32_t kind;
    /* SUBMIT's raster writes for the job, WRITEs only, ZLOADFORMAT not 3. */
    const struct sgx535_engine121_action *client;
    uint32_t client_count;
    const struct sgx535_engine121_scene_binding *scene;
    uint32_t planes; /* SGX535_SPM_DEPTH | SGX535_SPM_STENCIL; none: the dummy pair */
    uint32_t done;   /* partial renders before this one (FINAL: at least 1) */
    /* BGOBJTAG of the renders after the first; 0 keeps the job's own. */
    uint32_t continuation;
    struct sgx535_spm_dummy dummy;
};

/* C, E3 or F: the job's raster writes, its plane bits, PROCESSEMPTY and the
 * continuation background, then engine121's raster tail. C and E3 keep 0x658 and
 * 0x65c as B left them; F writes the tail whole. The engine's ISP reset precedes
 * every one, as every raster. */
enum sgx535_engine121_status sgx535_spm_render_build(const struct sgx535_spm_render *render,
                                                     struct sgx535_spm_program *out);

enum sgx535_spm_end_kind {
    SGX535_SPM_END_RESTART = 1, /* D after a partial render, ending in RESTART */
    SGX535_SPM_END_WHOLE        /* E4 and E5 after the whole-scene render */
};

/* D or E4 + E5. RESTART: with steps (TE steps since the last render) the ZLS goes
 * back to zls_base and the job's TE_PSG comes back first. The engine checks
 * progress (D3) before this program. */
enum sgx535_engine121_status sgx535_spm_end_build(uint32_t kind, uint32_t steps, uint32_t zls_base,
                                                  uint32_t te_psg, struct sgx535_spm_program *out);

/* E1, the TA finished after steps TE steps: ZLS back to zls_base and every data
 * master on; the handoff without B0 and the whole-scene render follow. */
enum sgx535_engine121_status sgx535_spm_ta_end_build(uint32_t steps, uint32_t zls_base,
                                                     struct sgx535_spm_program *out);

#endif
