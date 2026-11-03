/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* When the engine's 3D and 2D jobs may run at once, and how BANK0 routes the
 * requesters meanwhile (docs/kernel/execution.md). Pure functions, no
 * device access or locking: the engine calls them under its IRQ lock.
 *
 * - A 3D job is a TA pass, then a raster pass. Its TA allocates the scene in
 *   one of the DPM's two contexts and its raster frees it (HW-DPM-002): a TA may run while another job's raster runs, in
 *   the other context. A job whose TA finished while a raster runs is ready
 *   and holds its context until its own raster ends; so at most one TA, one
 *   ready job and one raster, and at most two of them.
 * - A TA joins jobs that hold contexts only when the caller says it may
 *   (JOINS: the same directory and another region-header array);
 *   otherwise it waits until no job holds a context.
 * - A 2D job owns the TA requester's directory index (the 2D core writes
 *   through BANK0.TA, HW-ADDR-004), so it never runs during a TA pass; it may
 *   run during a raster pass, which uses the TA requester for nothing
 *   (HW-ADDR-006). When both could start, the TA goes first (it is short and
 *   keeps the raster fed).
 * - BANK0 (EDM 3:0, TA 7:4, HOST 11:8, 3D 15:12, 2D 19:16): slot 0 holds the 3D
 *   jobs' directory (BASE0), slot 1 the 2D job's (DIR_LIST_BASE1), slot 15 the
 *   kernel's. The 3D and TA requesters are at slot 0 while any 3D job holds a
 *   context: the TA's writes can still arrive after its pass ends
 *   (HW-ADDR-007). A 2D job takes TA and 2D at slot 1; anything unused stays
 *   at 15.
 * - The 3D scheduler has three credits: a raster, a TA or ready job, and the
 *   next job waiting in the engine, so that each start happens in the event
 *   that allows it. The 2D scheduler has one. */
#ifndef POULSBACK_OVERLAP_H
#define POULSBACK_OVERLAP_H

#include "sgx535_types.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif

#define POULSBACK_OVERLAP_CREDITS_3D 3

struct poulsback_overlap {
	bool ta;	  /* a 3D job's TA pass runs */
	bool ready;	  /* a 3D job's TA finished; its raster waits */
	bool raster;	  /* a 3D job's raster pass runs */
	bool run2d;	  /* a 2D job runs */
	bool wait2d;	  /* a 2D job waits to start */
	sgx535_u8 wait3d; /* 3D jobs waiting to start their TA, in order */
};

/* What a caller does after an event: start the ready job's raster, then the
 * first waiting 3D job's TA or the waiting 2D job (never both), then write
 * BANK0. */
struct poulsback_overlap_start {
	bool raster, ta, twod;
	sgx535_u32 bank0;
};

#define POULSBACK_OVERLAP_BANK0_IDLE 0x000fffffU

enum poulsback_overlap_event {
	POULSBACK_OVERLAP_ARRIVE3D,    /* a 3D job joins the engine */
	POULSBACK_OVERLAP_ARRIVE2D,    /* a 2D job joins the engine */
	POULSBACK_OVERLAP_TA_FINISHED, /* the running TA pass ended */
	POULSBACK_OVERLAP_DONE3D,      /* the running raster pass ended */
	POULSBACK_OVERLAP_DONE2D       /* the running 2D job ended */
};

sgx535_u32 poulsback_overlap_bank0(const struct poulsback_overlap *);
/* EVENT happened. JOINS: the first waiting 3D job may start its TA beside the
 * jobs that hold DPM contexts after the event. False, changing nothing, when
 * the event does not fit the state (an engine bug: the caller wedges). */
bool poulsback_overlap_event(struct poulsback_overlap *, enum poulsback_overlap_event event,
			     bool joins, struct poulsback_overlap_start *);
#endif
