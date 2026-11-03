// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_file.h"
#include "poulsback_engine.h"
#include "poulsback_errsite.h"
#include "poulsback_vm.h"

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <linux/bug.h>
#include <linux/slab.h>

struct poulsback_file {
	struct poulsback_vm *vm;
	struct poulsback_exec_file *exec;
};

int poulsback_file_open(struct drm_device *dev, struct drm_file *file)
{
	struct poulsback_file *pf;
	if (WARN_ON(file->driver_priv))
		return POULSBACK_ERRSITE(-EINVAL);
	pf = kzalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return -ENOMEM;
	pf->vm = poulsback_vm_create();
	pf->exec = poulsback_exec_file_create();
	if (!pf->vm || !pf->exec) {
		poulsback_exec_file_destroy(pf->exec);
		poulsback_vm_destroy(pf->vm);
		kfree(pf);
		return -ENOMEM;
	}
	file->driver_priv = pf;
	return 0;
}

void poulsback_file_close(struct drm_device *dev, struct drm_file *file)
{
	struct poulsback_file *pf = file->driver_priv;
	if (!pf)
		return;
	file->driver_priv = NULL;
	/* Entities flush their queued jobs before the address space goes. */
	poulsback_exec_file_destroy(pf->exec);
	poulsback_vm_destroy(pf->vm);
	kfree(pf);
}

struct poulsback_vm *poulsback_file_vm(struct drm_file *file)
{
	struct poulsback_file *pf = file ? file->driver_priv : NULL;
	return pf ? pf->vm : NULL;
}

struct poulsback_exec_file *poulsback_file_exec(struct drm_file *file)
{
	struct poulsback_file *pf = file ? file->driver_priv : NULL;
	return pf ? pf->exec : NULL;
}
