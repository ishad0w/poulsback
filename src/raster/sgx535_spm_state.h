// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_SPM_STATE_H
#define SGX535_SPM_STATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* SPM state (docs/native-3d.md#partial-render-construction): program selection after each
 * event of the job whose TA holds DPM context c, and who holds the DPM's two
 * contexts. Pure functions, no device access or locking: an executor must serialize
 * transitions under its IRQ lock, as poulsback_overlap does. Invariants:
 * - one owner per context: the TA job's TA on c; a render of its scene on !c
 *   (partial or whole); a job's own raster on its context;
 * - DALLOC names the context of the render or raster that runs, and moves only
 *   when none runs; at most one runs;
 * - the ZLS threshold is zls_base after each partial render and at the job's end;
 * - the job's fence once: after its own raster, or after the whole-scene render;
 * - an event that does not fit returns HOLD and leaves the state HELD, which every
 *   later event returns too: the engine wedges, releases nothing and retries
 *   nothing. */

enum sgx535_spm_phase {
    SGX535_SPM_IDLE,      /* no TA job */
    SGX535_SPM_TA,        /* its TA runs, or restarts */
    SGX535_SPM_DEFERRED,  /* out of memory beside a raster: RESTART when it ends */
    SGX535_SPM_ABORTING,  /* ABORTALL written: TA_TERMINATE next */
    SGX535_SPM_PARTIAL,   /* a partial render on !c */
    SGX535_SPM_FINISHING, /* TA finished after TE steps beside a raster */
    SGX535_SPM_WHOLE,     /* the whole-scene render on !c after TE steps */
    SGX535_SPM_READY,     /* TA finished; its raster waits for the running one */
    SGX535_SPM_RASTER,    /* the job's own raster on c */
    SGX535_SPM_HELD
};

enum sgx535_spm_owner {
    SGX535_SPM_FREE,
    SGX535_SPM_OWNER_TA,
    SGX535_SPM_OWNER_RENDER, /* a partial or whole-scene render of the TA's scene */
    SGX535_SPM_OWNER_RASTER  /* a job's own raster */
};

/* What the engine runs now (the sgx535_spm.h builders), with no raster running
 * unless it says otherwise. */
enum sgx535_spm_run {
    SGX535_SPM_WAIT,          /* nothing until the next event */
    SGX535_SPM_RUN_TE_STEP,   /* T: sgx535_spm_te_step_build(zls - 1, zls_ceiling) */
    SGX535_SPM_RUN_ABORT,     /* A */
    SGX535_SPM_RUN_RESTART,   /* 0x69c = 1 only */
    SGX535_SPM_RUN_PARTIAL,   /* B with B0, then C (done = partials) */
    SGX535_SPM_RUN_END,       /* D: sgx535_spm_end_build(RESTART, undo, ...) */
    SGX535_SPM_RUN_WHOLE,     /* E1 (undo steps), B without B0, then E3 (done = partials) */
    SGX535_SPM_RUN_WHOLE_END, /* E4 and E5, then the job's fence */
    SGX535_SPM_RUN_RASTER,    /* the job's own raster on c; F after partial renders */
    SGX535_SPM_SIGNAL,        /* the job's fence */
    SGX535_SPM_HOLD
};

/* Event bits of an SPM render. */
#define SGX535_SPM_PIXELBE_END 1u
#define SGX535_SPM_MEM_FREE 2u

struct sgx535_spm {
    uint8_t phase;    /* enum sgx535_spm_phase */
    uint8_t context;  /* c */
    uint8_t owner[2]; /* enum sgx535_spm_owner per DPM context */
    uint8_t dalloc;   /* the context DALLOC names */
    uint8_t seen;     /* the running SPM render's event bits */
    uint32_t steps;   /* TE steps since the last render of the scene */
    uint32_t undo;    /* TE steps the program just returned takes back */
    uint32_t partials;
    uint32_t zls, zls_base, zls_ceiling;
};

/* zls_base is 0x624 at every TA start (N - 477); TE steps stop at zls_ceiling
 * (N - 4). */
enum sgx535_spm_run sgx535_spm_init(struct sgx535_spm *, uint32_t zls_base, uint32_t zls_ceiling);
/* Events. */
enum sgx535_spm_run sgx535_spm_ta_start(struct sgx535_spm *, uint32_t context);
/* Another job's raster starts on its context, beside this TA. */
enum sgx535_spm_run sgx535_spm_raster_start(struct sgx535_spm *, uint32_t context);
/* A job's own raster ended (both its events). */
enum sgx535_spm_run sgx535_spm_raster_end(struct sgx535_spm *);
/* requesting: 0x734 after the settle; 1 TE, 2 MTE. */
enum sgx535_spm_run sgx535_spm_oom(struct sgx535_spm *, uint32_t requesting);
enum sgx535_spm_run sgx535_spm_terminated(struct sgx535_spm *);
/* An SPM render's event bits; with the second, ta_pages (0x724 >> 16) must be
 * below zls_base after a partial render (D3, progress). */
enum sgx535_spm_run sgx535_spm_render_event(struct sgx535_spm *, uint32_t bits, uint32_t ta_pages);
enum sgx535_spm_run sgx535_spm_ta_finished(struct sgx535_spm *);

#endif
