// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Included by power.c. System PM has frozen clients and framebuffer retirement. */
static int poulsback_sleep_source(struct drm_device *dev)
{
	struct sgx535_epoch_snapshot source;
	sgx535_epoch_snapshot(&source);
	if (!IS_PSB(dev) || !source.bound || source.loss || psb_gem_retained(dev))
		return -EIO;
	return 0;
}

int gma_system_prepare(struct device *device)
{
	struct drm_device *dev = pci_get_drvdata(to_pci_dev(device));
	int ret = poulsback_sleep_source(dev);
	if (!ret)
		ret = poulsback_display_psb_sleep_check(dev, false);
	if (!ret)
		ret = poulsback_engine_quiesce(dev);
	return ret;
}

void gma_system_complete(struct device *device)
{
	/* Another device may have refused sleep after our prepare callback. */
	poulsback_engine_unquiesce(pci_get_drvdata(to_pci_dev(device)));
}

static int poulsback_save_sleep_gtt(struct drm_device *dev)
{
	struct drm_psb_private *p = to_drm_psb_private(dev);
	u32 i, count = p->gtt.gatt_pages;
	if (p->poulsback_sleep_gtt || !count ||
	    (u64)count * sizeof(u32) > (u64)p->gtt.gtt_pages * PAGE_SIZE)
		return -EINVAL;
	p->poulsback_sleep_gtt = kvmalloc_array(count, sizeof(u32), GFP_KERNEL);
	if (!p->poulsback_sleep_gtt)
		return -ENOMEM;
	p->poulsback_sleep_entries = count;
	mutex_lock(&p->gtt_mutex);
	for (i = 0; i < count; i++)
		p->poulsback_sleep_gtt[i] = ioread32(p->gtt_map + i);
	mutex_unlock(&p->gtt_mutex);
	return 0;
}

static int poulsback_restore_sleep_gtt(struct drm_device *dev)
{
	struct drm_psb_private *p = to_drm_psb_private(dev);
	u32 i, stolen;
	int ret = 0;
	if (!p->poulsback_sleeping || !p->poulsback_sleep_gtt ||
	    p->poulsback_sleep_entries != p->gtt.gatt_pages)
		return -EINVAL;
	pci_read_config_dword(to_pci_dev(dev->dev), PSB_BSM, &stolen);
	if (stolen != p->stolen_base)
		return -ENODEV;
	/* The snapshot includes native reservations that are not legacy GEM objects. */
	mutex_lock(&p->gtt_mutex);
	for (i = 0; i < p->poulsback_sleep_entries; i++)
		iowrite32(p->poulsback_sleep_gtt[i], p->gtt_map + i);
	for (i = 0; i < p->poulsback_sleep_entries; i++)
		if (ioread32(p->gtt_map + i) != p->poulsback_sleep_gtt[i]) {
			ret = -EIO;
			break;
		}
	mutex_unlock(&p->gtt_mutex);
	return ret;
}

int gma_system_suspend(struct device *device)
{
	struct pci_dev *pci = to_pci_dev(device);
	struct drm_device *dev = pci_get_drvdata(pci);
	struct drm_psb_private *p = to_drm_psb_private(dev);
	int ret = poulsback_sleep_source(dev);
	if (ret)
		return ret;
	if (p->poulsback_sleeping || p->poulsback_sleep_gtt)
		return -EBUSY;
	drm_kms_helper_poll_disable(dev);
	/* The normal PM console switch must release native scanout first. */
	ret = poulsback_display_psb_sleep_check(dev, false);
	if (ret)
		goto poll;
	ret = poulsback_engine_quiesce(dev);
	if (ret)
		goto poll;
	ret = poulsback_save_sleep_gtt(dev);
	if (ret)
		goto unpark;
	ret = p->ops->save_regs(dev);
	if (ret)
		goto discard;
	p->poulsback_sleep_clocks = readl(p->sgx_reg + PSB_CR_CLKGATECTL);
	ret = poulsback_engine_power_down(dev);
	if (ret)
		goto discard;
	p->poulsback_sleeping = true;
	wmb();
	gma_irq_uninstall(dev);
	ret = p->ops->power_down(dev);
	if (!ret)
		ret = gma_suspend_pci(pci);
	if (ret) {
		int cleanup = gma_system_resume(device);
		drm_err(dev, "System sleep failed (%d), restore returned %d\n", ret, cleanup);
		return ret;
	}
	drm_info(dev, "System sleep: GPU drained, %u GTT entries saved\n",
		 p->poulsback_sleep_entries);
	return 0;
discard:
	kvfree(p->poulsback_sleep_gtt);
	p->poulsback_sleep_gtt = NULL;
	p->poulsback_sleep_entries = 0;
unpark:
	poulsback_engine_unquiesce(dev);
poll:
	drm_kms_helper_poll_enable(dev);
	drm_notice(dev, "System sleep refused before power-off (%d)\n", ret);
	return ret;
}

int gma_system_resume(struct device *device)
{
	struct pci_dev *pci = to_pci_dev(device);
	struct drm_device *dev = pci_get_drvdata(pci);
	struct drm_psb_private *p = to_drm_psb_private(dev);
	int ret;
	if (!p->poulsback_sleeping)
		return 0;
	ret = poulsback_sleep_source(dev);
	if (!ret)
		ret = gma_resume_pci(pci);
	if (!ret)
		ret = gma_resume_display(pci);
	if (!ret)
		ret = poulsback_display_psb_sleep_check(dev, true);
	if (!ret)
		ret = poulsback_sgx_resume(dev);
	if (!ret)
		ret = gma_irq_install(dev);
	ret = poulsback_engine_resume(dev, ret);
	if (ret) {
		sgx535_epoch_lost(dev, SGX535_EPOCH_PM);
		drm_err(dev, "System resume failed (%d); GPU resources retained until reboot\n",
			ret);
		return ret;
	}
	p->poulsback_sleeping = false;
	kvfree(p->poulsback_sleep_gtt);
	p->poulsback_sleep_gtt = NULL;
	p->poulsback_sleep_entries = 0;
	drm_kms_helper_poll_enable(dev);
	drm_info(dev, "System resume: display mappings and GPU restored\n");
	return 0;
}
