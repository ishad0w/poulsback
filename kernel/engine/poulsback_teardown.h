/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* The engine's teardown decisions and register words
 * (docs/kernel/execution.md). Pure functions, no device access or locking.
 *
 * - Quiesce: the schedulers parked, the engine drained (no job in a slot),
 *   then a read-only idle check of the event, 2D and BIF fault registers.
 *   Refused while the engine is lost or holds a job (HOLD).
 * - Power-down: EMGD's deinit, eight registers written 0, the host enables before the clock
 *   gating.
 * - Free: only after quiesce, with the engine ready, no job in a slot or held
 *   and no hardware fence alive (each pins the module until its release). */
#ifndef POULSBACK_TEARDOWN_H
#define POULSBACK_TEARDOWN_H

#include "sgx535_types.h"

struct poulsback_teardown_write {
	sgx535_u32 offset, value;
};

#define POULSBACK_TEARDOWN_POWER_DOWN_WRITES 8U
extern const struct poulsback_teardown_write
	poulsback_teardown_power_down[POULSBACK_TEARDOWN_POWER_DOWN_WRITES];

/* Why the engine may not quiesce or be freed now; 0 when it may. */
enum poulsback_teardown_refusal {
	POULSBACK_TEARDOWN_OK = 0,
	POULSBACK_TEARDOWN_LOST,	/* wedged or a failed bring-up: HOLD */
	POULSBACK_TEARDOWN_HELD,	/* a job the engine stopped with */
	POULSBACK_TEARDOWN_BUSY,	/* a job still in a slot */
	POULSBACK_TEARDOWN_EVENT,	/* an unacknowledged SGX event */
	POULSBACK_TEARDOWN_TWOD,	/* the 2D core busy (BLIT_STATUS bit 24) */
	POULSBACK_TEARDOWN_FAULT,	/* a BIF fault pending (BIF_INT_STAT) */
	POULSBACK_TEARDOWN_FENCES,	/* an engine hardware fence alive */
	POULSBACK_TEARDOWN_UNQUIESCED	/* release before quiesce */
};

/* The engine state for quiesce and release, read under its locks. */
struct poulsback_teardown_engine {
	int ready, quiesced;
	sgx535_u32 slots, held, fences;
};

/* The idle registers quiesce reads; EVENT_STATUS bit 31 (the master bit) reads 1
 * at idle. */
struct poulsback_teardown_idle {
	sgx535_u32 event1, event2, blit_status, bif_int_stat;
};

/* Before the idle reads: refused when lost, holding or not drained. */
enum poulsback_teardown_refusal poulsback_teardown_drained(const struct poulsback_teardown_engine *);
/* Quiesce's idle reads. */
enum poulsback_teardown_refusal poulsback_teardown_idle(const struct poulsback_teardown_idle *);
/* Release: after quiesce, still ready, no job, no hold, no live fence. */
enum poulsback_teardown_refusal poulsback_teardown_may_free(const struct poulsback_teardown_engine *);
#endif
