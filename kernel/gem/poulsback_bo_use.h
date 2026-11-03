/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* Who holds a BO's mapping in one address space (docs/kernel/memory.md): the
 * file's handles to the BO and the jobs that resolved it through one. The
 * mapping goes with the last of both, so a job that resolved the BO keeps its
 * VA mapped until the job is released, even when the last handle closes
 * first. Pure; the caller holds the BO's lock. */
#ifndef POULSBACK_BO_USE_H
#define POULSBACK_BO_USE_H

#include "sgx535_types.h"

struct poulsback_bo_use {
	sgx535_u32 handles, jobs;
	sgx535_u32 kept; /* kept for good (HOLD): no holder drops it */
};

/* A handle closed / a job was released: whether that was the last holder of
 * a mapping not kept, which is then unmapped or kept. */
sgx535_u32 poulsback_bo_use_close(struct poulsback_bo_use *use);
sgx535_u32 poulsback_bo_use_release(struct poulsback_bo_use *use);
#endif
