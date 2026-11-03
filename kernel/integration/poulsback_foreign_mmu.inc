// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Included in the pinned Linux MMU translation unit. Normal callers own a
 * complete foreign transaction; a nested MMU helper joins only that exact
 * task/device boundary; no other writer can borrow it.
 * Void teardown helpers retain their storage on refusal. Their audited callers
 * must hold the whole transaction or preserve the complete containing owner.
 */
static int foreign_mmu_begin(struct drm_device *dev, bool *owned)
{
	int ret;
	*owned = false;
	if (sgx535_epoch_external_active(dev))
		return 0;
	ret = sgx535_epoch_external_begin(dev);
	if (!ret)
		*owned = true;
	return ret;
}
static void foreign_mmu_end(struct drm_device *dev, bool owned)
{
	if (owned)
		WARN_ON(sgx535_epoch_external_end(dev));
}
void psb_mmu_flush(struct psb_mmu_driver *driver)
{
	bool owned;
	if (foreign_mmu_begin(driver->dev, &owned))
		return;
	psb_mmu_flush_unguarded(driver);
	foreign_mmu_end(driver->dev, owned);
}
void psb_mmu_set_pd_context(struct psb_mmu_pd *pd, int context)
{
	struct drm_device *dev = pd->driver->dev;
	bool owned;
	if (foreign_mmu_begin(dev, &owned))
		return;
	psb_mmu_set_pd_context_unguarded(pd, context);
	foreign_mmu_end(dev, owned);
}
void psb_mmu_free_pagedir(struct psb_mmu_pd *pd)
{
	struct drm_device *dev = pd->driver->dev;
	bool owned;
	if (foreign_mmu_begin(dev, &owned))
		return;
	psb_mmu_free_pagedir_unguarded(pd);
	foreign_mmu_end(dev, owned);
}
void psb_mmu_driver_takedown(struct psb_mmu_driver *driver)
{
	struct drm_device *dev = driver->dev;
	bool owned;
	if (foreign_mmu_begin(dev, &owned))
		return;
	psb_mmu_driver_takedown_unguarded(driver);
	foreign_mmu_end(dev, owned);
}
struct psb_mmu_driver *psb_mmu_driver_init(struct drm_device *dev, int trap, int type,
					   atomic_t *invaldc)
{
	struct psb_mmu_driver *driver;
	bool owned;
	if (foreign_mmu_begin(dev, &owned))
		return NULL;
	driver = psb_mmu_driver_init_unguarded(dev, trap, type, invaldc);
	foreign_mmu_end(dev, owned);
	return driver;
}
