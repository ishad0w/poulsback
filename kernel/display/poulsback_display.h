/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef POULSBACK_DISPLAY_H
#define POULSBACK_DISPLAY_H
#include <linux/ioport.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
struct dma_fence;
struct drm_device;
struct drm_gem_object;
struct page;
struct poulsback_display_domain;
struct poulsback_display_image;

/* Whether [a, a + na) and [b, b + nb) share a byte. */
static inline bool poulsback_overlap(const void *a, u64 na, const void *b, u64 nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;

	return x < y ? y - x < na : x - y < nb;
}
/* Domain-owned reservation witness, independent of the mutable image/GTT node.
 * Established after reservation, cleared only by authenticated local release. */
struct poulsback_display_extent {
	struct poulsback_display_image *owner;
	struct drm_gem_object *object;
	resource_size_t start, end;
	bool reserved;
};

/* Trusted kernel adapter only. Identity authenticates exact GTT/register/power
 * ownership and a retained generation; insert/remove check exact empty/owned
 * PTEs. flush and retirement return checked visibility/no-old-fetch results.
 * flip/disable may have mutated hardware even when returning an error.
 * EVERY callback must implement its own authenticated validate/mutate/commit
 * transaction over the retained source epoch. Generic pre/post checks and DRM
 * SRCU detect loss/protect device storage; they do not serialize a reset or
 * power invalidation with arbitrary MMIO or establish hardware visibility.
 * The one implementation is the static owner's (poulsback_display_psb_source.c). */
struct poulsback_display_provider {
	int (*identity)(struct poulsback_display_domain *);
	int (*publish_cache)(struct poulsback_display_image *);
	int (*insert)(struct poulsback_display_image *, struct page *, u64);
	int (*remove)(struct poulsback_display_image *, struct page *, u64);
	int (*flush)(struct poulsback_display_domain *);
	int (*flip)(struct poulsback_display_domain *, struct poulsback_display_image *, u64);
	/* EAGAIN means not retired yet, with no mutation; any other failure is HOLD.
	 * serial zero authenticates a mapped image never selected for scanout. */
	int (*retirement)(struct poulsback_display_image *, u64);
	void *context;
};
enum poulsback_display_phase {
	POULSBACK_DISPLAY_EMPTY,
	POULSBACK_DISPLAY_MAPPED,
	POULSBACK_DISPLAY_ACTIVE,
	POULSBACK_DISPLAY_PENDING,
	POULSBACK_DISPLAY_HOLD,
};
/* GTT offsets of shown buffers are 256 KiB aligned (EMGD's async-flip rule). */
#define POULSBACK_DISPLAY_SLOT_ALIGN SZ_256K
/* Images one domain can bind, and its GTT window: eight 1280x800 buffers in
 * 256 KiB-aligned slots, the front and a fullscreen client's three back
 * buffers with room for replacements still retiring. */
#define POULSBACK_DISPLAY_IMAGES 8
#define POULSBACK_DISPLAY_WINDOW SZ_32M
/* The display's hold on one SCANOUT BO (poulsback_bo_display_pin): a GEM
 * reference and a display pin. The BO's WC pages stay pinned for its life.
 * held: HOLD, the pin is never dropped. */
struct poulsback_display_pages {
	struct drm_gem_object *object;
	struct page **pages;
	u64 size;
	u32 count;
	bool mapping_owned, held;
};
/* A framebuffer's view of its BO, fixed at ADDFB2; size is the BO's. */
struct poulsback_display_layout {
	u32 width, height, stride, format;
	u64 size;
};
struct poulsback_display_domain {
	struct drm_device *dev;
	const struct poulsback_display_provider *provider;
	struct poulsback_display_provider bound_provider;
	void *gtt_identity;
	void *bound_gtt_identity;
	struct page *dummy, *bound_dummy;
	u64 generation, bound_generation, fence_context, serial, bytes;
	u64 bound_start, bound_end;
	struct resource aperture;
	struct mutex lock;
	spinlock_t source_lock;
	bool initialized, held, transitioning;
	int primary_error;
	struct poulsback_display_image *images[POULSBACK_DISPLAY_IMAGES], *active, *pending;
	struct poulsback_display_extent extents[POULSBACK_DISPLAY_IMAGES];
	u32 count;
};
struct poulsback_display_image {
	struct poulsback_display_domain *domain;
	struct drm_gem_object *object;
	struct poulsback_display_pages pin;
	struct poulsback_display_layout layout;
	struct resource gtt;
	struct dma_fence *read_fence;
	u64 retirement_serial;
	u32 mapped;
	bool reserved, possible, tracked, object_ref, certificate;
	int primary_error, cleanup_error;
	enum poulsback_display_phase phase;
};
/* Heap/domain/provider/image storage survives HOLD. Domain mutex serializes all
 * operations; provider callbacks may sleep and must not recurse these APIs.
 * SGX VA and display GTT are separate addresses over the same BO pages.
 * All mappings retain an unsignalled BOOKKEEP dma-resv fence until full
 * retirement: GPU writes to a shown BO never wait on it, closing a handle
 * does not either. No user handle/address/packet is accepted. */
int poulsback_display_domain_init(struct poulsback_display_domain *, struct drm_device *,
				  const struct poulsback_display_provider *, void *gtt_identity,
				  struct page *dummy, u64 generation, u64 start, u64 size);
/* obj is a SCANOUT BO; layout is its framebuffer's checked view. */
int poulsback_display_prepare(struct poulsback_display_domain *, struct drm_gem_object *,
			      const struct poulsback_display_layout *,
			      struct poulsback_display_image *);
/* Before a show: wait (interruptible, bounded) for the BO's WRITE fences. A
 * failed writer refuses the show; a timeout returns -EBUSY. */
int poulsback_display_wait_writers(struct drm_gem_object *);
int poulsback_display_flip(struct poulsback_display_domain *, struct poulsback_display_image *);
int poulsback_display_disable(struct poulsback_display_domain *);
int poulsback_display_retire(struct poulsback_display_image *);
/* Post-switch CPU/source refusal, with domain.lock held. Preserve each image's
 * first error and pin custody; the provider retains its own switch/GEM facts.
 * This exact transition does not invoke callbacks or signal/remove anything. */
void poulsback_display_switch_hold_locked(struct poulsback_display_domain *, int error);
#endif
