/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* SGX interrupt routing between the engine and the stock gma500 handler. */
#ifndef POULSBACK_ENGINE_IRQ_H
#define POULSBACK_ENGINE_IRQ_H

/* The engine sets its event enables once at bring-up (TA, raster, OOM and
 * fault events and the 2D core's completion, bit 27); its handler
 * acknowledges each event it is given. */
#include <linux/types.h>
struct drm_device;
enum sgx535_engine_irq_route {
	SGX535_ENGINE_IRQ_LEGACY = 0,
	SGX535_ENGINE_IRQ_POLL_OWNER
};
unsigned long sgx535_engine_irq_lock(void);
void sgx535_engine_irq_unlock(unsigned long flags);
/* The engine's route: every SGX interrupt of the device goes to handler,
 * in hard IRQ context under the engine IRQ lock, until the engine's teardown
 * unbinds it (its host enables already 0). */
int sgx535_engine_irq_bind_executor_locked(struct drm_device *,
					   void (*handler)(struct drm_device *, u32, u32));
void sgx535_engine_irq_unbind_executor_locked(struct drm_device *);
/* Caller holds the lock around the stock read/ack interval. */
void sgx535_engine_irq_capture_locked(struct drm_device *dev, u32 status1, u32 status2);
/* POLL_OWNER: the engine took the event and owns its ACK; LEGACY: the stock
 * handler captures and acknowledges it. */
enum sgx535_engine_irq_route sgx535_engine_irq_route_locked(struct drm_device *, u32, u32);
#endif
