// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_engine.h"
#include "poulsback_errsite.h"
#include "poulsback_exec_uapi.h"
#include "poulsback_job.h"
#include "poulsback_file.h"
#include "poulsback_vm.h"

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/gpu_scheduler.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#define FILE_CONTEXTS 64U

/* A file's execution contexts; each is two scheduler entities, for 3D and 2D
 * jobs on separate schedulers (docs/kernel/execution.md). Jobs
 * of one context are ordered by their buffers' reservations, not by the
 * context. */
struct poulsback_exec_file {
	struct mutex lock;
	struct idr contexts;
	u32 count;
};

/* Held by the file's table and by each submission from lookup to push, so
 * a destroy during a submission frees it only after the push. `push` is held
 * from arming a job to pushing it: the entities get their jobs in the order
 * of the jobs' fences (drm_sched_entity_push_job). */
struct poulsback_exec_ctx {
	struct kref ref;
	struct mutex push;
	struct drm_sched_entity entity, entity2d;
	struct poulsback_scene *scene;
};

int poulsback_exec_context(struct drm_file *file, u32 handle, struct poulsback_exec_ctx **held,
			   struct drm_sched_entity **entity, struct poulsback_scene **scene)
{
	struct poulsback_exec_file *exec = poulsback_file_exec(file);
	struct poulsback_exec_ctx *ctx;
	int ret = 0;
	if (!exec)
		return POULSBACK_ERRSITE(-ENODEV);
	mutex_lock(&exec->lock);
	ctx = idr_find(&exec->contexts, handle);
	if (!ctx)
		ret = POULSBACK_REFUSE(-ENOENT);
	else if (scene && !ctx->scene)
		ctx->scene = poulsback_scene_create(poulsback_file_vm(file), handle);
	if (ctx && scene && !ctx->scene)
		ret = -ENOMEM;
	if (!ret) {
		if (scene) {
			kref_get(&ctx->scene->ref);
			*scene = ctx->scene;
		}
		kref_get(&ctx->ref);
		*held = ctx;
		*entity = scene ? &ctx->entity : &ctx->entity2d;
	}
	mutex_unlock(&exec->lock);
	return ret;
}

void poulsback_exec_ctx_lock(struct poulsback_exec_ctx *ctx)
{
	mutex_lock(&ctx->push);
}

void poulsback_exec_ctx_unlock(struct poulsback_exec_ctx *ctx)
{
	mutex_unlock(&ctx->push);
}

struct poulsback_exec_file *poulsback_exec_file_create(void)
{
	struct poulsback_exec_file *exec = kzalloc(sizeof(*exec), GFP_KERNEL);
	if (!exec)
		return NULL;
	mutex_init(&exec->lock);
	idr_init(&exec->contexts);
	return exec;
}

static void context_release(struct kref *ref)
{
	struct poulsback_exec_ctx *ctx = container_of(ref, struct poulsback_exec_ctx, ref);
	drm_sched_entity_destroy(&ctx->entity);
	drm_sched_entity_destroy(&ctx->entity2d);
	poulsback_scene_put(ctx->scene);
	mutex_destroy(&ctx->push);
	kfree(ctx);
}

void poulsback_exec_ctx_put(struct poulsback_exec_ctx *ctx)
{
	if (ctx)
		kref_put(&ctx->ref, context_release);
}

void poulsback_exec_file_destroy(struct poulsback_exec_file *exec)
{
	struct poulsback_exec_ctx *ctx;
	int id;
	if (!exec)
		return;
	idr_for_each_entry(&exec->contexts, ctx, id) poulsback_exec_ctx_put(ctx);
	idr_destroy(&exec->contexts);
	mutex_destroy(&exec->lock);
	kfree(exec);
}

int poulsback_context_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_context_create *args = data;
	struct poulsback_exec_file *exec = poulsback_file_exec(file);
	struct drm_gpu_scheduler *sched;
	struct poulsback_engine *engine;
	struct poulsback_exec_ctx *ctx;
	u32 i;
	int ret, idx;
	if (args->abi != POULSBACK_EXEC_ABI || args->flags || args->handle)
		return POULSBACK_REFUSE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(args->reserved); i++)
		if (args->reserved[i])
			return POULSBACK_REFUSE(-EINVAL);
	if (!exec || !poulsback_file_vm(file))
		return POULSBACK_ERRSITE(-ENODEV);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	engine = poulsback_engine_get(dev);
	if (IS_ERR(engine)) {
		ret = PTR_ERR(engine);
		goto exit;
	}
	poulsback_engine_link_vm(engine, poulsback_file_vm(file));
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto exit;
	}
	kref_init(&ctx->ref);
	mutex_init(&ctx->push);
	sched = poulsback_engine_scheduler(engine, false);
	ret = drm_sched_entity_init(&ctx->entity, DRM_SCHED_PRIORITY_NORMAL, &sched, 1, NULL);
	if (ret) {
		mutex_destroy(&ctx->push);
		kfree(ctx);
		goto exit;
	}
	sched = poulsback_engine_scheduler(engine, true);
	ret = drm_sched_entity_init(&ctx->entity2d, DRM_SCHED_PRIORITY_NORMAL, &sched, 1, NULL);
	if (ret) {
		drm_sched_entity_destroy(&ctx->entity);
		mutex_destroy(&ctx->push);
		kfree(ctx);
		goto exit;
	}
	mutex_lock(&exec->lock);
	ret = exec->count < FILE_CONTEXTS ? idr_alloc(&exec->contexts, ctx, 1, 0, GFP_KERNEL)
					  : POULSBACK_ERRSITE(-ENOSPC);
	if (ret > 0) {
		exec->count++;
		args->handle = ret;
		ret = 0;
	}
	mutex_unlock(&exec->lock);
	if (ret)
		poulsback_exec_ctx_put(ctx);
exit:
	drm_dev_exit(idx);
	return ret;
}

int poulsback_context_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_context_destroy *args = data;
	struct poulsback_exec_file *exec = poulsback_file_exec(file);
	struct poulsback_exec_ctx *ctx;
	if (args->abi != POULSBACK_EXEC_ABI || args->reserved[0] || args->reserved[1] || !exec)
		return POULSBACK_REFUSE(-EINVAL);
	mutex_lock(&exec->lock);
	ctx = idr_remove(&exec->contexts, args->handle);
	if (ctx)
		exec->count--;
	mutex_unlock(&exec->lock);
	if (!ctx)
		return POULSBACK_REFUSE(-ENOENT);
	/* The table's reference; a submission still holding it frees it. */
	poulsback_exec_ctx_put(ctx);
	return 0;
}
