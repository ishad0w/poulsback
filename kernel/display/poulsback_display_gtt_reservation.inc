// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Included by the locked GMA500 gtt.c after its real resource allocator.
 * Metadata only: no GTT/PTE/cache/plane access or physical provider admission.
 */
#include "poulsback_errsite.h"
#include "poulsback_display.h"
#include "poulsback_display_gtt.h"
#include "poulsback_display_psb_source.h"
#include "sgx535_source_epoch.h"
#include <drm/drm_drv.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>


struct poulsback_display_gtt_reservation {
	struct poulsback_display_gtt_reservation *self;
	struct poulsback_display_gtt_reservation **slot;
	struct drm_device *dev;
	struct drm_psb_private *pdev;
	struct resource *root, window;
	u32 __iomem *map;
	struct page *scratch;
	struct poulsback_display_domain *domain;
	const struct poulsback_display_psb_source *source_owner;
	struct poulsback_display_gtt_source source;
	u64 begin, bytes;
	bool refs, scratch_ref, reserved, bound, held;
	int primary_error, cleanup_error;
};
static int gtt_reservation_snapshot(struct drm_device *dev, struct drm_psb_private *p,
				    struct poulsback_display_gtt_source *s)
{
	struct sgx535_epoch_snapshot epoch;
	u64 bytes, table;

	sgx535_epoch_snapshot(&epoch);
	if (!epoch.bound || !epoch.epoch || epoch.loss || !sgx535_epoch_external_active(dev))
		return POULSBACK_ERRSITE(-ESTALE);
	if (p != to_drm_psb_private(dev) || !p->gtt_mem || !p->gtt_map || !p->scratch_page)
		return POULSBACK_ERRSITE(-ESTALE);
	bytes = resource_size(p->gtt_mem);
	table = (u64)p->gtt.gtt_pages << PAGE_SHIFT;
	if ((p->gtt_mem->start | bytes | p->gtt.stolen_size) & (PAGE_SIZE - 1) ||
	    p->gtt_mem->start > U32_MAX || !bytes || bytes > (u64)U32_MAX + 1 - p->gtt_mem->start ||
	    p->gtt.stolen_size > bytes || p->gtt.gatt_start != p->gtt_mem->start ||
	    !p->gtt.gatt_pages || (u64)p->gtt.gatt_pages * PAGE_SIZE != bytes || !table ||
	    p->gtt.gatt_pages > table / sizeof(*p->gtt_map))
		return POULSBACK_ERRSITE(-ERANGE);
	*s = (struct poulsback_display_gtt_source){.dev = dev,
						   .gtt_identity = p->gtt_map,
						   .scratch = p->scratch_page,
						   .generation = epoch.epoch,
						   .aperture_start = p->gtt_mem->start,
						   .aperture_bytes = bytes,
						   .stolen_bytes = p->gtt.stolen_size,
						   .mapped_table_bytes = table,
						   .active_entries = p->gtt.gatt_pages};
	return 0;
}
static bool gtt_reservation_same_source(const struct poulsback_display_gtt_source *a,
					const struct poulsback_display_gtt_source *b)
{
	return a->dev == b->dev && a->gtt_identity == b->gtt_identity && a->scratch == b->scratch &&
	       a->generation == b->generation && a->aperture_start == b->aperture_start &&
	       a->aperture_bytes == b->aperture_bytes && a->stolen_bytes == b->stolen_bytes &&
	       a->mapped_table_bytes == b->mapped_table_bytes &&
	       a->active_entries == b->active_entries;
}
static int gtt_reservation_check_locked(struct poulsback_display_gtt_reservation *r)
{
	struct poulsback_display_gtt_source source;
	struct poulsback_display_domain *d = r->domain;
	int ret;

	/* Compare retained roots before following a replacement source pointer. */
	if (r->self != r || *r->slot != r || r->held || !r->refs || r->root != r->pdev->gtt_mem ||
	    r->map != r->pdev->gtt_map || r->scratch != r->pdev->scratch_page || !r->scratch_ref)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = gtt_reservation_snapshot(r->dev, r->pdev, &source);
	if (!ret && !gtt_reservation_same_source(&source, &r->source))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret && r->reserved &&
	    (r->window.parent != r->root || r->window.child || r->window.flags ||
	     r->window.start != r->begin || resource_size(&r->window) != r->bytes ||
	     r->begin < source.aperture_start + source.stolen_bytes ||
	     r->window.end > r->root->end))
		ret = POULSBACK_ERRSITE(-ESTALE);
	/* Other legacy nodes can change sibling links. Only this live root child
   * is ours; never copy or traverse an unlocked mutable legacy tree. */
	if (!ret && r->bound &&
	    (d->dev != r->dev || !d->initialized || d->held || d->gtt_identity != r->map ||
	     d->bound_gtt_identity != r->map || d->dummy != r->scratch ||
	     d->bound_dummy != r->scratch || d->generation != source.generation ||
	     d->bound_generation != source.generation || d->bound_start != r->begin ||
	     d->bound_end != r->window.end || d->aperture.start != r->begin ||
	     d->aperture.end != r->window.end || d->aperture.parent || d->aperture.sibling ||
	     d->aperture.flags))
		ret = POULSBACK_ERRSITE(-ESTALE);
	return ret;
}
static int gtt_reservation_hold(struct poulsback_display_gtt_reservation *r, int error)
{
	if (!r->primary_error)
		r->primary_error = error;
	r->held = true;
	return error;
}
static int poulsback_display_gtt_reservation_check(struct poulsback_display_gtt_reservation *,
						   struct poulsback_display_gtt_source *);
int poulsback_display_gtt_reservation_prepare(struct drm_device *dev, u64 bytes,
					      struct poulsback_display_domain *domain,
					      const struct poulsback_display_provider *provider,
					      struct poulsback_display_gtt_reservation **out)
{
	struct poulsback_display_gtt_reservation *r = NULL;
	struct poulsback_display_gtt_source source;
	struct drm_psb_private *p;
	struct resource *root;
	u32 offset;
	int ret, idx;

	if (!dev || !domain || !provider || !out || !bytes || (bytes & (PAGE_SIZE - 1)) ||
	    bytes > (u64)POULSBACK_DISPLAY_GTT_MAX_PAGES * PAGE_SIZE || domain->initialized ||
	    domain->dev)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = sgx535_epoch_external_begin(dev);
	if (ret)
		goto exit;
	p = to_drm_psb_private(dev);
	ret = gtt_reservation_snapshot(dev, p, &source);
	if (ret)
		goto external_end;
	root = p->gtt_mem;
	/* No output load/store may touch a driver owner, scratch or mapped GTT.
   * Live source pointers are followed only under SRCU + admission. */
	if (poulsback_overlap(out, sizeof(*out), p, sizeof(*p)) ||
	    poulsback_overlap(out, sizeof(*out), domain, sizeof(*domain)) ||
	    poulsback_overlap(out, sizeof(*out), provider, sizeof(*provider)) ||
	    poulsback_overlap(out, sizeof(*out), root, sizeof(*root)) ||
	    poulsback_overlap(out, sizeof(*out), source.scratch, sizeof(*source.scratch)) ||
	    poulsback_overlap(out, sizeof(*out), source.gtt_identity, source.mapped_table_bytes) ||
	    (page_address(source.scratch) &&
	     poulsback_overlap(out, sizeof(*out), page_address(source.scratch), PAGE_SIZE)) ||
	    *out) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto external_end;
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		ret = -ENOMEM;
		goto external_end;
	}
	if (!try_module_get(THIS_MODULE)) {
		kfree(r);
		r = NULL;
		ret = POULSBACK_ERRSITE(-ENODEV);
		goto external_end;
	}
	drm_dev_get(dev);
	r->self = r;
	r->slot = out;
	r->dev = dev;
	r->pdev = p;
	r->domain = domain;
	r->bytes = bytes;
	r->refs = true;
	r->source = source;
	r->root = root;
	r->map = source.gtt_identity;
	r->scratch = source.scratch;
	*out = r; /* Root before the GTT-mutex sleep or real reservation. */
	get_page(r->scratch);
	r->scratch_ref = true;
	mutex_lock(&p->gtt_mutex);
	ret = gtt_reservation_check_locked(r);
	if (!ret) {
		ret = psb_gtt_allocate_resource(p, &r->window, "poulsback-display-window", bytes,
						POULSBACK_DISPLAY_SLOT_ALIGN, false, &offset);
		if (!ret) {
			r->reserved = true;
			r->begin = r->window.start;
			if ((u64)offset != r->begin - r->source.aperture_start)
				ret = POULSBACK_ERRSITE(-ESTALE);
		}
	}
	if (!ret)
		ret = gtt_reservation_check_locked(r);
	mutex_unlock(&p->gtt_mutex);
external_end:
	if (sgx535_epoch_external_end(dev) && !ret)
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret) {
		ret = poulsback_display_domain_init(domain, dev, provider, r->map, r->scratch,
						    r->source.generation, r->begin, bytes);
		if (!ret) {
			r->bound = true;
			ret = poulsback_display_gtt_reservation_check(r, NULL);
		}
	}
exit:
	drm_dev_exit(idx);
	return ret && r ? gtt_reservation_hold(r, ret) : ret;
}
static int poulsback_display_gtt_reservation_check(struct poulsback_display_gtt_reservation *r,
						   struct poulsback_display_gtt_source *out)
{
	int ret, idx;

	if (!r || r->self != r || !r->bound || !r->reserved || !r->domain ||
	    (out && (poulsback_overlap(out, sizeof(*out), r, sizeof(*r)) ||
		     poulsback_overlap(out, sizeof(*out), r->domain, sizeof(*r->domain)) ||
		     poulsback_overlap(out, sizeof(*out), r->domain->provider,
				       sizeof(*r->domain->provider)) ||
		     poulsback_overlap(out, sizeof(*out), r->pdev, sizeof(*r->pdev)) ||
		     poulsback_overlap(out, sizeof(*out), r->root, sizeof(*r->root)) ||
		     poulsback_overlap(out, sizeof(*out), r->slot, sizeof(*r->slot)) ||
		     poulsback_overlap(out, sizeof(*out), r->scratch, sizeof(*r->scratch)) ||
		     poulsback_overlap(out, sizeof(*out), r->map, r->source.mapped_table_bytes) ||
		     (page_address(r->scratch) &&
		      poulsback_overlap(out, sizeof(*out), page_address(r->scratch), PAGE_SIZE)))))
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&r->domain->lock);
	if (!drm_dev_enter(r->dev, &idx)) {
		ret = POULSBACK_ERRSITE(-ENODEV);
		goto unlock;
	}
	ret = sgx535_epoch_external_begin(r->dev);
	if (!ret) {
		mutex_lock(&r->pdev->gtt_mutex);
		ret = gtt_reservation_check_locked(r);
		mutex_unlock(&r->pdev->gtt_mutex);
		if (sgx535_epoch_external_end(r->dev) && !ret)
			ret = POULSBACK_ERRSITE(-ESTALE);
	}
	if (!ret && out)
		*out = r->source;
	drm_dev_exit(idx);
unlock:
	mutex_unlock(&r->domain->lock);
	return ret ? gtt_reservation_hold(r, ret) : 0;
}
/* Exact domain membership before following a caller-selected domain. The
 * optional owner slot is an alias exclusion only, not writer permission. */
int poulsback_display_gtt_reservation_source(struct poulsback_display_gtt_reservation *r,
					     struct poulsback_display_domain *domain,
					     struct poulsback_display_gtt_source *out,
					     const void *owner_slot,
					     const struct poulsback_display_psb_source *owner)
{
	if (!r || r->self != r || r->domain != domain || !domain)
		return POULSBACK_ERRSITE(-EINVAL);
	if (r->source_owner != owner)
		return POULSBACK_ERRSITE(-ESTALE);
	if (owner_slot &&
	    (poulsback_overlap(owner_slot, sizeof(void *), r, sizeof(*r)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), domain, sizeof(*domain)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), domain->provider,
			       sizeof(*domain->provider)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), r->pdev, sizeof(*r->pdev)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), r->root, sizeof(*r->root)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), r->slot, sizeof(*r->slot)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), r->scratch, sizeof(*r->scratch)) ||
	     poulsback_overlap(owner_slot, sizeof(void *), r->map, r->source.mapped_table_bytes) ||
	     (page_address(r->scratch) &&
	      poulsback_overlap(owner_slot, sizeof(void *), page_address(r->scratch), PAGE_SIZE))))
		return POULSBACK_ERRSITE(-EINVAL);
	return poulsback_display_gtt_reservation_check(r, out);
}
int poulsback_display_gtt_reservation_source_locked(
	struct poulsback_display_gtt_reservation *r, struct poulsback_display_domain *domain,
	const struct poulsback_display_psb_source *owner,
	const struct poulsback_display_gtt_source *source)
{
	int ret;
	if (!r || r->self != r || !domain || r->domain != domain || !owner ||
	    r->source_owner != owner || !source)
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&domain->lock);
	lockdep_assert_held(&r->pdev->gtt_mutex);
	ret = gtt_reservation_check_locked(r);
	if (!ret && !gtt_reservation_same_source(source, &r->source))
		ret = POULSBACK_ERRSITE(-ESTALE);
	return ret ? gtt_reservation_hold(r, ret) : 0;
}
/* One exact source borrower, not an execution/visibility certificate. The
 * caller serializes reservation API lifetime until this binding is published. */
struct gtt_source_owner_action {
	struct poulsback_display_gtt_reservation *reservation;
	struct poulsback_display_psb_source *owner;
};
static int gtt_source_owner_check(void *context)
{
	struct gtt_source_owner_action *a = context;
	if (a->reservation->source_owner)
		return POULSBACK_ERRSITE(-ESTALE);
	return poulsback_display_psb_source_link_check(a->owner, a->reservation,
						       a->reservation->domain);
}
static void gtt_source_owner_commit(void *context)
{
	struct gtt_source_owner_action *a = context;
	a->reservation->source_owner = a->owner;
	poulsback_display_psb_source_link_commit(a->owner);
}
static int gtt_source_owner_finalize(struct gtt_source_owner_action *a)
{
	struct poulsback_display_gtt_reservation *r = a->reservation;
	return sgx535_epoch_external_finalize(r->dev, r->source.generation, gtt_source_owner_check,
					      gtt_source_owner_commit, a);
}
int poulsback_display_gtt_reservation_source_acquire(struct poulsback_display_gtt_reservation *r,
						     struct poulsback_display_domain *domain,
						     struct poulsback_display_psb_source *owner)
{
	struct gtt_source_owner_action action = {r, owner};
	int ret, idx;
	if (!r || r->self != r || r->domain != domain || !domain || !owner || !r->bound ||
	    !r->reserved)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&domain->lock);
	if (r->source_owner) {
		ret = POULSBACK_ERRSITE(-ESTALE);
		goto unlock;
	}
	if (r->held || !domain->initialized || domain->serial || domain->count || domain->bytes ||
	    domain->active || domain->pending || domain->transitioning || domain->aperture.child ||
	    domain->held || memchr_inv(domain->images, 0, sizeof(domain->images)) ||
	    memchr_inv(domain->extents, 0, sizeof(domain->extents))) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!drm_dev_enter(r->dev, &idx)) {
		ret = POULSBACK_ERRSITE(-ENODEV);
		goto unlock;
	}
	ret = sgx535_epoch_external_begin(r->dev);
	if (!ret) {
		mutex_lock(&r->pdev->gtt_mutex);
		ret = gtt_reservation_check_locked(r);
		/* Domain/GTT locks preserve the checked metadata. IRQ epoch loss
		 * remains possible and is closed by the final source-held commit. */
		if (!ret)
			ret = gtt_source_owner_finalize(&action);
		mutex_unlock(&r->pdev->gtt_mutex);
		/* Pure commit cannot change this exact task/device admission pair. */
		WARN_ON(sgx535_epoch_external_end(r->dev));
	}
	drm_dev_exit(idx);
unlock:
	mutex_unlock(&domain->lock);
	return ret;
}
