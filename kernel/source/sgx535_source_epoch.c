// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include <linux/errno.h>
#include "poulsback_errsite.h"
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <drm/drm_drv.h>
#include "sgx535_source_epoch.h"
#include "sgx535_debug.h"
static void diag_after(u32 kind, int error, u32 reason)
{
	struct sgx535_epoch_snapshot s;
	sgx535_epoch_snapshot(&s);
	sgx535_diag_record(kind, 0, s.epoch, 0, s.claimed, error, reason, 0);
}
/* One physical SGX engine. */
static DEFINE_MUTEX(admission);
static DEFINE_SPINLOCK(source_lock);
static struct drm_device *bound;
static u64 epoch = 1;
static u32 lost;
/* closed: PM or removal ended the boot's source; claimed: the engine owns it. */
static bool closed, claimed;
static struct drm_device *external_device;
static struct task_struct *external_task;
int sgx535_epoch_external_begin(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	if (!dev)
		return POULSBACK_ERRSITE(-EINVAL);
	/* Diagnose same-task recursion before taking the nonrecursive mutex.
	 * Other tasks wait for the whole owned operation before deciding. */
	spin_lock_irqsave(&source_lock, flags);
	if (external_task == current)
		ret = -EDEADLK;
	spin_unlock_irqrestore(&source_lock, flags);
	if (ret)
		return ret;
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (bound && bound != dev)
		ret = POULSBACK_ERRSITE(-ENODEV);
	else {
		external_device = dev;
		external_task = current;
	}
	spin_unlock_irqrestore(&source_lock, flags);
	if (ret)
		mutex_unlock(&admission);
	return ret;
}
bool sgx535_epoch_external_active(struct drm_device *dev)
{
	unsigned long flags;
	bool result;
	spin_lock_irqsave(&source_lock, flags);
	result = dev && external_device == dev && external_task == current;
	spin_unlock_irqrestore(&source_lock, flags);
	return result;
}
int sgx535_epoch_external_end(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	spin_lock_irqsave(&source_lock, flags);
	if (!dev || external_device != dev || external_task != current)
		ret = POULSBACK_ERRSITE(-EPERM);
	else {
		external_device = NULL;
		external_task = NULL;
	}
	spin_unlock_irqrestore(&source_lock, flags);
	if (!ret)
		mutex_unlock(&admission);
	return ret;
}
int sgx535_epoch_external_finalize(struct drm_device *dev, u64 expected_epoch, int (*check)(void *),
				   void (*commit)(void *), void *context)
{
	unsigned long flags;
	int ret = 0;
	if (!dev || !expected_epoch || !check || !commit || !context)
		return POULSBACK_ERRSITE(-EINVAL);
	spin_lock_irqsave(&source_lock, flags);
	if (external_device != dev || external_task != current)
		ret = POULSBACK_ERRSITE(-EPERM);
	else if (bound != dev || epoch != expected_epoch || lost)
		ret = POULSBACK_ERRSITE(-ESTALE);
	else
		ret = check(context);
	if (!ret)
		commit(context);
	spin_unlock_irqrestore(&source_lock, flags);
	return ret;
}
int sgx535_epoch_bind(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	if (!dev)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (bound)
		ret = -EBUSY;
	spin_unlock_irqrestore(&source_lock, flags);
	if (!ret) {
		spin_lock_irqsave(&source_lock, flags);
		bound = dev;
		drm_dev_get(dev);
		spin_unlock_irqrestore(&source_lock, flags);
	}
	mutex_unlock(&admission);
	diag_after(SGX535_DIAG_EPOCH, ret, 0);
	return ret;
}
static int epoch_unbind(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	bool put = false;
	if (!dev)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (dev != bound)
		ret = POULSBACK_ERRSITE(-ENODEV);
	else if (claimed)
		ret = -EBUSY;
	else {
		bound = NULL;
		closed = true;
		lost |= SGX535_EPOCH_REMOVE;
		if (epoch != U64_MAX)
			epoch++;
		put = true;
	}
	spin_unlock_irqrestore(&source_lock, flags);
	mutex_unlock(&admission);
	if (put)
		drm_dev_put(dev);
	diag_after(SGX535_DIAG_EPOCH, ret, SGX535_EPOCH_REMOVE);
	return ret;
}
int sgx535_epoch_close(struct drm_device *dev)
{
	sgx535_epoch_lost(dev, SGX535_EPOCH_REMOVE);
	if (sgx535_epoch_busy(dev))
		return -EBUSY;
	WARN_ON(epoch_unbind(dev));
	return 0;
}
int sgx535_epoch_executor_claim(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	if (!dev)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (bound != dev)
		ret = POULSBACK_ERRSITE(-ENODEV);
	else if (claimed || closed)
		ret = POULSBACK_ERRSITE(-EBUSY);
	else
		claimed = true; /* The executor owns the SGX for the rest of the boot. */
	spin_unlock_irqrestore(&source_lock, flags);
	mutex_unlock(&admission);
	diag_after(SGX535_DIAG_LEASE, ret, 0);
	return ret;
}
static void lost_locked(struct drm_device *dev, u32 reason)
{
	/* Before the epoch is bound nothing is published, so the driver's
	 * start-up reset (psb_spank) loses nothing and must not close admission. */
	if (bound != dev)
		return;
	lost |= reason;
	if (reason & (SGX535_EPOCH_PM | SGX535_EPOCH_REMOVE))
		closed = true;
	if (epoch != U64_MAX)
		epoch++;
}
void sgx535_epoch_lost(struct drm_device *dev, u32 reason)
{
	unsigned long flags;
	if (!dev || !reason)
		return;
	spin_lock_irqsave(&source_lock, flags);
	lost_locked(dev, reason);
	spin_unlock_irqrestore(&source_lock, flags);
	diag_after(SGX535_DIAG_EPOCH, -ESTALE, reason);
}
/* Close admission atomically with idle PM decision; no common mutex in IRQ. */
void sgx535_epoch_executor_release(struct drm_device *dev)
{
	unsigned long flags;
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (bound == dev)
		claimed = false;
	spin_unlock_irqrestore(&source_lock, flags);
	mutex_unlock(&admission);
	diag_after(SGX535_DIAG_LEASE, 0, 0);
}
int sgx535_epoch_quiesce(struct drm_device *dev, u32 reason)
{
	unsigned long flags;
	int ret = 0;
	if (!dev || !reason)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	lost_locked(dev, reason);
	if (bound == dev && claimed)
		ret = -EBUSY;
	spin_unlock_irqrestore(&source_lock, flags);
	mutex_unlock(&admission);
	diag_after(SGX535_DIAG_EPOCH, ret, reason);
	return ret;
}
int sgx535_epoch_pm_begin(struct drm_device *dev)
{
	unsigned long flags;
	int ret = 0;
	/* A claimed SGX refuses PM without changing source; idle closes it first. */
	mutex_lock(&admission);
	spin_lock_irqsave(&source_lock, flags);
	if (bound == dev && claimed)
		ret = -EBUSY;
	else
		lost_locked(dev, SGX535_EPOCH_PM);
	spin_unlock_irqrestore(&source_lock, flags);
	mutex_unlock(&admission);
	diag_after(SGX535_DIAG_EPOCH, ret, ret ? 0 : SGX535_EPOCH_PM);
	return ret;
}
bool sgx535_epoch_busy(struct drm_device *dev)
{
	unsigned long flags;
	bool busy;
	spin_lock_irqsave(&source_lock, flags);
	busy = bound == dev && claimed;
	spin_unlock_irqrestore(&source_lock, flags);
	return busy;
}
void sgx535_epoch_snapshot(struct sgx535_epoch_snapshot *s)
{
	unsigned long flags;
	if (!s)
		return;
	spin_lock_irqsave(&source_lock, flags);
	s->epoch = epoch;
	s->loss = lost;
	s->bound = !!bound;
	s->claimed = claimed;
	spin_unlock_irqrestore(&source_lock, flags);
}
