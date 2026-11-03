// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* SPM state and DPM accounting (docs/native-3d.md#partial-render-construction): event orders,
 * refusals and
 * random event sequences against the invariants; then a page model that runs
 * the register programs on a simulated register file and DPM (the pool, the two
 * contexts, the state-table slot) through a scene of four pools. CPU only: the
 * model implements the stated invariants, not the device. */
#include "sgx535_spm.h"
#include "sgx535_spm_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define POOL 8192u
#define BASE (POOL - 477u)
#define CEILING (POOL - 4u)
#define PIXELBE SGX535_SPM_PIXELBE_END
#define MEMFREE SGX535_SPM_MEM_FREE

static uint32_t running(const struct sgx535_spm *s)
{
    uint32_t i;
    for (i = 0; i < 2; i++)
        if (s->owner[i] == SGX535_SPM_OWNER_RASTER || s->owner[i] == SGX535_SPM_OWNER_RENDER)
            return i;
    return 2;
}

/* The invariants of sgx535_spm_state.h after every event; prev is the state
 * before it. */
static void invariants(const struct sgx535_spm *s, const struct sgx535_spm *prev)
{
    uint32_t x = running(s), px = running(prev), c = s->context, busy = 0, i;
    int ta_phase = s->phase == SGX535_SPM_TA || s->phase == SGX535_SPM_DEFERRED ||
                   s->phase == SGX535_SPM_ABORTING || s->phase == SGX535_SPM_PARTIAL ||
                   s->phase == SGX535_SPM_FINISHING || s->phase == SGX535_SPM_READY;
    if (s->phase == SGX535_SPM_HELD)
        return;
    for (i = 0; i < 2; i++)
        busy += s->owner[i] == SGX535_SPM_OWNER_RASTER || s->owner[i] == SGX535_SPM_OWNER_RENDER;
    CHECK(busy <= 1);
    CHECK(x == 2 || s->dalloc == x);
    /* DALLOC moves only when nothing runs. */
    if (px < 2)
        CHECK(s->dalloc == prev->dalloc || s->owner[px] != prev->owner[px]);
    CHECK((s->phase == SGX535_SPM_PARTIAL || s->phase == SGX535_SPM_WHOLE) ==
          (s->owner[c ^ 1u] == SGX535_SPM_OWNER_RENDER));
    CHECK(s->owner[c] != SGX535_SPM_OWNER_RENDER);
    CHECK(!ta_phase || s->owner[c] == SGX535_SPM_OWNER_TA);
    CHECK(s->phase != SGX535_SPM_RASTER || s->owner[c] == SGX535_SPM_OWNER_RASTER);
    CHECK(s->phase != SGX535_SPM_IDLE ||
          (s->owner[0] != SGX535_SPM_OWNER_TA && s->owner[1] != SGX535_SPM_OWNER_TA));
    CHECK((s->phase != SGX535_SPM_DEFERRED && s->phase != SGX535_SPM_FINISHING &&
           s->phase != SGX535_SPM_READY) ||
          s->owner[c ^ 1u] == SGX535_SPM_OWNER_RASTER);
    /* ZLS: zls_base and one page per TE step since the last render. */
    CHECK(s->zls == s->zls_base + s->steps && s->zls <= s->zls_ceiling);
    CHECK(s->phase != SGX535_SPM_IDLE || s->zls == s->zls_base);
}

static struct sgx535_spm state;

static void event(enum sgx535_spm_run got, enum sgx535_spm_run want, const struct sgx535_spm *prev,
                  int line)
{
    ++checks;
    if (got != want) {
        fprintf(stderr, "FAIL line %d: run %d, expected %d (phase %u)\n", line, got, want,
                state.phase);
        exit(1);
    }
    invariants(&state, prev);
}
#define EV(call, want)                                                                             \
    do {                                                                                           \
        struct sgx535_spm prev_ = state;                                                           \
        event(call, want, &prev_, __LINE__);                                                       \
    } while (0)

/* A misfit holds, and every event after it holds too. */
static void held(void)
{
    CHECK(state.phase == SGX535_SPM_HELD);
    CHECK(sgx535_spm_ta_start(&state, 0) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_raster_start(&state, 0) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_raster_end(&state) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_oom(&state, 2) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_terminated(&state) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_render_event(&state, PIXELBE, 0) == SGX535_SPM_HOLD);
    CHECK(sgx535_spm_ta_finished(&state) == SGX535_SPM_HOLD);
    CHECK(state.phase == SGX535_SPM_HELD);
}
#define MISFIT(call)                                                                               \
    do {                                                                                           \
        CHECK((call) == SGX535_SPM_HOLD);                                                          \
        held();                                                                                    \
    } while (0)

static void fresh(void)
{
    CHECK(sgx535_spm_init(&state, BASE, CEILING) == SGX535_SPM_WAIT);
}

static void orders(void)
{
    uint32_t i;
    /* A job without OOM: its own raster on its context, the fence. */
    fresh();
    EV(sgx535_spm_ta_start(&state, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_ta_finished(&state), SGX535_SPM_RUN_RASTER);
    CHECK(state.dalloc == 0 && state.owner[0] == SGX535_SPM_OWNER_RASTER);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_SIGNAL);
    CHECK(state.phase == SGX535_SPM_IDLE);
    /* An MTE denial: abort, terminate, a partial render on !c, its end, the
     * job's raster on c; the render's two events in either order. */
    EV(sgx535_spm_ta_start(&state, 1), SGX535_SPM_WAIT);
    EV(sgx535_spm_oom(&state, 2), SGX535_SPM_RUN_ABORT);
    EV(sgx535_spm_terminated(&state), SGX535_SPM_RUN_PARTIAL);
    CHECK(state.dalloc == 0 && state.owner[0] == SGX535_SPM_OWNER_RENDER && state.partials == 0);
    EV(sgx535_spm_render_event(&state, MEMFREE, 5000), SGX535_SPM_WAIT);
    EV(sgx535_spm_render_event(&state, PIXELBE, 0), SGX535_SPM_RUN_END);
    CHECK(state.partials == 1 && state.undo == 0 && state.owner[0] == SGX535_SPM_FREE);
    EV(sgx535_spm_oom(&state, 2), SGX535_SPM_RUN_ABORT);
    EV(sgx535_spm_terminated(&state), SGX535_SPM_RUN_PARTIAL);
    EV(sgx535_spm_render_event(&state, PIXELBE | MEMFREE, BASE - 1u), SGX535_SPM_RUN_END);
    EV(sgx535_spm_ta_finished(&state), SGX535_SPM_RUN_RASTER);
    CHECK(state.dalloc == 1 && state.partials == 2);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_SIGNAL);
    /* TE steps then an abort: the end takes the steps back. */
    EV(sgx535_spm_ta_start(&state, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_oom(&state, 1), SGX535_SPM_RUN_TE_STEP);
    EV(sgx535_spm_oom(&state, 5), SGX535_SPM_RUN_TE_STEP); /* 0x734's bits above 1:0 */
    CHECK(state.zls == BASE + 2u && state.steps == 2);
    EV(sgx535_spm_oom(&state, 2), SGX535_SPM_RUN_ABORT);
    EV(sgx535_spm_terminated(&state), SGX535_SPM_RUN_PARTIAL);
    EV(sgx535_spm_render_event(&state, PIXELBE, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_render_event(&state, MEMFREE, 0), SGX535_SPM_RUN_END);
    CHECK(state.undo == 2 && state.zls == BASE && state.steps == 0);
    /* Then TE steps and the TA finishes: the whole-scene render (E), no raster
     * of the job's own, the fence after it. */
    EV(sgx535_spm_oom(&state, 1), SGX535_SPM_RUN_TE_STEP);
    EV(sgx535_spm_ta_finished(&state), SGX535_SPM_RUN_WHOLE);
    CHECK(state.undo == 1 && state.zls == BASE && state.dalloc == 1 && state.partials == 1);
    EV(sgx535_spm_render_event(&state, MEMFREE, 9000), SGX535_SPM_WAIT); /* no progress check */
    EV(sgx535_spm_render_event(&state, PIXELBE, 9000), SGX535_SPM_RUN_WHOLE_END);
    CHECK(state.phase == SGX535_SPM_IDLE && state.owner[0] == SGX535_SPM_FREE &&
          state.owner[1] == SGX535_SPM_FREE);
    /* Beside another job's raster on !c: an OOM waits for its end, then only
     * RESTART; a later OOM aborts. */
    EV(sgx535_spm_raster_start(&state, 1), SGX535_SPM_WAIT);
    EV(sgx535_spm_ta_start(&state, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_oom(&state, 1), SGX535_SPM_WAIT);
    CHECK(state.phase == SGX535_SPM_DEFERRED && state.steps == 0);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_RUN_RESTART);
    EV(sgx535_spm_oom(&state, 2), SGX535_SPM_RUN_ABORT);
    EV(sgx535_spm_terminated(&state), SGX535_SPM_RUN_PARTIAL);
    EV(sgx535_spm_render_event(&state, PIXELBE | MEMFREE, 0), SGX535_SPM_RUN_END);
    /* The TA finishes beside a raster: its raster waits for that one's end. */
    EV(sgx535_spm_raster_start(&state, 1), SGX535_SPM_WAIT);
    EV(sgx535_spm_ta_finished(&state), SGX535_SPM_WAIT);
    CHECK(state.phase == SGX535_SPM_READY && state.dalloc == 1);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_RUN_RASTER);
    CHECK(state.dalloc == 0);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_SIGNAL);
    /* After TE steps, the whole-scene render waits for a running raster too. */
    EV(sgx535_spm_ta_start(&state, 1), SGX535_SPM_WAIT);
    EV(sgx535_spm_oom(&state, 1), SGX535_SPM_RUN_TE_STEP);
    EV(sgx535_spm_raster_start(&state, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_ta_finished(&state), SGX535_SPM_WAIT);
    CHECK(state.phase == SGX535_SPM_FINISHING);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_RUN_WHOLE);
    CHECK(state.dalloc == 0 && state.owner[0] == SGX535_SPM_OWNER_RENDER);
    EV(sgx535_spm_render_event(&state, PIXELBE | MEMFREE, 0), SGX535_SPM_RUN_WHOLE_END);
    /* Another job's raster ending while the TA runs changes nothing else. */
    EV(sgx535_spm_raster_start(&state, 0), SGX535_SPM_WAIT);
    EV(sgx535_spm_ta_start(&state, 1), SGX535_SPM_WAIT);
    EV(sgx535_spm_raster_end(&state), SGX535_SPM_WAIT);
    CHECK(state.phase == SGX535_SPM_TA);
    /* TE steps up to the ceiling (N - 4), never past it. */
    for (i = BASE; i < CEILING; i++)
        EV(sgx535_spm_oom(&state, 1), SGX535_SPM_RUN_TE_STEP);
    CHECK(state.zls == CEILING && state.steps == CEILING - BASE);
    MISFIT(sgx535_spm_oom(&state, 1));
}

/* Reach a phase with context 0, from a fresh state. */
static void reach(uint32_t phase)
{
    fresh();
    if (phase == SGX535_SPM_IDLE)
        return;
    if (phase == SGX535_SPM_DEFERRED || phase == SGX535_SPM_READY)
        CHECK(sgx535_spm_raster_start(&state, 1) == SGX535_SPM_WAIT);
    CHECK(sgx535_spm_ta_start(&state, 0) == SGX535_SPM_WAIT);
    if (phase == SGX535_SPM_DEFERRED)
        CHECK(sgx535_spm_oom(&state, 2) == SGX535_SPM_WAIT);
    if (phase == SGX535_SPM_READY)
        CHECK(sgx535_spm_ta_finished(&state) == SGX535_SPM_WAIT);
    if (phase == SGX535_SPM_ABORTING || phase == SGX535_SPM_PARTIAL)
        CHECK(sgx535_spm_oom(&state, 2) == SGX535_SPM_RUN_ABORT);
    if (phase == SGX535_SPM_PARTIAL)
        CHECK(sgx535_spm_terminated(&state) == SGX535_SPM_RUN_PARTIAL);
    if (phase == SGX535_SPM_WHOLE) {
        CHECK(sgx535_spm_oom(&state, 1) == SGX535_SPM_RUN_TE_STEP);
        CHECK(sgx535_spm_ta_finished(&state) == SGX535_SPM_RUN_WHOLE);
    }
    if (phase == SGX535_SPM_RASTER)
        CHECK(sgx535_spm_ta_finished(&state) == SGX535_SPM_RUN_RASTER);
    CHECK(state.phase == phase);
}

static void misfits(void)
{
    struct sgx535_spm s;
    CHECK(sgx535_spm_init(&s, CEILING, CEILING) == SGX535_SPM_HOLD && s.phase == SGX535_SPM_HELD);
    CHECK(sgx535_spm_init(&s, BASE, 0x10000) == SGX535_SPM_HOLD);
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_terminated(&state));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_render_event(&state, PIXELBE, 0));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_raster_end(&state));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_ta_start(&state, 2));
    reach(SGX535_SPM_IDLE);
    MISFIT(sgx535_spm_raster_start(&state, 2));
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_oom(&state, 0)); /* neither requester */
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_oom(&state, 3)); /* both: not designed */
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_terminated(&state));
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_render_event(&state, MEMFREE, 0));
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_ta_start(&state, 1));
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_raster_end(&state));
    reach(SGX535_SPM_TA);
    MISFIT(sgx535_spm_raster_start(&state, 0)); /* the TA's context */
    reach(SGX535_SPM_DEFERRED);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_DEFERRED);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_DEFERRED);
    MISFIT(sgx535_spm_terminated(&state));
    reach(SGX535_SPM_DEFERRED);
    MISFIT(sgx535_spm_raster_start(&state, 1));
    reach(SGX535_SPM_ABORTING);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_ABORTING);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_ABORTING);
    MISFIT(sgx535_spm_render_event(&state, PIXELBE, 0));
    reach(SGX535_SPM_ABORTING);
    MISFIT(sgx535_spm_raster_start(&state, 1));
    reach(SGX535_SPM_ABORTING);
    MISFIT(sgx535_spm_raster_end(&state));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_terminated(&state));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_raster_start(&state, 1));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_raster_end(&state)); /* a render, no raster */
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_render_event(&state, 4, 0));
    reach(SGX535_SPM_PARTIAL);
    MISFIT(sgx535_spm_render_event(&state, 0, 0));
    reach(SGX535_SPM_PARTIAL);
    CHECK(sgx535_spm_render_event(&state, PIXELBE, 0) == SGX535_SPM_WAIT);
    MISFIT(sgx535_spm_render_event(&state, PIXELBE, 0)); /* the same event twice */
    reach(SGX535_SPM_PARTIAL); /* D3: the partial render freed too little */
    MISFIT(sgx535_spm_render_event(&state, PIXELBE | MEMFREE, BASE));
    reach(SGX535_SPM_WHOLE);
    MISFIT(sgx535_spm_oom(&state, 1));
    reach(SGX535_SPM_WHOLE);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_WHOLE);
    MISFIT(sgx535_spm_terminated(&state));
    reach(SGX535_SPM_READY);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_READY);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_RASTER);
    MISFIT(sgx535_spm_oom(&state, 2));
    reach(SGX535_SPM_RASTER);
    MISFIT(sgx535_spm_ta_finished(&state));
    reach(SGX535_SPM_RASTER);
    MISFIT(sgx535_spm_raster_start(&state, 1)); /* one raster */
    reach(SGX535_SPM_RASTER);
    MISFIT(sgx535_spm_render_event(&state, PIXELBE, 0));
    reach(SGX535_SPM_RASTER);
    MISFIT(sgx535_spm_ta_start(&state, 1));
    /* An owner on !c when the abort completes is an engine bug. */
    reach(SGX535_SPM_ABORTING);
    state.owner[1] = SGX535_SPM_OWNER_TA;
    MISFIT(sgx535_spm_terminated(&state));
}

/* An event that usually fits the phase (index into the switch below). */
static uint32_t likely(uint32_t phase, uint32_t r)
{
    switch (phase) {
    case SGX535_SPM_IDLE:
        return r % 2u; /* a TA or a raster starts */
    case SGX535_SPM_TA:
        return 1u + r % 4u; /* a raster, OOMs */
    case SGX535_SPM_ABORTING:
        return 5; /* TA_TERMINATE */
    case SGX535_SPM_PARTIAL:
    case SGX535_SPM_WHOLE:
        return 6; /* the render's events */
    default:
        return r % 3u ? 2u : 7u; /* a raster's end, the TA's */
    }
}

/* Random events with random arguments, most of them fitting: the invariants
 * after each, HOLD sticky, the fence at most once a job, every outcome seen. */
static void random_orders(void)
{
    uint32_t seed = 0x2545f491u, round, step, fences = 0, jobs = 0, holds = 0, runs[11] = {0};
    for (round = 0; round < 20000; round++) {
        fresh();
        fences = 0;
        for (step = 0; step < 60 && state.phase != SGX535_SPM_HELD; step++) {
            struct sgx535_spm prev = state;
            enum sgx535_spm_run run;
            uint32_t pick, arg;
            seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
            arg = (seed >> 8) % 4u;
            pick = seed % 5u ? likely(state.phase, seed >> 4) : (seed >> 12) % 8u;
            if (state.phase == SGX535_SPM_TA && seed % 7u == 0)
                pick = 7; /* the TA finishes */
            switch (pick) {
            case 0:
                run = sgx535_spm_ta_start(&state, arg % 3u);
                break;
            case 1:
                run = sgx535_spm_raster_start(&state, arg % 3u);
                break;
            case 2:
                run = sgx535_spm_raster_end(&state);
                break;
            case 3:
                run = sgx535_spm_oom(&state, arg);
                break;
            case 4:
                run = sgx535_spm_oom(&state, arg & 1u ? 1u : 2u);
                break;
            case 5:
                run = sgx535_spm_terminated(&state);
                break;
            case 6:
                run = sgx535_spm_render_event(&state, arg ? arg : 1u, (seed >> 16) % 5u ? 0 : BASE);
                break;
            default:
                run = sgx535_spm_ta_finished(&state);
                break;
            }
            runs[run]++;
            if (prev.phase == SGX535_SPM_IDLE && pick == 0 && run == SGX535_SPM_WAIT)
                fences = 0, jobs++;
            if (run == SGX535_SPM_SIGNAL || run == SGX535_SPM_RUN_WHOLE_END)
                CHECK(++fences == 1);
            if (run == SGX535_SPM_HOLD) {
                holds++;
                held();
            }
            invariants(&state, &prev);
        }
    }
    CHECK(jobs > 5000 && holds > 10000);
    for (step = 0; step < 11; step++)
        CHECK(runs[step] > 50); /* every outcome, often */
}

/* The page model: a register file and the DPM's pages. The TA allocates its
 * scene into ALLOC's context and is out of memory when the pool's used pages
 * reach 0x624; STORE, CLEAR and LOAD move a context's pages through the state
 * table slot; a render frees DALLOC's context (0x63c = 3, global). */
struct gpu {
    uint32_t r[0x400];
    uint32_t pool, pages[2], table, want, planes;
    int ta, terminate, render;
    uint32_t renders, partial_renders, reloads;
    uint32_t zlsctl[16], bgobjtag[16];
};
#define REG(g, o) ((g)->r[(o) >> 2])

static void write_reg(struct gpu *g, uint32_t o, uint32_t v)
{
    uint32_t ls = REG(g, 0x630) & 1u, d = REG(g, 0x630) >> 1 & 1u;
    REG(g, o) = v;
    switch (o) {
    case 0x134:
        REG(g, 0x12c) &= ~v;
        break;
    case 0x140:
        REG(g, 0x138) &= ~v;
        break;
    case 0x24c:
        REG(g, 0x12c) |= 0x00100000u;
        break; /* the OTPM invalidated */
    case 0x698:
        REG(g, 0x12c) |= 0x200u;
        break; /* the control state cleared */
    case 0x804:
        REG(g, 0x12c) |= 0x04000000u;
        break;
    case 0xad4:
        REG(g, 0x138) |= 0x44u;
        break;
    case 0xae0:
        REG(g, 0x138) |= 1u;
        break;
    case 0x694:
        if (v == 1) {
            g->table = g->pages[ls];
            REG(g, 0x12c) |= 0x10u;
        } else if (v == 2) {
            g->pages[ls] = 0; /* pages only the table slot still names */
            REG(g, 0x12c) |= 0x40u;
        } else {
            CHECK(v == 4 && g->pages[ls] == 0);
            g->pages[ls] = g->table;
            REG(g, 0x12c) |= 0x20u;
        }
        break;
    case 0x69c:
        CHECK(v == 1 || v == 4);
        g->ta = v == 1;
        g->terminate = v == 4;
        break;
    case 0x428:
        CHECK(REG(g, 0x63c) == 3 && g->renders < 16);
        g->zlsctl[g->renders] = REG(g, 0x480);
        g->bgobjtag[g->renders] = REG(g, 0x4c4);
        g->partial_renders += REG(g, 0x658) == 1;
        g->renders++;
        g->pool += g->pages[d];
        g->pages[d] = 0;
        g->render = 1;
        break;
    default:
        break;
    }
}

static void exec(struct gpu *g, const struct sgx535_spm_program *p)
{
    uint32_t i;
    for (i = 0; i < p->count; i++) {
        const struct sgx535_engine121_action *a = &p->actions[i];
        switch (a->kind) {
        case SGX535_ENGINE121_WRITE:
            write_reg(g, a->offset, a->value);
            break;
        case SGX535_ENGINE121_WAIT_SET:
            CHECK((REG(g, a->offset) & a->mask) == a->value);
            break;
        case SGX535_ENGINE121_WAIT_CLEAR:
            CHECK(!(REG(g, a->offset) & a->mask));
            break;
        case SGX535_ENGINE121_READBACK:
            break;
        case SGX535_SPM_SET_BITS:
            write_reg(g, a->offset, REG(g, a->offset) | a->value);
            break;
        case SGX535_SPM_CLEAR_BITS:
            write_reg(g, a->offset, REG(g, a->offset) & ~a->value);
            break;
        case SGX535_SPM_PAUSE:
            REG(g, 0xa74) |= 1u;
            break;
        case SGX535_SPM_RELOAD:
            g->reloads++;
            break;
        default:
            CHECK(0);
        }
    }
}

#define SCENE UINT32_C(0x00100000)
static const struct sgx535_engine121_scene_binding scene = {1, SCENE, 0x5000, 300, 300, 0, 0, 0};
static const struct sgx535_engine121_action client[] = {
    {SGX535_ENGINE121_WRITE, 0x480, 0x004513d0, 0},
    {SGX535_ENGINE121_WRITE, 0x484, 0x02000000, 0},
    {SGX535_ENGINE121_WRITE, 0x48c, 0x02000000, 0},
    {SGX535_ENGINE121_WRITE, 0x4c4, 0x08012340, 0},
};
#define TE_PSG UINT32_C(0x00000003)
#define CONTINUATION UINT32_C(0x08045670)

struct job {
    uint32_t context, pages, planes;
    const char *denials; /* the requester of each out of memory, in turn: T(E) or M(TE) */
};

/* B (with B0 for a partial render), then C or E3. */
static void handoff_and_render(struct gpu *g, const struct sgx535_spm_render *r, uint32_t id,
                               int partial)
{
    static struct sgx535_spm_program p;
    uint32_t c = state.context;
    CHECK(sgx535_spm_handoff_build(id, c, SCENE + 0x2b50u, partial, &p) == 0);
    exec(g, &p);
    CHECK(REG(g, 0x630) == SGX535_SPM_HANDOFF_DPM_ID(id, c) && !(REG(g, 0xc74) & 0x200u));
    CHECK((REG(g, 0x630) >> 1 & 1u) == state.dalloc);
    CHECK(sgx535_spm_render_build(r, &p) == 0);
    exec(g, &p);
}

/* What the engine does with each run: build the programs and execute them. */
static void apply(struct gpu *g, enum sgx535_spm_run run, uint32_t *fences)
{
    static struct sgx535_spm_program p;
    struct sgx535_spm_render r = {0,
                                  client,
                                  4,
                                  &scene,
                                  0,
                                  state.partials,
                                  CONTINUATION,
                                  {0x00f00000, 0x00f40000, 0x00f80000}};
    size_t n;
    uint32_t c = state.context, id = REG(g, 0x630);
    r.planes = g->planes;
    switch (run) {
    case SGX535_SPM_WAIT:
        break;
    case SGX535_SPM_RUN_TE_STEP:
        CHECK(sgx535_spm_te_step_build(state.zls - 1u, state.zls_ceiling, &p) == 0);
        exec(g, &p);
        CHECK((REG(g, 0x218) & 0x00400000u) && (REG(g, 0xa74) & 1u));
        break;
    case SGX535_SPM_RUN_ABORT:
        CHECK(sgx535_spm_abort_build(&p) == 0);
        exec(g, &p);
        break;
    case SGX535_SPM_RUN_RESTART:
        write_reg(g, 0x69c, 1);
        break;
    case SGX535_SPM_RUN_WHOLE:
        CHECK(sgx535_spm_ta_end_build(state.undo, state.zls_base, &p) == 0);
        exec(g, &p);
        r.kind = SGX535_SPM_RENDER_WHOLE;
        handoff_and_render(g, &r, id, 0);
        break;
    case SGX535_SPM_RUN_PARTIAL:
        r.kind = SGX535_SPM_RENDER_PARTIAL;
        handoff_and_render(g, &r, id, 1);
        break;
    case SGX535_SPM_RUN_END:
        CHECK(sgx535_spm_end_build(SGX535_SPM_END_RESTART, state.undo, state.zls_base, TE_PSG,
                                   &p) == 0);
        exec(g, &p);
        CHECK(!(REG(g, 0xa74) & 1u) && REG(g, 0x658) == 0 && REG(g, 0x65c) == 0);
        CHECK(!state.undo || REG(g, 0x218) == TE_PSG);
        break;
    case SGX535_SPM_RUN_WHOLE_END:
        CHECK(sgx535_spm_end_build(SGX535_SPM_END_WHOLE, 0, state.zls_base, TE_PSG, &p) == 0);
        exec(g, &p);
        ++*fences;
        break;
    case SGX535_SPM_RUN_RASTER:
        /* The engine points DALLOC at c (KE:512-513), then the job's raster. */
        write_reg(g, 0x630, (id & ~2u) | c << 1);
        if (state.partials) {
            r.kind = SGX535_SPM_RENDER_FINAL;
            CHECK(sgx535_spm_render_build(&r, &p) == 0);
        } else {
            memcpy(p.actions, client, sizeof(client));
            CHECK(sgx535_engine121_raster_build(&scene, p.actions + 4, SGX535_SPM_ACTIONS - 4u,
                                                &n) == 0);
            p.count = 4u + (uint32_t)n;
        }
        exec(g, &p);
        break;
    case SGX535_SPM_SIGNAL:
        ++*fences;
        break;
    default:
        CHECK(0);
    }
    /* The model's ZLS is the register's. */
    CHECK(REG(g, 0x624) == state.zls);
}

/* One job through the page model; returns its fences. */
static uint32_t run_job(struct gpu *g, const struct job *j)
{
    uint32_t fences = 0, oom = 0, alloc;
    g->planes = j->planes;
    REG(g, 0x624) = state.zls_base; /* each TA starts at the base (KE:602-607) */
    write_reg(g, 0x630, (REG(g, 0x630) & 2u) | j->context << 2 | j->context);
    CHECK(sgx535_spm_ta_start(&state, j->context) == SGX535_SPM_WAIT);
    g->want = j->pages;
    g->ta = 1;
    while (state.phase != SGX535_SPM_IDLE) {
        struct sgx535_spm prev = state;
        enum sgx535_spm_run run = SGX535_SPM_WAIT;
        CHECK(state.phase != SGX535_SPM_HELD);
        if (g->terminate) {
            g->terminate = 0;
            run = sgx535_spm_terminated(&state);
        } else if (g->render && state.phase == SGX535_SPM_RASTER) {
            g->render = 0;
            run = sgx535_spm_raster_end(&state);
        } else if (g->render) {
            CHECK(sgx535_spm_render_event(&state, PIXELBE, POOL - g->pool) == SGX535_SPM_WAIT);
            g->render = 0;
            run = sgx535_spm_render_event(&state, MEMFREE, POOL - g->pool);
        } else {
            CHECK(g->ta);
            alloc = REG(g, 0x630) >> 2 & 1u;
            while (g->want && POOL - g->pool < REG(g, 0x624)) {
                g->pool--, g->pages[alloc]++, g->want--;
            }
            g->ta = 0;
            if (g->want)
                run =
                    sgx535_spm_oom(&state, j->denials[oom++ % strlen(j->denials)] == 'T' ? 1u : 2u);
            else
                run = sgx535_spm_ta_finished(&state);
        }
        invariants(&state, &prev);
        apply(g, run, &fences);
    }
    CHECK(fences == 1);
    CHECK(!(REG(g, 0xa74) & 0xfu) && REG(g, 0x624) == state.zls_base);
    return fences;
}

static void page_model(void)
{
    static struct gpu g;
    const uint32_t ds = SGX535_SPM_DEPTH | SGX535_SPM_STENCIL;
    struct job four = {0, 4u * POOL, ds, "M"};
    struct job te_then_mte = {1, 4u * POOL, SGX535_SPM_DEPTH, "TM"};
    struct job te_finish = {0, BASE + 1u, 0, "T"};
    struct job partial_then_te = {1, 2u * BASE + 1u, ds, "MT"};
    fresh();
    memset(&g, 0, sizeof(g));
    g.pool = POOL;
    REG(&g, 0xa74) = UINT32_C(0x05188200); /* bootstrap's DMS_CTRL */
    REG(&g, 0xc74) = 0x200u;
    /* Four pools: 32,768 pages, the TA out of memory at 7,715 used: four partial
     * renders, then the job's own raster, and no page left. */
    run_job(&g, &four);
    CHECK(state.partials == 4 && g.renders == 5 && g.partial_renders == 4);
    CHECK(g.pool == POOL && !g.pages[0] && !g.pages[1]);
    /* ZLSCTL: the job's bits with ZLOADEN, ZSTOREEN, FORCEZSTORE, STOREMASK and
     * the stencil pair; LOADMASK from the second; the final raster loads. */
    CHECK(g.zlsctl[0] == 0x004513d6u && g.zlsctl[1] == 0x006513d6u && g.zlsctl[3] == 0x006513d6u);
    CHECK(g.zlsctl[4] == 0x006513d6u);
    CHECK(g.bgobjtag[0] == 0x08012340u && g.bgobjtag[1] == CONTINUATION &&
          g.bgobjtag[4] == CONTINUATION);
    /* A TE step before each abort: 7,716 pages a segment, still four. */
    g.renders = g.partial_renders = 0;
    run_job(&g, &te_then_mte);
    CHECK(state.partials == 4 && g.renders == 5 && g.partial_renders == 4 && g.pool == POOL);
    CHECK(g.zlsctl[0] == 0x004513d4u && g.zlsctl[4] == 0x006513d4u);
    /* A TE step and then the TA finishes: the whole-scene render (E), no
     * raster of the job's own; no planes: the dummy pair. */
    g.renders = g.partial_renders = 0;
    run_job(&g, &te_finish);
    CHECK(state.partials == 0 && g.renders == 1 && g.partial_renders == 0 && g.pool == POOL);
    CHECK(g.zlsctl[0] == 0x82u && REG(&g, 0xcb0) == 0x00f00000u && REG(&g, 0x48c) == 0x80040000u);
    /* A partial render, then a TE step and the end of the TA: the whole-scene
     * render loads what the partial render stored. */
    g.renders = g.partial_renders = 0;
    run_job(&g, &partial_then_te);
    CHECK(state.partials == 1 && g.renders == 2 && g.partial_renders == 1 && g.pool == POOL);
    CHECK(g.zlsctl[1] == 0x006513d6u && g.bgobjtag[1] == CONTINUATION);
    CHECK(g.reloads > 0);
}

int main(void)
{
    orders();
    misfits();
    random_orders();
    page_model();
    printf("sgx535 spm state: %lu checks\n", checks);
    return 0;
}
