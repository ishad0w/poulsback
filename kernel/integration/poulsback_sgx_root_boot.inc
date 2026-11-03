// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Included by the locked Linux psb_drv translation unit. All pointers come
 * from its new canonical default PD, before installing context0. No root is
 * switched at runtime and no PTE, GTT, MMIO or cache writer is hidden here. */
#include "poulsback_errsite.h"
#include <linux/highmem.h>
static int poulsback_sgx_root_boot_admit(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct psb_mmu_driver *mmu = dev_priv->mmu;
	struct psb_mmu_pd *pd;
	struct pci_dev *pdev;
	const u32 *directory, *dummy;
	unsigned long flags;
	u32 i;
	int ret = -EOPNOTSUPP;
	if (!dev->dev || !dev_is_pci(dev->dev) || PAGE_SIZE != 4096 || !mmu)
		return ret;
	pdev = to_pci_dev(dev->dev);
	if (pdev->vendor != 0x8086 || pdev->device != 0x8108)
		return ret;
	if (PSB_RSGX32(PSB_CR_CORE_ID) != 0x01130000U ||
	    PSB_RSGX32(PSB_CR_CORE_REVISION) != 0x00010201U)
		return ret;
	down_read(&mmu->sem);
	spin_lock_irqsave(&mmu->lock, flags);
	pd = mmu->default_pd;
	if (!pd || pd->driver != mmu || mmu->dev != dev || pd->hw_context != -1 || !pd->tables ||
	    pd->invalid_pde || pd->invalid_pte || pd->pd_mask != PSB_PTE_VALID || !pd->p ||
	    !pd->dummy_pt || !pd->dummy_page || pd->p == pd->dummy_pt || pd->p == pd->dummy_page ||
	    pd->dummy_pt == pd->dummy_page || PageHighMem(pd->p) || PageHighMem(pd->dummy_pt) ||
	    PageHighMem(pd->dummy_page) || !page_address(pd->p) || !page_address(pd->dummy_pt) ||
	    !page_address(pd->dummy_page) || page_to_phys(pd->p) > 0xfffff000ULL ||
	    page_to_phys(pd->dummy_pt) > 0xfffff000ULL ||
	    page_to_phys(pd->dummy_page) > 0xfffff000ULL)
		goto out;
	directory = page_address(pd->p);
	dummy = page_address(pd->dummy_pt);
	for (i = 0; i < 1024; i++)
		if (pd->tables[i] || directory[i] || dummy[i])
			goto out;
	ret = 0;
out:
	spin_unlock_irqrestore(&mmu->lock, flags);
	up_read(&mmu->sem);
	return ret;
}
