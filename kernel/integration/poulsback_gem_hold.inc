// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Full Linux GEM owner retention, including GTT reservation and page array.
 * This list has no reclaim/reset/retry operation. It does not resurrect a zero
 * GEM kref: final free keeps the allocation itself; live callers gain a kref.
 */
#include "poulsback_errsite.h"
#include <drm/drm_drv.h>
static LIST_HEAD(poulsback_gem_holds);
static DEFINE_MUTEX(poulsback_gem_holds_lock);
static int poulsback_gem_hold_record(struct psb_gem_object *pobj, int error,
				     bool transfer_pages_ref)
{
	struct drm_device *dev = pobj->base.dev;
	bool live;
	int ret = 0;
	mutex_lock(&poulsback_gem_holds_lock);
	if (!pobj->poulsback_held) {
		WRITE_ONCE(pobj->poulsback_held, true);
		/* Every genuinely published nonstolen page array already owns this
		 * reference. Transfer it, never try to pin code after a cache/PTE
		 * failure. Stolen admission refusal has an existing engine owner
		 * pinning the module; retain a separate reference for the full BO. */
		if (transfer_pages_ref &&
		    (pobj->poulsback_pages_module_ref || pobj->poulsback_put_after_free)) {
			pobj->poulsback_pages_module_ref = false;
			pobj->poulsback_put_after_free = false;
			pobj->poulsback_module_ref = true;
		} else {
			pobj->poulsback_module_ref = try_module_get(THIS_MODULE);
			if (!pobj->poulsback_module_ref)
				ret = POULSBACK_ERRSITE(-ENODEV);
		}
		drm_dev_get(dev);
		live = kref_get_unless_zero(&pobj->base.refcount);
		list_add_tail(&pobj->poulsback_hold_link, &poulsback_gem_holds);
		dev_err(dev->dev,
			"poulsback: legacy GEM retained, error=%d pins=%d module=%u live=%u\n",
			error, READ_ONCE(pobj->in_gart), pobj->poulsback_module_ref, live);
	} else if (!pobj->poulsback_module_ref) {
		ret = POULSBACK_ERRSITE(-ENODEV);
	}
	mutex_unlock(&poulsback_gem_holds_lock);
	/* Loss forbids a new engine epoch; retained(dev) also bars PM/teardown.
	 * No device operation or IRQ source lock is taken while this mutex lives. */
	sgx535_epoch_lost(dev, SGX535_EPOCH_FAULT);
	/* A contradictory/corrupted owner without a publication/producer ref is
	 * an explicit refusal, never a successful code-lifetime certificate. */
	return ret;
}
/* The normal transition reads and transfers page/code ownership only while
 * holding the same reservation lock as pin/unpin. The admission lock is never
 * acquired from here: failed admission has already released that boundary. */
static int poulsback_gem_hold_locked(struct psb_gem_object *pobj, int error)
{
	dma_resv_assert_held(pobj->base.resv);
	return poulsback_gem_hold_record(pobj, error, true);
}
static int poulsback_gem_hold(struct psb_gem_object *pobj, int error)
{
	int ret = dma_resv_lock(pobj->base.resv, NULL);
	if (ret) {
		/* A NULL-context, noninterruptible reservation lock does not normally
		 * fail. Preserve the complete object on an unexpected refusal without
		 * reading/transferring the unsynchronized page reference. Its existing
		 * publication reference stays with the page array; a separate module
		 * reference is attempted, never fabricated on failure. */
		(void)poulsback_gem_hold_record(pobj, error, false);
		return ret;
	}
	ret = poulsback_gem_hold_locked(pobj, error);
	dma_resv_unlock(pobj->base.resv);
	return ret;
}
bool psb_gem_retained(struct drm_device *dev)
{
	struct psb_gem_object *pobj;
	bool retained = false;
	mutex_lock(&poulsback_gem_holds_lock);
	list_for_each_entry(pobj, &poulsback_gem_holds,
			    poulsback_hold_link) if (pobj->base.dev == dev)
	{
		retained = true;
		break;
	}
	mutex_unlock(&poulsback_gem_holds_lock);
	return retained;
}
