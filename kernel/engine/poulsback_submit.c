// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_bo.h"
#include "poulsback_engine.h"
#include "poulsback_errsite.h"
#include "poulsback_exec_uapi.h"
#include "poulsback_job.h"
#include "poulsback_regtable.h"
#include "poulsback_file.h"
#include "poulsback_twod.h"
#include "poulsback_vm.h"

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>
#include <linux/dma-resv.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ww_mutex.h>

/* SUBMIT (docs/kernel/execution.md): the request is validated
 * once here; the executor runs what this builds without re-checking it. */

static int submit_regs(u32 phase, const struct poulsback_submit *args, u64 user, u32 count,
		       const u64 *va, const u64 *size, struct sgx535_engine121_action *out,
		       u32 *written)
{
	struct poulsback_submit_reg regs[POULSBACK_SUBMIT_MAX_REGS];
	u32 i, seen = 0;
	BUILD_BUG_ON(POULSBACK_REG_TA_SLOTS > POULSBACK_SUBMIT_MAX_REGS ||
		     POULSBACK_REG_RASTER_SLOTS > POULSBACK_SUBMIT_MAX_REGS);
	/* More registers than slots would set one twice. */
	if (count >
	    (phase == POULSBACK_REG_TA ? POULSBACK_REG_TA_SLOTS : POULSBACK_REG_RASTER_SLOTS))
		return POULSBACK_REFUSE(-EINVAL);
	if (copy_from_user(regs, u64_to_user_ptr(user), count * sizeof(regs[0])))
		return -EFAULT;
	for (i = 0; i < count; i++) {
		struct poulsback_reg_request r = {regs[i].offset, regs[i].value, 0, 0};
		u32 value;
		if (regs[i].bo) {
			u32 index = regs[i].bo - 1;
			if (index >= args->bo_count || regs[i].bo_offset >= size[index])
				return POULSBACK_REFUSE(-EINVAL);
			r.has_va = 1;
			r.va = va[index] + regs[i].bo_offset;
		} else if (regs[i].bo_offset) {
			return POULSBACK_REFUSE(-EINVAL);
		}
		if (poulsback_reg_resolve(phase, &r, &seen, &value))
			return POULSBACK_REFUSE(-EINVAL);
		out[i] = (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, regs[i].offset,
							  value, 0};
	}
	/* The whole table, or an earlier job's stream base and raster state
	 * would run with this job. */
	if (!poulsback_reg_complete(phase, seen))
		return POULSBACK_REFUSE(-EINVAL);
	*written = count;
	return 0;
}

/* Both ioctls' start: the engine (started on first use), a job of BYTES with
 * the file's address space, and CONTEXT held with its entity, with its scene
 * for 3D. Returns the job, or an ERR_PTR with nothing held. */
static struct poulsback_job *job_begin(struct drm_device *dev, struct drm_file *file, u32 context,
				       size_t bytes, bool scene, struct poulsback_engine **engine,
				       struct poulsback_exec_ctx **ctx,
				       struct drm_sched_entity **entity)
{
	struct poulsback_job *job;
	int ret;
	*engine = poulsback_engine_get(dev);
	if (IS_ERR(*engine))
		return ERR_CAST(*engine);
	job = kzalloc(bytes, GFP_KERNEL);
	if (!job)
		return ERR_PTR(-ENOMEM);
	job->vm = poulsback_vm_get(poulsback_file_vm(file));
	ret = poulsback_exec_context(file, context, ctx, entity, scene ? &job->scene : NULL);
	if (ret) {
		poulsback_job_release(job);
		return ERR_PTR(ret);
	}
	return job;
}

/* Resolves the request's buffers into the job, each listed once; a flag other
 * than ALLOWED is refused. Each BO's VA and size go to va[] and size[], and
 * WRITE flags to *writes. */
static int job_resolve(struct poulsback_job *job, struct drm_file *file,
		       const struct poulsback_submit_bo *bos, u32 count, u32 allowed, u64 *va,
		       u64 *size, u64 *writes)
{
	u32 i, k;
	int ret;
	for (i = 0; i < count; i++) {
		if (bos[i].flags & ~allowed)
			return POULSBACK_REFUSE(-EINVAL);
		ret = poulsback_bo_resolve(file, bos[i].handle, &job->bos[i], &va[i], &size[i]);
		if (ret)
			return ret;
		job->bo_count = i + 1;
		for (k = 0; k < i; k++)
			if (job->bos[k] == job->bos[i])
				return POULSBACK_REFUSE(-EINVAL);
		if (bos[i].flags & POULSBACK_SUBMIT_BO_WRITE)
			*writes |= 1ULL << i;
	}
	return 0;
}

/* Queues a built job on CTX's entity: it waits for the in-syncobj and the BOs'
 * reservations (writes: the BOs it writes), fences them and the out-syncobj.
 * On failure nothing is armed and the caller frees the job. */
static int job_push(struct poulsback_job *job, struct poulsback_exec_ctx *ctx,
		    struct drm_sched_entity *entity, struct drm_file *file, u32 in_syncobj,
		    u32 out_syncobj, u64 writes)
{
	struct ww_acquire_ctx acquire;
	struct drm_syncobj *out = NULL;
	struct dma_fence *fence;
	u32 i;
	int ret;
	if (out_syncobj) {
		out = drm_syncobj_find(file, out_syncobj);
		if (!out)
			return POULSBACK_REFUSE(-ENOENT);
	}
	ret = drm_sched_job_init(&job->base, entity, file);
	if (ret)
		goto put;
	if (in_syncobj) {
		ret = drm_syncobj_find_fence(file, in_syncobj, 0, 0, &fence);
		if (!ret)
			ret = drm_sched_job_add_dependency(&job->base, fence);
		if (ret)
			goto cleanup;
	}
	ret = drm_gem_lock_reservations(job->bos, job->bo_count, &acquire);
	if (ret)
		goto cleanup;
	for (i = 0; i < job->bo_count && !ret; i++) {
		ret = dma_resv_reserve_fences(job->bos[i]->resv, 1);
		if (!ret)
			ret = drm_sched_job_add_implicit_dependencies(&job->base, job->bos[i],
								      writes & (1ULL << i));
	}
	if (ret) {
		drm_gem_unlock_reservations(job->bos, job->bo_count, &acquire);
		goto cleanup;
	}
	/* Taken with the reservations held; nothing waits for one under it. */
	poulsback_exec_ctx_lock(ctx);
	drm_sched_job_arm(&job->base);
	fence = dma_fence_get(&job->base.s_fence->finished);
	for (i = 0; i < job->bo_count; i++)
		dma_resv_add_fence(job->bos[i]->resv, fence,
				   writes & (1ULL << i) ? DMA_RESV_USAGE_WRITE
							: DMA_RESV_USAGE_READ);
	drm_gem_unlock_reservations(job->bos, job->bo_count, &acquire);
	drm_sched_entity_push_job(&job->base);
	poulsback_exec_ctx_unlock(ctx);
	if (out)
		drm_syncobj_replace_fence(out, fence);
	dma_fence_put(fence);
	goto put;
cleanup:
	drm_sched_job_cleanup(&job->base);
put:
	if (out)
		drm_syncobj_put(out);
	return ret;
}

int poulsback_submit_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_submit *args = data;
	struct poulsback_submit_bo *bos = NULL;
	struct sgx535_engine121_scene_binding binding;
	struct sgx535_engine121_scene sizes;
	struct poulsback_exec_ctx *ctx;
	struct drm_sched_entity *entity;
	struct poulsback_engine *engine;
	struct poulsback_job *job;
	u64 *va, *size, writes = 0, start = ktime_get_ns();
	size_t built = 0;
	u32 i, n = 0;
	int ret, idx;
	if (args->abi != POULSBACK_EXEC_ABI || args->flags || !args->context || !args->width ||
	    !args->height || args->width > POULSBACK_SUBMIT_MAX_SIZE ||
	    args->height > POULSBACK_SUBMIT_MAX_SIZE || !args->bo_count ||
	    args->bo_count > POULSBACK_SUBMIT_MAX_BOS)
		return POULSBACK_REFUSE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(args->reserved); i++)
		if (args->reserved[i])
			return POULSBACK_REFUSE(-EINVAL);
	if (sgx535_engine121_scene_build(args->width, args->height, &sizes))
		return POULSBACK_REFUSE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	job = job_begin(
		dev, file, args->context,
		struct_size(job, programs, POULSBACK_JOB_TA_ACTIONS + POULSBACK_JOB_RASTER_ACTIONS),
		true, &engine, &ctx, &entity);
	if (IS_ERR(job)) {
		ret = PTR_ERR(job);
		goto exit;
	}
	job->ta = job->programs;
	job->raster = job->programs + POULSBACK_JOB_TA_ACTIONS;

	/* Buffers: every one the job touches, read or written. One scratch block
	 * holds the request's list and the VA and size each BO resolved to. */
	BUILD_BUG_ON(sizeof(*bos) % sizeof(u64));
	bos = kvmalloc_array(args->bo_count, sizeof(*bos) + 2 * sizeof(u64), GFP_KERNEL);
	job->bos = kvcalloc(args->bo_count, sizeof(*job->bos), GFP_KERNEL);
	if (!bos || !job->bos) {
		ret = -ENOMEM;
		goto free;
	}
	va = (u64 *)(bos + args->bo_count);
	size = va + args->bo_count;
	if (copy_from_user(bos, u64_to_user_ptr(args->bos), args->bo_count * sizeof(*bos))) {
		ret = -EFAULT;
		goto free;
	}
	ret = job_resolve(job, file, bos, args->bo_count, POULSBACK_SUBMIT_BO_WRITE, va, size,
			  &writes);
	if (ret)
		goto free;

	/* Programs: the job's table registers, then the kernel's scene, DPM,
	 * cache and kick sequence for the context's scene buffer. */
	job->region_array = (u32)atomic_inc_return(&job->scene->arrays) & 1;
	binding = (struct sgx535_engine121_scene_binding){
		.storage = 1,
		.gpu_va_bytes = job->scene->node.start,
		.allocation_bytes = job->scene->node.size,
		.width = args->width,
		.height = args->height,
		.region_array = job->region_array,
		/* scene_create's layout: the region arrays at offsets no render size
		 * moves, as two jobs of different sizes may be in flight. */
		.layout_width = POULSBACK_SUBMIT_MAX_SIZE,
		.layout_height = POULSBACK_SUBMIT_MAX_SIZE,
	};
	ret = submit_regs(POULSBACK_REG_TA, args, args->ta_regs, args->ta_reg_count, va, size,
			  job->ta, &n);
	if (!ret &&
	    sgx535_engine121_ta_build(&binding, job->ta + n, POULSBACK_JOB_TA_ACTIONS - n, &built))
		ret = POULSBACK_REFUSE(-EINVAL);
	job->ta_count = n + built;
	if (!ret)
		ret = submit_regs(POULSBACK_REG_RASTER, args, args->raster_regs,
				  args->raster_reg_count, va, size, job->raster, &n);
	/* The TA's DPM context stays loaded for the 3D, which then frees its
	 * pages (the engine selects the context). The full raster program would
	 * load a state table this TA never stored, and the pages would leak. */
	if (!ret && sgx535_engine121_raster_build(&binding, job->raster + n,
						  POULSBACK_JOB_RASTER_ACTIONS - n, &built))
		ret = POULSBACK_REFUSE(-EINVAL);
	job->raster_count = n + built;
	job->clear_start = sizes.clear_page_start;
	job->clear_count = sizes.clear_page_count;
	if (!ret)
		ret = job_push(job, ctx, entity, file, args->in_syncobj, args->out_syncobj, writes);
	if (!ret) {
		poulsback_engine_account_submit(engine, false, ktime_get_ns() - start);
		goto exit_arrays;
	}
free:
	poulsback_job_release(job);
exit_arrays:
	kvfree(bos);
	poulsback_exec_ctx_put(ctx);
exit:
	drm_dev_exit(idx);
	return ret;
}

/* SUBMIT_2D (docs/kernel/execution.md, "2D jobs"): the planner checks the
 * surfaces and operations and encodes them here, at the trust boundary; the
 * executor only feeds the words. */
int poulsback_submit_2d_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_submit_2d *args = data;
	struct poulsback_2d_surface surfaces[POULSBACK_2D_MAX_SURFACES];
	struct poulsback_submit_bo bos[POULSBACK_2D_MAX_BOS];
	struct poulsback_twod_bo resolved[POULSBACK_2D_MAX_BOS];
	u64 va[POULSBACK_2D_MAX_BOS], size[POULSBACK_2D_MAX_BOS], writes = 0;
	struct poulsback_2d_op *ops = NULL;
	struct poulsback_twod_plan plan;
	struct poulsback_exec_ctx *ctx;
	struct drm_sched_entity *entity;
	struct poulsback_engine *engine;
	struct poulsback_job *job;
	u64 start = ktime_get_ns();
	u32 i, capacity;
	int ret, idx;
	if (args->abi != POULSBACK_EXEC_ABI || args->flags || !args->context ||
	    !args->bo_count || args->bo_count > POULSBACK_2D_MAX_BOS || !args->surface_count ||
	    args->surface_count > POULSBACK_2D_MAX_SURFACES || !args->op_count ||
	    args->op_count > POULSBACK_2D_MAX_OPS)
		return POULSBACK_REFUSE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(args->reserved); i++)
		if (args->reserved[i])
			return POULSBACK_REFUSE(-EINVAL);
	if (copy_from_user(bos, u64_to_user_ptr(args->bos), args->bo_count * sizeof(*bos)) ||
	    copy_from_user(surfaces, u64_to_user_ptr(args->surfaces),
			   args->surface_count * sizeof(*surfaces)))
		return -EFAULT;
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	job = job_begin(dev, file, args->context, sizeof(*job), false, &engine, &ctx, &entity);
	if (IS_ERR(job)) {
		ret = PTR_ERR(job);
		goto exit;
	}
	job->twod = true;
	capacity = POULSBACK_TWOD_WORDS(args->op_count);
	ops = kvmalloc_array(args->op_count, sizeof(*ops), GFP_KERNEL);
	job->words = kvmalloc_array(capacity, sizeof(*job->words), GFP_KERNEL);
	job->bos = kvcalloc(args->bo_count, sizeof(*job->bos), GFP_KERNEL);
	if (!ops || !job->words || !job->bos) {
		ret = -ENOMEM;
		goto free;
	}
	if (copy_from_user(ops, u64_to_user_ptr(args->ops), args->op_count * sizeof(*ops))) {
		ret = -EFAULT;
		goto free;
	}
	/* Buffers: 2D-class BOs, each listed once; reads and writes come from the
	 * operations. */
	ret = job_resolve(job, file, bos, args->bo_count, 0, va, size, &writes);
	if (ret)
		goto free;
	for (i = 0; i < args->bo_count; i++)
		resolved[i] = (struct poulsback_twod_bo){va[i], size[i]};
	switch (poulsback_twod_plan(resolved, args->bo_count, surfaces, args->surface_count, ops,
				    args->op_count, job->words, capacity, &plan)) {
	case POULSBACK_TWOD_OK:
		break;
	case POULSBACK_TWOD_UNSUPPORTED:
		ret = POULSBACK_REFUSE(-EOPNOTSUPP);
		goto free;
	default:
		ret = POULSBACK_REFUSE(-EINVAL);
		goto free;
	}
	job->count = plan.count;
	job->expected = plan.expected;
	ret = job_push(job, ctx, entity, file, args->in_syncobj, args->out_syncobj, plan.writes);
	if (!ret) {
		poulsback_engine_account_submit(engine, true, ktime_get_ns() - start);
		goto exit_ops;
	}
free:
	poulsback_job_release(job);
exit_ops:
	kvfree(ops);
	poulsback_exec_ctx_put(ctx);
exit:
	drm_dev_exit(idx);
	return ret;
}
