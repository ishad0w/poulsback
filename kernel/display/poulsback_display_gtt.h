/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef POULSBACK_DISPLAY_GTT_H
#define POULSBACK_DISPLAY_GTT_H
#include <linux/types.h>
struct drm_device;
struct page;
struct poulsback_display_image;
struct poulsback_display_gtt_binding;
struct poulsback_display_domain;
struct poulsback_display_provider;
struct poulsback_display_gtt_reservation;
struct poulsback_display_psb_source;

#define POULSBACK_DISPLAY_GTT_MAX_PAGES 16384U
/* Trusted cached GMA500 source snapshot, under the provider's GTT/source lock.
 * Coordinates match the parent GTT resource, not SGX VA or host physical PA.
 * mapped_table_bytes is mapped PTE storage; active_entries is aperture pages.
 * The caller must authenticate the live driver/root/power/epoch around this
 * leaf. Repeating caller-supplied metadata is not hardware authentication. */
struct poulsback_display_gtt_source {
	struct drm_device *dev;
	void *gtt_identity;
	struct page *scratch;
	u64 generation, aperture_start, aperture_bytes, stolen_bytes;
	u64 mapped_table_bytes;
	u32 active_entries;
};
struct poulsback_display_gtt_entry {
	u64 resource_byte_address;
	u64 aperture_byte_offset;
	u32 slot, empty_pte, image_pte;
};
/* The binding's own state; first errors are kept for HOLD diagnosis. */
struct poulsback_display_gtt_report {
	u64 preallocated_bytes;
	u32 retained_pages, retained_scratch, prepared, held;
	int preparation_error, primary_error, cleanup_error;
};
/* Internal binding leaf. It performs no MMIO/PTE write, cache
 * maintenance, flip, provider registration or completion certification.
 * Caller serializes domain->lock then the actual provider GTT/source lock.
 * Parent domain/image/source/output-slot storage survives this owner/HOLD.
 * The caller's source lease keeps the scratch page live through prepare;
 * after its UC-minus/physical and post-sleep source checks, this owner takes its own
 * page reference. That reference survives possible publication or HOLD and is
 * released only by checked withdrawal of a retired mapping
 * (poulsback_display_gtt_dispose_withdrawn). It is not a source,
 * cache visibility, GTT publication or last-fetch certificate.
 * Create before the first cache/PTE effect; publish_cache may also attach it
 * after parent's possible flag.
 * A partial holder is reachable before page refs are acquired. */
int poulsback_display_gtt_prepare(struct poulsback_display_image *,
				  const struct poulsback_display_gtt_source *,
				  struct poulsback_display_gtt_binding **);
/* Cached intent only; never authority to access GTT or release an owner. */
int poulsback_display_gtt_entry(const struct poulsback_display_gtt_binding *, u32 index,
				struct poulsback_display_gtt_entry *);
/* Physical publication leaf, with domain lock held. Authenticate the binding
 * and this slot's exact canonical page/address before returning its words.
 * Unlike entry(), this can sleep while checking canonical ownership. The
 * caller must recheck its hardware source after return and before any store. */
int poulsback_display_gtt_publication_entry(struct poulsback_display_gtt_binding *,
					    struct poulsback_display_image *, struct page *, u64,
					    struct poulsback_display_gtt_entry *);
/* The binding is m's, with every page checked (bind, unbind, flip). */
int poulsback_display_gtt_publication_validate(struct poulsback_display_gtt_binding *,
					       struct poulsback_display_image *);
/* The binding is m's, by its tuple, owner and scratch only (per-page steps). */
int poulsback_display_gtt_publication_owned(struct poulsback_display_gtt_binding *,
					    struct poulsback_display_image *);
/* Keep the original binding/image graph after a possible publication failure.
 * This never removes PTEs, signals a fence, restores WB or releases references. */
void poulsback_display_gtt_publication_hold(struct poulsback_display_gtt_binding *, int, int);
/* Trusted provider only: after full empty-PTE/source readback of a retired
 * (never-selected or certified replaced) mapping. Domain/GTT/source ownership
 * stays live; this consumes only binding references, not the BO display pin,
 * resource or BOOKKEEP fence. Any refusal preserves the binding. */
int poulsback_display_gtt_dispose_withdrawn(struct poulsback_display_gtt_binding **,
					    struct poulsback_display_image *);
/* Metadata-only GMA500 envelope. Caller owns stable, disjoint domain/provider/
 * output-slot storage for its life or HOLD, and serializes API
 * lifetime. It runs under external admission (sgx535_epoch_external_begin).
 * This initializes the existing detached suballocator inside one actual
 * non-stolen GTT resource node for the caller's provider (the static owner's,
 * poulsback_display_psb_source.c). An allocated partial owner is returned on failure and
 * retained without retry. check is process-only, takes domain lock and external
 * admission; never call it recursively from a provider callback or an existing
 * source transaction on the external route. Its copied source is metadata, not
 * cache/PTE/plane/last-fetch authority. */
int poulsback_display_gtt_reservation_prepare(struct drm_device *, u64 bytes,
					      struct poulsback_display_domain *,
					      const struct poulsback_display_provider *,
					      struct poulsback_display_gtt_reservation **);
/* Process-side source-owner join. Exact reservation/domain membership and
 * optional pointer-sized owner-slot exclusion; metadata only, no writer permit.
 * No caller domain lock or source transaction may be held. */
int poulsback_display_gtt_reservation_source(struct poulsback_display_gtt_reservation *,
					     struct poulsback_display_domain *,
					     struct poulsback_display_gtt_source *, const void *,
					     const struct poulsback_display_psb_source *);
/* Provider-side check only: domain lock, external admission and actual GTT
 * mutex are already held. No recursive domain/admission acquisition or MMIO. */
int poulsback_display_gtt_reservation_source_locked(struct poulsback_display_gtt_reservation *,
						    struct poulsback_display_domain *,
						    const struct poulsback_display_psb_source *,
						    const struct poulsback_display_gtt_source *);
/* Source custody binds one exact owner before PM/register-read sleeps; it is
 * never released. */
int poulsback_display_gtt_reservation_source_acquire(struct poulsback_display_gtt_reservation *,
						     struct poulsback_display_domain *,
						     struct poulsback_display_psb_source *);
#endif
