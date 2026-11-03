// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_bo.h"
#include "poulsback_bo_use.h"
#include "poulsback_bo_uapi.h"
#include "poulsback_errsite.h"
#include "poulsback_file.h"
#include "poulsback_vm.h"
#include "poulsback_vm_layout.h"

#include <asm/set_memory.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/fdtable.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/mutex.h>
#include <linux/slab.h>

/* All BOs of the device together; the Dell has 1 GiB. */
#define BO_TOTAL_BYTES (256ULL << 20)

static DEFINE_MUTEX(bo_account_lock);
static u64 bo_total_bytes;

/* Pages stay pinned for the BO's life. Each mapping is the BO's VA in one
 * file's address space; it exists while that file holds a handle. */
struct poulsback_bo {
	struct drm_gem_object base;
	struct page **pages;
	u32 count, flags, vm_class, display_pins;
	u64 alignment;
	struct mutex lock;
	struct list_head mappings;
};

struct bo_mapping {
	struct list_head link;
	struct poulsback_vm *vm;
	struct drm_mm_node node;
	/* Its handles and jobs; kept for good (HOLD) with a reference on the BO. */
	struct poulsback_bo_use use;
};

static const struct drm_gem_object_funcs bo_funcs;

static struct poulsback_bo *to_bo(struct drm_gem_object *obj)
{
	return obj && obj->funcs == &bo_funcs ? container_of(obj, struct poulsback_bo, base) : NULL;
}

bool poulsback_bo_is_object(const struct drm_gem_object *obj)
{
	return obj && obj->funcs == &bo_funcs;
}

bool poulsback_bo_is_scanout(const struct drm_gem_object *obj)
{
	return poulsback_bo_is_object(obj) &&
	       (container_of(obj, struct poulsback_bo, base)->flags & POULSBACK_BO_SCANOUT);
}

int poulsback_bo_display_pin(struct drm_gem_object *obj, struct page ***pages, u32 *count)
{
	struct poulsback_bo *bo = poulsback_bo_is_scanout(obj) ? to_bo(obj) : NULL;
	int ret = 0;
	if (!bo)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&bo->lock);
	if (bo->display_pins == U32_MAX)
		ret = POULSBACK_ERRSITE(-EOVERFLOW);
	else
		WRITE_ONCE(bo->display_pins, bo->display_pins + 1);
	mutex_unlock(&bo->lock);
	if (ret)
		return ret;
	drm_gem_object_get(obj);
	*pages = bo->pages;
	*count = bo->count;
	return 0;
}

void poulsback_bo_display_unpin(struct drm_gem_object *obj)
{
	struct poulsback_bo *bo = to_bo(obj);
	mutex_lock(&bo->lock);
	if (!WARN_ON(!bo->display_pins))
		WRITE_ONCE(bo->display_pins, bo->display_pins - 1);
	mutex_unlock(&bo->lock);
	drm_gem_object_put(obj);
}

int poulsback_bo_display_check(struct drm_gem_object *obj, struct page **pages, u32 count)
{
	struct poulsback_bo *bo = poulsback_bo_is_scanout(obj) ? to_bo(obj) : NULL;
	/* Lockless: callers hold their own pin and may hold GTT/source locks. */
	if (!bo || bo->pages != pages || bo->count != count || !(bo->flags & POULSBACK_BO_CPU_WC) ||
	    !READ_ONCE(bo->display_pins))
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}

static struct bo_mapping *bo_mapping(struct poulsback_bo *bo, struct poulsback_vm *vm)
{
	struct bo_mapping *m;
	list_for_each_entry(m, &bo->mappings, link) if (m->vm == vm) return m;
	return NULL;
}

static int bo_open(struct drm_gem_object *obj, struct drm_file *file)
{
	struct poulsback_bo *bo = to_bo(obj);
	struct poulsback_vm *vm = poulsback_file_vm(file);
	struct bo_mapping *m;
	int ret = 0;
	if (!vm)
		return POULSBACK_ERRSITE(-ENODEV);
	mutex_lock(&bo->lock);
	m = bo_mapping(bo, vm);
	if (m) {
		m->use.handles++;
		goto unlock;
	}
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		ret = -ENOMEM;
		goto unlock;
	}
	ret = poulsback_vm_map(vm, bo->vm_class, bo->alignment, bo->pages, bo->count,
			       !(bo->flags & POULSBACK_BO_CPU_WC), &m->node);
	if (ret) {
		kfree(m);
		goto unlock;
	}
	m->vm = vm;
	m->use.handles = 1;
	list_add(&m->link, &bo->mappings);
unlock:
	mutex_unlock(&bo->lock);
	return ret;
}

/* DRM releases a file's handles before its contexts (postclose), so a job may
 * still use this BO through the file's address space: wait for every job that
 * holds it (their READ and WRITE fences sit in its reservation) before
 * unmapping. The display's BOOKKEEP fence is not waited for: it reads through
 * the GTT, not this VA. A BO still busy after the wait (a hung engine), or one
 * whose address space the loss froze, keeps its mapping and pages for good
 * (HOLD): it is never unmapped under the GPU. */
#define BO_CLOSE_WAIT (10 * HZ)

/* The mapping's last holder is gone: unmap it, or keep it for good (HOLD). */
static void bo_unmap(struct poulsback_bo *bo, struct bo_mapping *m, bool idle)
{
	if (idle && poulsback_vm_unmap(m->vm, &m->node)) {
		list_del(&m->link);
		kfree(m);
	} else {
		m->use.kept = 1;
		drm_gem_object_get(&bo->base);
		if (!idle)
			(void)POULSBACK_ERRSITE(-EBUSY);
	}
}

static void bo_close(struct drm_gem_object *obj, struct drm_file *file)
{
	struct poulsback_bo *bo = to_bo(obj);
	long idle = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_READ, false, BO_CLOSE_WAIT);
	struct bo_mapping *m;
	mutex_lock(&bo->lock);
	m = bo_mapping(bo, poulsback_file_vm(file));
	if (!WARN_ON(!m) && poulsback_bo_use_close(&m->use))
		bo_unmap(bo, m, idle > 0);
	mutex_unlock(&bo->lock);
}

static vm_fault_t bo_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct poulsback_bo *bo = to_bo(vma->vm_private_data);
	unsigned long index = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	if (!bo || index >= bo->count)
		return VM_FAULT_SIGBUS;
	return vmf_insert_pfn(vma, vmf->address, page_to_pfn(bo->pages[index]));
}

static const struct vm_operations_struct bo_vm_ops = {
	.fault = bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct poulsback_bo *bo = to_bo(obj);
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (bo->flags & POULSBACK_BO_CPU_WC)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_ops = &bo_vm_ops;
	return 0;
}

static void bo_free(struct drm_gem_object *obj)
{
	struct poulsback_bo *bo = to_bo(obj);
	WARN_ON(!list_empty(&bo->mappings));
	if (bo->pages) {
		int ret = 0;
		if (bo->flags & POULSBACK_BO_CPU_WC)
			ret = set_pages_array_wb(bo->pages, bo->count);
		/* Pages whose write-back attribute was not restored never return to
		 * the allocator: HOLD, as the patched gma500 unpin does. */
		if (ret)
			drm_err(obj->dev, "poulsback: BO pages held, set_pages_array_wb %d\n",
				POULSBACK_ERRSITE(ret));
		else
			drm_gem_put_pages(obj, bo->pages, true, false);
	}
	drm_gem_free_mmap_offset(obj);
	drm_gem_object_release(obj);
	mutex_lock(&bo_account_lock);
	bo_total_bytes -= obj->size;
	mutex_unlock(&bo_account_lock);
	mutex_destroy(&bo->lock);
	kfree(bo);
}

/* Exported BOs carry their own ops so that imports can recognise them; the
 * callbacks are DRM's GEM helpers. */
static const struct dma_buf_ops bo_dmabuf_ops = {
	.cache_sgt_mapping = true,
	.attach = drm_gem_map_attach,
	.detach = drm_gem_map_detach,
	.map_dma_buf = drm_gem_map_dma_buf,
	.unmap_dma_buf = drm_gem_unmap_dma_buf,
	.release = drm_gem_dmabuf_release,
	.mmap = drm_gem_dmabuf_mmap,
	.vmap = drm_gem_dmabuf_vmap,
	.vunmap = drm_gem_dmabuf_vunmap,
};

static struct dma_buf *bo_export(struct drm_gem_object *obj, int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	info.ops = &bo_dmabuf_ops;
	info.size = obj->size;
	info.flags = flags;
	info.priv = obj;
	info.resv = obj->resv;
	return drm_gem_dmabuf_export(obj->dev, &info);
}

static struct sg_table *bo_get_sg_table(struct drm_gem_object *obj)
{
	struct poulsback_bo *bo = to_bo(obj);
	return drm_prime_pages_to_sg(obj->dev, bo->pages, bo->count);
}

static const struct drm_gem_object_funcs bo_funcs = {
	.free = bo_free,
	.open = bo_open,
	.close = bo_close,
	.export = bo_export,
	.get_sg_table = bo_get_sg_table,
	.mmap = bo_mmap,
	.vm_ops = &bo_vm_ops,
};

/* PRIME entry points of the driver: BOs go through DRM's helpers (an import
 * is the same object, mapped into the importing file by bo_open); every other
 * object (gma500's own) and foreign dma-bufs are refused. */
static bool bo_dmabuf(struct drm_device *dev, struct dma_buf *buf)
{
	return buf->ops == &bo_dmabuf_ops && ((struct drm_gem_object *)buf->priv)->dev == dev;
}

int poulsback_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file, u32 handle,
				 u32 flags, int *fd)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);
	struct dma_buf *buf;
	bool bo = to_bo(obj);
	int ret;
	if (obj)
		drm_gem_object_put(obj);
	if (!bo)
		return POULSBACK_REFUSE(-EOPNOTSUPP);
	ret = drm_gem_prime_handle_to_fd(dev, file, handle, flags, fd);
	if (ret)
		return ret;
	/* The handle may have been replaced since the lookup. */
	buf = dma_buf_get(*fd);
	if (IS_ERR(buf) || !bo_dmabuf(dev, buf))
		ret = POULSBACK_REFUSE(-EOPNOTSUPP);
	if (!IS_ERR(buf))
		dma_buf_put(buf);
	if (ret)
		close_fd(*fd);
	return ret;
}

int poulsback_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file, int fd, u32 *handle)
{
	struct dma_buf *buf = dma_buf_get(fd);
	bool bo;
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	bo = bo_dmabuf(dev, buf);
	dma_buf_put(buf);
	return bo ? drm_gem_prime_fd_to_handle(dev, file, fd, handle)
		  : POULSBACK_REFUSE(-EOPNOTSUPP);
}

struct drm_gem_object *poulsback_prime_import(struct drm_device *dev, struct dma_buf *buf)
{
	struct drm_gem_object *obj = buf->priv;
	if (!bo_dmabuf(dev, buf))
		return ERR_PTR(POULSBACK_REFUSE(-EOPNOTSUPP));
	drm_gem_object_get(obj);
	return obj;
}

static int bo_fill(struct poulsback_bo *bo, struct drm_file *file, u64 *gpu_va, u64 *mmap_offset)
{
	struct bo_mapping *m;
	mutex_lock(&bo->lock);
	m = bo_mapping(bo, poulsback_file_vm(file));
	*gpu_va = m ? m->node.start : 0;
	mutex_unlock(&bo->lock);
	*mmap_offset = drm_vma_node_offset_addr(&bo->base.vma_node);
	return m ? 0 : POULSBACK_ERRSITE(-ENOENT);
}

int poulsback_bo_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_bo_create *args = data;
	struct poulsback_vm *vm = poulsback_file_vm(file);
	struct poulsback_bo *bo;
	u64 size, alignment;
	u32 i, handle;
	int ret, idx;
	BUILD_BUG_ON(POULSBACK_BO_CLASS_GENERAL != POULSBACK_VM_GENERAL ||
		     POULSBACK_BO_CLASS_PDS != POULSBACK_VM_PDS ||
		     POULSBACK_BO_CLASS_CODE != POULSBACK_VM_CODE ||
		     POULSBACK_BO_CLASS_DEPTH != POULSBACK_VM_DEPTH ||
		     POULSBACK_BO_CLASS_2D != POULSBACK_VM_2D ||
		     POULSBACK_BO_CLASS_PARAMETER != POULSBACK_VM_PARAMETER);
	if (args->abi != POULSBACK_BO_ABI ||
	    (args->flags & ~(POULSBACK_BO_CPU_WC | POULSBACK_BO_SCANOUT)) || args->handle ||
	    args->gpu_va || args->mmap_offset)
		return POULSBACK_REFUSE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(args->reserved); i++)
		if (args->reserved[i])
			return POULSBACK_REFUSE(-EINVAL);
	switch (poulsback_vm_check_bo(args->bo_class, args->size, &size, &alignment)) {
	case POULSBACK_VM_OK:
		break;
	case POULSBACK_VM_TOO_BIG:
		return POULSBACK_REFUSE(-E2BIG);
	default:
		return POULSBACK_REFUSE(-EINVAL);
	}
	/* A scanout BO is WC for its life: the display reads it uncached and no
	 * WB alias ever exists, so no cache flush is needed before a show. */
	if ((args->flags & POULSBACK_BO_SCANOUT) &&
	    (!(args->flags & POULSBACK_BO_CPU_WC) ||
	     !poulsback_vm_scanout_ok(args->bo_class, size)))
		return POULSBACK_REFUSE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	/* After the loss a file whose address space the GPU may still walk
	 * creates no BO (docs/kernel/execution.md, "The loss"); a file opened
	 * later does, in a space no BIF slot names (a restarted X's front). */
	if (vm && poulsback_vm_held(vm)) {
		ret = POULSBACK_ERRSITE(-EIO);
		goto exit;
	}
	mutex_lock(&bo_account_lock);
	if (size > BO_TOTAL_BYTES - bo_total_bytes) {
		mutex_unlock(&bo_account_lock);
		ret = POULSBACK_ERRSITE(-ENOSPC);
		goto exit;
	}
	bo_total_bytes += size;
	mutex_unlock(&bo_account_lock);
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo) {
		mutex_lock(&bo_account_lock);
		bo_total_bytes -= size;
		mutex_unlock(&bo_account_lock);
		ret = -ENOMEM;
		goto exit;
	}
	bo->base.funcs = &bo_funcs;
	bo->count = size >> PAGE_SHIFT;
	bo->flags = args->flags;
	bo->vm_class = args->bo_class;
	bo->alignment = alignment;
	mutex_init(&bo->lock);
	INIT_LIST_HEAD(&bo->mappings);
	/* On failure the object is still initialised; bo_free releases it. */
	ret = drm_gem_object_init(dev, &bo->base, size);
	if (ret)
		goto put;
	mapping_set_gfp_mask(bo->base.filp->f_mapping, GFP_USER | __GFP_DMA32);
	bo->pages = drm_gem_get_pages(&bo->base);
	if (IS_ERR(bo->pages)) {
		ret = PTR_ERR(bo->pages);
		bo->pages = NULL;
		goto put;
	}
	if ((bo->flags & POULSBACK_BO_CPU_WC) && set_pages_array_wc(bo->pages, bo->count)) {
		ret = POULSBACK_ERRSITE(-ENOMEM);
		bo->flags &= ~POULSBACK_BO_CPU_WC;
		goto put;
	}
	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret)
		goto put;
	ret = drm_gem_handle_create(file, &bo->base, &handle);
	if (ret)
		goto put;
	ret = bo_fill(bo, file, &args->gpu_va, &args->mmap_offset);
	if (!ret)
		args->handle = handle;
	else
		drm_gem_handle_delete(file, handle);
put:
	drm_gem_object_put(&bo->base);
exit:
	drm_dev_exit(idx);
	return ret;
}

int poulsback_bo_resolve(struct drm_file *file, u32 handle, struct drm_gem_object **out, u64 *va,
			 u64 *size)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);
	struct poulsback_bo *bo = to_bo(obj);
	struct bo_mapping *m;
	if (!bo) {
		if (obj)
			drm_gem_object_put(obj);
		return POULSBACK_REFUSE(-ENOENT);
	}
	mutex_lock(&bo->lock);
	m = bo_mapping(bo, poulsback_file_vm(file));
	if (m) {
		*va = m->node.start;
		m->use.jobs++;
	}
	mutex_unlock(&bo->lock);
	if (!m) {
		drm_gem_object_put(obj);
		return POULSBACK_REFUSE(-ENOENT);
	}
	*size = obj->size;
	*out = obj;
	return 0;
}

void poulsback_bo_release(struct drm_gem_object *obj, struct poulsback_vm *vm)
{
	struct poulsback_bo *bo = to_bo(obj);
	struct bo_mapping *m;
	mutex_lock(&bo->lock);
	m = bo_mapping(bo, vm);
	/* No job of this file holds it any more, so the GPU is done with the VA. */
	if (!WARN_ON(!m) && poulsback_bo_use_release(&m->use))
		bo_unmap(bo, m, true);
	mutex_unlock(&bo->lock);
	drm_gem_object_put(obj);
}

int poulsback_bo_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_bo_info *args = data;
	struct drm_gem_object *obj;
	struct poulsback_bo *bo;
	u32 i;
	int ret;
	if (args->abi != POULSBACK_BO_ABI || args->flags || args->bo_class || args->size ||
	    args->gpu_va || args->mmap_offset)
		return POULSBACK_REFUSE(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(args->reserved); i++)
		if (args->reserved[i])
			return POULSBACK_REFUSE(-EINVAL);
	obj = drm_gem_object_lookup(file, args->handle);
	bo = to_bo(obj);
	if (!bo) {
		ret = POULSBACK_REFUSE(-ENOENT);
		goto put;
	}
	ret = bo_fill(bo, file, &args->gpu_va, &args->mmap_offset);
	if (!ret) {
		args->size = obj->size;
		args->flags = bo->flags;
		args->bo_class = bo->vm_class;
	}
put:
	if (obj)
		drm_gem_object_put(obj);
	return ret;
}

/* dma_buf_get/put in the PRIME hooks. */
MODULE_IMPORT_NS(DMA_BUF);
