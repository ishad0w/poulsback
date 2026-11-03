// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Display lifetime of SCANOUT BOs shown through the provider's GTT window. */
#include <linux/dma-fence.h>
#include "poulsback_errsite.h"
#include <linux/dma-resv.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include "poulsback_bo.h"
#include "poulsback_display.h"
#include "sgx535_page_validate.h"

struct display_fence {
	struct dma_fence base;
	spinlock_t lock;
};
static const char *fence_driver(struct dma_fence *f)
{
	(void)f;
	return "poulsback";
}
static const char *fence_timeline(struct dma_fence *f)
{
	(void)f;
	return "display-scanout";
}
static void fence_release(struct dma_fence *f)
{
	/* Reservation readers can acquire refs under RCU after domain teardown.
	 * The base is first; generic deferred free owns this allocation. */
	dma_fence_free(f);
	module_put(THIS_MODULE);
}
static const struct dma_fence_ops fence_ops = {
	.use_64bit_seqno = true,
	.get_driver_name = fence_driver,
	.get_timeline_name = fence_timeline,
	.release = fence_release,
};

static void hold_images(struct poulsback_display_domain *d)
{
	u32 i;
	d->held = true;
	for (i = 0; i < ARRAY_SIZE(d->images); i++) {
		struct poulsback_display_image *m = d->extents[i].owner;
		if (!m || !m->possible)
			continue;
		m->phase = POULSBACK_DISPLAY_HOLD;
		m->pin.held = true;
	}
}
void poulsback_display_switch_hold_locked(struct poulsback_display_domain *d, int error)
{
	u32 i;

	lockdep_assert_held(&d->lock);
	d->held = true;
	if (!d->primary_error)
		d->primary_error = error;
	for (i = 0; i < ARRAY_SIZE(d->images); i++) {
		struct poulsback_display_image *m = d->images[i];
		if (!m)
			continue;
		m->phase = POULSBACK_DISPLAY_HOLD;
		m->pin.held = true;
		if (!m->primary_error)
			m->primary_error = error;
	}
}
static int source_binding(struct poulsback_display_domain *d)
{
	const struct poulsback_display_provider *p = d->provider, *b = &d->bound_provider;
	if (!d->initialized || !p)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (d->generation != d->bound_generation)
		return POULSBACK_ERRSITE(-ESTALE);
	if (d->held)
		return POULSBACK_ERRSITE(-EIO);
	if (d->gtt_identity != d->bound_gtt_identity || d->dummy != d->bound_dummy ||
	    d->aperture.start != d->bound_start || d->aperture.end != d->bound_end ||
	    d->aperture.flags || d->aperture.parent || d->aperture.sibling)
		return POULSBACK_ERRSITE(-EXDEV);
	if (p->identity != b->identity || p->publish_cache != b->publish_cache ||
	    p->insert != b->insert || p->remove != b->remove || p->flush != b->flush ||
	    p->flip != b->flip || p->retirement != b->retirement || p->context != b->context)
		return POULSBACK_ERRSITE(-EXDEV);
	return 0;
}
static int source_valid(struct poulsback_display_domain *d)
{
	int ret = source_binding(d);
	if (ret)
		return ret;
	ret = d->bound_provider.identity(d);
	/* identity may sleep. Never authorize another callback after recorded loss
	 * or a changed tuple even if identity itself returned optimistic success. */
	return ret ? ret : source_binding(d);
}
static int step(struct poulsback_display_domain *d)
{
	int ret = source_valid(d);
	if (ret) {
		if (!d->primary_error)
			d->primary_error = ret;
		hold_images(d);
	}
	return ret;
}
/* Caller holds domain mutex. Retain DRM SRCU across every sleeping provider
 * operation so removal cannot free private/register/GTT state mid-callback. */
static int enter_device(struct poulsback_display_domain *d, int *idx)
{
	if (!d->initialized)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (!drm_dev_enter(d->dev, idx)) {
		hold_images(d);
		return POULSBACK_ERRSITE(-ENODEV);
	}
	return 0;
}
/* Authenticate each original node and the complete bounded resource topology.
 * This is pure local validation; it grants no hardware release certificate. */
static int reservations_valid(struct poulsback_display_domain *d, struct poulsback_display_image *m)
{
	struct poulsback_display_image *ordered[ARRAY_SIZE(d->images)];
	u64 bytes = 0;
	u32 i, count = 0, tracked = 0, found = 0;
	for (i = 0; i < ARRAY_SIZE(d->images); i++) {
		const struct poulsback_display_extent *e = &d->extents[i];
		struct poulsback_display_image *item = e->owner;
		u32 at;
		/* Compare the retained identity before dereferencing a mutable slot. */
		if (d->images[i] != item)
			return POULSBACK_ERRSITE(-ESTALE);
		if (!item) {
			if (e->object || e->reserved || e->start || e->end)
				return POULSBACK_ERRSITE(-ESTALE);
			continue;
		}
		if (item->domain != d || item->object != e->object || !item->tracked ||
		    !item->object_ref || !e->object || item->pin.object != e->object ||
		    item->reserved != e->reserved || e->object->size > 64ULL * 1024 * 1024 - bytes)
			return POULSBACK_ERRSITE(-ESTALE);
		bytes += e->object->size;
		tracked++;
		if (item == m)
			found++;
		if (!e->reserved)
			continue;
		if (item->gtt.start != e->start || item->gtt.end != e->end || item->gtt.flags ||
		    item->gtt.parent != &d->aperture || item->gtt.child ||
		    e->start < d->bound_start || e->end > d->bound_end || e->end < e->start ||
		    (e->start & (POULSBACK_DISPLAY_SLOT_ALIGN - 1)) ||
		    resource_size(&item->gtt) != e->object->size)
			return POULSBACK_ERRSITE(-ESTALE);
		at = count;
		while (at && ordered[at - 1]->gtt.start > e->start) {
			ordered[at] = ordered[at - 1];
			at--;
		}
		ordered[at] = item;
		count++;
	}
	if ((m && (found != 1 || !m->reserved)) || tracked != d->count || bytes != d->bytes ||
	    d->aperture.child != (count ? &ordered[0]->gtt : NULL))
		return POULSBACK_ERRSITE(-ESTALE);
	for (i = 0; i < count; i++)
		if (ordered[i]->gtt.sibling != (i + 1 < count ? &ordered[i + 1]->gtt : NULL) ||
		    (i && ordered[i - 1]->gtt.end >= ordered[i]->gtt.start))
			return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
static int reservation_valid(struct poulsback_display_image *m)
{
	return reservations_valid(m->domain, m);
}
static int domain_reservations_step(struct poulsback_display_domain *d)
{
	int ret = reservations_valid(d, NULL);
	if (ret) {
		if (!d->primary_error)
			d->primary_error = ret;
		hold_images(d);
	}
	return ret;
}
/* An image's own state and the domain's source binding, without stepping the
 * domain: for a caller that stepped it just before under the same lock. */
static int image_checked(struct poulsback_display_image *m)
{
	int ret = reservation_valid(m);
	if (ret)
		return ret;
	ret = poulsback_bo_display_check(m->pin.object, m->pin.pages, m->pin.count);
	if (ret)
		return ret;
	if (m->pin.object != m->object || m->object->dev != m->domain->dev ||
	    m->mapped > m->pin.count || !m->tracked || !m->object_ref)
		return POULSBACK_ERRSITE(-EXDEV);
	if (m->layout.size != m->object->size || m->pin.size != m->object->size || !m->reserved ||
	    m->gtt.parent != &m->domain->aperture || resource_size(&m->gtt) != m->pin.size)
		return POULSBACK_ERRSITE(-ESTALE);
	return source_binding(m->domain);
}
static int image_valid(struct poulsback_display_image *m)
{
	int ret = image_checked(m);
	if (!ret)
		ret = step(m->domain);
	/* identity may sleep. Recheck the frozen reservation after it,
	 * immediately before the next provider callback. */
	if (!ret)
		ret = source_binding(m->domain);
	return ret ? ret : reservation_valid(m);
}

static int track_image(struct poulsback_display_domain *d, struct poulsback_display_image *m)
{
	u32 slot;
	unsigned long flags;
	int ret = domain_reservations_step(d);
	if (ret)
		return ret;
	for (slot = 0; slot < ARRAY_SIZE(d->images) && d->images[slot]; slot++) {
	}
	if (slot == ARRAY_SIZE(d->images))
		return POULSBACK_REFUSE(-ENOSPC);
	drm_gem_object_get(m->object);
	m->object_ref = true;
	spin_lock_irqsave(&d->source_lock, flags);
	d->images[slot] = m;
	d->extents[slot] = (struct poulsback_display_extent){.owner = m, .object = m->object};
	d->count++;
	d->bytes += m->object->size;
	m->tracked = true;
	spin_unlock_irqrestore(&d->source_lock, flags);
	return 0;
}
static int tracking_slot(struct poulsback_display_image *m, u32 *found)
{
	struct poulsback_display_domain *d = m->domain;
	u64 bytes = 0;
	u32 slot, count = 0, matches = 0;
	for (slot = 0; slot < ARRAY_SIZE(d->images); slot++) {
		struct poulsback_display_image *item = d->images[slot];
		if (item != d->extents[slot].owner)
			return POULSBACK_ERRSITE(-ESTALE);
		if (!item)
			continue;
		if (item->domain != d || !item->tracked || !item->object_ref || !item->object ||
		    item->object->size > 64ULL * 1024 * 1024 - bytes)
			return POULSBACK_ERRSITE(-ESTALE);
		bytes += item->object->size;
		count++;
		if (item == m) {
			*found = slot;
			matches++;
		}
	}
	if (matches != 1 || !m->tracked || count != d->count || bytes != d->bytes)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
/* Domain mutex protects the preflight through this no-fail commit. No provider
 * callback may run after pin release; IRQ loss changes no tracker metadata. */
static void untrack_commit(struct poulsback_display_image *m, u32 slot)
{
	struct poulsback_display_domain *d = m->domain;
	unsigned long flags;
	spin_lock_irqsave(&d->source_lock, flags);
	d->images[slot] = NULL;
	memset(&d->extents[slot], 0, sizeof(d->extents[slot]));
	d->count--;
	d->bytes -= m->object->size;
	m->tracked = false;
	spin_unlock_irqrestore(&d->source_lock, flags);
}

int poulsback_display_domain_init(struct poulsback_display_domain *d, struct drm_device *dev,
				  const struct poulsback_display_provider *p, void *gtt_identity,
				  struct page *dummy, u64 generation, u64 start, u64 size)
{
	int ret, idx;
	if (!d || !dev || !gtt_identity || !dummy || !generation || !size ||
	    (start & (PAGE_SIZE - 1)) || (size & (PAGE_SIZE - 1)) || start > U32_MAX ||
	    size > (u64)U32_MAX + 1 - start)
		return POULSBACK_ERRSITE(-EINVAL);
	if (d->initialized || d->dev || d->count || d->bytes || d->active || d->pending ||
	    memchr_inv(d->images, 0, sizeof(d->images)) ||
	    memchr_inv(d->extents, 0, sizeof(d->extents)))
		return -EBUSY;
	if (!p || !p->identity || !p->publish_cache || !p->insert || !p->remove || !p->flush ||
	    !p->flip || !p->retirement)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (!try_module_get(THIS_MODULE))
		return POULSBACK_ERRSITE(-ENODEV);
	drm_dev_get(dev);
	d->dev = dev;
	d->provider = p;
	d->bound_provider = *p;
	d->gtt_identity = d->bound_gtt_identity = gtt_identity;
	d->dummy = d->bound_dummy = dummy;
	d->generation = d->bound_generation = generation;
	d->fence_context = dma_fence_context_alloc(1);
	/* Display GTT offsets are virtual ranges, not host e820 MMIO resources. */
	d->aperture = (struct resource){.start = start,
					.end = start + size - 1,
					.flags = 0,
					.name = "poulsback-display-owned-gtt"};
	d->bound_start = start;
	d->bound_end = start + size - 1;
	mutex_init(&d->lock);
	spin_lock_init(&d->source_lock);
	d->initialized = true;
	ret = enter_device(d, &idx);
	if (!ret) {
		ret = source_valid(d);
		drm_dev_exit(idx);
	}
	if (ret) {
		memset(d, 0, sizeof(*d));
		drm_dev_put(dev);
		module_put(THIS_MODULE);
	}
	return ret;
}

/* Bounded wait for the writers of a BO about to be shown. */
#define DISPLAY_WRITE_WAIT (2 * HZ)

int poulsback_display_wait_writers(struct drm_gem_object *obj)
{
	struct dma_resv_iter cursor;
	struct dma_fence *writer;
	long idle =
		dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_WRITE, true, DISPLAY_WRITE_WAIT);
	int ret;
	if (idle < 0)
		return idle;
	if (!idle)
		return POULSBACK_ERRSITE(-EBUSY);
	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		return ret;
	/* The unlocked wait skips already-signalled failed fences. Under this
	 * lock inspect every retained writer: a failed one refuses the show,
	 * except one lost with the GPU (-EIO, docs/kernel/execution.md "The
	 * loss"): the pages stay pinned and readable, and only their contents
	 * are undefined, so the display shows what is there. */
	dma_resv_for_each_fence(&cursor, obj->resv, DMA_RESV_USAGE_WRITE, writer)
	{
		int status = dma_fence_get_status(writer);
		if (status < 0 && status != -EIO) {
			ret = status;
			break;
		}
	}
	dma_resv_unlock(obj->resv);
	return ret;
}

/* Add the display's BOOKKEEP fence under the reservation lock. The caller
 * waits for writers before each show (poulsback_display_wait_writers). */
static int reserve_display(struct poulsback_display_image *m)
{
	struct dma_resv *resv = m->object->resv;
	struct display_fence *f;
	int ret;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	if (!try_module_get(THIS_MODULE)) {
		kfree(f);
		return POULSBACK_ERRSITE(-ENODEV);
	}
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &fence_ops, &f->lock, m->domain->fence_context,
		       ++m->domain->serial);
	ret = dma_resv_lock(resv, NULL);
	if (ret)
		goto put;
	ret = dma_resv_reserve_fences(resv, 1);
	if (!ret) {
		dma_resv_add_fence(resv, &f->base, DMA_RESV_USAGE_BOOKKEEP);
		m->read_fence = &f->base;
	}
	dma_resv_unlock(resv);
put:
	if (ret)
		dma_fence_put(&f->base);
	return ret;
}

int poulsback_display_prepare(struct poulsback_display_domain *d, struct drm_gem_object *obj,
			      const struct poulsback_display_layout *layout,
			      struct poulsback_display_image *m)
{
	u32 i, slot = 0;
	int ret, idx;
	bool entered = false;
	unsigned long flags;
	if (!d || !m || !layout || !poulsback_bo_is_scanout(obj) || obj->dev != d->dev)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!d->initialized)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	mutex_lock(&d->lock);
	ret = enter_device(d, &idx);
	if (ret)
		goto out;
	entered = true;
	if (memchr_inv(m, 0, sizeof(*m))) {
		ret = -EBUSY;
		goto out;
	}
	/* Do not traverse a changed resource chain or overwrite a lost mutable
	 * tracker. The canonical ledger must survive even this unpublished refusal. */
	ret = domain_reservations_step(d);
	if (ret)
		goto out;
	ret = step(d);
	if (ret)
		goto out;
	if (d->count >= ARRAY_SIZE(d->images) || d->bytes > 64ULL * 1024 * 1024 ||
	    obj->size > 64ULL * 1024 * 1024 - d->bytes || d->serial == U64_MAX) {
		ret = POULSBACK_REFUSE(-ENOSPC);
		goto out;
	}
	m->layout = *layout;
	if (!m->layout.width || !m->layout.height || m->layout.stride & 63 ||
	    m->layout.stride < (u64)m->layout.width * 4 ||
	    (u64)m->layout.stride * m->layout.height > obj->size || m->layout.size != obj->size ||
	    (m->layout.format != DRM_FORMAT_ARGB8888 && m->layout.format != DRM_FORMAT_XRGB8888)) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto clear;
	}
	ret = poulsback_bo_display_pin(obj, &m->pin.pages, &m->pin.count);
	if (ret)
		goto clear;
	m->pin.object = obj;
	m->pin.size = obj->size;
	m->domain = d;
	m->object = obj;
	ret = track_image(d, m);
	if (ret)
		goto unpin;
	for (i = 0; i < m->pin.count; i++) {
		if (!m->pin.pages[i] || !sgx535_page_addressable(page_to_phys(m->pin.pages[i]),
								 page_to_phys(d->dummy))) {
			ret = POULSBACK_ERRSITE(-EOPNOTSUPP);
			goto unpin;
		}
	}
	ret = allocate_resource(&d->aperture, &m->gtt, obj->size, d->aperture.start,
				d->aperture.end, POULSBACK_DISPLAY_SLOT_ALIGN, NULL, NULL);
	if (ret)
		goto unpin;
	m->reserved = true;
	for (i = 0; i < ARRAY_SIZE(d->images); i++)
		if (d->extents[i].owner == m) {
			d->extents[i].start = m->gtt.start;
			d->extents[i].end = m->gtt.end;
			d->extents[i].reserved = true;
			break;
		}
	/* Sleeping allocation can race source/owner loss; repeat before any writer. */
	ret = image_valid(m);
	if (ret)
		goto unreserve;
	ret = reserve_display(m);
	if (ret)
		goto unreserve;
	ret = image_valid(m);
	if (ret)
		goto unreserve;
	spin_lock_irqsave(&d->source_lock, flags);
	/* Mark possible BEFORE the first cache/GTT writer, not after its return. */
	m->possible = m->pin.mapping_owned = true;
	spin_unlock_irqrestore(&d->source_lock, flags);
	ret = d->bound_provider.publish_cache(m);
	if (ret)
		goto hold;
	ret = image_valid(m);
	if (ret)
		goto hold;
	for (i = 0; i < m->pin.count; i++) {
		ret = d->bound_provider.insert(m, m->pin.pages[i],
					       m->gtt.start + (u64)i * PAGE_SIZE);
		if (ret)
			goto hold;
		m->mapped++;
		ret = image_valid(m);
		if (ret)
			goto hold;
	}
	ret = d->bound_provider.flush(d);
	if (ret)
		goto hold;
	ret = image_valid(m);
	if (ret)
		goto hold;
	spin_lock_irqsave(&d->source_lock, flags);
	m->phase = POULSBACK_DISPLAY_MAPPED;
	spin_unlock_irqrestore(&d->source_lock, flags);
	goto out;
hold:
	m->primary_error = ret;
	if (!d->primary_error)
		d->primary_error = ret;
	hold_images(d);
	goto out;
unreserve:
	/* A changed local reservation is not rollback permission, even before
	 * cache publication. Retain its pin/fence and complete tracker witness. */
	m->cleanup_error = reservation_valid(m);
	if (m->cleanup_error) {
		m->phase = POULSBACK_DISPLAY_HOLD;
		m->primary_error = ret;
		d->held = true;
		m->pin.held = true;
		hold_images(d);
		goto out;
	}
	/* No cache/PTE writer has run. Complete the unused dependency as a no-op. */
	if (m->read_fence) {
		dma_fence_signal(m->read_fence);
		dma_fence_put(m->read_fence);
		m->read_fence = NULL;
	}
	if (m->reserved) {
		int cleanup = release_resource(&m->gtt);
		if (cleanup) {
			/* Retain the canonical pin on an uncertain local reservation. */
			m->phase = POULSBACK_DISPLAY_HOLD;
			m->primary_error = ret;
			m->cleanup_error = cleanup;
			d->held = true;
			m->pin.held = true;
			goto out;
		}
		m->reserved = false;
		for (i = 0; i < ARRAY_SIZE(d->images); i++)
			if (d->extents[i].owner == m)
				d->extents[i].reserved = false;
	}
unpin:
	if (m->tracked && (m->cleanup_error = tracking_slot(m, &slot))) {
		m->phase = POULSBACK_DISPLAY_HOLD;
		m->primary_error = ret;
		d->held = true;
		goto out;
	}
	if (m->pin.object)
		poulsback_bo_display_unpin(m->pin.object);
	if (m->tracked)
		untrack_commit(m, slot);
	if (m->object_ref)
		drm_gem_object_put(obj);
clear:
	memset(m, 0, sizeof(*m));
out:
	if (entered)
		drm_dev_exit(idx);
	mutex_unlock(&d->lock);
	return ret;
}

static int flip_locked(struct poulsback_display_domain *d, struct poulsback_display_image *next)
{
	struct poulsback_display_image *old = d->active;
	unsigned long flags;
	bool showable = false;
	u64 serial;
	int ret = step(d);
	if (ret)
		return ret;
	if (d->transitioning || d->serial == U64_MAX)
		return -EBUSY;
	if (next == old)
		return next ? -EALREADY : 0;
	/* A replaced (PENDING) image may be shown again before it retires. */
	if (next)
		showable = next->phase == POULSBACK_DISPLAY_MAPPED ||
			   next->phase == POULSBACK_DISPLAY_PENDING;
	if (next && (next->domain != d || !showable || !next->read_fence ||
		     dma_fence_is_signaled(next->read_fence)))
		return POULSBACK_ERRSITE(-EINVAL);
	/* The domain was stepped above (its identity pass may sleep), so the
	 * images' own checks come right before the provider callback; one
	 * identity pass before the store and one after (six took a flip's time). */
	if (next && (ret = image_checked(next))) {
		hold_images(d);
		return ret;
	}
	if (old && (ret = image_checked(old))) {
		hold_images(d);
		return ret;
	}
	serial = ++d->serial;
	d->transitioning = true;
	d->pending = next;
	ret = d->bound_provider.flip(d, next, serial);
	if (ret)
		goto hold;
	ret = step(d);
	if (ret)
		goto hold;
	if (next && (ret = image_checked(next)))
		goto hold;
	if (old && (ret = image_checked(old)))
		goto hold;
	spin_lock_irqsave(&d->source_lock, flags);
	if (old) {
		old->phase = POULSBACK_DISPLAY_PENDING;
		old->retirement_serial = serial;
	}
	if (next) {
		next->phase = POULSBACK_DISPLAY_ACTIVE;
		next->retirement_serial = 0;
	}
	d->active = next;
	d->pending = NULL;
	d->transitioning = false;
	spin_unlock_irqrestore(&d->source_lock, flags);
	return 0;
hold:
	/* pending/current and BOTH pin/fence sets survive ambiguous register writes. */
	if (!d->primary_error)
		d->primary_error = ret;
	hold_images(d);
	return ret;
}
int poulsback_display_flip(struct poulsback_display_domain *d, struct poulsback_display_image *next)
{
	int ret, idx;
	if (!d || !d->initialized)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	mutex_lock(&d->lock);
	ret = enter_device(d, &idx);
	if (!ret) {
		ret = flip_locked(d, next);
		drm_dev_exit(idx);
	}
	mutex_unlock(&d->lock);
	return ret;
}
int poulsback_display_disable(struct poulsback_display_domain *d)
{
	return poulsback_display_flip(d, NULL);
}

int poulsback_display_retire(struct poulsback_display_image *m)
{
	struct poulsback_display_domain *d;
	struct drm_gem_object *obj;
	unsigned long flags;
	u32 i, slot = 0;
	int ret, idx;
	bool entered = false;
	if (!m || !(d = m->domain) || !d->initialized)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&d->lock);
	ret = enter_device(d, &idx);
	if (ret)
		goto out;
	entered = true;
	if (m->phase != POULSBACK_DISPLAY_MAPPED && m->phase != POULSBACK_DISPLAY_PENDING) {
		ret = -EBUSY;
		goto out;
	}
	if (d->active == m || d->pending == m || d->transitioning) {
		ret = -EBUSY;
		goto out;
	}
	ret = tracking_slot(m, &slot);
	if (ret)
		goto hold;
	ret = image_valid(m);
	if (ret)
		goto hold;
	ret = d->bound_provider.retirement(m, m->retirement_serial);
	if (ret == -EAGAIN) {
		int validated = image_valid(m);
		if (validated) {
			ret = validated;
			goto hold;
		}
		goto out;
	}
	if (ret)
		goto hold;
	ret = image_valid(m);
	if (ret)
		goto hold;
	for (i = m->mapped; i > 0; i--) {
		ret = d->bound_provider.remove(m, m->pin.pages[i - 1],
					       m->gtt.start + (u64)(i - 1) * PAGE_SIZE);
		if (ret)
			goto hold;
		m->mapped--;
		ret = image_valid(m);
		if (ret)
			goto hold;
	}
	ret = d->bound_provider.flush(d);
	if (ret)
		goto hold;
	ret = image_valid(m);
	if (ret)
		goto hold;
	ret = tracking_slot(m, &slot);
	if (ret)
		goto hold;
	/* Commit lifetime only after the checked certificate and all writer returns. */
	spin_lock_irqsave(&d->source_lock, flags);
	m->certificate = true;
	spin_unlock_irqrestore(&d->source_lock, flags);
	ret = release_resource(&m->gtt);
	if (ret)
		goto hold;
	/* Linux detaches parent but retains the old sibling link. Clear only
	 * this detached node; the live resource tree keeps its neighbour. */
	m->gtt.sibling = NULL;
	m->reserved = false;
	d->extents[slot].reserved = false;
	m->pin.mapping_owned = false;
	/* Removed and certified: drop the display pin before the BOOKKEEP fence
	 * signals; the tracker's own reference is dropped last. */
	poulsback_bo_display_unpin(m->pin.object);
	untrack_commit(m, slot);
	obj = m->object;
	dma_fence_signal(m->read_fence);
	dma_fence_put(m->read_fence);
	memset(m, 0, sizeof(*m));
	drm_gem_object_put(obj);
	goto out;
hold:
	m->phase = POULSBACK_DISPLAY_HOLD;
	if (!m->cleanup_error)
		m->primary_error = ret;
	if (!d->primary_error)
		d->primary_error = ret;
	if (m->pin.object)
		m->pin.held = true;
	hold_images(d);
out:
	if (entered)
		drm_dev_exit(idx);
	mutex_unlock(&d->lock);
	return ret;
}
