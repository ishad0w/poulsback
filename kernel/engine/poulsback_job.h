/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* One engine job, shared by SUBMIT, SUBMIT_2D and the executor: a
 * TA -> 3D job, or a 2D job of 2D core words (`twod`, no scene). */
#ifndef POULSBACK_JOB_H
#define POULSBACK_JOB_H

#include "poulsback_regtable.h"
#include "poulsback_scene.h"
#include "sgx535_engine121.h"

#include <drm/gpu_scheduler.h>
#include <drm/drm_mm.h>
#include <linux/kref.h>

/* A 3D job's programs: the client's registers of the phase (each table slot
 * at most once), then the kernel's TA or raster sequence. */
#define POULSBACK_JOB_TA_ACTIONS (POULSBACK_REG_TA_SLOTS + SGX535_ENGINE121_TA_ACTIONS)
#define POULSBACK_JOB_RASTER_ACTIONS (POULSBACK_REG_RASTER_SLOTS + SGX535_ENGINE121_RASTER_ACTIONS)

struct poulsback_job {
	struct drm_sched_job base;
	struct poulsback_vm *vm;
	struct poulsback_scene *scene;
	struct drm_gem_object **bos;
	u32 bo_count;
	struct sgx535_engine121_action *ta, *raster; /* in `programs` */
	u32 ta_count, raster_count;
	u32 clear_start, clear_count; /* scene root pages cleared before the TA */
	u32 region_array; /* the scene's region-header array its TA and raster use */
	u32 dpm; /* the DPM context its TA allocates in and its raster frees */
	struct dma_fence *done;
	u32 events;
	bool oom_resumed, held, twod;
	/* 2D: the words, how many the executor wrote, the BLIT_STATUS count at
	 * the start and its expected delta at completion. */
	u32 *words;
	u32 count, fed, start, expected;
	/* 3D jobs only, allocated with the job: the TA program, then the raster
	 * one. */
	struct sgx535_engine121_action programs[];
};

/* Drops a job's buffers (bo_count counts only resolved ones), its words,
 * scene, address space and done fence, and frees it. */
void poulsback_job_release(struct poulsback_job *job);
/* A context's 3D entity and scene buffer (allocated on first use, for the
 * largest scene); takes a scene reference. A NULL scene asks for its 2D
 * entity. */
/* FILE's context HANDLE for one submission, held until poulsback_exec_ctx_put
 * (a destroyed context lives until its last holder): its 3D entity with its
 * scene (also held) when SCENE is given, else its 2D entity. A job is armed
 * and pushed with poulsback_exec_ctx_lock held, so jobs reach the entity in
 * the order of their fences. */
struct poulsback_exec_ctx;
int poulsback_exec_context(struct drm_file *file, u32 handle, struct poulsback_exec_ctx **held,
			   struct drm_sched_entity **entity, struct poulsback_scene **scene);
void poulsback_exec_ctx_lock(struct poulsback_exec_ctx *ctx);
void poulsback_exec_ctx_unlock(struct poulsback_exec_ctx *ctx);
void poulsback_exec_ctx_put(struct poulsback_exec_ctx *ctx);
#endif
