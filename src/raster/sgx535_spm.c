// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_spm.h"

/* Event status and ack (0x12c, 0x134) bits the programs poll. */
#define OTPM_INVALIDATED UINT32_C(0x00100000)
#define STATE_STORED 0x10u
#define STATE_LOADED 0x20u
#define STATE_CLEARED 0x40u
#define CONTROL_CLEARED 0x200u
#define ZLS_FORMAT UINT32_C(0x00030000)
#define ZLS_FORMAT_DPM ZLS_FORMAT /* ZLOADFORMAT 3: the DPM's own buffer */
#define ADDRESS_LIMIT UINT32_C(0x10000000)

static void add(struct sgx535_spm_program *p, uint32_t kind, uint32_t offset, uint32_t value)
{
    p->actions[p->count++] = (struct sgx535_engine121_action){kind, offset, value, 0};
}

static void wait_ack(struct sgx535_spm_program *p, uint32_t bits)
{
    p->actions[p->count++] =
        (struct sgx535_engine121_action){SGX535_ENGINE121_WAIT_SET, 0x12c, bits, bits};
    add(p, SGX535_ENGINE121_WRITE, 0x134, bits);
    p->actions[p->count++] =
        (struct sgx535_engine121_action){SGX535_ENGINE121_WAIT_CLEAR, 0x12c, 0, bits};
}

enum sgx535_engine121_status sgx535_spm_te_step_build(uint32_t zls, uint32_t ceiling,
                                                      struct sgx535_spm_program *out)
{
    if (!out)
        return SGX535_ENGINE121_INVALID;
    if (ceiling > 0xffffu || zls >= ceiling)
        return SGX535_ENGINE121_RANGE;
    out->count = 0;
    add(out, SGX535_SPM_PAUSE, 0xa74, 1);
    add(out, SGX535_ENGINE121_WRITE, 0x24c, 1);
    wait_ack(out, OTPM_INVALIDATED);
    add(out, SGX535_ENGINE121_WRITE, 0x624, zls + 1u);
    add(out, SGX535_SPM_SET_BITS, 0x218, UINT32_C(0x00400000)); /* COMPLETEONTERMINATE */
    add(out, SGX535_SPM_RELOAD, 0x684, 1);
    add(out, SGX535_ENGINE121_WRITE, 0x69c, 1);
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status sgx535_spm_abort_build(struct sgx535_spm_program *out)
{
    if (!out)
        return SGX535_ENGINE121_INVALID;
    out->count = 0;
    add(out, SGX535_ENGINE121_WRITE, 0x24c, 1);
    wait_ack(out, OTPM_INVALIDATED);
    add(out, SGX535_ENGINE121_WRITE, 0x69c, 4); /* ABORTALL */
    add(out, SGX535_SPM_SET_BITS, 0xa74, 1);
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status sgx535_spm_handoff_build(uint32_t dpm_id, uint32_t context,
                                                      uint32_t state_table, int partial,
                                                      struct sgx535_spm_program *out)
{
    uint32_t id;
    if (!out || context > 1 || !state_table)
        return SGX535_ENGINE121_INVALID;
    if (state_table & 15u)
        return SGX535_ENGINE121_ALIGNMENT;
    if ((dpm_id >> 2 & 1u) != context || (dpm_id & 1u) != context)
        return SGX535_ENGINE121_STALE;
    id = SGX535_SPM_HANDOFF_DPM_ID(dpm_id, context);
    out->count = 0;
    if (partial)
        add(out, SGX535_ENGINE121_WRITE, 0x658, 1);
    add(out, SGX535_ENGINE121_WRITE, 0x630, id);
    add(out, SGX535_ENGINE121_WRITE, 0x62c, state_table);
    add(out, SGX535_SPM_CLEAR_BITS, 0xc74, 0x200); /* store and load through the TA requester */
    add(out, SGX535_ENGINE121_WRITE, 0x694, 1);
    wait_ack(out, STATE_STORED);
    add(out, SGX535_ENGINE121_WRITE, 0x694, 2);
    wait_ack(out, STATE_CLEARED);
    add(out, SGX535_ENGINE121_WRITE, 0x630, id ^ 1u); /* LS on !c */
    add(out, SGX535_ENGINE121_WRITE, 0x694, 4);
    wait_ack(out, STATE_LOADED);
    add(out, SGX535_ENGINE121_WRITE, 0x630, id);
    add(out, SGX535_ENGINE121_WRITE, 0x65c, 0);
    return SGX535_ENGINE121_OK;
}

static int dummy_valid(const struct sgx535_spm_dummy *d)
{
    uint32_t load = d->load - d->base, store = d->store - d->base;
    if ((d->base & 0xfffffu) || d->load < d->base || d->store < d->base || ((load | store) & 15u))
        return 0;
    if (load > ADDRESS_LIMIT - SGX535_SPM_DUMMY_BYTES ||
        store > ADDRESS_LIMIT - SGX535_SPM_DUMMY_BYTES)
        return 0;
    return load >= store + SGX535_SPM_DUMMY_BYTES || store >= load + SGX535_SPM_DUMMY_BYTES;
}

static int kept(uint32_t kind, const struct sgx535_engine121_action *a)
{
    return kind == SGX535_SPM_RENDER_FINAL || a->kind != SGX535_ENGINE121_WRITE ||
           (a->offset != 0x658 && a->offset != 0x65c);
}

enum sgx535_engine121_status sgx535_spm_render_build(const struct sgx535_spm_render *r,
                                                     struct sgx535_spm_program *out)
{
    struct sgx535_engine121_action tail[SGX535_ENGINE121_RASTER_ACTIONS];
    enum sgx535_engine121_status status;
    size_t tail_count, i;
    uint32_t bits = 0, dropped = 0;
    int final;
    if (!out || !r || (r->client_count && !r->client) || r->planes > 3u)
        return SGX535_ENGINE121_INVALID;
    if (r->kind < SGX535_SPM_RENDER_PARTIAL || r->kind > SGX535_SPM_RENDER_FINAL)
        return SGX535_ENGINE121_INVALID;
    final = r->kind == SGX535_SPM_RENDER_FINAL;
    if (final && !r->done)
        return SGX535_ENGINE121_INVALID;
    /* At most six writes of ours between the job's writes and the tail. */
    if (r->client_count > SGX535_SPM_ACTIONS - 6u - SGX535_ENGINE121_RASTER_ACTIONS)
        return SGX535_ENGINE121_NO_SPACE;
    for (i = 0; i < r->client_count; i++) {
        if (r->client[i].kind != SGX535_ENGINE121_WRITE)
            return SGX535_ENGINE121_INVALID;
        if (r->client[i].offset == 0x480 && (r->client[i].value & ZLS_FORMAT) == ZLS_FORMAT_DPM)
            return SGX535_ENGINE121_UNSUPPORTED;
    }
    if (!r->planes && !dummy_valid(&r->dummy))
        return SGX535_ENGINE121_RANGE;
    status =
        sgx535_engine121_raster_build(r->scene, tail, SGX535_ENGINE121_RASTER_ACTIONS, &tail_count);
    if (status != SGX535_ENGINE121_OK)
        return status;
    /* A partial or whole-scene render keeps PARTIAL_RENDER and the LSS context
     * as the handoff left them: the tail's one write of each goes. */
    for (i = 0; i < tail_count; i++)
        dropped += kept(r->kind, &tail[i]) ? 0u : 1u;
    if (!final && dropped != 2)
        return SGX535_ENGINE121_FAULT;

    out->count = 0;
    for (i = 0; i < r->client_count; i++)
        out->actions[out->count++] = r->client[i];
    if (!r->planes) {
        add(out, SGX535_ENGINE121_WRITE, 0xcb0, r->dummy.base);
        add(out, SGX535_ENGINE121_WRITE, 0x48c,
            (r->dummy.load - r->dummy.base) | SGX535_SPM_DUMMY_ENABLE);
        add(out, SGX535_ENGINE121_WRITE, 0x490,
            (r->dummy.store - r->dummy.base) | SGX535_SPM_DUMMY_ENABLE);
        add(out, SGX535_ENGINE121_WRITE, 0x480, SGX535_SPM_ZLS_STENCIL);
    } else if (final) {
        bits = (r->planes & SGX535_SPM_DEPTH ? SGX535_SPM_ZLS_FINAL_DEPTH : 0) |
               (r->planes & SGX535_SPM_STENCIL ? SGX535_SPM_ZLS_FINAL_STENCIL : 0);
        add(out, SGX535_SPM_SET_BITS, 0x480, bits);
    } else {
        if (r->planes & SGX535_SPM_DEPTH)
            bits = SGX535_SPM_ZLS_PARTIAL_DEPTH | (r->done ? SGX535_SPM_ZLS_LOADMASK : 0);
        if (r->planes & SGX535_SPM_STENCIL)
            bits |= SGX535_SPM_ZLS_STENCIL;
        add(out, SGX535_SPM_SET_BITS, 0x480, bits);
    }
    add(out, SGX535_SPM_SET_BITS, 0x414, 0x100); /* PROCESSEMPTY */
    if (r->continuation && r->done)
        add(out, SGX535_ENGINE121_WRITE, 0x4c4, r->continuation);
    for (i = 0; i < tail_count; i++)
        if (kept(r->kind, &tail[i]))
            out->actions[out->count++] = tail[i];
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status sgx535_spm_end_build(uint32_t kind, uint32_t steps, uint32_t zls_base,
                                                  uint32_t te_psg, struct sgx535_spm_program *out)
{
    if (!out || (kind != SGX535_SPM_END_RESTART && kind != SGX535_SPM_END_WHOLE))
        return SGX535_ENGINE121_INVALID;
    if (kind == SGX535_SPM_END_WHOLE && steps)
        return SGX535_ENGINE121_INVALID;
    if (steps && zls_base > 0xffffu)
        return SGX535_ENGINE121_RANGE;
    out->count = 0;
    if (steps) {
        add(out, SGX535_ENGINE121_WRITE, 0x624, zls_base);
        add(out, SGX535_SPM_RELOAD, 0x684, 1);
        add(out, SGX535_ENGINE121_WRITE, 0x218, te_psg);
    }
    add(out, SGX535_SPM_CLEAR_BITS, 0xa74, 1);
    add(out, SGX535_ENGINE121_WRITE, 0x698, 2); /* CONTROL CLEAR */
    add(out, SGX535_ENGINE121_WRITE, 0x658, 0);
    wait_ack(out, CONTROL_CLEARED);
    add(out, SGX535_ENGINE121_WRITE, 0x65c, 0);
    if (kind == SGX535_SPM_END_RESTART) {
        add(out, SGX535_ENGINE121_WRITE, 0x69c, 1);
        return SGX535_ENGINE121_OK;
    }
    add(out, SGX535_ENGINE121_WRITE, 0x694, 2);
    add(out, SGX535_ENGINE121_WRITE, 0x698, 2);
    add(out, SGX535_ENGINE121_WRITE, 0x24c, 1);
    wait_ack(out, STATE_CLEARED | CONTROL_CLEARED | OTPM_INVALIDATED);
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status sgx535_spm_ta_end_build(uint32_t steps, uint32_t zls_base,
                                                     struct sgx535_spm_program *out)
{
    if (!out || !steps)
        return SGX535_ENGINE121_INVALID;
    if (zls_base > 0xffffu)
        return SGX535_ENGINE121_RANGE;
    out->count = 0;
    add(out, SGX535_ENGINE121_WRITE, 0x624, zls_base);
    add(out, SGX535_SPM_RELOAD, 0x684, 1);
    add(out, SGX535_SPM_CLEAR_BITS, 0xa74, 0xf); /* every data master on */
    return SGX535_ENGINE121_OK;
}
