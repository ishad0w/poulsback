// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

/* Retained source custody and the static BASE-only provider that shows
 * SCANOUT BO framebuffers at the legacy mode. A replaced buffer is certified
 * two frames after its successor's BASE store (pipe frame counter).
 */
#include <linux/debugfs.h>
#include <linux/err.h>
#include "poulsback_errsite.h"
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include "framebuffer.h"
#include "gma_display.h"
#include "gem.h"
#include "mmu.h"
#include "psb_drv.h"
#include "psb_intel_drv.h"
#include "psb_intel_reg.h"
#include "poulsback_display.h"
#include "poulsback_display_gtt.h"
#include "poulsback_display_psb_source.h"
#include "poulsback_framebuffer.h"
#include "power.h"
#include "poulsback_bo.h"
#include "poulsback_bo_uapi.h"
#include "sgx535_source_epoch.h"
#include "sgx535_debug.h"
#include <linux/bits.h>
#include <linux/limits.h>
#include <drm/drm_drv.h>
#include <drm/drm_crtc.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_vblank.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>


/* The original external operation's loss-zero gate. */
static int psb_source_epoch(u64 expected)
{
	struct sgx535_epoch_snapshot epoch;
	sgx535_epoch_snapshot(&epoch);
	return !epoch.bound || epoch.loss || epoch.epoch != expected ? POULSBACK_ERRSITE(-ESTALE) : 0;
}
struct psb_source_route {
	struct drm_connector *connector;
	struct drm_encoder *encoder;
};
struct psb_source_image {
	struct drm_framebuffer *fb;
	struct drm_gem_object *object;
	struct poulsback_framebuffer_display_owner owner;
};
/* shown: the typed framebuffer on the plane (fb NULL: the legacy source).
 * staged: the typed framebuffer of a transition in progress. Each holds a DRM
 * and a GEM reference, dropped once a successful transition replaces it. */
struct psb_source_switch {
	struct psb_source_image shown, staged;
	struct drm_framebuffer *previous, *next;
	struct psb_source_route *route;
	const struct drm_mode_set *request;
	u64 serial;
	u32 count;
	bool entered, restoring, held;
};
struct poulsback_display_psb_source {
	struct poulsback_display_psb_source *self;
	struct poulsback_display_psb_source **slot;
	struct drm_device *dev;
	struct drm_psb_private *pdev;
	struct drm_crtc *crtc;
	struct drm_plane *primary;
	struct drm_framebuffer *fb;
	struct drm_gem_object *object;
	struct psb_gem_display_pin pin;
	struct resource *root;
	const struct drm_format_info *format_info;
	u64 object_bytes;
	struct poulsback_display_domain *domain;
	struct poulsback_display_gtt_reservation *reservation;
	struct poulsback_display_gtt_source gtt;
	const struct psb_offset *regmap;
	u8 __iomem *vdc;
	struct drm_display_mode mode;
	u32 stride, width, height, format, pipe;
	u32 words[7];
	struct poulsback_display_psb_source_report report;
	struct psb_source_switch change;
	struct psb_base_context *base_context;
};
static int psb_source_registered(struct drm_device *dev, struct drm_crtc *crtc)
{
	struct drm_crtc *member;
	bool found = false;
	if (!drm_modeset_is_locked(&dev->mode_config.connection_mutex))
		return -EDEADLK;
	drm_modeset_lock_assert_held(&dev->mode_config.connection_mutex);
	drm_for_each_crtc(member, dev)
	{
		if (member == crtc)
			found = true;
	}
	/* Membership and exact producer table precede to_gma_crtc. */
	if (!found || crtc->dev != dev || crtc->helper_private != &psb_intel_helper_funcs ||
	    !crtc->primary)
		return POULSBACK_ERRSITE(-EXDEV);
	return 0;
}
static int psb_source_member(struct drm_device *dev, struct drm_crtc *crtc)
{
	int ret = psb_source_registered(dev, crtc);
	if (ret)
		return ret;
	if (!drm_modeset_is_locked(&crtc->mutex) || !drm_modeset_is_locked(&crtc->primary->mutex))
		return -EDEADLK;
	drm_modeset_lock_assert_held(&crtc->mutex);
	drm_modeset_lock_assert_held(&crtc->primary->mutex);
	return 0;
}
/* fb NULL: the framebuffer on the plane. */
static int psb_source_legacy_fb(struct drm_device *dev, struct drm_crtc *crtc,
				struct drm_framebuffer *fb)
{
	struct gma_crtc *gma;
	u64 end;
	int ret = psb_source_member(dev, crtc);
	if (ret)
		return ret;
	/* This leaf's static owner is positive legacy type authentication.
   * Preserve the separate negative native/private namespace guard. */
	if (!fb)
		fb = crtc->primary->fb;
	if (!fb || fb->dev != dev || !psb_framebuffer_is_legacy(fb))
		return POULSBACK_ERRSITE(-EXDEV);
	ret = poulsback_framebuffer_legacy_check(fb, NULL);
	if (ret)
		return ret;
	gma = to_gma_crtc(crtc);
	if (!IS_PSB(dev) || !gma->active || gma->pipe < 0 || gma->pipe > 1 ||
	    gma->plane != gma->pipe ||
	    to_drm_psb_private(dev)->plane_to_crtc_mapping[gma->pipe] != crtc ||
	    crtc->primary->crtc != crtc || !crtc->enabled || crtc->x || crtc->y ||
	    crtc->mode.hdisplay <= 0 || crtc->mode.vdisplay <= 0 || crtc->mode.hdisplay > 65536 ||
	    crtc->mode.vdisplay > 65536 || !fb->obj[0] || fb->obj[0]->dev != dev || !fb->format ||
	    fb->format->format != DRM_FORMAT_XRGB8888 || fb->format->cpp[0] != 4 ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->offsets[0] ||
	    fb->width < (u32)crtc->mode.hdisplay || fb->height < (u32)crtc->mode.vdisplay ||
	    (fb->pitches[0] & 63) || fb->pitches[0] < (u64)crtc->mode.hdisplay * 4)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	end = (u64)(crtc->mode.vdisplay - 1) * fb->pitches[0] + (u64)crtc->mode.hdisplay * 4;
	return end <= fb->obj[0]->size ? 0 : POULSBACK_ERRSITE(-ERANGE);
}
static int psb_source_legacy(struct drm_device *dev, struct drm_crtc *crtc)
{
	return psb_source_legacy_fb(dev, crtc, NULL);
}
static bool psb_source_same_gtt(const struct poulsback_display_gtt_source *a,
				const struct poulsback_display_gtt_source *b)
{
	return a->dev == b->dev && a->gtt_identity == b->gtt_identity && a->scratch == b->scratch &&
	       a->generation == b->generation && a->aperture_start == b->aperture_start &&
	       a->aperture_bytes == b->aperture_bytes && a->stolen_bytes == b->stolen_bytes &&
	       a->mapped_table_bytes == b->mapped_table_bytes &&
	       a->active_entries == b->active_entries;
}
static int psb_source_root_identity(struct poulsback_display_psb_source *s)
{
	struct drm_psb_private *p = to_drm_psb_private(s->dev);
	struct drm_crtc *c = s->crtc;
	int ret;
	if (s->pipe > 1)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = psb_source_registered(s->dev, c);
	if (ret)
		return ret;
	/* Original pointers are compared before following replacement data. */
	if ((s->report.registry_ref && READ_ONCE(p->poulsback_display_sources[s->pipe]) != s) ||
	    p != s->pdev || c->primary != s->primary || s->fb->obj[0] != s->object ||
	    p->vdc_reg != s->vdc || p->regmap != s->regmap || p->gtt_map != s->gtt.gtt_identity ||
	    p->scratch_page != s->gtt.scratch || p->gtt_mem != s->root ||
	    s->root->start != s->gtt.aperture_start ||
	    resource_size(s->root) != s->gtt.aperture_bytes ||
	    p->gtt.stolen_size != s->gtt.stolen_bytes ||
	    (u64)p->gtt.gtt_pages * PAGE_SIZE != s->gtt.mapped_table_bytes ||
	    p->gtt.gatt_pages != s->gtt.active_entries || s->object->size != s->object_bytes ||
	    s->fb->width != s->width || s->fb->height != s->height ||
	    s->fb->pitches[0] != s->stride || s->fb->format != s->format_info ||
	    s->fb->format->format != s->format || memcmp(&c->mode, &s->mode, sizeof(s->mode)) ||
	    to_gma_crtc(c)->pipe != (int)s->pipe)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
/* The plane shows the legacy source or a typed framebuffer this source
 * selected (shown, or staged during a transition); the legacy source stays
 * retained and checked either way. */
static int psb_source_identity(struct poulsback_display_psb_source *s)
{
	const struct psb_source_switch *w = &s->change;
	struct drm_framebuffer *fb;
	int ret = psb_source_root_identity(s);
	if (ret)
		return ret;
	fb = s->primary->fb;
	if (!fb || (fb != s->fb && fb != w->shown.fb && fb != w->staged.fb))
		return POULSBACK_ERRSITE(-ESTALE);
	return psb_source_legacy_fb(s->dev, s->crtc, s->fb);
}
bool poulsback_display_psb_source_retained(struct drm_device *dev)
{
	struct drm_psb_private *p;
	if (!dev)
		return false;
	/* REMOVE has closed finalization before these cached root reads. */
	p = to_drm_psb_private(dev);
	return READ_ONCE(p->poulsback_display_sources[0]) ||
	       READ_ONCE(p->poulsback_display_sources[1]) ||
	       READ_ONCE(p->poulsback_display_static_owners[0]) ||
	       READ_ONCE(p->poulsback_display_static_owners[1]);
}
static int psb_source_claim_check(void *context)
{
	struct poulsback_display_psb_source *s = context;
	int ret = psb_source_identity(s);
	if (ret)
		return ret;
	return s->self == s && !*s->slot && !s->report.registry_ref &&
			       !READ_ONCE(s->pdev->poulsback_display_sources[s->pipe]) &&
			       s->report.module_ref && s->report.device_ref &&
			       s->report.framebuffer_ref
		       ? 0
		       : POULSBACK_ERRSITE(-ESTALE);
}
static void psb_source_claim_commit(void *context)
{
	struct poulsback_display_psb_source *s = context;
	WRITE_ONCE(s->pdev->poulsback_display_sources[s->pipe], s);
	s->report.registry_ref = 1;
	*s->slot = s;
}
static int psb_source_pin_identity(const struct psb_gem_display_pin *pin, void *context)
{
	struct poulsback_display_psb_source *s = context;
	int ret = psb_source_identity(s);
	if (ret)
		return ret;
	if (pin != &s->pin || pin->dev != s->dev || pin->object != s->object ||
	    pin->pipe != s->pipe || pin->epoch != s->gtt.generation ||
	    pin->offset != s->words[2] || !s->report.registry_ref || !s->report.reservation_ref ||
	    !s->report.power_ref || s->report.pin_ref != pin->acquired || s->report.held)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
static int psb_source_pin_lock(struct poulsback_display_psb_source *s)
{
	return psb_gem_display_pin_lock(s->dev, s->object, &s->pin);
}
static int psb_source_pin_acquire(struct poulsback_display_psb_source *s)
{
	return psb_gem_display_pin_acquire(s->dev, s->object, s->pipe, s->gtt.generation, &s->pin,
					   psb_source_pin_identity, s);
}
/* GTT owns domain/GTT locks; source_lock serializes the final borrower/root
 * transition. These methods neither acquire locks nor access registers. */
int poulsback_display_psb_source_link_check(struct poulsback_display_psb_source *s,
					    struct poulsback_display_gtt_reservation *reservation,
					    struct poulsback_display_domain *domain)
{
	if (!s || s->self != s || *s->slot != s || s->pipe > 1 || s->report.held ||
	    s->reservation != reservation || s->domain != domain || !s->report.registry_ref ||
	    READ_ONCE(s->pdev->poulsback_display_sources[s->pipe]) != s ||
	    s->report.reservation_ref)
		return POULSBACK_ERRSITE(-ESTALE);
	return psb_source_identity(s);
}
void poulsback_display_psb_source_link_commit(struct poulsback_display_psb_source *s)
{
	s->report.reservation_ref = 1;
}
static bool psb_source_output(struct poulsback_display_psb_source *s, const void *out, u64 bytes)
{
	return !poulsback_overlap(out, bytes, s, sizeof(*s)) &&
	       !poulsback_overlap(out, bytes, s->dev, sizeof(*s->dev)) &&
	       !poulsback_overlap(out, bytes, s->pdev, sizeof(*s->pdev)) &&
	       !poulsback_overlap(out, bytes, s->crtc, sizeof(struct gma_crtc)) &&
	       !poulsback_overlap(out, bytes, s->primary, sizeof(*s->primary)) &&
	       !poulsback_overlap(out, bytes, s->fb, sizeof(*s->fb)) &&
	       !poulsback_overlap(out, bytes, s->object, sizeof(struct psb_gem_object)) &&
	       !poulsback_overlap(out, bytes, s->regmap, 2 * sizeof(*s->regmap)) &&
	       !poulsback_overlap(out, bytes, s->gtt.gtt_identity, s->gtt.mapped_table_bytes);
}
static const u32 *psb_source_expected(struct poulsback_display_psb_source *);
/* Existing source register check, under the original external operation and
 * GTT mutex. No allocation, PM resume, source recursion or register write.
 * Later reads must equal the provider's current words (its last BASE). */
static int psb_source_registers(struct poulsback_display_psb_source *s, bool initial, u32 words[7])
{
	const struct psb_offset *map;
	int ret;
	lockdep_assert_held(&s->pdev->gtt_mutex);
	ret = psb_source_identity(s);
	if (!ret)
		ret = psb_source_epoch(s->gtt.generation);
	if (!ret) {
		map = &s->regmap[s->pipe];
		/* PSB fixed map, not another chip's SURF branch. Validate offsets
     * before any read through the inherited register accessor. */
		if (map->cntr != (s->pipe ? DSPBCNTR : DSPACNTR) ||
		    map->base != (s->pipe ? DSPBBASE : DSPABASE) ||
		    map->stride != (s->pipe ? DSPBSTRIDE : DSPASTRIDE) ||
		    map->size != (s->pipe ? DSPBSIZE : DSPASIZE) ||
		    map->src != (s->pipe ? PIPEBSRC : PIPEASRC) ||
		    map->conf != (s->pipe ? PIPEBCONF : PIPEACONF))
			ret = POULSBACK_ERRSITE(-EXDEV);
		else {
			words[0] = REGISTER_READ(s->dev, PSB_PGETBL_CTL);
			words[1] = REGISTER_READ(s->dev, map->cntr);
			words[2] = REGISTER_READ(s->dev, map->base);
			words[3] = REGISTER_READ(s->dev, map->stride);
			words[4] = REGISTER_READ(s->dev, map->size);
			words[5] = REGISTER_READ(s->dev, map->src);
			words[6] = REGISTER_READ(s->dev, map->conf);
			if (words[0] != (s->pdev->pge_ctl | _PSB_PGETBL_ENABLED) ||
			    !(words[1] & BIT(31)) || !(words[6] & BIT(31)) ||
			    (words[1] & DISPPLANE_SEL_PIPE_MASK) !=
				    (s->pipe ? DISPPLANE_SEL_PIPE_B : DISPPLANE_SEL_PIPE_A) ||
			    (words[1] & DISPPLANE_PIXFORMAT_MASK) != DISPPLANE_32BPP_NO_ALPHA ||
			    words[3] != s->stride ||
			    words[4] != ((u32)(s->mode.vdisplay - 1) << 16 |
					 (u32)(s->mode.hdisplay - 1)) ||
			    words[5] != ((u32)(s->mode.hdisplay - 1) << 16 |
					 (u32)(s->mode.vdisplay - 1)) ||
			    (u64)words[2] >= s->gtt.aperture_bytes ||
			    (u64)(s->mode.vdisplay - 1) * s->stride + (u64)s->mode.hdisplay * 4 >
				    s->gtt.aperture_bytes - words[2])
				ret = POULSBACK_ERRSITE(-ESTALE);
			if (!ret && !initial &&
			    memcmp(words, psb_source_expected(s), sizeof(s->words)))
				ret = POULSBACK_ERRSITE(-ESTALE);
		}
	}
	if (!ret)
		ret = psb_source_identity(s);
	return ret;
}
static int psb_source_read(struct poulsback_display_psb_source *s, bool initial)
{
	struct poulsback_display_gtt_source now;
	u32 words[7];
	bool pin_locked = false;
	int ret;
	ret = poulsback_display_gtt_reservation_source(s->reservation, s->domain, &now, NULL, s);
	if (ret || !psb_source_same_gtt(&now, &s->gtt))
		return ret ?: POULSBACK_ERRSITE(-ESTALE);
	ret = psb_source_identity(s);
	if (ret || !s->report.power_ref)
		return ret ?: POULSBACK_ERRSITE(-ESTALE);
	ret = sgx535_epoch_external_begin(s->dev);
	if (ret)
		return ret;
	if (s->report.pin_ref) {
		ret = psb_source_pin_lock(s);
		if (ret)
			goto external_end;
		pin_locked = true;
	}
	mutex_lock(&s->pdev->gtt_mutex);
	ret = psb_source_registers(s, initial, words);
	mutex_unlock(&s->pdev->gtt_mutex);
	if (pin_locked)
		psb_gem_display_pin_unlock(s->object);
external_end:
	if (sgx535_epoch_external_end(s->dev) && !ret)
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret)
		ret = poulsback_display_gtt_reservation_source(s->reservation, s->domain, &now,
							       NULL, s);
	if (!ret && !psb_source_same_gtt(&now, &s->gtt))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret)
		ret = psb_source_identity(s);
	if (!ret && initial)
		memcpy(s->words, words, sizeof(words));
	return ret;
}
/* prepare/check establish source ownership only,
 * not a provider, scanout admission or cached-register writer authority.
 * Caller holds actual connection/CRTC/primary modeset locks for every method;
 * source and original output-slot storage survive all errors/HOLD. No domain
 * lock may be held on entry. The external source requires no existing
 * external/source transaction and still refuses active SGX.
 * The exact reservation/domain must already exist.
 * Only already-active power and one specifically owned resident legacy pin
 * are retained; no resume, PTE/cache or plane effect is performed. Old framebuffer/GEM are never cast to PSB BOs. */
static int
poulsback_display_psb_source_prepare(struct drm_device *dev, struct drm_crtc *crtc,
				     struct poulsback_display_gtt_reservation *reservation,
				     struct poulsback_display_domain *domain,
				     struct poulsback_display_psb_source **out)
{
	struct poulsback_display_psb_source *s, original = {};
	int ret, idx;
	if (!dev || !crtc || !reservation || !domain || !out)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = psb_source_legacy(dev, crtc);
	if (ret)
		goto exit;
	original.dev = dev;
	original.pdev = to_drm_psb_private(dev);
	original.crtc = crtc;
	original.primary = crtc->primary;
	original.fb = crtc->primary->fb;
	original.object = original.fb->obj[0];
	original.object_bytes = original.object->size;
	original.format_info = original.fb->format;
	original.root = original.pdev->gtt_mem;
	original.regmap = original.pdev->regmap;
	original.vdc = original.pdev->vdc_reg;
	original.mode = crtc->mode;
	original.pipe = to_gma_crtc(crtc)->pipe;
	original.width = original.fb->width;
	original.height = original.fb->height;
	original.stride = original.fb->pitches[0];
	original.format = original.fb->format->format;
	original.domain = domain;
	original.reservation = reservation;
	ret = poulsback_display_gtt_reservation_source(reservation, domain, &original.gtt, out,
						       NULL);
	if (ret)
		goto exit;
	if (!original.vdc || !original.regmap || !psb_source_output(&original, out, sizeof(*out)) ||
	    poulsback_overlap(out, sizeof(*out), domain, sizeof(*domain)) || *out) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto exit;
	}
	ret = sgx535_epoch_external_begin(dev);
	if (ret)
		goto exit;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		ret = -ENOMEM;
		goto admission_end;
	}
	*s = original;
	s->self = s;
	s->slot = out;
	ret = psb_source_identity(s);
	if (!ret && (!psb_source_output(s, out, sizeof(*out)) || *out))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret && !try_module_get(THIS_MODULE))
		ret = POULSBACK_ERRSITE(-ENODEV);
	if (ret) {
		kfree(s);
		goto admission_end;
	}
	drm_dev_get(dev);
	drm_framebuffer_get(s->fb);
	s->report.version = 1;
	s->report.module_ref = s->report.device_ref = s->report.framebuffer_ref = 1;
	ret = sgx535_epoch_external_finalize(dev, s->gtt.generation, psb_source_claim_check,
					     psb_source_claim_commit, s);
	if (ret) {
		/* No source/pin/PAT effect preceded a rejected publication. */
		drm_framebuffer_put(s->fb);
		drm_dev_put(dev);
		module_put(THIS_MODULE);
		kfree(s);
		goto admission_end;
	}
	WARN_ON(sgx535_epoch_external_end(dev));
	/* Both roots precede sleeping reservation and active-only PM access. */
	ret = poulsback_display_gtt_reservation_source_acquire(reservation, domain, s);
	if (!ret) {
		s->report.power_ref = gma_power_begin(dev, false);
		ret = s->report.power_ref ? psb_source_read(s, true) : -EAGAIN;
	}
	if (!ret) {
		ret = psb_source_pin_acquire(s);
		if (s->pin.acquired) {
			s->report.pin_ref = 1;
			s->report.pin_kind = s->pin.kind;
		}
		if (!ret) {
			/* Pin admission/resv acquisition can sleep; preserve the original
			 * framebuffer and register snapshot through another source check. */
			ret = psb_source_read(s, false);
		}
	}
	if (ret) {
		s->report.primary_error = ret;
		s->report.held = 1;
	} else {
		s->report.prepared = 1;
		s->report.generation = s->gtt.generation;
		s->report.pipe = s->pipe;
		s->report.width = s->mode.hdisplay;
		s->report.height = s->mode.vdisplay;
		s->report.stride = s->stride;
		s->report.format = s->format;
		s->report.pgetbl = s->words[0];
		s->report.plane_control = s->words[1];
		s->report.plane_base = s->words[2];
		s->report.plane_size = s->words[4];
		s->report.pipe_source = s->words[5];
	}
	goto exit;
admission_end:
	WARN_ON(sgx535_epoch_external_end(dev));
exit:
	drm_dev_exit(idx);
	return ret;
}
static int poulsback_display_psb_source_check(struct poulsback_display_psb_source *s)
{
	int ret, idx;
	if (!s || s->self != s || *s->slot != s || !s->report.prepared || s->report.held ||
	    !s->report.registry_ref)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(s->dev, &idx))
		ret = POULSBACK_ERRSITE(-ENODEV);
	else {
		ret = psb_source_read(s, false);
		drm_dev_exit(idx);
	}
	if (ret) {
		if (!s->report.primary_error)
			s->report.primary_error = ret;
		s->report.held = 1;
	}
	return ret;
}
static int psb_source_unselected(struct poulsback_display_psb_source *,
				 struct poulsback_display_image *);
static int psb_source_neighbor_words(struct poulsback_display_psb_source *,
				     struct poulsback_display_image *);
static int psb_source_gtt(struct poulsback_display_psb_source *s,
			  struct poulsback_display_gtt_binding *binding,
			  struct poulsback_display_image *m, struct page *page, u64 address,
			  bool flush, bool withdraw)
{
	struct poulsback_display_gtt_entry entry;
	u32 words[7], i, count;
	u32 __iomem *slot;
	bool pin_locked = false, gtt_locked = false, sweep;
	int ret, cleanup = 0, idx;
	if (!s || s->self != s || !s->slot || *s->slot != s || !binding || !m ||
	    m->domain != s->domain || !s->report.prepared || s->report.held ||
	    !s->report.registry_ref || !s->report.reservation_ref || !s->report.pin_ref ||
	    !s->report.power_ref || !m->possible || !m->pin.mapping_owned)
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&s->domain->lock);
	count = m->pin.count;
	if (!count || count > POULSBACK_DISPLAY_GTT_MAX_PAGES ||
	    (withdraw && (!m->mapped && !flush)) ||
	    (!flush && (!page || address < m->gtt.start || address > m->gtt.end ||
			(address & (PAGE_SIZE - 1)) ||
			(address - m->gtt.start) / PAGE_SIZE != m->mapped - withdraw)) ||
	    (flush && m->mapped != (withdraw ? 0 : count)))
		return POULSBACK_ERRSITE(-EINVAL);
	/* Reject a foreign binding before any source effect or failure retention
	 * can touch it. This may sleep; source identity is checked afterwards. */
	ret = flush ? poulsback_display_gtt_publication_validate(binding, m)
		    : poulsback_display_gtt_publication_entry(binding, m, page, address, &entry);
	if (ret)
		return ret;
	/* Sweep the other images' PTEs before the first withdrawn page and around
	 * the flush, not per page (docs/kernel/display.md). */
	sweep = withdraw && (flush || m->mapped == count);
	if (withdraw && psb_source_unselected(s, m))
		return POULSBACK_ERRSITE(-ESTALE);
	if (!drm_dev_enter(s->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = psb_source_identity(s);
	if (ret)
		goto exit;
	ret = sgx535_epoch_external_begin(s->dev);
	if (ret)
		goto exit;
	/* Legacy source pin locking may sleep; canonical checks below may also
	 * sleep. The source/register tuple is repeated after both, before store. */
	ret = psb_source_pin_lock(s);
	if (ret)
		goto end;
	pin_locked = true;
	mutex_lock(&s->pdev->gtt_mutex);
	gtt_locked = true;
	ret = poulsback_display_gtt_reservation_source_locked(s->reservation, s->domain, s,
							      &s->gtt);
	if (!ret)
		ret = psb_source_registers(s, false, words);
	if (!ret)
		ret = flush ? poulsback_display_gtt_publication_validate(binding, m)
			    : poulsback_display_gtt_publication_entry(binding, m, page, address,
								      &entry);
	if (!ret)
		ret = poulsback_display_gtt_reservation_source_locked(s->reservation, s->domain, s,
								      &s->gtt);
	if (!ret)
		ret = psb_source_registers(s, false, words);
	if (!ret && m->pin.count != count)
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret && withdraw)
		ret = psb_source_unselected(s, m);
	if (!ret && sweep)
		ret = psb_source_neighbor_words(s, m);
	if (ret)
		goto end;
	if (flush) {
		/* One full binding check above and one below enclose the complete
		 * owned range; cached entry reads do not acquire another source gate. */
		for (i = 0; i < count && !ret; i++) {
			ret = poulsback_display_gtt_entry(binding, i, &entry);
			if (!ret && ioread32(s->pdev->gtt_map + entry.slot) !=
					    (withdraw ? entry.empty_pte : entry.image_pte))
				ret = POULSBACK_ERRSITE(-ESTALE);
		}
	} else {
		slot = s->pdev->gtt_map + entry.slot;
		if (ioread32(slot) != (withdraw ? entry.image_pte : entry.empty_pte))
			ret = POULSBACK_ERRSITE(-ESTALE);
		else {
			/* Parent possible flags, canonical claim/pin/read fence, binding
			 * slot and per-pipe source roots already precede this first store.
			 * An error cannot cancel any of that graph or restore EMPTY. */
			ret = psb_source_registers(s, false, words);
			if (!ret)
				iowrite32(withdraw ? entry.empty_pte : entry.image_pte, slot);
			if (!ret &&
			    ioread32(slot) != (withdraw ? entry.empty_pte : entry.image_pte))
				ret = POULSBACK_ERRSITE(-EIO);
		}
	}
	if (!ret && sweep)
		ret = psb_source_neighbor_words(s, m);
	if (!ret)
		ret = flush ? poulsback_display_gtt_publication_validate(binding, m)
			    : poulsback_display_gtt_publication_entry(binding, m, page, address,
								      &entry);
	if (!ret)
		ret = poulsback_display_gtt_reservation_source_locked(s->reservation, s->domain, s,
								      &s->gtt);
	if (!ret)
		ret = psb_source_registers(s, false, words);
end:
	if (gtt_locked)
		mutex_unlock(&s->pdev->gtt_mutex);
	if (pin_locked)
		psb_gem_display_pin_unlock(s->object);
	cleanup = sgx535_epoch_external_end(s->dev);
	if (cleanup && !s->report.cleanup_error)
		s->report.cleanup_error = cleanup;
	if (!ret && !cleanup) {
		/* end may run sleeping diagnostics before releasing admission.
		 * Check CPU source identity again, without MMIO after that release. */
		ret = psb_source_epoch(s->gtt.generation);
		if (!ret)
			ret = psb_source_identity(s);
	}
	ret = ret ?: cleanup;
exit:
	if (ret) {
		poulsback_display_gtt_publication_hold(binding, ret, cleanup);
		if (!s->report.primary_error)
			s->report.primary_error = ret;
		s->report.held = 1;
	}
	drm_dev_exit(idx);
	return ret;
}
/* Existing provider's physical GTT leaves only, not provider registration or
 * plane admission. Caller holds the original modeset and domain locks. The
 * complete old source and canonical binding/image owner survive every error.
 * Insert accepts one exact next EMPTY slot; flush observes all owned IMAGE
 * words. Posted readback establishes neither SGX visibility nor last fetch.
 * There is deliberately no removal, rollback, BASE or release operation. */
static int poulsback_display_psb_source_gtt_insert(struct poulsback_display_psb_source *s,
						   struct poulsback_display_gtt_binding *binding,
						   struct poulsback_display_image *m,
						   struct page *page, u64 address)
{
	return psb_source_gtt(s, binding, m, page, address, false, false);
}
static int poulsback_display_psb_source_gtt_flush(struct poulsback_display_psb_source *s,
						  struct poulsback_display_gtt_binding *binding,
						  struct poulsback_display_image *m)
{
	return psb_source_gtt(s, binding, m, NULL, 0, true, false);
}
static int psb_switch_route(struct poulsback_display_psb_source *s, const struct drm_mode_set *set)
{
	struct psb_source_switch *w = &s->change;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	u32 found = 0, i;
	int ret = 0;
	if (!set->connectors || set->num_connectors <= 0 || (u32)set->num_connectors != w->count ||
	    !w->route)
		return POULSBACK_ERRSITE(-ESTALE);
	drm_connector_list_iter_begin(s->dev, &iter);
	drm_for_each_connector_iter(connector, &iter)
	{
		if (!connector->encoder || connector->encoder->crtc != s->crtc)
			continue;
		for (i = 0; i < w->count; i++)
			if (w->route[i].connector == connector)
				break;
		if (i == w->count || connector->dev != s->dev ||
		    connector->encoder->dev != s->dev ||
		    connector->encoder != w->route[i].encoder || set->connectors[i] != connector) {
			ret = POULSBACK_ERRSITE(-ESTALE);
			break;
		}
		found++;
	}
	drm_connector_list_iter_end(&iter);
	if (!ret && found != w->count)
		ret = POULSBACK_ERRSITE(-ESTALE);
	drm_for_each_encoder(encoder, s->dev)
	{
		if (ret || encoder->crtc != s->crtc)
			continue;
		for (i = 0; i < w->count; i++)
			if (w->route[i].encoder == encoder)
				break;
		if (i == w->count || encoder->dev != s->dev)
			ret = POULSBACK_ERRSITE(-ESTALE);
	}
	return ret;
}
static int psb_switch_entry(struct poulsback_display_psb_source *s,
			    const struct psb_source_image *r)
{
	if (!r->fb || !r->object || !r->owner.framebuffer || !r->owner.image ||
	    !poulsback_framebuffer_is_object(r->fb) || r->fb->dev != s->dev ||
	    r->fb->obj[0] != r->object || r->owner.image->object != r->object ||
	    r->owner.image->domain != s->domain)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
static int psb_switch_images(struct poulsback_display_psb_source *s)
{
	struct psb_source_switch *w = &s->change;
	int ret = 0;
	if (w->shown.fb)
		ret = psb_switch_entry(s, &w->shown);
	if (!ret && w->staged.fb)
		ret = psb_switch_entry(s, &w->staged);
	if (ret)
		return ret;
	if ((w->shown.fb &&
	     (w->shown.fb == w->staged.fb || w->shown.owner.image == w->staged.owner.image)) ||
	    w->previous != (w->shown.fb ?: s->fb) ||
	    (w->restoring ? !w->shown.fb || w->staged.fb || w->next != s->fb
			  : !w->staged.fb || w->next != w->staged.fb))
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
static int psb_switch_tuple(struct poulsback_display_psb_source *s, const struct drm_mode_set *set,
			    struct drm_framebuffer *expected)
{
	struct psb_source_switch *w = &s->change;
	/* The typed framebuffer of this transition: the one leaving on restore. */
	const struct psb_source_image *t = w->restoring ? &w->shown : &w->staged;
	int ret = psb_source_root_identity(s);
	if (!ret)
		ret = psb_switch_images(s);
	if (ret)
		return ret;
	if (!s->report.prepared || s->report.held || !s->report.pin_ref || !s->report.power_ref ||
	    !s->report.registry_ref || !s->report.reservation_ref || !set || set->crtc != s->crtc ||
	    set->fb != w->next || !set->mode || !drm_mode_equal(set->mode, &s->mode) || set->x ||
	    set->y || s->crtc->x || s->crtc->y || s->primary->crtc != s->crtc ||
	    to_gma_crtc(s->crtc)->plane != (int)s->pipe ||
	    s->pdev->plane_to_crtc_mapping[s->pipe] != s->crtc || s->primary->fb != expected ||
	    s->primary->old_fb != w->previous || to_gma_crtc(s->crtc)->page_flip_event ||
	    !s->crtc->enabled || !to_gma_crtc(s->crtc)->active || !t->fb->format ||
	    !psb_framebuffer_is_legacy(s->fb) || s->format != DRM_FORMAT_XRGB8888 ||
	    s->format_info->cpp[0] != 4 || s->format_info->depth != 24 ||
	    s->format_info->num_planes != 1 ||
	    (t->fb->format->format != DRM_FORMAT_ARGB8888 &&
	     t->fb->format->format != DRM_FORMAT_XRGB8888) ||
	    t->fb->format->cpp[0] != 4 || t->fb->format->num_planes != 1 ||
	    t->fb->modifier != DRM_FORMAT_MOD_LINEAR || t->fb->offsets[0] || s->width != 1280 ||
	    s->height != 800 || s->stride != 5120 || s->mode.hdisplay != 1280 ||
	    s->mode.vdisplay != 800 || t->fb->width != 1280 || t->fb->height != 800 ||
	    t->fb->pitches[0] != 5120 ||
	    (s->words[1] & DISPPLANE_PIXFORMAT_MASK) != DISPPLANE_32BPP_NO_ALPHA)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = psb_switch_route(s, set);
	if (!ret)
		ret = psb_source_epoch(s->gtt.generation);
	return ret;
}
static void psb_switch_release(struct psb_source_image *r)
{
	if (r->object)
		drm_gem_object_put(r->object);
	if (r->fb)
		drm_framebuffer_put(r->fb);
	memset(r, 0, sizeof(*r));
}
struct psb_static_owner;
static int psb_static_pin(struct psb_static_owner *, struct poulsback_framebuffer *);
/* Stage a typed framebuffer to replace the plane's current one. A framebuffer
 * not yet mapped is prepared now, also while another one is shown. */
static int psb_switch_stage(struct poulsback_display_psb_source *s, struct drm_mode_set *set)
{
	struct psb_source_switch *w = &s->change;
	struct psb_source_image *staged = &w->staged;
	struct drm_framebuffer *previous = w->shown.fb ?: s->fb;
	struct poulsback_framebuffer_report report;
	struct poulsback_framebuffer *native;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	u32 i, j;
	int ret;
	if (!poulsback_framebuffer_is_object(set->fb) || set->fb->dev != s->dev ||
	    s->primary->fb != previous || s->primary->old_fb != previous || !set->mode ||
	    !drm_mode_equal(set->mode, &s->mode) || set->x || set->y || set->num_connectors <= 0 ||
	    set->num_connectors > s->dev->mode_config.num_connector || !set->connectors ||
	    (size_t)set->num_connectors > SIZE_MAX / sizeof(*w->route))
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (staged->fb || w->restoring)
		return POULSBACK_ERRSITE(-EBUSY);
	ret = w->route ? psb_switch_route(s, set) : 0;
	if (!ret)
		ret = poulsback_display_psb_source_check(s);
	if (ret)
		return ret;
	native = poulsback_framebuffer_from_base(set->fb, s->dev);
	if (IS_ERR(native))
		return PTR_ERR(native);
	ret = poulsback_framebuffer_status(native, &report);
	if (ret)
		return ret;
	/* This durable root precedes pin/owner/route allocation sleeps. It is
	 * independent of core's temporary old_fb and ioctl lookup references. */
	drm_framebuffer_get(set->fb);
	staged->fb = set->fb;
	w->previous = previous;
	w->next = set->fb;
	staged->object = poulsback_framebuffer_object_get(set->fb, s->dev);
	if (IS_ERR(staged->object)) {
		ret = PTR_ERR(staged->object);
		staged->object = NULL;
		return ret;
	}
	if (!report.pin_attempted) {
		ret = psb_static_pin(READ_ONCE(s->pdev->poulsback_display_static_owners[s->pipe]),
				     native);
		if (ret)
			return ret;
	}
	ret = poulsback_framebuffer_display_owner(set->fb, s->dev, s->domain, &staged->owner);
	if (ret)
		return ret;
	if (w->route)
		return psb_switch_tuple(s, set, previous);
	w->count = set->num_connectors;
	w->route = kzalloc(w->count * sizeof(*w->route), GFP_KERNEL);
	if (!w->route)
		return -ENOMEM;
	for (i = 0; i < w->count; i++) {
		for (j = 0; j < i; j++)
			if (set->connectors[j] == set->connectors[i])
				return POULSBACK_ERRSITE(-EINVAL);
		drm_connector_list_iter_begin(s->dev, &iter);
		drm_for_each_connector_iter(connector, &iter)
		{
			if (connector != set->connectors[i])
				continue;
			if (connector->dev == s->dev && connector->encoder &&
			    connector->encoder->dev == s->dev &&
			    connector->encoder->crtc == s->crtc) {
				drm_connector_get(connector);
				w->route[i] =
					(struct psb_source_route){connector, connector->encoder};
			}
			break;
		}
		drm_connector_list_iter_end(&iter);
		if (!w->route[i].connector)
			return POULSBACK_ERRSITE(-EXDEV);
	}
	ret = poulsback_framebuffer_display_owner(set->fb, s->dev, s->domain, &staged->owner);
	return ret ?: psb_switch_tuple(s, set, previous);
}
/* Provider-side software witness only. Domain/external admission and modeset
 * locks already held; this authenticates no hardware register or last fetch. */
static int poulsback_display_psb_switch_check(struct poulsback_display_psb_source *s,
					      struct poulsback_display_image *m, u64 serial)
{
	struct psb_source_switch *w;
	struct poulsback_display_domain *d;
	int ret;
	if (!s || s->self != s || *s->slot != s)
		return POULSBACK_ERRSITE(-EINVAL);
	w = &s->change;
	d = s->domain;
	lockdep_assert_held(&d->lock);
	if (!w->entered || !w->request || w->held || !sgx535_epoch_external_active(s->dev) ||
	    !d->transitioning || d->pending != m || !serial || serial != d->serial ||
	    serial != w->serial || d->active != w->shown.owner.image ||
	    m != (w->restoring ? NULL : w->staged.owner.image))
		return POULSBACK_ERRSITE(-ESTALE);
	ret = psb_switch_tuple(s, w->request, w->next);
	if (!ret)
		ret = psb_source_epoch(s->gtt.generation);
	return ret;
}

/* The enclosing provider owns context, CRTC and source-slot storage through
 * domain teardown/HOLD. Install this one identity callback before reservation
 * creation; never replace the domain's immutable provider tuple. BOOTSTRAP
 * authenticates only an unused metadata domain, not cache/PTE/plane work.
 * Attach the fully prepared source before preparing any image.
 * Original modeset locks serialize source-slot reads;
 * domain.lock serializes attach/bind/release and every later callback.
 * Slots: free (no image), preparing (image, not bound) or bound. A slot keeps
 * its address for its binding's life; any bound slot may be withdrawn once
 * retired and is then reused. frame: pipe frame count at the BASE store that
 * replaced this slot's image on the plane. */
enum psb_base_phase { PSB_BASE_BOOTSTRAP, PSB_BASE_SOURCE, PSB_BASE_MAPPED };
struct psb_base_context {
	struct psb_base_context *self;
	struct poulsback_display_psb_source *source;
	struct poulsback_display_psb_source **source_slot;
	struct poulsback_display_domain *domain;
	struct drm_device *dev;
	struct drm_crtc *crtc;
	struct resource *root;
	struct poulsback_display_gtt_source bootstrap;
	enum psb_base_phase phase;
	struct {
		struct poulsback_display_image *image;
		struct poulsback_display_gtt_binding *binding, *retained_binding;
		u32 frame;
		bool bound;
	} images[POULSBACK_DISPLAY_IMAGES];
	struct poulsback_display_image *current_image, *preparing, *withdrawing;
	u32 original[7], current_words[7], count;
	bool held;
	int primary_error, cleanup_error;
};
static const u32 *psb_source_expected(struct poulsback_display_psb_source *s)
{
	struct psb_base_context *c = s->base_context;
	return c && c->self == c && c->source == s ? c->current_words : s->words;
}
/* The one slot holding m, or -1. */
static int psb_base_slot(const struct psb_base_context *c, const struct poulsback_display_image *m)
{
	int i, found = -1;
	for (i = 0; m && i < (int)ARRAY_SIZE(c->images); i++)
		if (c->images[i].image == m) {
			if (found >= 0)
				return -1;
			found = i;
		}
	return found;
}
/* A bound image that retirement certified (never shown, or replaced and past
 * its frames), off the plane, with no transition or preparation running. */
static int psb_source_unselected(struct poulsback_display_psb_source *s,
				 struct poulsback_display_image *m)
{
	struct psb_base_context *c = s ? s->base_context : NULL;
	struct poulsback_display_domain *d = m ? m->domain : NULL;
	int slot;
	if (!c || c->self != c || c->source != s || c->domain != d || c->held ||
	    c->phase != PSB_BASE_MAPPED || !c->count || c->count > ARRAY_SIZE(c->images) ||
	    (slot = psb_base_slot(c, m)) < 0 || !c->images[slot].bound || c->withdrawing != m ||
	    c->current_image == m || c->preparing ||
	    (m->phase != POULSBACK_DISPLAY_MAPPED && m->phase != POULSBACK_DISPLAY_PENDING) ||
	    d->active == m || d->pending || d->transitioning)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
/* Every other bound image keeps all of its PTEs while one is withdrawn. */
static int psb_source_neighbor_words(struct poulsback_display_psb_source *s,
				     struct poulsback_display_image *m)
{
	struct psb_base_context *c = s->base_context;
	struct poulsback_display_gtt_entry e;
	u32 i, page;
	int ret = psb_source_unselected(s, m);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(c->images) && !ret; i++) {
		struct poulsback_display_image *neighbor = c->images[i].image;
		if (!neighbor || neighbor == m)
			continue;
		if (!c->images[i].bound || !c->images[i].binding ||
		    c->images[i].binding != c->images[i].retained_binding ||
		    neighbor->mapped != neighbor->pin.count)
			return POULSBACK_ERRSITE(-ESTALE);
		for (page = 0; page < neighbor->pin.count && !ret; page++) {
			ret = poulsback_display_gtt_entry(c->images[i].binding, page, &e);
			if (!ret && ioread32(s->pdev->gtt_map + e.slot) != e.image_pte)
				ret = POULSBACK_ERRSITE(-ESTALE);
		}
	}
	return ret;
}
static int psb_base_identity(struct poulsback_display_domain *);
/* Compare parent-owned pointers and actual modeset locks before following the
 * borrowed source. A release commit clears that borrow under domain.lock. */
static int psb_base_parent(struct psb_base_context *c, struct poulsback_display_domain *d)
{
	if (!c || !d || c->self != c || c->domain != d || d->bound_provider.context != c ||
	    d->bound_provider.identity != psb_base_identity || c->held || !c->source_slot ||
	    d->dev != c->dev)
		return POULSBACK_ERRSITE(-EINVAL);
	return psb_source_member(c->dev, c->crtc);
}
static int psb_base_bootstrap(struct psb_base_context *c)
{
	struct poulsback_display_domain *d = c->domain;
	struct drm_psb_private *p = to_drm_psb_private(c->dev);
	const struct poulsback_display_gtt_source *b = &c->bootstrap;
	int ret;
	if (c->source || *c->source_slot || c->count || c->current_image || d->count || d->bytes ||
	    d->serial || d->active || d->pending || d->transitioning || d->aperture.child ||
	    memchr_inv(d->images, 0, sizeof(d->images)) ||
	    memchr_inv(d->extents, 0, sizeof(d->extents)))
		return -EBUSY;
	ret = psb_source_legacy(c->dev, c->crtc);
	if (ret)
		return ret;
	if (psb_source_epoch(b->generation) || p->gtt_mem != c->root ||
	    p->gtt_map != b->gtt_identity || p->scratch_page != b->scratch ||
	    c->root->start != b->aperture_start || resource_size(c->root) != b->aperture_bytes ||
	    p->gtt.stolen_size != b->stolen_bytes || p->gtt.gatt_pages != b->active_entries ||
	    (u64)p->gtt.gtt_pages * PAGE_SIZE != b->mapped_table_bytes ||
	    d->gtt_identity != b->gtt_identity || d->dummy != b->scratch ||
	    d->generation != b->generation ||
	    d->bound_start < b->aperture_start + b->stolen_bytes || d->bound_end < d->bound_start ||
	    d->bound_end - b->aperture_start >= b->aperture_bytes)
		return POULSBACK_ERRSITE(-ESTALE);
	return 0;
}
static int psb_base_context_init(struct psb_base_context *c, struct drm_device *dev,
				 struct drm_crtc *crtc, struct poulsback_display_domain *d,
				 struct poulsback_display_psb_source **slot)
{
	struct drm_psb_private *p;
	struct sgx535_epoch_snapshot epoch;
	int ret, idx;
	if (!c || !dev || !crtc || !d || !slot || *slot || memchr_inv(c, 0, sizeof(*c)) ||
	    d->initialized || d->dev || poulsback_overlap(c, sizeof(*c), d, sizeof(*d)) ||
	    poulsback_overlap(c, sizeof(*c), slot, sizeof(*slot)))
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = psb_source_legacy(dev, crtc);
	if (ret)
		goto exit;
	p = to_drm_psb_private(dev);
	sgx535_epoch_snapshot(&epoch);
	if (!epoch.bound || !epoch.epoch || epoch.loss || !p->gtt_mem || !p->gtt_map ||
	    !p->scratch_page) {
		ret = POULSBACK_ERRSITE(-ESTALE);
		goto exit;
	}
	/* These are checked source coordinates, not a hardware writer permit.
	 * Reservation prepare independently validates and retains their roots. */
	c->self = c;
	c->domain = d;
	c->dev = dev;
	c->crtc = crtc;
	c->source_slot = slot;
	c->root = p->gtt_mem;
	c->bootstrap = (struct poulsback_display_gtt_source){
		.dev = dev,
		.gtt_identity = p->gtt_map,
		.scratch = p->scratch_page,
		.generation = epoch.epoch,
		.aperture_start = p->gtt_mem->start,
		.aperture_bytes = resource_size(p->gtt_mem),
		.stolen_bytes = p->gtt.stolen_size,
		.mapped_table_bytes = (u64)p->gtt.gtt_pages * PAGE_SIZE,
		.active_entries = p->gtt.gatt_pages};
	ret = 0;
exit:
	drm_dev_exit(idx);
	return ret;
}
static int psb_base_context_attach(struct psb_base_context *c)
{
	struct poulsback_display_psb_source *s;
	int ret = psb_base_parent(c, c ? c->domain : NULL);
	if (ret)
		return ret;
	lockdep_assert_held(&c->domain->lock);
	if (c->phase != PSB_BASE_BOOTSTRAP || c->source || c->count || c->domain->count ||
	    !(s = *c->source_slot) ||
	    s != READ_ONCE(to_drm_psb_private(c->dev)
				   ->poulsback_display_sources[to_gma_crtc(c->crtc)->pipe]) ||
	    s->self != s || s->slot != c->source_slot || s->domain != c->domain ||
	    !s->report.prepared || s->report.held || s->change.shown.fb || s->change.staged.fb ||
	    s->base_context || !psb_source_same_gtt(&s->gtt, &c->bootstrap))
		return POULSBACK_ERRSITE(-EINVAL);
	c->source = s;
	s->base_context = c;
	c->phase = PSB_BASE_SOURCE;
	memcpy(c->original, s->words, sizeof(c->original));
	memcpy(c->current_words, s->words, sizeof(c->current_words));
	return psb_base_identity(c->domain);
}
static int psb_base_context_bind(struct psb_base_context *c, struct poulsback_display_image *m,
				 struct poulsback_display_gtt_binding *binding)
{
	int slot, ret;
	if (!c || psb_base_parent(c, c->domain) || c->phase == PSB_BASE_BOOTSTRAP || !c->source ||
	    *c->source_slot != c->source || !m || m->domain != c->domain ||
	    c->domain->bound_provider.context != c || c->count >= ARRAY_SIZE(c->images) ||
	    m->phase != POULSBACK_DISPLAY_MAPPED || m->mapped != m->pin.count || c->preparing != m)
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&c->domain->lock);
	slot = psb_base_slot(c, m);
	if (slot < 0 || c->images[slot].bound || !binding || c->images[slot].binding != binding ||
	    c->images[slot].retained_binding != binding)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = poulsback_display_gtt_publication_validate(binding, m);
	if (ret)
		return ret;
	c->images[slot].bound = true;
	c->preparing = NULL;
	c->count++;
	c->phase = PSB_BASE_MAPPED;
	return 0;
}
static int psb_base_registers(struct psb_base_context *c, const u32 expected[7])
{
	struct poulsback_display_psb_source *s = c->source;
	const struct psb_offset *map;
	u32 words[7];
	int ret = psb_source_root_identity(s);
	if (ret)
		return ret;
	if (memcmp(c->original, s->words, sizeof(c->original)))
		return POULSBACK_ERRSITE(-ESTALE);
	map = &s->regmap[s->pipe];
	if (map->cntr != (s->pipe ? DSPBCNTR : DSPACNTR) ||
	    map->base != (s->pipe ? DSPBBASE : DSPABASE) ||
	    map->stride != (s->pipe ? DSPBSTRIDE : DSPASTRIDE) ||
	    map->size != (s->pipe ? DSPBSIZE : DSPASIZE) ||
	    map->src != (s->pipe ? PIPEBSRC : PIPEASRC) ||
	    map->conf != (s->pipe ? PIPEBCONF : PIPEACONF))
		return POULSBACK_ERRSITE(-EXDEV);
	words[0] = REGISTER_READ(s->dev, PSB_PGETBL_CTL);
	words[1] = REGISTER_READ(s->dev, map->cntr);
	words[2] = REGISTER_READ(s->dev, map->base);
	words[3] = REGISTER_READ(s->dev, map->stride);
	words[4] = REGISTER_READ(s->dev, map->size);
	words[5] = REGISTER_READ(s->dev, map->src);
	words[6] = REGISTER_READ(s->dev, map->conf);
	return memcmp(words, expected, sizeof(words)) ? POULSBACK_ERRSITE(-ESTALE) : 0;
}
/* Existing provider.identity signature, with the checked current BASE tuple.
 * The original source checker remains unchanged and strict-old. */
static int psb_base_identity(struct poulsback_display_domain *d)
{
	struct psb_base_context *c = d ? d->bound_provider.context : NULL;
	struct poulsback_display_psb_source *s;
	bool external = false, pinned = false, locked = false;
	u32 i;
	int ret, cleanup = 0, idx;
	ret = psb_base_parent(c, d);
	if (ret)
		return ret;
	if (c->phase == PSB_BASE_BOOTSTRAP)
		return psb_base_bootstrap(c);
	lockdep_assert_held(&d->lock);
	if (!(s = c->source) || *c->source_slot != s || s->self != s || s->slot != c->source_slot ||
	    s->domain != d || s->report.held || !s->report.prepared ||
	    (c->phase == PSB_BASE_SOURCE ? c->count != 0
					 : c->phase != PSB_BASE_MAPPED || !c->count ||
						   c->count > ARRAY_SIZE(c->images)))
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(d->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = sgx535_epoch_external_begin(d->dev);
	if (ret)
		goto exit;
	external = true;
	ret = psb_source_pin_lock(s);
	if (ret)
		goto exit;
	pinned = true;
	mutex_lock(&s->pdev->gtt_mutex);
	locked = true;
	ret = poulsback_display_gtt_reservation_source_locked(s->reservation, d, s, &s->gtt);
	if (!ret)
		ret = psb_base_registers(c, c->current_words);
	/* Bound and preparing slots alike: every binding is the slot's own. */
	for (i = 0; i < ARRAY_SIZE(c->images) && !ret; i++) {
		if (!c->images[i].image)
			continue;
		if (c->images[i].binding != c->images[i].retained_binding)
			ret = POULSBACK_ERRSITE(-ESTALE);
		else
			ret = poulsback_display_gtt_publication_owned(c->images[i].binding,
								      c->images[i].image);
	}
	if (!ret)
		ret = poulsback_display_gtt_reservation_source_locked(s->reservation, d, s,
								      &s->gtt);
	if (!ret)
		ret = psb_base_registers(c, c->current_words);
exit:
	if (locked)
		mutex_unlock(&s->pdev->gtt_mutex);
	if (pinned)
		psb_gem_display_pin_unlock(s->object);
	if (external)
		cleanup = sgx535_epoch_external_end(d->dev);
	if (!ret && !cleanup) {
		ret = psb_source_epoch(s->gtt.generation);
		if (!ret)
			ret = psb_source_root_identity(s);
	}
	ret = ret ?: cleanup;
	if (ret) {
		c->held = true;
		if (!c->primary_error)
			c->primary_error = ret;
		if (cleanup && !c->cleanup_error)
			c->cleanup_error = cleanup;
	}
	drm_dev_exit(idx);
	return ret;
}

/* Existing publish_cache signature. The SCANOUT BO is WC for its life, which
 * binding preparation checks page by page; nothing is converted. The free
 * slot is the original output owner before binding allocation, also while
 * another image is shown. No error clears it, unpins, or removes PTEs.
 * The later post-mapping bind consumes this exact slot, not an imported copy. */
static int psb_base_publish_cache(struct poulsback_display_image *m)
{
	struct poulsback_display_domain *d = m ? m->domain : NULL;
	struct psb_base_context *c = d ? d->bound_provider.context : NULL;
	struct poulsback_display_psb_source *s;
	struct poulsback_display_gtt_binding *binding = NULL;
	u32 words[7], slot;
	bool external = false, pinned = false, locked = false;
	int ret, cleanup = 0, idx;
	ret = psb_base_parent(c, d);
	if (ret)
		return ret;
	lockdep_assert_held(&d->lock);
	if (c->phase == PSB_BASE_BOOTSTRAP || !(s = c->source) || *c->source_slot != s ||
	    c->preparing || c->withdrawing || !m->possible || !m->pin.mapping_owned || m->mapped)
		return POULSBACK_ERRSITE(-EINVAL);
	if (psb_base_slot(c, m) >= 0)
		return -EEXIST;
	for (slot = 0; slot < ARRAY_SIZE(c->images); slot++)
		if (!c->images[slot].image && !c->images[slot].binding &&
		    !c->images[slot].retained_binding && !c->images[slot].bound)
			break;
	if (slot == ARRAY_SIZE(c->images))
		return -EBUSY;
	ret = psb_base_identity(d);
	if (ret)
		return ret;
	if (!drm_dev_enter(d->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	/* This original slot stays reachable even if prepare returns a partial
	 * owner. */
	c->images[slot].image = m;
	c->preparing = m;
	ret = poulsback_display_gtt_prepare(m, &s->gtt, &c->images[slot].binding);
	binding = c->images[slot].binding;
	/* Preserve the original output owner even if its lookup field later drifts. */
	c->images[slot].retained_binding = binding;
	if (!ret && (c->images[slot].image != m || !binding))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (ret)
		goto exit;
	ret = sgx535_epoch_external_begin(d->dev);
	if (ret)
		goto exit;
	external = true;
	ret = psb_source_pin_lock(s);
	if (ret)
		goto exit;
	pinned = true;
	mutex_lock(&s->pdev->gtt_mutex);
	locked = true;
	ret = poulsback_display_gtt_reservation_source_locked(s->reservation, d, s, &s->gtt);
	if (!ret)
		ret = psb_source_registers(s, false, words);
	if (!ret)
		ret = poulsback_display_gtt_publication_validate(binding, m);
	if (!ret && (c->images[slot].image != m || c->images[slot].binding != binding))
		ret = POULSBACK_ERRSITE(-ESTALE);
exit:
	if (locked)
		mutex_unlock(&s->pdev->gtt_mutex);
	if (pinned)
		psb_gem_display_pin_unlock(s->object);
	if (external)
		cleanup = sgx535_epoch_external_end(d->dev);
	if (!ret && !cleanup &&
	    (c->source != s || *c->source_slot != s || c->images[slot].image != m ||
	     c->images[slot].binding != binding || c->images[slot].retained_binding != binding))
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret && !cleanup) {
		ret = psb_source_epoch(s->gtt.generation);
		if (!ret)
			ret = psb_source_identity(s);
	}
	ret = ret ?: cleanup;
	if (ret) {
		c->held = true;
		if (!c->primary_error)
			c->primary_error = ret;
		if (cleanup && !c->cleanup_error)
			c->cleanup_error = cleanup;
		if (binding)
			poulsback_display_gtt_publication_hold(binding, ret, cleanup);
	}
	drm_dev_exit(idx);
	return ret;
}

/* The bound slot of m, with its own unchanged binding; or NULL. */
static struct poulsback_display_gtt_binding *psb_base_binding(struct psb_base_context *c,
							      struct poulsback_display_image *m)
{
	int slot = psb_base_slot(c, m);
	if (slot < 0 || !c->images[slot].bound ||
	    c->images[slot].binding != c->images[slot].retained_binding)
		return NULL;
	return c->images[slot].binding;
}

/* The old selected image remains a possible fetch source until a matched
 * retirement certificate exists. Its original PTEs must survive replacement;
 * checking only the new image would miss a changed old mapping. Its binding's
 * tuple and its first and last GTT words, under the caller's GTT mutex and
 * domain lock (whole checks run where pages change: bind, unbind, retirement). */
static int psb_base_previous_words(struct psb_base_context *c, struct poulsback_display_image *next)
{
	struct poulsback_display_image *m = c->current_image;
	struct poulsback_display_gtt_binding *binding;
	struct poulsback_display_gtt_entry entry;
	u32 i, spot[2];
	int ret;
	if (!m || m == next)
		return 0;
	binding = psb_base_binding(c, m);
	if (!binding || !m->pin.count || m->mapped != m->pin.count)
		return POULSBACK_ERRSITE(-ESTALE);
	spot[0] = 0;
	spot[1] = m->pin.count - 1;
	ret = poulsback_display_gtt_publication_owned(binding, m);
	for (i = 0; !ret && i < ARRAY_SIZE(spot); i++) {
		ret = poulsback_display_gtt_entry(binding, spot[i], &entry);
		if (!ret && ioread32(c->source->pdev->gtt_map + entry.slot) != entry.image_pte)
			ret = POULSBACK_ERRSITE(-ESTALE);
	}
	return ret;
}

/* The pipe's position: the hardware frame counter (24 bits, as gma500's vblank
 * counter) and pixel counter of one PIPExFRAMEPIXEL read, and the scanline read
 * right after it; all zero for a disabled pipe. The display is powered: the
 * caller's source transaction, GTT mutex, gma_power_begin or the pipe's
 * interrupt. */
struct psb_pipe_position {
	u32 frame, pixel, scanline;
	bool enabled;
};
static int psb_pipe_position(struct drm_device *dev, u32 pipe, struct psb_pipe_position *at)
{
	u32 conf_reg = pipe ? PIPEBCONF : PIPEACONF;
	u32 high_reg = pipe ? PIPEBFRAMEHIGH : PIPEAFRAMEHIGH;
	u32 low_reg = pipe ? PIPEBFRAMEPIXEL : PIPEAFRAMEPIXEL;
	u32 high, low, scanline, again, tries;
	*at = (struct psb_pipe_position){};
	/* Read only PIPECONF on a disabled pipe: its other registers may hang
	 * the device (HW-DISP-001, docs/hardware.md). */
	at->enabled = REGISTER_READ(dev, conf_reg) & PIPEACONF_ENABLE;
	if (!at->enabled)
		return 0;
	/* High and low are not latched together: take a low between equal highs. */
	for (tries = 0; tries < 4; tries++) {
		high = REGISTER_READ(dev, high_reg) & PIPE_FRAME_HIGH_MASK;
		low = REGISTER_READ(dev, low_reg);
		scanline = REGISTER_READ(dev, conf_reg - 8);
		again = REGISTER_READ(dev, high_reg) & PIPE_FRAME_HIGH_MASK;
		if (high == again) {
			at->frame =
				(high << 8 | (low & PIPE_FRAME_LOW_MASK) >> PIPE_FRAME_LOW_SHIFT) &
				0xffffff;
			at->pixel = low & PIPE_PIXEL_MASK;
			at->scanline = scanline;
			return 0;
		}
	}
	return POULSBACK_ERRSITE(-EIO);
}
static int psb_pipe_frame(struct drm_device *dev, u32 pipe, u32 *frame, bool *enabled)
{
	struct psb_pipe_position at;
	int ret = psb_pipe_position(dev, pipe, &at);
	*frame = at.frame;
	*enabled = at.enabled;
	return ret;
}
/* One timing record (debugfs dri/N/poulsback_flips), with the display debug
 * category only: the flip and vblank paths otherwise take no lock or clock
 * for it. */
static void psb_flips_record(struct drm_device *dev, u32 kind, u32 pipe,
			     const struct psb_pipe_position *at)
{
	struct poulsback_display_flips *flips = &to_drm_psb_private(dev)->poulsback_display_flips;
	struct poulsback_display_flip_record *r;
	unsigned long flags;
	u64 ns;
	if (!sgx535_debug_on(SGX535_DEBUG_DISPLAY))
		return;
	ns = ktime_get_ns();
	spin_lock_irqsave(&dev->event_lock, flags);
	r = &flips->record[flips->count++ % ARRAY_SIZE(flips->record)];
	r->ns = ns;
	r->kind = kind;
	r->pipe = pipe;
	r->scanline = at->scanline;
	r->frame = at->frame;
	r->pixel = at->pixel;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}
static int psb_base_frame(struct poulsback_display_psb_source *s, u32 *frame, bool *enabled)
{
	return psb_pipe_frame(s->dev, s->pipe, frame, enabled);
}

/* The replaced image's last fetch is over once the frame count, with the pipe
 * running, is two past the count read right after its successor's BASE
 * store: the store latches at the next vblank and that frame is the last
 * one read from the old buffer. EAGAIN before that, with no mutation. */
static int psb_base_frames_after(struct psb_base_context *c, u32 stored)
{
	struct poulsback_display_psb_source *s = c->source;
	bool enabled = false, pinned = false;
	u32 now = 0;
	int ret, cleanup, idx;
	if (!drm_dev_enter(c->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = sgx535_epoch_external_begin(c->dev);
	if (ret)
		goto exit;
	ret = psb_source_pin_lock(s);
	if (!ret) {
		pinned = true;
		mutex_lock(&s->pdev->gtt_mutex);
		ret = psb_base_registers(c, c->current_words);
		if (!ret)
			ret = psb_base_frame(s, &now, &enabled);
		mutex_unlock(&s->pdev->gtt_mutex);
	}
	if (pinned)
		psb_gem_display_pin_unlock(s->object);
	cleanup = sgx535_epoch_external_end(c->dev);
	ret = ret ?: cleanup;
	if (!ret && (!enabled || ((now - stored) & 0xffffff) < 2))
		ret = -EAGAIN;
exit:
	drm_dev_exit(idx);
	return ret;
}

/* Existing provider.flip signature; submitted same-mode BASE replacement/restore.
 * No completion, fence signal, page removal or release. The replaced image's
 * slot records the frame count read right after the store. */
static int psb_base_flip(struct poulsback_display_domain *d, struct poulsback_display_image *next,
			 u64 serial)
{
	struct psb_base_context *c = d ? d->bound_provider.context : NULL;
	struct poulsback_display_psb_source *s;
	struct poulsback_display_image *m, *expected;
	struct poulsback_display_gtt_binding *binding;
	struct poulsback_display_gtt_entry entry;
	struct psb_pipe_position at = {};
	u32 desired[7], i, spot[2];
	int old = -1;
	bool external = false, pinned = false, locked = false;
	int ret, cleanup = 0, idx;
	if (!c || psb_base_parent(c, d) || c->phase != PSB_BASE_MAPPED || !c->source ||
	    *c->source_slot != c->source || c->source->self != c->source || !c->source->slot ||
	    *c->source->slot != c->source || c->source->domain != d || !c->count ||
	    c->count > ARRAY_SIZE(c->images) || c->preparing || c->withdrawing)
		return POULSBACK_ERRSITE(-EINVAL);
	lockdep_assert_held(&d->lock);
	s = c->source;
	m = next ? next : c->current_image;
	expected = next ? s->change.staged.owner.image : s->change.shown.owner.image;
	if (!m || c->current_image != s->change.shown.owner.image || m != expected ||
	    (!next && !s->change.restoring))
		return POULSBACK_ERRSITE(-EINVAL);
	binding = psb_base_binding(c, m);
	if (c->current_image) {
		old = psb_base_slot(c, c->current_image);
		if (old < 0 || !c->images[old].bound)
			return POULSBACK_ERRSITE(-EINVAL);
	}
	if (!binding || m->layout.width != 1280 || m->layout.height != 800 ||
	    m->layout.stride != 5120 ||
	    (m->layout.format != DRM_FORMAT_ARGB8888 && m->layout.format != DRM_FORMAT_XRGB8888) ||
	    !m->pin.count || m->mapped != m->pin.count)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(d->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = sgx535_epoch_external_begin(d->dev);
	if (ret)
		goto exit;
	external = true;
	ret = psb_source_pin_lock(s);
	if (ret)
		goto exit;
	pinned = true;
	mutex_lock(&s->pdev->gtt_mutex);
	locked = true;
	ret = poulsback_display_psb_switch_check(s, next, serial);
	if (!ret)
		ret = poulsback_display_gtt_reservation_source_locked(s->reservation, d, s,
								      &s->gtt);
	if (!ret)
		ret = psb_base_registers(c, c->current_words);
	if (!ret)
		ret = poulsback_bo_display_check(m->pin.object, m->pin.pages, m->pin.count);
	if (!ret)
		ret = poulsback_display_gtt_publication_owned(binding, m);
	memcpy(desired, c->current_words, sizeof(desired));
	spot[0] = 0;
	spot[1] = m->pin.count - 1;
	for (i = 0; i < ARRAY_SIZE(spot) && !ret; i++) {
		ret = poulsback_display_gtt_entry(binding, spot[i], &entry);
		if (!ret &&
		    (entry.resource_byte_address != m->gtt.start + (u64)spot[i] * PAGE_SIZE ||
		     entry.aperture_byte_offset !=
			     entry.resource_byte_address - s->gtt.aperture_start ||
		     ioread32(s->pdev->gtt_map + entry.slot) != entry.image_pte))
			ret = POULSBACK_ERRSITE(-ESTALE);
		if (!ret && !i)
			desired[2] = next ? entry.aperture_byte_offset : c->original[2];
	}
	if (!ret && (desired[2] & (POULSBACK_DISPLAY_SLOT_ALIGN - 1)) && next)
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret)
		ret = psb_base_previous_words(c, m);
	if (ret)
		goto exit;
	/* All possible ownership precedes this one store. Each image's binding
	 * tuple and first and last GTT words were checked: its pages change only at
	 * bind, unbind and retirement, which check whole bindings, and the GTT
	 * mutex, the domain lock and the epoch admission keep every other writer
	 * out until the unlock (docs/kernel/display.md).
	 * CPU writes through WC mappings are pushed out first. A posted read
	 * verifies the submitted word; the frame count read next dates it for the
	 * old image's retirement. */
	wmb();
	iowrite32(desired[2], (u32 __iomem *)(s->vdc + s->regmap[s->pipe].base));
	if (ioread32((u32 __iomem *)(s->vdc + s->regmap[s->pipe].base)) != desired[2])
		ret = POULSBACK_ERRSITE(-EIO);
	if (!ret)
		ret = psb_pipe_position(s->dev, s->pipe, &at);
	if (!ret && !at.enabled)
		ret = POULSBACK_ERRSITE(-ESTALE);
	if (!ret)
		psb_flips_record(s->dev, POULSBACK_DISPLAY_FLIP_STORE, s->pipe, &at);
exit:
	if (locked)
		mutex_unlock(&s->pdev->gtt_mutex);
	if (pinned)
		psb_gem_display_pin_unlock(s->object);
	if (external)
		cleanup = sgx535_epoch_external_end(d->dev);
	if (!ret && !cleanup) {
		ret = psb_source_epoch(s->gtt.generation);
		if (!ret)
			ret = psb_source_root_identity(s);
	}
	ret = ret ?: cleanup;
	if (ret) {
		c->held = true;
		if (!c->primary_error)
			c->primary_error = ret;
		if (cleanup && !c->cleanup_error)
			c->cleanup_error = cleanup;
		poulsback_display_gtt_publication_hold(binding, ret, cleanup);
	} else {
		memcpy(c->current_words, desired, sizeof(c->current_words));
		if (old >= 0)
			c->images[old].frame = at.frame;
		c->current_image = next;
	}
	drm_dev_exit(idx);
	return ret;
}

/* Trusted per-pipe storage has independent caller and device roots before any
 * reservation/source sleep. It is separate from drm_psb_private: the source
 * output slot must not overlap that parent or its registry slot. The caller
 * retains its original slot and CRTC through HOLD and teardown.
 * The selected set_config caller supplies a separate heap output slot; the
 * existing native hook guard and public capabilities remain separate. */
struct psb_static_owner {
	struct psb_static_owner *self, **slot;
	struct drm_device *dev;
	struct drm_psb_private *pdev;
	struct drm_crtc *crtc;
	struct psb_base_context base;
	struct poulsback_display_domain domain;
	struct poulsback_display_provider provider;
	struct poulsback_display_gtt_reservation *reservation;
	struct poulsback_display_psb_source *source;
	u64 generation;
	u32 pipe;
	bool refs, prepared, held;
	int primary_error, cleanup_error;
};

static bool psb_static_owner_output(struct drm_device *dev, struct drm_crtc *crtc,
				    struct psb_static_owner **out)
{
	struct drm_psb_private *p = to_drm_psb_private(dev);
	struct drm_framebuffer *fb = crtc->primary->fb;
	return !poulsback_overlap(out, sizeof(*out), dev, sizeof(*dev)) &&
	       !poulsback_overlap(out, sizeof(*out), p, sizeof(*p)) &&
	       !poulsback_overlap(out, sizeof(*out), crtc, sizeof(struct gma_crtc)) &&
	       !poulsback_overlap(out, sizeof(*out), crtc->primary, sizeof(*crtc->primary)) &&
	       !poulsback_overlap(out, sizeof(*out), fb, sizeof(*fb)) &&
	       !poulsback_overlap(out, sizeof(*out), fb->obj[0], sizeof(struct psb_gem_object)) &&
	       !poulsback_overlap(out, sizeof(*out), p->regmap, 2 * sizeof(*p->regmap)) &&
	       !poulsback_overlap(out, sizeof(*out), p->gtt_map, (u64)p->gtt.gtt_pages * PAGE_SIZE);
}
static int psb_static_owner_identity(struct psb_static_owner *o)
{
	int ret = psb_source_member(o->dev, o->crtc);
	if (ret)
		return ret;
	return o->self == o && o->pdev == to_drm_psb_private(o->dev) && o->pipe < 2 &&
			       to_gma_crtc(o->crtc)->pipe == (int)o->pipe && o->refs &&
			       *o->slot == o &&
			       READ_ONCE(o->pdev->poulsback_display_static_owners[o->pipe]) == o
		       ? 0
		       : POULSBACK_ERRSITE(-ESTALE);
}
/* External finalization holds source_lock; neither callback sleeps or touches
 * hardware. REMOVE closes this transaction before inspecting either root. */
static int psb_static_owner_claim_check(void *context)
{
	struct psb_static_owner *o = context;
	int ret = psb_source_member(o->dev, o->crtc);
	if (ret)
		return ret;
	if (o->pipe > 1 || to_gma_crtc(o->crtc)->pipe != (int)o->pipe)
		return POULSBACK_ERRSITE(-ESTALE);
	ret = psb_source_legacy(o->dev, o->crtc);
	if (ret)
		return ret;
	return o->self == o && o->pdev == to_drm_psb_private(o->dev) && o->pipe < 2 &&
			       to_gma_crtc(o->crtc)->pipe == (int)o->pipe && o->refs &&
			       psb_static_owner_output(o->dev, o->crtc, o->slot) &&
			       !poulsback_overlap(o->slot, sizeof(*o->slot), o, sizeof(*o)) &&
			       !*o->slot &&
			       !READ_ONCE(o->pdev->poulsback_display_static_owners[o->pipe]) &&
			       !READ_ONCE(o->pdev->poulsback_display_sources[o->pipe])
		       ? 0
		       : POULSBACK_ERRSITE(-ESTALE);
}
static void psb_static_owner_claim_commit(void *context)
{
	struct psb_static_owner *o = context;
	WRITE_ONCE(o->pdev->poulsback_display_static_owners[o->pipe], o);
	*o->slot = o;
}

static struct poulsback_display_gtt_binding *psb_static_binding(struct poulsback_display_image *m)
{
	struct poulsback_display_domain *d = m ? m->domain : NULL;
	struct psb_base_context *c = d ? d->bound_provider.context : NULL;
	u32 i;
	if (psb_base_parent(c, d) || !c->source || *c->source_slot != c->source)
		return NULL;
	lockdep_assert_held(&d->lock);
	for (i = 0; i < ARRAY_SIZE(c->images); i++)
		if (c->images[i].image == m)
			return c->images[i].binding == c->images[i].retained_binding
				       ? c->images[i].retained_binding
				       : NULL;
	return NULL;
}

static int psb_static_insert(struct poulsback_display_image *m, struct page *page, u64 address)
{
	struct poulsback_display_gtt_binding *binding = psb_static_binding(m);
	struct psb_base_context *c = m && m->domain ? m->domain->bound_provider.context : NULL;
	return binding ? poulsback_display_psb_source_gtt_insert(c->source, binding, m, page,
								 address)
		       : POULSBACK_ERRSITE(-ESTALE);
}

static int psb_static_flush(struct poulsback_display_domain *d)
{
	struct psb_base_context *c = d ? d->bound_provider.context : NULL;
	struct poulsback_display_image *m;
	struct poulsback_display_gtt_binding *binding;
	if (psb_base_parent(c, d) || c->count > ARRAY_SIZE(c->images))
		return POULSBACK_ERRSITE(-ESTALE);
	lockdep_assert_held(&d->lock);
	if (c->withdrawing) {
		int slot, ret;
		m = c->withdrawing;
		slot = psb_base_slot(c, m);
		binding = psb_static_binding(m);
		if (slot < 0 || !binding || psb_source_unselected(c->source, m) || m->mapped)
			return POULSBACK_ERRSITE(-ESTALE);
		ret = psb_source_gtt(c->source, binding, m, NULL, 0, true, true);
		if (!ret)
			ret = poulsback_display_gtt_dispose_withdrawn(&c->images[slot].binding, m);
		if (!ret) {
			/* The slot, wherever it is, is free for the next image. */
			memset(&c->images[slot], 0, sizeof(c->images[0]));
			c->count--;
			c->withdrawing = NULL;
			c->phase = c->count ? PSB_BASE_MAPPED : PSB_BASE_SOURCE;
		}
		return ret;
	}
	m = c->preparing;
	binding = m ? psb_static_binding(m) : NULL;
	return binding ? poulsback_display_psb_source_gtt_flush(c->source, binding, m)
		       : POULSBACK_ERRSITE(-ESTALE);
}

/* A never-shown image (serial zero) is certified at once. A replaced one is
 * certified two frames after its successor's BASE store (pipe running);
 * EAGAIN until then. Either way the plane must still show what the provider
 * last stored, which is not this image. */
static int psb_static_retirement(struct poulsback_display_image *m, u64 serial)
{
	struct psb_base_context *c = m && m->domain ? m->domain->bound_provider.context : NULL;
	int slot, ret;
	if (!psb_static_binding(m) || (slot = psb_base_slot(c, m)) < 0 || !c->images[slot].bound ||
	    serial != m->retirement_serial)
		return POULSBACK_ERRSITE(-ESTALE);
	if (c->withdrawing || c->preparing)
		return -EBUSY;
	if (c->current_image == m)
		return -EAGAIN;
	ret = psb_base_identity(m->domain);
	if (!ret && serial)
		ret = psb_base_frames_after(c, c->images[slot].frame);
	if (ret)
		return ret;
	c->withdrawing = m;
	return psb_source_unselected(c->source, m);
}
static int psb_static_remove(struct poulsback_display_image *m, struct page *page, u64 address)
{
	struct poulsback_display_gtt_binding *binding = psb_static_binding(m);
	struct psb_base_context *c = m && m->domain ? m->domain->bound_provider.context : NULL;
	return binding && !psb_source_unselected(c->source, m)
		       ? psb_source_gtt(c->source, binding, m, page, address, false, true)
		       : POULSBACK_ERRSITE(-ESTALE);
}

static int psb_static_owner_prepare(struct drm_device *dev, struct drm_crtc *crtc, u64 bytes,
				    struct psb_static_owner **out)
{
	struct psb_static_owner *o;
	struct sgx535_epoch_snapshot epoch;
	u32 pipe;
	int ret, idx;
	if (!dev || !crtc || !out || !bytes || (bytes & (PAGE_SIZE - 1)))
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = psb_source_legacy(dev, crtc);
	if (ret)
		goto exit;
	if (!psb_static_owner_output(dev, crtc, out) || *out) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto exit;
	}
	pipe = to_gma_crtc(crtc)->pipe;
	sgx535_epoch_snapshot(&epoch);
	if (!try_module_get(THIS_MODULE)) {
		ret = POULSBACK_ERRSITE(-ENODEV);
		goto exit;
	}
	drm_dev_get(dev);
	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (!o) {
		drm_dev_put(dev);
		module_put(THIS_MODULE);
		ret = -ENOMEM;
		goto exit;
	}
	o->self = o;
	o->slot = out;
	o->dev = dev;
	o->pdev = to_drm_psb_private(dev);
	o->crtc = crtc;
	o->generation = epoch.epoch;
	o->pipe = pipe;
	o->refs = true;
	ret = sgx535_epoch_external_begin(dev);
	if (!ret) {
		ret = sgx535_epoch_external_finalize(dev, o->generation,
						     psb_static_owner_claim_check,
						     psb_static_owner_claim_commit, o);
		WARN_ON(sgx535_epoch_external_end(dev));
	}
	if (ret) {
		kfree(o);
		drm_dev_put(dev);
		module_put(THIS_MODULE);
		goto exit;
	}
	ret = psb_base_context_init(&o->base, dev, crtc, &o->domain, &o->source);
	if (ret)
		goto retained;
	o->provider = (struct poulsback_display_provider){
		.identity = psb_base_identity,
		.publish_cache = psb_base_publish_cache,
		.insert = psb_static_insert,
		.remove = psb_static_remove,
		.flush = psb_static_flush,
		.flip = psb_base_flip,
		.retirement = psb_static_retirement,
		.context = &o->base,
	};
	ret = poulsback_display_gtt_reservation_prepare(dev, bytes, &o->domain, &o->provider,
							&o->reservation);
	if (ret)
		goto retained;
	ret = poulsback_display_psb_source_prepare(dev, crtc, o->reservation, &o->domain,
						   &o->source);
	if (ret)
		goto retained;
	if (o->source->mode.hdisplay != 1280 || o->source->mode.vdisplay != 800 ||
	    o->source->stride != 5120) {
		ret = POULSBACK_ERRSITE(-EOPNOTSUPP);
		goto retained;
	}
	mutex_lock(&o->domain.lock);
	ret = psb_base_context_attach(&o->base);
	mutex_unlock(&o->domain.lock);
	if (!ret)
		o->prepared = true;
retained:
	if (ret)
		o->primary_error = ret;
exit:
	drm_dev_exit(idx);
	return ret;
}

/* Map one typed framebuffer into a free slot of the window, also while another
 * one is shown. Caller owns a live DRM reference through entry. A refusal that
 * left no partial display state (the framebuffer, domain and context unheld)
 * holds nothing; any other failure holds the owner. Never use an error as
 * permission for legacy fallthrough. */
static int psb_static_pin(struct psb_static_owner *o, struct poulsback_framebuffer *native)
{
	struct poulsback_framebuffer_display_owner display;
	struct poulsback_framebuffer_report report = {};
	struct drm_framebuffer *fb;
	int slot, ret, idx;
	if (!o || o->self != o || !o->slot || *o->slot != o || !o->prepared || o->held || !native)
		return POULSBACK_ERRSITE(-EINVAL);
	if (!drm_dev_enter(o->dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	ret = psb_static_owner_identity(o);
	if (ret)
		goto exit;
	fb = poulsback_framebuffer_base(native);
	if (!fb || fb->dev != o->dev) {
		ret = POULSBACK_ERRSITE(-EXDEV);
		goto exit;
	}
	/* This BASE-only provider preserves the existing size/stride registers. */
	if (fb->width != 1280 || fb->height != 800 || fb->pitches[0] != 5120 || !fb->format ||
	    (fb->format->format != DRM_FORMAT_ARGB8888 &&
	     fb->format->format != DRM_FORMAT_XRGB8888) ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR || fb->offsets[0]) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto exit;
	}
	ret = poulsback_framebuffer_pin(native, &o->domain);
	if (ret) {
		bool clean;
		mutex_lock(&o->domain.lock);
		clean = !o->domain.held && !o->base.held;
		mutex_unlock(&o->domain.lock);
		if (clean && !poulsback_framebuffer_status(native, &report) && !report.held)
			goto exit;
		goto retained;
	}
	ret = poulsback_framebuffer_display_owner(fb, o->dev, &o->domain, &display);
	if (ret)
		goto retained;
	mutex_lock(&o->domain.lock);
	slot = psb_base_slot(&o->base, display.image);
	ret = slot < 0 ? POULSBACK_ERRSITE(-ESTALE)
		       : psb_base_context_bind(&o->base, display.image,
					       o->base.images[slot].retained_binding);
	mutex_unlock(&o->domain.lock);
retained:
	if (ret) {
		o->held = o->base.held = true;
		if (!o->primary_error)
			o->primary_error = ret;
		if (o->base.cleanup_error && !o->cleanup_error)
			o->cleanup_error = o->base.cleanup_error;
	}
exit:
	drm_dev_exit(idx);
	return ret;
}

/* Refuse impossible BASE-only requests before owning a reservation or changing
 * cache/GTT state. Modeset locks protect this bounded routing check; the source
 * witness still revalidates the exact tuple after all preparation sleeps. */
static int psb_static_request(struct drm_mode_set *set)
{
	struct drm_crtc *crtc = set->crtc;
	struct drm_device *dev = crtc->dev;
	struct drm_framebuffer *fb = crtc->primary->fb;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	u32 i, j, found = 0;
	int ret = 0;
	if (!set->fb || !set->mode || set->x || set->y || crtc->primary->old_fb != fb ||
	    !drm_mode_equal(set->mode, &crtc->mode) || crtc->mode.hdisplay != 1280 ||
	    crtc->mode.vdisplay != 800 || fb->width != 1280 || fb->height != 800 ||
	    fb->pitches[0] != 5120 || fb->format->depth != 24 || fb->format->num_planes != 1 ||
	    to_gma_crtc(crtc)->page_flip_event || set->num_connectors <= 0 ||
	    set->num_connectors > dev->mode_config.num_connector || !set->connectors ||
	    set->fb->width != 1280 || set->fb->height != 800 || set->fb->pitches[0] != 5120 ||
	    !set->fb->format ||
	    (set->fb->format->format != DRM_FORMAT_ARGB8888 &&
	     set->fb->format->format != DRM_FORMAT_XRGB8888) ||
	    set->fb->modifier != DRM_FORMAT_MOD_LINEAR || set->fb->offsets[0])
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	for (i = 0; i < (u32)set->num_connectors; i++)
		for (j = 0; j < i; j++)
			if (set->connectors[j] == set->connectors[i])
				return POULSBACK_ERRSITE(-EINVAL);
	/* Authenticate each requested pointer through the registered list before
	 * following its encoder. Every current connector must remain selected. */
	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter)
	{
		for (i = 0; i < (u32)set->num_connectors; i++)
			if (set->connectors[i] == connector)
				break;
		if (i < (u32)set->num_connectors) {
			if (connector->dev != dev || !connector->encoder ||
			    connector->encoder->dev != dev || connector->encoder->crtc != crtc) {
				ret = POULSBACK_ERRSITE(-EXDEV);
				break;
			}
			found++;
		} else if (connector->encoder && connector->encoder->crtc == crtc) {
			ret = POULSBACK_ERRSITE(-ESTALE);
			break;
		}
	}
	drm_connector_list_iter_end(&iter);
	if (ret || found != (u32)set->num_connectors)
		return ret ?: POULSBACK_ERRSITE(-EXDEV);
	drm_for_each_encoder(encoder, dev)
	{
		if (encoder->crtc != crtc)
			continue;
		for (i = 0; i < (u32)set->num_connectors; i++)
			if (set->connectors[i]->encoder == encoder)
				break;
		if (i == (u32)set->num_connectors || encoder->dev != dev)
			return POULSBACK_ERRSITE(-ESTALE);
	}
	return 0;
}
/* Core keeps the requested framebuffer and all three modeset locks alive.
 * The heap slot survives the call through the device's independently anchored
 * owner. No failed constructor/pin may fall through to legacy PSB casts. */
static int psb_static_owner_start(struct drm_mode_set *set,
				  struct poulsback_display_psb_source **source)
{
	struct drm_device *dev = set->crtc->dev;
	struct drm_psb_private *p = to_drm_psb_private(dev);
	struct poulsback_framebuffer *native;
	struct psb_static_owner **slot, *o;
	u32 pipe;
	int ret;
	ret = psb_source_legacy(dev, set->crtc);
	if (ret)
		return ret;
	pipe = to_gma_crtc(set->crtc)->pipe;
	if (READ_ONCE(p->poulsback_display_static_owners[pipe]) ||
	    READ_ONCE(p->poulsback_display_sources[pipe]))
		return -EBUSY;
	ret = psb_static_request(set);
	if (ret)
		return ret;
	native = poulsback_framebuffer_from_base(set->fb, dev);
	if (IS_ERR(native))
		return PTR_ERR(native);
	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;
	/* Four 256 KiB-aligned slots of one 1280x800 buffer each. */
	ret = psb_static_owner_prepare(dev, set->crtc, POULSBACK_DISPLAY_WINDOW, slot);
	o = *slot;
	if (!o) {
		kfree(slot);
		return ret;
	}
	if (!ret)
		ret = psb_static_pin(o, native);
	if (!ret)
		ret = psb_static_owner_identity(o);
	if (!ret) {
		*source = o->source;
	} else {
		o->held = o->base.held = true;
		if (!o->primary_error)
			o->primary_error = ret;
	}
	return ret;
}
/* A refused stage that left no partial display state drops its references
 * instead of holding the source: nothing was selected or published. */
static bool psb_switch_unstage(struct poulsback_display_psb_source *s)
{
	struct psb_source_switch *w = &s->change;
	struct poulsback_framebuffer_report report = {};
	struct poulsback_framebuffer *native;
	bool held;
	if (!w->staged.fb)
		return true;
	native = poulsback_framebuffer_from_base(w->staged.fb, s->dev);
	if (IS_ERR(native) || poulsback_framebuffer_status(native, &report) || report.held)
		return false;
	mutex_lock(&s->domain->lock);
	held = s->domain->held || s->domain->transitioning ||
	       (s->base_context && s->base_context->held);
	mutex_unlock(&s->domain->lock);
	if (held)
		return false;
	psb_switch_release(&w->staged);
	w->previous = w->next = NULL;
	return true;
}
/* A failed switch that may have reached the hardware freezes its typed owners
 * without signalling fences or pretending that software rollback ended the
 * hardware fetch; the shown and staged references are never dropped. */
static void psb_switch_hold(struct poulsback_display_psb_source *s, int ret)
{
	struct psb_source_switch *w = &s->change;
	if (!w->shown.fb && !w->staged.fb)
		return;
	mutex_lock(&s->domain->lock);
	poulsback_display_switch_hold_locked(s->domain, ret);
	mutex_unlock(&s->domain->lock);
	w->held = true;
	s->report.held = 1;
	if (!s->report.primary_error)
		s->report.primary_error = ret;
}
/* Stage a typed framebuffer; before a show its BO's GPU writes are complete
 * and none failed. A failure unstages it, or holds when that is not certain. */
static int psb_switch_stage_ready(struct poulsback_display_psb_source *s, struct drm_mode_set *set)
{
	int ret = psb_switch_stage(s, set);
	if (!ret)
		ret = poulsback_display_wait_writers(s->change.staged.object);
	if (ret && !psb_switch_unstage(s))
		psb_switch_hold(s, ret);
	return ret;
}
/* The switch from the shown framebuffer to the staged one (or, restoring, to
 * the legacy source): the provider's one BASE store. Success hands the
 * replaced typed framebuffer's roots to *old (none for the legacy source); its
 * image stays mapped (PENDING) and retires two frames after this store, when
 * its last reference goes. A failure holds. */
static int psb_switch_commit(struct poulsback_display_psb_source *s, const struct drm_mode_set *set,
			     struct psb_source_image *old)
{
	struct psb_source_switch *w = &s->change;
	int ret = 0;
	mutex_lock(&s->domain->lock);
	if (s->domain->held || s->domain->transitioning || s->domain->serial == U64_MAX ||
	    s->domain->active != w->shown.owner.image)
		ret = POULSBACK_ERRSITE(-ESTALE);
	else
		w->serial = s->domain->serial + 1;
	mutex_unlock(&s->domain->lock);
	if (!ret)
		ret = psb_switch_tuple(s, set, w->previous);
	if (ret) {
		psb_switch_hold(s, ret);
		return ret;
	}
	w->entered = true;
	w->request = set; /* Call-local only; never followed after returning to core. */
	s->primary->fb = w->next;
	ret = w->restoring ? poulsback_display_disable(s->domain)
			   : poulsback_framebuffer_flip(w->staged.owner.framebuffer);
	if (!ret)
		ret = psb_switch_tuple(s, set, w->next);
	if (!ret) {
		mutex_lock(&s->domain->lock);
		if (s->domain->held || s->domain->transitioning || s->domain->pending ||
		    s->domain->serial != w->serial ||
		    s->domain->active != (w->restoring ? NULL : w->staged.owner.image))
			ret = POULSBACK_ERRSITE(-ESTALE);
		mutex_unlock(&s->domain->lock);
	}
	w->entered = false;
	w->request = NULL;
	if (ret) {
		/* Conditional software rollback never certifies hardware rollback.
		 * A foreign tuple is retained, never overwritten with a guessed FB. */
		if (s->primary->fb == w->next && s->primary->old_fb == w->previous &&
		    s->primary->crtc == s->crtc)
			s->primary->fb = w->previous;
		psb_switch_hold(s, ret);
		return ret;
	}
	*old = w->shown;
	w->shown = w->restoring ? (struct psb_source_image){} : w->staged;
	memset(&w->staged, 0, sizeof(w->staged));
	w->restoring = false;
	w->previous = w->next = NULL;
	return 0;
}
/* The source of crtc's pipe, ready for a switch: its own and unheld, and its
 * static owner, if any, prepared and unheld. */
static int psb_switch_source(struct drm_crtc *crtc, struct poulsback_display_psb_source **out)
{
	struct drm_psb_private *p = to_drm_psb_private(crtc->dev);
	u32 pipe = to_gma_crtc(crtc)->pipe;
	struct poulsback_display_psb_source *s =
		pipe < 2 ? p->poulsback_display_sources[pipe] : NULL;
	struct psb_static_owner *o =
		pipe < 2 ? READ_ONCE(p->poulsback_display_static_owners[pipe]) : NULL;
	int ret;
	if (!s || s->self != s || *s->slot != s || s->crtc != crtc || s->report.held)
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (o) {
		ret = psb_static_owner_identity(o);
		if (ret)
			return ret;
		if (!o->prepared || o->held || o->base.held || o->source != s)
			return -EBUSY;
	}
	*out = s;
	return 0;
}
/* The replaced framebuffer of a flip, released after its vblank: a typed
 * one's switch roots, or a legacy one's gma500 pin and reference. */
struct psb_flip_release {
	struct work_struct work;
	struct psb_source_image old;
	struct drm_framebuffer *unpin;
};
static void psb_flip_release_work(struct work_struct *work)
{
	struct psb_flip_release *r = container_of(work, struct psb_flip_release, work);
	psb_switch_release(&r->old);
	if (r->unpin) {
		psb_gem_unpin(to_psb_gem_object(r->unpin->obj[0]));
		drm_framebuffer_put(r->unpin);
	}
	kfree(r);
}
/* Under dev->event_lock: queues the release of the pipe's last flip once DRM's
 * vblank count has reached its target; true while that flip is pending. */
static bool psb_flip_settle_locked(struct poulsback_display_flips *flips, u32 pipe,
				   struct drm_crtc *crtc)
{
	struct work_struct *release = flips->pending[pipe].release;
	if (!release)
		return false;
	if ((s64)(drm_crtc_vblank_count(crtc) - flips->pending[pipe].target) < 0)
		return true;
	flips->pending[pipe].release = NULL;
	schedule_work(release);
	drm_crtc_vblank_put(crtc);
	return false;
}
static bool psb_flip_pending_pipe(struct drm_crtc *crtc, u32 pipe)
{
	struct drm_device *dev = crtc->dev;
	bool pending;
	spin_lock_irq(&dev->event_lock);
	pending = psb_flip_settle_locked(&to_drm_psb_private(dev)->poulsback_display_flips, pipe,
					 crtc);
	spin_unlock_irq(&dev->event_lock);
	return pending;
}
static bool psb_flip_pending(struct poulsback_display_psb_source *s)
{
	return psb_flip_pending_pipe(s->crtc, s->pipe);
}
/* One vblank reference for the release and one for the event: an off pipe
 * refuses them before the store. *refs counts those taken. */
static int psb_flip_refs(struct drm_crtc *crtc, bool event, u32 *refs)
{
	int ret = drm_crtc_vblank_get(crtc);
	*refs = !ret;
	if (!ret && event) {
		ret = drm_crtc_vblank_get(crtc);
		*refs += !ret;
	}
	return ret;
}
/* After a flip's store: DRM's event for the vblank count that proves it
 * latched, and the release of the replaced framebuffer at that count. Takes
 * over release and both vblank references. */
static void psb_flip_arm(struct drm_crtc *crtc, u32 pipe, struct drm_pending_vblank_event *event,
			 struct psb_flip_release *release)
{
	struct drm_device *dev = crtc->dev;
	struct poulsback_display_flips *flips = &to_drm_psb_private(dev)->poulsback_display_flips;
	u64 target;
	INIT_WORK(&release->work, psb_flip_release_work);
	spin_lock_irq(&dev->event_lock);
	if (event) {
		drm_crtc_arm_vblank_event(crtc, event);
		target = event->sequence;
	} else {
		target = drm_crtc_accurate_vblank_count(crtc) + 1;
	}
	flips->pending[pipe].target = target;
	flips->pending[pipe].release = &release->work;
	spin_unlock_irq(&dev->event_lock);
}
int poulsback_display_psb_set_config(struct drm_mode_set *set, bool *handled)
{
	struct poulsback_display_psb_source *s = NULL;
	struct drm_psb_private *p;
	struct psb_source_switch *w;
	struct psb_source_image old;
	struct drm_framebuffer *current_fb;
	bool typed;
	int ret = 0, idx;
	u32 pipe;
	if (!handled || !set || !set->crtc || !set->crtc->primary)
		return POULSBACK_ERRSITE(-EINVAL);
	*handled = false;
	current_fb = set->crtc->primary->fb;
	typed = poulsback_framebuffer_is_object(set->fb) ||
		poulsback_framebuffer_is_object(current_fb) ||
		poulsback_framebuffer_is_object(set->crtc->primary->old_fb);
	if (!IS_PSB(set->crtc->dev)) {
		*handled = typed;
		return typed ? POULSBACK_ERRSITE(-EOPNOTSUPP) : 0;
	}
	if (!drm_dev_enter(set->crtc->dev, &idx)) {
		*handled = true;
		return POULSBACK_ERRSITE(-ENODEV);
	}
	p = to_drm_psb_private(set->crtc->dev);
	/* A retained static witness excludes another set_config stealing its
	 * connectors. This is not a claim about every inherited writer callback. */
	for (pipe = 0; pipe < 2; pipe++) {
		struct poulsback_display_psb_source *member =
			READ_ONCE(p->poulsback_display_sources[pipe]);
		if (!member || !member->change.shown.fb)
			continue;
		*handled = true;
		if (member->crtc != set->crtc) {
			ret = -EBUSY;
			goto exit;
		}
		s = member;
	}
	if (!typed && !s)
		goto exit;
	*handled = true;
	ret = psb_source_member(set->crtc->dev, set->crtc);
	if (ret)
		goto exit;
	pipe = to_gma_crtc(set->crtc)->pipe;
	if (pipe < 2 && !s && !READ_ONCE(p->poulsback_display_sources[pipe]) &&
	    poulsback_framebuffer_is_object(set->fb)) {
		ret = psb_static_owner_start(set, &s);
		if (ret)
			goto exit;
	}
	ret = psb_switch_source(set->crtc, &s);
	if (ret)
		goto exit;
	w = &s->change;
	if (w->entered || w->held || w->restoring || w->staged.fb || psb_flip_pending(s)) {
		ret = -EBUSY;
		goto exit;
	}
	if (poulsback_framebuffer_is_object(set->fb) && set->fb == w->shown.fb) {
		/* Already on the plane. */
		ret = set->mode && drm_mode_equal(set->mode, &s->mode) && !set->x && !set->y
			      ? psb_switch_route(s, set)
			      : POULSBACK_ERRSITE(-EOPNOTSUPP);
		goto exit;
	}
	if (poulsback_framebuffer_is_object(set->fb)) {
		ret = psb_switch_stage_ready(s, set);
		if (ret)
			goto exit;
	} else {
		/* Only the legacy source returns; any other request is refused. */
		if (!w->shown.fb || set->fb != s->fb || current_fb != w->shown.fb) {
			ret = POULSBACK_ERRSITE(-EOPNOTSUPP);
			goto exit;
		}
		w->previous = w->shown.fb;
		w->next = s->fb;
		w->restoring = true;
		ret = psb_switch_tuple(s, set, w->previous);
		if (ret) {
			psb_switch_hold(s, ret);
			goto exit;
		}
	}
	/* The plane's own reference to the replaced framebuffer outlives this
	 * call, so no destroy runs here. */
	ret = psb_switch_commit(s, set, &old);
	if (!ret)
		psb_switch_release(&old);
exit:
	drm_dev_exit(idx);
	return ret;
}

/* A typed-to-typed page flip is set_config's switch for the plane's mode and
 * route. Its event is DRM's, armed after the store: gma500's vblank counter
 * counts vblank starts, so the event's count proves the store latched
 * (HW-DISP-004). A work item drops the replaced framebuffer's roots once that
 * count has come; until then another flip or set_config gets -EBUSY. */
int poulsback_display_psb_page_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
				    struct drm_pending_vblank_event *event,
				    struct drm_modeset_acquire_ctx *ctx, bool *handled)
{
	struct drm_device *dev = crtc->dev;
	struct poulsback_display_psb_source *s;
	struct psb_flip_release *release = NULL;
	struct drm_connector **connectors = NULL;
	struct psb_source_switch *w;
	struct drm_mode_set set;
	u32 i, refs = 0;
	int ret, idx;
	*handled = poulsback_framebuffer_is_object(fb) ||
		   poulsback_framebuffer_is_object(crtc->primary->fb) ||
		   poulsback_framebuffer_is_object(crtc->primary->old_fb);
	if (!*handled)
		return 0;
	/* The legacy framebuffer comes and goes through SetCrtc. */
	if (!IS_PSB(dev) || !poulsback_framebuffer_is_object(fb) ||
	    !poulsback_framebuffer_is_object(crtc->primary->fb))
		return POULSBACK_ERRSITE(-EOPNOTSUPP);
	if (!drm_dev_enter(dev, &idx))
		return POULSBACK_ERRSITE(-ENODEV);
	/* Every switch runs under all modeset locks; -EDEADLK backs off in core. */
	ret = drm_modeset_lock_all_ctx(dev, ctx);
	if (!ret)
		ret = psb_source_member(dev, crtc);
	if (!ret)
		ret = psb_switch_source(crtc, &s);
	if (ret)
		goto exit;
	w = &s->change;
	if (w->entered || w->held || w->restoring || w->staged.fb || psb_flip_pending(s)) {
		ret = -EBUSY;
		goto exit;
	}
	if (!w->shown.fb || fb == w->shown.fb || !w->route) {
		ret = POULSBACK_ERRSITE(-EINVAL);
		goto exit;
	}
	release = kzalloc(sizeof(*release), GFP_KERNEL);
	connectors = kcalloc(w->count, sizeof(*connectors), GFP_KERNEL);
	if (!release || !connectors) {
		ret = -ENOMEM;
		goto exit;
	}
	for (i = 0; i < w->count; i++)
		connectors[i] = w->route[i].connector;
	set = (struct drm_mode_set){.crtc = crtc,
				    .fb = fb,
				    .mode = &s->mode,
				    .connectors = connectors,
				    .num_connectors = w->count};
	ret = psb_flip_refs(crtc, event != NULL, &refs);
	if (!ret)
		ret = psb_switch_stage_ready(s, &set);
	if (!ret)
		ret = psb_switch_commit(s, &set, &release->old);
	if (ret)
		goto exit;
	psb_flip_arm(crtc, s->pipe, event, release);
	release = NULL;
	refs = 0;
exit:
	while (refs--)
		drm_crtc_vblank_put(crtc);
	kfree(release);
	kfree(connectors);
	drm_dev_exit(idx);
	return ret;
}
/* gma500's own flip of its legacy framebuffers, with the typed flip's pending
 * rule, vblank references and DRM event. gma500's set_base stores and pins the
 * new buffer; the replaced one keeps its pin and a reference until the vblank
 * that proves the store latched (gma500 unpinned it at the store). */
int poulsback_display_psb_legacy_flip(struct drm_crtc *crtc, struct drm_framebuffer *fb,
				      struct drm_pending_vblank_event *event)
{
	const struct drm_crtc_helper_funcs *funcs = crtc->helper_private;
	struct drm_framebuffer *old = crtc->primary->fb;
	struct psb_flip_release *release;
	u32 pipe = to_gma_crtc(crtc)->pipe, refs = 0;
	int ret;
	if (pipe >= 2 || !funcs->mode_set_base || !old)
		return -EINVAL;
	if (psb_flip_pending_pipe(crtc, pipe))
		return -EBUSY;
	release = kzalloc(sizeof(*release), GFP_KERNEL);
	if (!release)
		return -ENOMEM;
	ret = psb_flip_refs(crtc, event != NULL, &refs);
	if (!ret) {
		/* set_base shows crtc->primary->fb; no old_fb: no unpin now. */
		crtc->primary->fb = fb;
		ret = funcs->mode_set_base(crtc, crtc->x, crtc->y, NULL);
		if (ret)
			crtc->primary->fb = old;
	}
	if (ret) {
		while (refs--)
			drm_crtc_vblank_put(crtc);
		kfree(release);
		return ret;
	}
	if (sgx535_debug_on(SGX535_DEBUG_DISPLAY) && gma_power_begin(crtc->dev, false)) {
		struct psb_pipe_position at;
		if (!psb_pipe_position(crtc->dev, pipe, &at) && at.enabled)
			psb_flips_record(crtc->dev, POULSBACK_DISPLAY_FLIP_STORE, pipe, &at);
		gma_power_end(crtc->dev);
	}
	drm_framebuffer_get(old);
	release->unpin = old;
	psb_flip_arm(crtc, pipe, event, release);
	return 0;
}

/* Read-only debugfs dri/N/poulsback_display: per pipe, the plane and cursor
 * registers, a hash of the palette (FNV-1a over its 256 words) and how many
 * words are the identity ramp, the frame count and, for the image the provider
 * last stored in DSPBASE, its GTT offset and size and whether every GTT word is
 * page address | 9. */
static void psb_display_show_pipe(struct seq_file *f, struct drm_device *dev, u32 pipe)
{
	struct drm_psb_private *p = to_drm_psb_private(dev);
	struct psb_static_owner *o = READ_ONCE(p->poulsback_display_static_owners[pipe]);
	const struct psb_offset *map = &p->regmap[pipe];
	struct poulsback_display_image *m = NULL;
	u32 conf, cntr, stride, src, base, frame = 0, i, ok = 0, bad = U32_MAX, read = 0, want = 0;
	u32 cursor[3] = {0}, palette_hash = 2166136261u, identity = 0;
	bool enabled = false, owner = o && o->self == o && o->prepared;
	u64 offset = 0;
	int ret;
	if (owner)
		mutex_lock(&o->domain.lock);
	if (!gma_power_begin(dev, false)) {
		seq_printf(f, "pipe%u power off\n", pipe);
		goto unlock;
	}
	ret = sgx535_epoch_external_begin(dev);
	if (ret) {
		seq_printf(f, "pipe%u error %d\n", pipe, ret);
		goto power;
	}
	mutex_lock(&p->gtt_mutex);
	/* Only PIPECONF of a disabled pipe is read (see psb_pipe_position). */
	conf = REGISTER_READ(dev, map->conf);
	enabled = conf & PIPEACONF_ENABLE;
	cntr = enabled ? REGISTER_READ(dev, map->cntr) : 0;
	stride = enabled ? REGISTER_READ(dev, map->stride) : 0;
	src = enabled ? REGISTER_READ(dev, map->src) : 0;
	base = enabled ? REGISTER_READ(dev, map->base) : 0;
	for (i = 0; enabled && i < 3; i++)
		cursor[i] = REGISTER_READ(dev, (pipe ? CURBCNTR : CURACNTR) + 4 * i);
	for (i = 0; enabled && i < 256; i++) {
		u32 word = REGISTER_READ(dev, map->palette + 4 * i);
		palette_hash = (palette_hash ^ word) * 16777619u;
		identity += word == (i << 16 | i << 8 | i);
	}
	ret = psb_pipe_frame(dev, pipe, &frame, &enabled);
	if (owner && o->base.self == &o->base)
		m = o->base.current_image;
	if (m) {
		offset = m->gtt.start - p->gtt_mem->start;
		for (i = 0; i < m->pin.count; i++) {
			u32 word = ioread32(p->gtt_map + (offset >> PAGE_SHIFT) + i);
			u32 expected = (u32)page_to_phys(m->pin.pages[i]) | 9;
			if (word == expected)
				ok++;
			else if (bad == U32_MAX) {
				bad = i;
				read = word;
				want = expected;
			}
		}
	}
	mutex_unlock(&p->gtt_mutex);
	WARN_ON(sgx535_epoch_external_end(dev));
	seq_printf(f, "pipe%u PIPECONF 0x%08x\npipe%u DSPCNTR 0x%08x\npipe%u DSPSTRIDE %u\n", pipe,
		   conf, pipe, cntr, pipe, stride);
	seq_printf(f, "pipe%u PIPESRC 0x%08x\npipe%u DSPBASE 0x%08x\n", pipe, src, pipe, base);
	seq_printf(f, "pipe%u CURCNTR 0x%08x CURBASE 0x%08x CURPOS 0x%08x\n", pipe, cursor[0],
		   cursor[1], cursor[2]);
	seq_printf(f, "pipe%u palette fnv 0x%08x identity %u\n", pipe, enabled ? palette_hash : 0,
		   identity);
	seq_printf(f, "pipe%u frame %u enabled %d error %d\n", pipe, frame, enabled, ret);
	if (!m)
		seq_printf(f, "pipe%u selected none owner %d\n", pipe, owner);
	else
		seq_printf(f,
			   "pipe%u selected gtt_offset 0x%08llx size %llu pages %u words_ok %u\n",
			   pipe, offset, m->pin.size, m->pin.count, ok);
	if (bad != U32_MAX)
		seq_printf(f, "pipe%u first_bad_word %u read 0x%08x expected 0x%08x\n", pipe, bad,
			   read, want);
power:
	gma_power_end(dev);
unlock:
	if (owner)
		mutex_unlock(&o->domain.lock);
}
static int psb_display_show(struct seq_file *f, void *unused)
{
	struct drm_device *dev = f->private;
	u32 pipe, counts[5];
	int idx;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;
	for (pipe = 0; pipe < 2; pipe++)
		psb_display_show_pipe(f, dev, pipe);
	poulsback_framebuffer_counts(counts);
	seq_printf(f, "framebuffers created %u destroyed %u retire_queued %u freed %u held %u\n",
		   counts[0], counts[1], counts[2], counts[3], counts[4]);
	drm_dev_exit(idx);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(psb_display);

/* Read-only debugfs dri/N/poulsback_position: per enabled pipe one position
 * with its ktime and the raw timing registers (HTOTAL, HBLANK, HSYNC, VTOTAL,
 * VBLANK, VSYNC); a disabled pipe shows that it is disabled. */
static int psb_position_show(struct seq_file *f, void *unused)
{
	struct drm_device *dev = f->private;
	struct drm_psb_private *p = to_drm_psb_private(dev);
	struct psb_pipe_position at;
	u32 pipe, timing[6] = {0};
	u64 ns;
	int ret;
	for (pipe = 0; pipe < 2; pipe++) {
		const struct psb_offset *map = &p->regmap[pipe];
		if (!gma_power_begin(dev, false)) {
			seq_printf(f, "pipe%u power off\n", pipe);
			continue;
		}
		ns = ktime_get_ns();
		ret = psb_pipe_position(dev, pipe, &at);
		if (!ret && at.enabled) {
			timing[0] = REGISTER_READ(dev, map->htotal);
			timing[1] = REGISTER_READ(dev, map->hblank);
			timing[2] = REGISTER_READ(dev, map->hsync);
			timing[3] = REGISTER_READ(dev, map->vtotal);
			timing[4] = REGISTER_READ(dev, map->vblank);
			timing[5] = REGISTER_READ(dev, map->vsync);
		}
		gma_power_end(dev);
		if (ret || !at.enabled) {
			seq_printf(f, "pipe%u enabled %d error %d\n", pipe, at.enabled, ret);
			continue;
		}
		seq_printf(f, "pipe%u ns %llu scanline %u frame %u pixel %u\n", pipe, ns,
			   at.scanline, at.frame, at.pixel);
		seq_printf(f, "pipe%u timing 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n", pipe,
			   timing[0], timing[1], timing[2], timing[3], timing[4], timing[5]);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(psb_position);

void poulsback_display_psb_vblank(struct drm_device *dev, u32 pipe)
{
	struct poulsback_display_flips *flips = &to_drm_psb_private(dev)->poulsback_display_flips;
	struct drm_crtc *crtc = drm_crtc_from_index(dev, pipe);
	struct psb_pipe_position at;
	unsigned long flags;
	if (sgx535_debug_on(SGX535_DEBUG_DISPLAY) && !psb_pipe_position(dev, pipe, &at) &&
	    at.enabled)
		psb_flips_record(dev, POULSBACK_DISPLAY_FLIP_VBLANK, pipe, &at);
	if (!crtc || pipe >= ARRAY_SIZE(flips->pending))
		return;
	spin_lock_irqsave(&dev->event_lock, flags);
	psb_flip_settle_locked(flips, pipe, crtc);
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/* Read-only debugfs dri/N/poulsback_flips: the timing records still in the
 * ring, oldest first, each with its sequence number. */
static int psb_flips_show(struct seq_file *f, void *unused)
{
	struct drm_device *dev = f->private;
	struct poulsback_display_flips *copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	const u32 n = ARRAY_SIZE(copy->record);
	u32 i;
	if (!copy)
		return -ENOMEM;
	spin_lock_irq(&dev->event_lock);
	*copy = to_drm_psb_private(dev)->poulsback_display_flips;
	spin_unlock_irq(&dev->event_lock);
	for (i = copy->count > n ? copy->count - n : 0; i != copy->count; i++) {
		const struct poulsback_display_flip_record *r = &copy->record[i % n];
		seq_printf(f, "%u %s pipe%u ns %llu scanline %u frame %u pixel %u\n", i,
			   r->kind == POULSBACK_DISPLAY_FLIP_STORE ? "store" : "vblank", r->pipe,
			   r->ns, r->scanline, r->frame, r->pixel);
	}
	kfree(copy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(psb_flips);

int poulsback_display_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct poulsback_display_info *info = data;
	struct drm_crtc *crtc;
	(void)file;
	if (info->abi != POULSBACK_BO_ABI || memchr_inv(info->reserved, 0, sizeof(info->reserved)))
		return POULSBACK_ERRSITE(-EINVAL);
	info->width = info->height = info->stride = info->format = 0;
	/* Software state only: no register of any pipe is read. */
	drm_modeset_lock_all(dev);
	drm_for_each_crtc(crtc, dev)
	{
		if (!crtc->enabled || !crtc->primary || !crtc->primary->fb)
			continue;
		info->width = crtc->mode.hdisplay;
		info->height = crtc->mode.vdisplay;
		info->stride = ALIGN(info->width * 4, 64);
		info->format = DRM_FORMAT_XRGB8888;
		break;
	}
	drm_modeset_unlock_all(dev);
	return 0;
}

void poulsback_display_debugfs_init(struct drm_minor *minor)
{
	if (minor->type != DRM_MINOR_PRIMARY)
		return;
	debugfs_create_file("poulsback_display", 0444, minor->debugfs_root, minor->dev,
			    &psb_display_fops);
	debugfs_create_file("poulsback_position", 0444, minor->debugfs_root, minor->dev,
			    &psb_position_fops);
	debugfs_create_file("poulsback_flips", 0444, minor->debugfs_root, minor->dev,
			    &psb_flips_fops);
}
