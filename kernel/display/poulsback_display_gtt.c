// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Display GTT bindings of SCANOUT BOs: ownership checks and PTE intents. */
#include <linux/errno.h>
#include "poulsback_errsite.h"
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <drm/drm_gem.h>
#include <drm/drm_drv.h>
#include "poulsback_bo.h"
#include "poulsback_display.h"
#include "poulsback_display_gtt.h"


/* Linux gma500 psb_gtt_mask_pte(PFN, PSB_MMU_CACHED_MEMORY): VALID1|CACHED8.
 * These are display GTT bits, not SGX MMU access flags or PAT settings. */
#define DISPLAY_GTT_CACHED_VALID 9U
struct display_gtt_page {
	struct page *page;
	void *cpu;
	u64 physical;
	struct poulsback_display_gtt_entry intent;
};
struct poulsback_display_gtt_binding {
	struct poulsback_display_gtt_binding *self;
	struct poulsback_display_gtt_binding **slot;
	struct poulsback_display_image *image;
	struct poulsback_display_domain *domain;
	const struct poulsback_display_provider *provider;
	struct poulsback_display_provider bound_provider;
	struct drm_gem_object *object;
	struct page **canonical_pages;
	const struct poulsback_display_gtt_source *source;
	struct poulsback_display_gtt_source frozen;
	struct poulsback_display_layout layout;
	u64 begin, end;
	u64 scratch_physical;
	void *scratch_cpu;
	u32 count;
	bool refs, possible;
	struct poulsback_display_gtt_report report;
	struct display_gtt_page pages[];
};
static bool same_source(const struct poulsback_display_gtt_source *a,
			const struct poulsback_display_gtt_source *b)
{
	return a->dev == b->dev && a->gtt_identity == b->gtt_identity && a->scratch == b->scratch &&
	       a->generation == b->generation && a->aperture_start == b->aperture_start &&
	       a->aperture_bytes == b->aperture_bytes && a->stolen_bytes == b->stolen_bytes &&
	       a->mapped_table_bytes == b->mapped_table_bytes &&
	       a->active_entries == b->active_entries;
}
static bool same_provider(const struct poulsback_display_provider *a,
			  const struct poulsback_display_provider *b)
{
	return a && b && a->identity == b->identity && a->publish_cache == b->publish_cache &&
	       a->insert == b->insert && a->remove == b->remove && a->flush == b->flush &&
	       a->flip == b->flip && a->retirement == b->retirement && a->context == b->context;
}
static bool same_layout(const struct poulsback_display_layout *a,
			const struct poulsback_display_layout *b)
{
	return a->width == b->width && a->height == b->height && a->stride == b->stride &&
	       a->format == b->format && a->size == b->size;
}
static bool cache_mode(void *cpu, int mode)
{
	unsigned int level;
	pte_t *p = lookup_address((unsigned long)cpu, &level);
	unsigned long value;
	if (!p || !pte_present(*p))
		return false;
	value = pte_val(*p);
	if (level > PG_LEVEL_4K)
		value = (value & ~_PAGE_PAT) | ((value & _PAGE_PAT_LARGE) >> 5);
	return (value & _PAGE_CACHE_MASK) == cachemode2protval(mode);
}
static int physical_cache(struct page *p, u64 *address, void **cpu, int mode)
{
	if (!p || PageHighMem(p) || !page_address(p))
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	*address = page_to_phys(p);
	*cpu = page_address(p);
	if ((*address & (PAGE_SIZE - 1)) || *address > 0xfffff000ULL || !cache_mode(*cpu, mode))
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	return 0;
}
/* SCANOUT BO pages are WC for the BO's life: no WB alias, no flush. */
static int physical(struct page *p, u64 *address, void **cpu)
{
	return physical_cache(p, address, cpu, _PAGE_CACHE_MODE_WC);
}
/* Actual typed role and reservation are checked before following its pages.
 * The outer display coordinator separately validates the complete tree. */
static int owner_local(struct poulsback_display_image *m,
		       const struct poulsback_display_gtt_source *s)
{
	struct poulsback_display_domain *d;
	u64 capacity, source_end;
	u32 i, found = 0;
	if (!m || !(d = m->domain) || !s || !d->initialized || !m->object ||
	    m->object->dev != d->dev || s->dev != d->dev || d->held ||
	    d->generation != d->bound_generation || s->generation != d->bound_generation ||
	    !s->generation || !s->gtt_identity || s->gtt_identity != d->bound_gtt_identity ||
	    d->gtt_identity != d->bound_gtt_identity || !s->scratch ||
	    s->scratch != d->bound_dummy || d->dummy != d->bound_dummy || !m->tracked ||
	    !m->object_ref || !m->reserved || !same_provider(d->provider, &d->bound_provider) ||
	    m->pin.object != m->object || m->phase == POULSBACK_DISPLAY_HOLD || m->certificate ||
	    !m->pin.count || m->pin.count > POULSBACK_DISPLAY_GTT_MAX_PAGES ||
	    m->pin.size != (u64)m->pin.count * PAGE_SIZE || m->pin.size != m->object->size ||
	    m->mapped > m->pin.count || m->gtt.parent != &d->aperture || m->gtt.child ||
	    m->gtt.flags || resource_size(&m->gtt) != m->pin.size ||
	    m->gtt.start < d->bound_start || m->gtt.end > d->bound_end ||
	    d->aperture.start != d->bound_start || d->aperture.end != d->bound_end ||
	    d->aperture.flags || d->aperture.parent || d->aperture.sibling ||
	    (m->gtt.start & (POULSBACK_DISPLAY_SLOT_ALIGN - 1)))
		return POULSBACK_ERRSITE(-ESTALE);
	if (PAGE_SIZE != 4096 || !s->aperture_bytes ||
	    (s->aperture_start | s->aperture_bytes | s->stolen_bytes) & (PAGE_SIZE - 1) ||
	    s->aperture_start > U32_MAX ||
	    s->aperture_bytes > (u64)U32_MAX + 1 - s->aperture_start ||
	    s->stolen_bytes > s->aperture_bytes || !s->mapped_table_bytes ||
	    (s->mapped_table_bytes & (PAGE_SIZE - 1)))
		return POULSBACK_ERRSITE(-ERANGE);
	capacity = s->mapped_table_bytes / sizeof(u32);
	if (s->active_entries != s->aperture_bytes / PAGE_SIZE || s->active_entries > capacity)
		return POULSBACK_ERRSITE(-ERANGE);
	source_end = s->aperture_start + s->aperture_bytes - 1;
	if (d->bound_start < s->aperture_start + s->stolen_bytes || d->bound_end > source_end ||
	    m->gtt.start < s->aperture_start + s->stolen_bytes || m->gtt.end > source_end)
		return POULSBACK_ERRSITE(-ERANGE);
	for (i = 0; i < ARRAY_SIZE(d->extents); i++) {
		const struct poulsback_display_extent *e = &d->extents[i];
		if (d->images[i] != e->owner)
			return POULSBACK_ERRSITE(-ESTALE);
		if (e->owner == m) {
			if (!e->reserved || e->object != m->object || e->start != m->gtt.start ||
			    e->end != m->gtt.end)
				return POULSBACK_ERRSITE(-ESTALE);
			found++;
		} else if (e->owner && e->reserved && e->start <= m->gtt.end &&
			   m->gtt.start <= e->end) {
			return POULSBACK_ERRSITE(-EXDEV);
		}
	}
	return found == 1 ? 0 : POULSBACK_ERRSITE(-ESTALE);
}
static int owner(struct poulsback_display_image *m, const struct poulsback_display_gtt_source *s)
{
	int ret = owner_local(m, s);
	if (!ret)
		ret = poulsback_bo_display_check(m->pin.object, m->pin.pages, m->pin.count);
	return ret ? ret : owner_local(m, s);
}
static void hold(struct poulsback_display_gtt_binding *c, int error)
{
	if (!c->report.primary_error)
		c->report.primary_error = error;
	/* The binding's own GEM reference on c->object is never dropped now. */
	c->report.held = 1;
}
/* Outputs are callers' locals, kept apart from the binding's own state;
 * checking them does not scan the binding's pages. */
static bool output_ok(const struct poulsback_display_gtt_binding *c, const void *out, size_t bytes)
{
	if (!out || poulsback_overlap(c, (size_t)c->report.preallocated_bytes, out, bytes) ||
	    poulsback_overlap(c->image, sizeof(*c->image), out, bytes) ||
	    poulsback_overlap(c->domain, sizeof(*c->domain), out, bytes) ||
	    poulsback_overlap(c->source, sizeof(*c->source), out, bytes) ||
	    poulsback_overlap(c->object, sizeof(*c->object), out, bytes) ||
	    poulsback_overlap(c->frozen.scratch, sizeof(*c->frozen.scratch), out, bytes) ||
	    poulsback_overlap(c->scratch_cpu, PAGE_SIZE, out, bytes) ||
	    poulsback_overlap(c->canonical_pages, c->count * sizeof(*c->canonical_pages), out,
			      bytes))
		return false;
	return true;
}
static int compare_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;
	return x > y ? 1 : x < y ? -1 : 0;
}
/* The tuple, owner and scratch, and the pages [first, first + n). A whole
 * binding is checked at bind and unbind; a PTE store checks its own page
 * only. Buffer switches check the tuple and endpoint words under locks
 * that exclude page changes (docs/kernel/display.md). */
static int check_pages(struct poulsback_display_gtt_binding *c, u32 first, u32 n)
{
	struct poulsback_display_image *m = c->image;
	u64 pa;
	void *cpu;
	u32 i;
	int ret;
	if (first > c->count || n > c->count - first)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!same_source(c->source, &c->frozen) || c->domain->provider != c->provider ||
	    !same_provider(&c->domain->bound_provider, &c->bound_provider) ||
	    m->domain != c->domain || m->object != c->object ||
	    m->pin.pages != c->canonical_pages || m->pin.count != c->count ||
	    m->gtt.start != c->begin || m->gtt.end != c->end ||
	    !same_layout(&m->layout, &c->layout))
		return POULSBACK_ERRSITE(-ESTALE);
	ret = owner(m, &c->frozen);
	if (ret)
		return ret;
	/* The retained Linux gma500 producer applies set_pages_uc to scratch.
	 * That API selects UC_MINUS; the BO pages are WC. */
	ret = physical_cache(c->frozen.scratch, &pa, &cpu, _PAGE_CACHE_MODE_UC_MINUS);
	if (ret || pa != c->scratch_physical || cpu != c->scratch_cpu)
		return ret ? ret : POULSBACK_ERRSITE(-ESTALE);
	for (i = first; i < first + n; i++) {
		if (c->canonical_pages[i] != c->pages[i].page)
			return POULSBACK_ERRSITE(-ESTALE);
		ret = physical(c->pages[i].page, &pa, &cpu);
		if (ret || pa != c->pages[i].physical || cpu != c->pages[i].cpu)
			return ret ? ret : POULSBACK_ERRSITE(-ESTALE);
		if (c->pages[i].intent.resource_byte_address != c->begin + (u64)i * PAGE_SIZE ||
		    c->pages[i].intent.aperture_byte_offset !=
			    c->begin - c->frozen.aperture_start + (u64)i * PAGE_SIZE ||
		    c->pages[i].intent.slot !=
			    (c->begin - c->frozen.aperture_start) / PAGE_SIZE + i ||
		    c->pages[i].intent.empty_pte !=
			    ((u32)c->scratch_physical | DISPLAY_GTT_CACHED_VALID) ||
		    c->pages[i].intent.image_pte != ((u32)pa | DISPLAY_GTT_CACHED_VALID))
			return POULSBACK_ERRSITE(-ESTALE);
	}
	/* The pin check may sleep. Repeat tuple checks after it returns. */
	if (!same_source(c->source, &c->frozen) || c->domain->held ||
	    c->domain->provider != c->provider ||
	    !same_provider(&c->domain->bound_provider, &c->bound_provider) ||
	    m->domain != c->domain || m->object != c->object ||
	    m->pin.pages != c->canonical_pages || m->pin.count != c->count ||
	    m->gtt.start != c->begin || m->gtt.end != c->end)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = owner_local(m, &c->frozen);
	if (ret)
		return ret;
	c->possible |= m->possible || m->mapped || m->pin.mapping_owned;
	return 0;
}
static int check(struct poulsback_display_gtt_binding *c)
{
	return check_pages(c, 0, c->count);
}
int poulsback_display_gtt_prepare(struct poulsback_display_image *m,
				  const struct poulsback_display_gtt_source *s,
				  struct poulsback_display_gtt_binding **out)
{
	struct poulsback_display_gtt_binding *c;
	struct poulsback_display_gtt_source frozen;
	struct poulsback_display_domain *domain;
	struct drm_gem_object *object;
	struct page **canonical_pages;
	const struct poulsback_display_provider *provider;
	struct poulsback_display_provider provider_copy;
	struct poulsback_display_layout layout;
	u64 begin, end, image_bytes, scratch, *sorted;
	void *cpu;
	u32 i, count;
	size_t bytes;
	int ret;
	if (!m || !s || !out || poulsback_overlap(m, sizeof(*m), out, sizeof(*out)) ||
	    poulsback_overlap(s, sizeof(*s), out, sizeof(*out)))
		return POULSBACK_ERRSITE(-EINVAL);
	frozen = *s;
	ret = owner(m, &frozen);
	if (ret)
		return ret;
	domain = m->domain;
	object = m->object;
	canonical_pages = m->pin.pages;
	begin = m->gtt.start;
	end = m->gtt.end;
	provider = domain->provider;
	provider_copy = domain->bound_provider;
	layout = m->layout;
	count = m->pin.count;
	image_bytes = m->pin.size;
	if (poulsback_overlap(frozen.scratch, sizeof(*frozen.scratch), out, sizeof(*out)) ||
	    (page_address(frozen.scratch) &&
	     poulsback_overlap(page_address(frozen.scratch), PAGE_SIZE, out, sizeof(*out))))
		return POULSBACK_ERRSITE(-EINVAL);
	for (i = 0; i < count; i++)
		if (!m->pin.pages[i] ||
		    poulsback_overlap(page_address(m->pin.pages[i]), PAGE_SIZE, out,
				      sizeof(*out)) ||
		    poulsback_overlap(m->pin.pages[i], sizeof(*m->pin.pages[i]), out, sizeof(*out)))
			return POULSBACK_ERRSITE(-EINVAL);
	if (poulsback_overlap(m->domain, sizeof(*m->domain), out, sizeof(*out)) ||
	    poulsback_overlap(m->object, sizeof(*m->object), out, sizeof(*out)) ||
	    poulsback_overlap(m->pin.pages, count * sizeof(*m->pin.pages), out, sizeof(*out)))
		return POULSBACK_ERRSITE(-EINVAL);
	if (*out)
		return -EBUSY;
	bytes = sizeof(*c) + (size_t)count * (sizeof(c->pages[0]) + sizeof(u64));
	c = kzalloc(bytes, GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	if (*out || !same_source(s, &frozen) || m->domain != domain || m->object != object ||
	    domain->provider != provider ||
	    !same_provider(&domain->bound_provider, &provider_copy) ||
	    m->pin.pages != canonical_pages || m->pin.count != count ||
	    m->pin.size != image_bytes || !same_layout(&m->layout, &layout) ||
	    m->gtt.start != begin || m->gtt.end != end) {
		kfree(c);
		return POULSBACK_ERRSITE(-ESTALE);
	}
	if (!try_module_get(THIS_MODULE)) {
		kfree(c);
		return POULSBACK_ERRSITE(-ENODEV);
	}
	c->self = c;
	c->slot = out;
	c->image = m;
	c->domain = domain;
	c->object = object;
	c->canonical_pages = canonical_pages;
	c->provider = provider;
	c->bound_provider = provider_copy;
	c->layout = layout;
	c->source = s;
	c->frozen = frozen;
	c->count = count;
	c->begin = begin;
	c->end = end;
	c->possible = m->possible || m->mapped || m->pin.mapping_owned;
	c->report = (struct poulsback_display_gtt_report){.preallocated_bytes = bytes};
	drm_gem_object_get(c->object);
	drm_dev_get(c->domain->dev);
	c->refs = true;
	*out = c;
	ret = owner(m, &frozen);
	if (!ret)
		ret = physical_cache(frozen.scratch, &scratch, &cpu, _PAGE_CACHE_MODE_UC_MINUS);
	if (!ret) {
		c->scratch_physical = scratch;
		c->scratch_cpu = cpu;
	}
	/* A consistent replacement lease is still a different source. Never walk
	 * the pre-allocation array after a sleeping check without this join. */
	if (!ret && (!same_source(s, &frozen) || m->domain != domain || m->object != object ||
		     m->pin.pages != canonical_pages || m->pin.count != count ||
		     m->pin.size != image_bytes || !same_layout(&m->layout, &layout) ||
		     m->gtt.start != begin || m->gtt.end != end))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret) {
		get_page(c->frozen.scratch);
		c->report.retained_scratch = 1;
	}
	sorted = (u64 *)(c->pages + count);
	for (i = 0; i < count && !ret; i++) {
		struct display_gtt_page *p = c->pages + i;
		p->page = c->canonical_pages[i];
		ret = physical(p->page, &p->physical, &p->cpu);
		if (ret)
			break;
		if (p->physical == scratch) {
			ret = POULSBACK_ERRSITE(-EXDEV);
			break;
		}
		sorted[i] = p->physical;
		p->intent = (struct poulsback_display_gtt_entry){
			.resource_byte_address = c->begin + (u64)i * PAGE_SIZE,
			.aperture_byte_offset =
				c->begin - frozen.aperture_start + (u64)i * PAGE_SIZE,
			.slot = (c->begin - frozen.aperture_start) / PAGE_SIZE + i,
			.empty_pte = (u32)scratch | DISPLAY_GTT_CACHED_VALID,
			.image_pte = (u32)p->physical | DISPLAY_GTT_CACHED_VALID};
		get_page(p->page);
		c->report.retained_pages++;
	}
	if (!ret) {
		sort(sorted, count, sizeof(*sorted), compare_u64, NULL);
		for (i = 1; i < count; i++)
			if (sorted[i] == sorted[i - 1]) {
				ret = POULSBACK_ERRSITE(-EXDEV);
				break;
			}
	}
	if (!ret)
		ret = check(c);
	c->report.preparation_error = ret;
	if (ret)
		hold(c, ret);
	else
		c->report.prepared = 1;
	return ret;
}
int poulsback_display_gtt_entry(const struct poulsback_display_gtt_binding *c, u32 i,
				struct poulsback_display_gtt_entry *out)
{
	if (!c || c->self != c || !c->report.prepared || i >= c->count ||
	    !output_ok(c, out, sizeof(*out)))
		return POULSBACK_ERRSITE(-EINVAL);
	*out = c->pages[i].intent;
	return 0;
}
int poulsback_display_gtt_publication_entry(struct poulsback_display_gtt_binding *c,
					    struct poulsback_display_image *m, struct page *page,
					    u64 address, struct poulsback_display_gtt_entry *out)
{
	u64 index;
	int ret;
	if (!c || c->self != c || !c->slot || *c->slot != c || c->image != m || !page ||
	    address < c->begin || address > c->end || (address & (PAGE_SIZE - 1)) ||
	    !output_ok(c, out, sizeof(*out)))
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&c->domain->lock);
	index = (address - c->begin) / PAGE_SIZE;
	if (index >= c->count || c->pages[index].page != page)
		return POULSBACK_ERRSITE(-EXDEV);
	if (!c->report.prepared || c->report.held)
		return POULSBACK_ERRSITE(-EIO);
	ret = check_pages(c, index, 1);
	if (!ret &&
	    (*c->slot != c || c->image != m || index >= c->count || c->pages[index].page != page ||
	     c->pages[index].intent.resource_byte_address != address ||
	     c->pages[index].intent.aperture_byte_offset != address - c->frozen.aperture_start ||
	     c->pages[index].intent.slot != (address - c->frozen.aperture_start) / PAGE_SIZE))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (ret)
		hold(c, ret);
	else
		*out = c->pages[index].intent;
	return ret;
}
static int publication_check(struct poulsback_display_gtt_binding *c,
			     struct poulsback_display_image *m, bool pages)
{
	int ret;
	if (!c || c->self != c || !c->slot || *c->slot != c || c->image != m)
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&c->domain->lock);
	if (!c->report.prepared || c->report.held)
		return POULSBACK_ERRSITE(-EIO);
	ret = check_pages(c, 0, pages ? c->count : 0);
	if (!ret && (*c->slot != c || c->image != m))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (ret)
		hold(c, ret);
	return ret;
}
int poulsback_display_gtt_publication_validate(struct poulsback_display_gtt_binding *c,
					       struct poulsback_display_image *m)
{
	return publication_check(c, m, true);
}
int poulsback_display_gtt_publication_owned(struct poulsback_display_gtt_binding *c,
					    struct poulsback_display_image *m)
{
	return publication_check(c, m, false);
}
void poulsback_display_gtt_publication_hold(struct poulsback_display_gtt_binding *c, int primary,
					    int cleanup)
{
	if (!c || c->self != c)
		return;
	lockdep_assert_held(&c->domain->lock);
	if (cleanup && !c->report.cleanup_error)
		c->report.cleanup_error = cleanup;
	hold(c, primary ?: cleanup);
	if (!c->domain->primary_error)
		c->domain->primary_error = primary ?: cleanup;
	c->domain->held = true;
	c->image->phase = POULSBACK_DISPLAY_HOLD;
	if (!c->image->primary_error)
		c->image->primary_error = primary ?: cleanup;
	if (!c->image->cleanup_error)
		c->image->cleanup_error = cleanup;
}
static void dispose_binding(struct poulsback_display_gtt_binding *c,
			    struct poulsback_display_gtt_binding **out)
{
	struct drm_device *dev = c->domain->dev;
	u32 i;
	for (i = 0; i < c->report.retained_pages; i++)
		put_page(c->pages[i].page);
	if (c->report.retained_scratch)
		put_page(c->frozen.scratch);
	drm_gem_object_put(c->object);
	drm_dev_put(dev);
	c->self = NULL;
	kfree(c);
	*out = NULL;
	module_put(THIS_MODULE);
}
int poulsback_display_gtt_dispose_withdrawn(struct poulsback_display_gtt_binding **out,
					    struct poulsback_display_image *m)
{
	struct poulsback_display_gtt_binding *c;
	struct poulsback_display_domain *d;
	int ret;
	if (!out || !(c = *out) || c->self != c || c->slot != out || c->image != m)
		return POULSBACK_ERRSITE(-EINVAL);
	d = c->domain;
	lockdep_assert_held(&d->lock);
	/* A never-shown (MAPPED) or certified replaced (PENDING) image whose PTEs
	 * are all withdrawn; another image may be shown meanwhile. */
	if (!c->report.prepared || c->report.held || m->mapped ||
	    (m->phase != POULSBACK_DISPLAY_MAPPED && m->phase != POULSBACK_DISPLAY_PENDING) ||
	    d->active == m || d->pending == m || d->transitioning)
		return -EBUSY;
	ret = check(c);
	if (!ret && (*out != c || c->slot != out || m->mapped || d->active == m ||
		     d->pending == m || d->transitioning))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (ret)
		return ret;
	dispose_binding(c, out);
	return 0;
}
