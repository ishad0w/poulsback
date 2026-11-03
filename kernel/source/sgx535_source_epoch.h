/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef SGX535_SOURCE_EPOCH_H
#define SGX535_SOURCE_EPOCH_H
#include <linux/types.h>
struct drm_device;
/* Recorded loss of the bound SGX source. No MMIO here. */
enum sgx535_epoch_loss {
	SGX535_EPOCH_PM = 1,
	SGX535_EPOCH_RESET = 2,
	SGX535_EPOCH_REMOVE = 4,
	SGX535_EPOCH_FAULT = 8
};
struct sgx535_epoch_snapshot {
	u64 epoch;
	u32 loss;
	bool bound, claimed;
};
int sgx535_epoch_bind(struct drm_device *);
/* Device removal: mark the epoch lost, then unbind unless the SGX is claimed
 * (-EBUSY: the caller keeps the engine on HOLD). */
int sgx535_epoch_close(struct drm_device *);
/* Complete external MMU/GTT/GEM writer boundary, not a per-PTE precheck.
 * begin holds common admission until the matching same-task end. Lock order:
 * admission, caller reservation/MMU semaphore, source/PTE spinlocks. Before
 * epoch_bind ordinary initialization is permitted; this never clears loss. */
int sgx535_epoch_external_begin(struct drm_device *);
int sgx535_epoch_external_end(struct drm_device *);
/* Same-task external operation only. Requires an already bound, unchanged,
 * loss-free source. With source_lock held, check is pure and nonsleeping;
 * commit performs bounded CPU stores only and cannot fail. Neither callback
 * may recurse source APIs, sleep, change PAT/MMIO or free. This does not end
 * external admission or certify cache visibility/last-use. */
int sgx535_epoch_external_finalize(struct drm_device *, u64 expected_epoch, int (*check)(void *),
				   void (*commit)(void *), void *);
/* Internal MMU writers may join only an exact same-task external operation.
 * This observation cannot establish admission; the outer caller owns begin/end. */
bool sgx535_epoch_external_active(struct drm_device *);
/* The engine's claim: once per boot, refused (-EBUSY) after PM or removal
 * closed the source. A claimed SGX stays busy until the engine's teardown
 * releases it (its memory freed, the SGX powered down). */
int sgx535_epoch_executor_claim(struct drm_device *);
void sgx535_epoch_executor_release(struct drm_device *);
/* IRQ-safe, monotonic, permanent admission loss. */
void sgx535_epoch_lost(struct drm_device *, u32 reason);
int sgx535_epoch_quiesce(struct drm_device *, u32 reason);
int sgx535_epoch_pm_begin(struct drm_device *);
bool sgx535_epoch_busy(struct drm_device *);
void sgx535_epoch_snapshot(struct sgx535_epoch_snapshot *);
#endif
