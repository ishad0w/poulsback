// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Engine IRQ routing; no command, ACK or reset writer. */
#include "sgx535_engine_irq.h"
#include "poulsback_errsite.h"
#include "sgx535_source_epoch.h"
#include <linux/errno.h>
static struct drm_device *executor_device;
static void (*executor_irq)(struct drm_device *, u32, u32);

int sgx535_engine_irq_bind_executor_locked(struct drm_device *dev,
					   void (*handler)(struct drm_device *, u32, u32))
{
	if (!dev || !handler)
		return POULSBACK_ERRSITE(-EINVAL);
	if (executor_device)
		return -EBUSY;
	executor_device = dev;
	executor_irq = handler;
	return 0;
}

void sgx535_engine_irq_unbind_executor_locked(struct drm_device *dev)
{
	if (executor_device == dev) {
		executor_device = NULL;
		executor_irq = NULL;
	}
}

enum sgx535_engine_irq_route sgx535_engine_irq_route_locked(struct drm_device *dev, u32 status1,
							    u32 status2)
{
	if (executor_device == dev && dev) {
		executor_irq(dev, status1, status2);
		return SGX535_ENGINE_IRQ_POLL_OWNER;
	}
	return SGX535_ENGINE_IRQ_LEGACY;
}

void sgx535_engine_irq_capture_locked(struct drm_device *dev, u32 status1, u32 status2)
{
	/* No owner admits an event on this path: the global aggregate bit alone
	 * is no completion certificate. */
	if ((status1 & ~(1U << 31)) || status2)
		sgx535_epoch_lost(dev, SGX535_EPOCH_FAULT);
}
