// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_spm_state.h"

#define BOTH_EVENTS (SGX535_SPM_PIXELBE_END | SGX535_SPM_MEM_FREE)

static enum sgx535_spm_run hold(struct sgx535_spm *s)
{
    s->phase = SGX535_SPM_HELD;
    return SGX535_SPM_HOLD;
}

/* The context a raster or render runs on, or 2 for none. */
static uint32_t running(const struct sgx535_spm *s)
{
    uint32_t i;
    for (i = 0; i < 2; i++)
        if (s->owner[i] == SGX535_SPM_OWNER_RASTER || s->owner[i] == SGX535_SPM_OWNER_RENDER)
            return i;
    return 2;
}

/* A render of the TA's scene on !c, which must be free (an owner there is an
 * engine bug), with nothing else running. */
static enum sgx535_spm_run render(struct sgx535_spm *s, uint32_t phase, enum sgx535_spm_run run)
{
    uint32_t other = s->context ^ 1u;
    if (running(s) != 2 || s->owner[other] != SGX535_SPM_FREE)
        return hold(s);
    s->owner[other] = SGX535_SPM_OWNER_RENDER;
    s->dalloc = (uint8_t)other;
    s->seen = 0;
    s->phase = (uint8_t)phase;
    return run;
}

/* The TA finished: the scene's whole-scene render after TE steps (E), else the
 * job's own raster. */
static enum sgx535_spm_run finished(struct sgx535_spm *s)
{
    if (s->steps) {
        s->undo = s->steps;
        s->steps = 0;
        s->zls = s->zls_base;
        return render(s, SGX535_SPM_WHOLE, SGX535_SPM_RUN_WHOLE);
    }
    s->owner[s->context] = SGX535_SPM_OWNER_RASTER;
    s->dalloc = s->context;
    s->phase = SGX535_SPM_RASTER;
    return SGX535_SPM_RUN_RASTER;
}

enum sgx535_spm_run sgx535_spm_init(struct sgx535_spm *s, uint32_t zls_base, uint32_t zls_ceiling)
{
    *s = (struct sgx535_spm){0};
    s->zls_base = s->zls = zls_base;
    s->zls_ceiling = zls_ceiling;
    if (zls_base >= zls_ceiling || zls_ceiling > 0xffffu)
        return hold(s);
    return SGX535_SPM_WAIT;
}

enum sgx535_spm_run sgx535_spm_ta_start(struct sgx535_spm *s, uint32_t context)
{
    if (s->phase != SGX535_SPM_IDLE || context > 1 || s->owner[context] != SGX535_SPM_FREE)
        return hold(s);
    s->owner[context] = SGX535_SPM_OWNER_TA;
    s->context = (uint8_t)context;
    s->phase = SGX535_SPM_TA;
    s->steps = s->undo = s->partials = 0;
    s->zls = s->zls_base;
    return SGX535_SPM_WAIT;
}

enum sgx535_spm_run sgx535_spm_raster_start(struct sgx535_spm *s, uint32_t context)
{
    if ((s->phase != SGX535_SPM_IDLE && s->phase != SGX535_SPM_TA) || context > 1 ||
        running(s) != 2 || s->owner[context] != SGX535_SPM_FREE)
        return hold(s);
    s->owner[context] = SGX535_SPM_OWNER_RASTER;
    s->dalloc = (uint8_t)context;
    return SGX535_SPM_WAIT;
}

enum sgx535_spm_run sgx535_spm_raster_end(struct sgx535_spm *s)
{
    uint32_t x = running(s);
    if (s->phase == SGX535_SPM_HELD || x == 2 || s->owner[x] != SGX535_SPM_OWNER_RASTER)
        return hold(s);
    s->owner[x] = SGX535_SPM_FREE;
    switch (s->phase) {
    case SGX535_SPM_RASTER:
        if (x != s->context)
            return hold(s);
        s->phase = SGX535_SPM_IDLE;
        return SGX535_SPM_SIGNAL;
    case SGX535_SPM_DEFERRED:
        s->phase = SGX535_SPM_TA;
        return SGX535_SPM_RUN_RESTART;
    case SGX535_SPM_FINISHING:
    case SGX535_SPM_READY:
        return finished(s);
    case SGX535_SPM_IDLE:
    case SGX535_SPM_TA:
        return SGX535_SPM_WAIT;
    default:
        return hold(s);
    }
}

enum sgx535_spm_run sgx535_spm_oom(struct sgx535_spm *s, uint32_t requesting)
{
    if (s->phase != SGX535_SPM_TA)
        return hold(s);
    if (running(s) != 2) {
        s->phase = SGX535_SPM_DEFERRED;
        return SGX535_SPM_WAIT;
    }
    switch (requesting & 3u) {
    case 1: /* TE: one more page, never past the ceiling */
        if (s->zls >= s->zls_ceiling)
            return hold(s);
        s->zls++;
        s->steps++;
        return SGX535_SPM_RUN_TE_STEP;
    case 2: /* MTE */
        s->phase = SGX535_SPM_ABORTING;
        return SGX535_SPM_RUN_ABORT;
    default: /* neither or both: not designed */
        return hold(s);
    }
}

enum sgx535_spm_run sgx535_spm_terminated(struct sgx535_spm *s)
{
    if (s->phase != SGX535_SPM_ABORTING)
        return hold(s);
    return render(s, SGX535_SPM_PARTIAL, SGX535_SPM_RUN_PARTIAL);
}

enum sgx535_spm_run sgx535_spm_render_event(struct sgx535_spm *s, uint32_t bits, uint32_t ta_pages)
{
    uint32_t other = s->context ^ 1u;
    if ((s->phase != SGX535_SPM_PARTIAL && s->phase != SGX535_SPM_WHOLE) || !bits ||
        (bits & ~BOTH_EVENTS) || (bits & s->seen))
        return hold(s);
    s->seen |= (uint8_t)bits;
    if (s->seen != BOTH_EVENTS)
        return SGX535_SPM_WAIT;
    if (s->phase == SGX535_SPM_WHOLE) {
        s->owner[other] = s->owner[s->context] = SGX535_SPM_FREE;
        s->phase = SGX535_SPM_IDLE;
        return SGX535_SPM_RUN_WHOLE_END;
    }
    if (ta_pages >= s->zls_base) /* the render freed too little */
        return hold(s);
    s->owner[other] = SGX535_SPM_FREE;
    s->partials++;
    s->undo = s->steps;
    s->steps = 0;
    s->zls = s->zls_base;
    s->phase = SGX535_SPM_TA;
    return SGX535_SPM_RUN_END;
}

enum sgx535_spm_run sgx535_spm_ta_finished(struct sgx535_spm *s)
{
    if (s->phase != SGX535_SPM_TA)
        return hold(s);
    if (running(s) != 2) {
        s->phase = s->steps ? SGX535_SPM_FINISHING : SGX535_SPM_READY;
        return SGX535_SPM_WAIT;
    }
    return finished(s);
}
