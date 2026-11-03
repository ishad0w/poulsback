/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef POULSBACK_FRAMEBUFFER_H
#define POULSBACK_FRAMEBUFFER_H
#include <linux/types.h>
struct drm_device;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_mode_fb_cmd2;
struct poulsback_display_domain;
struct poulsback_framebuffer;
struct poulsback_display_image;
struct poulsback_framebuffer_display_owner {
	struct poulsback_framebuffer *framebuffer;
	struct poulsback_display_image *image;
};

struct poulsback_framebuffer_report {
	u64 image_bytes;
	u32 version, width, height, stride, format;
	u32 prepared, drm_registered, drm_cleaned, destroy_called, held;
	u32 object_ref, device_ref, module_ref;
	u32 pin_attempted, pin_completed, retire_attempts, retired;
	int primary_error, pin_error, flip_error, retire_error, cleanup_error;
};

/* Borrowed base, only while the caller owns a live DRM reference. Never pass
 * it to a legacy PSB helper. Held/refcount0 owners cannot return a DRM base. */
struct drm_framebuffer *poulsback_framebuffer_base(struct poulsback_framebuffer *);
/* Caller owns a live DRM reference. Return the checked opaque metadata owner,
 * or ERR_PTR; this borrows that reference and grants no pin/plane authority. */
struct poulsback_framebuffer *poulsback_framebuffer_from_base(struct drm_framebuffer *,
							      struct drm_device *);
/* created, destroyed, retire queued, freed, held (debugfs). */
void poulsback_framebuffer_counts(u32 out[5]);
bool poulsback_framebuffer_is_object(const struct drm_framebuffer *);
/* fb_create metadata adapter over an already looked-up SCANOUT BO.
 * The caller keeps its lookup reference and immutable command through return;
 * success transfers one new DRM framebuffer reference. A failed prepare keeps
 * every partial/HOLD owner reachable through the existing retained-owner list.
 * This does not pin GTT pages, select a plane or register a physical provider. */
struct drm_framebuffer *poulsback_framebuffer_create(struct drm_device *, struct drm_gem_object *,
						     const struct drm_mode_fb_cmd2 *);
/* Call before any power/MMIO operation or legacy PSB-object cast. Both the
 * new and previous framebuffer are checked; NULL means no framebuffer.
 * Typed framebuffers and BOs never enter the legacy pin/unpin path. */
int poulsback_framebuffer_legacy_check(const struct drm_framebuffer *,
				       const struct drm_framebuffer *);
/* Caller holds a DRM reference. Success returns a separate GEM reference. */
struct drm_gem_object *poulsback_framebuffer_object_get(struct drm_framebuffer *,
							struct drm_device *);
/* Caller already owns a live DRM reference. Return borrowed identities for
 * the exact prepared/mapped owner, not a new pin or publication permission.
 * Outputs live in distinct caller-owned storage. Lock order is owner->domain;
 * never call while holding the domain mutex. */
int poulsback_framebuffer_display_owner(struct drm_framebuffer *, struct drm_device *,
					struct poulsback_display_domain *,
					struct poulsback_framebuffer_display_owner *);
/* These compose the existing typed display owner and preserve its fences,
 * separate GTT mapping and qualified-provider boundary. Pin is one attempt.
 * EAGAIN retirement may be polled; no failed/held graph may rearm. Show
 * callers first wait for the BO's writers (poulsback_display_wait_writers). */
int poulsback_framebuffer_pin(struct poulsback_framebuffer *, struct poulsback_display_domain *);
int poulsback_framebuffer_flip(struct poulsback_framebuffer *);
/* Caller owns a live DRM reference or an already-retained opaque owner.
 * Cached-only; no source, backing, GTT or provider callback after HOLD. */
int poulsback_framebuffer_status(struct poulsback_framebuffer *,
				 struct poulsback_framebuffer_report *);

/* At the final DRM put, destroy unlinks the DRM base exactly once before any
 * sleeping display-retirement callback. A mapped framebuffer retires in a work
 * item under the modeset locks, polling EAGAIN for a bounded time. Success
 * may release the owner only
 * after existing display_retire clears its pin/fence/mapping graph. Any HOLD,
 * active/pending ownership or failure retains the unregistered heap owner and
 * all GEM/device/module/mapping refs. There is no retained-owner free/rearm API.
 * DRM owns its base.dev/list/IDR/refcount fields; this leaf never repairs them. */
#endif
