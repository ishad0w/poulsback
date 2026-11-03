/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* SUBMIT_2D's planner (docs/kernel/execution.md, "2D jobs"): a job's surfaces
 * and operations, checked once and encoded by sgx535_2d_raster into the words
 * the executor feeds to the 2D core. Pure functions; no device access. */
#ifndef POULSBACK_TWOD_H
#define POULSBACK_TWOD_H

#include "poulsback_exec_uapi.h"
#include "sgx535_2d_encoder.h"
#include "sgx535_types.h"

/* Words for n operations: every one a copy split in two (HW-2D-006), then
 * the scratch fill. */
#define POULSBACK_TWOD_WORDS(n)                                                                    \
	((n) * 2 * SGX535_2D_RASTER_COPY_WORDS + SGX535_2D_RASTER_FILL_WORDS)
#define POULSBACK_TWOD_MAX_WORDS POULSBACK_TWOD_WORDS(POULSBACK_2D_MAX_OPS)
/* An operation whose destination starts off a 64-byte boundary runs as a head
 * and an aligned body when it keeps at least this much of each row and this
 * many rows after clipping (HW-2D-006, docs/hardware.md). */
#define POULSBACK_TWOD_SPLIT_BYTES 192
#define POULSBACK_TWOD_SPLIT_ROWS 8
/* BLIT_STATUS's completed count per encoded operation (FENCE ... FLUSH). */
#define POULSBACK_TWOD_COUNT_PER_OP 2U

enum poulsback_twod_status {
	POULSBACK_TWOD_OK = 0,
	POULSBACK_TWOD_INVALID,	    /* a malformed request */
	POULSBACK_TWOD_UNSUPPORTED  /* valid, but not encodable (yet) */
};

/* A BO of the job's list as the kernel resolved it: VA in the file's space. */
struct poulsback_twod_bo {
	sgx535_u64 va, size;
};

struct poulsback_twod_plan {
	sgx535_u32 count;    /* words */
	sgx535_u32 expected; /* BLIT_STATUS count delta at completion */
	sgx535_u32 writes;   /* BO indices an operation writes */
};

/* Plans one job into words[0, capacity); capacity holds
 * POULSBACK_TWOD_WORDS(op_count). An operation encodes as zero, one or two
 * commands (HW-2D-006). A job that encodes anything ends with a 1x1 fill of
 * the file's 2D scratch page (POULSBACK_VM_2D_SCRATCH_VA, HW-2D-004). On
 * failure the plan is unset and words hold no defined content. */
enum poulsback_twod_status
poulsback_twod_plan(const struct poulsback_twod_bo *bos, sgx535_u32 bo_count,
		    const struct poulsback_2d_surface *surfaces, sgx535_u32 surface_count,
		    const struct poulsback_2d_op *ops, sgx535_u32 op_count, sgx535_u32 *words,
		    sgx535_u32 capacity, struct poulsback_twod_plan *plan);
#endif
