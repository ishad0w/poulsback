/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* The execution engine is the only writer of SGX registers once it owns the
 * device (docs/kernel/execution.md). */
#ifndef POULSBACK_ENGINE_H
#define POULSBACK_ENGINE_H

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_gpu_scheduler;
struct poulsback_engine;
struct poulsback_exec_file;
struct poulsback_vm;

/* The device's engine, brought up on first use; ERR_PTR when another path
 * owns the SGX in this boot, and ERR_PTR(-EIO) once the engine is lost
 * (wedged): the one error the engine ioctls publish after a loss. An engine
 * that exists, ready or lost, holds a module reference until its release. */
struct poulsback_engine *poulsback_engine_get(struct drm_device *dev);
/* True once the engine exists: its claim requires a complete teardown
 * before unload or power-down. Lifecycle callers are not wired yet. */
bool poulsback_engine_retained(struct drm_device *dev);
/* The scheduler of 3D jobs, or of 2D jobs (TWOD). */
struct drm_gpu_scheduler *poulsback_engine_scheduler(struct poulsback_engine *engine, bool twod);
/* CPU time of one SUBMIT (or SUBMIT_2D) ioctl, for the per-job figures in debugfs. */
void poulsback_engine_account_submit(struct poulsback_engine *engine, bool twod, u64 ns);
/* Points the file's directory at the device ranges (parameter buffer, DPM tables). */
void poulsback_engine_link_vm(struct poulsback_engine *engine, struct poulsback_vm *vm);

/* Teardown helpers (docs/kernel/execution.md); 0 without an engine.
 * Quiesce: park both schedulers, wait for
 * the engine to drain (two job timeouts and 100 ms, then it wedges), read
 * the idle registers; -EBUSY, the schedulers unparked and the engine as it
 * is, when lost, holding a job or not idle. Power-down: EMGD's deinit
 * writes, only after quiesce. Release: after quiesce, with the engine ready and no
 * hardware fence alive, stop the schedulers, unbind the IRQ route, free every
 * page, release the claim and drop the engine's module reference (taken at
 * its claim); -EBUSY otherwise, freeing nothing. */
int poulsback_engine_quiesce(struct drm_device *dev);
int poulsback_engine_power_down(struct drm_device *dev);
int poulsback_engine_release(struct drm_device *dev);

struct poulsback_exec_file *poulsback_exec_file_create(void);
void poulsback_exec_file_destroy(struct poulsback_exec_file *exec);

int poulsback_context_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int poulsback_context_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int poulsback_submit_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int poulsback_submit_2d_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
#endif
