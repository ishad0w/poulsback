/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef POULSBACK_DISPLAY_PSB_SOURCE_H
#define POULSBACK_DISPLAY_PSB_SOURCE_H
#include <linux/types.h>
struct drm_device;
struct drm_crtc;
struct drm_minor;
struct poulsback_display_domain;
struct poulsback_display_gtt_reservation;
struct poulsback_display_psb_source;
struct poulsback_display_gtt_binding;
struct poulsback_display_image;
struct page;
struct drm_mode_set;

struct poulsback_display_psb_source_report {
	u64 generation;
	u32 version, prepared, held, device_ref, module_ref, framebuffer_ref, power_ref,
		reservation_ref, registry_ref, pin_ref, pin_kind;
	u32 pipe, width, height, stride, format;
	u32 pgetbl, plane_control, plane_base, plane_size, pipe_source;
	int primary_error, cleanup_error;
};
/* Cached discovery only, after REMOVE source closure. This neither accesses
 * hardware nor certifies fetch retirement; a live slot bars manual teardown. */
bool poulsback_display_psb_source_retained(struct drm_device *);
/* Existing GTT borrower transaction only: caller holds domain/GTT/source locks.
 * Pure check and nofail CPU stores; no MMIO, locking or reference release. */
int poulsback_display_psb_source_link_check(struct poulsback_display_psb_source *,
					    struct poulsback_display_gtt_reservation *,
					    struct poulsback_display_domain *);
void poulsback_display_psb_source_link_commit(struct poulsback_display_psb_source *);
/* Exact same-mode static typed set_config boundary for the existing GMA hook.
 * With *handled true the caller MUST return this result, never fall through to
 * a legacy helper, even on error. The first call on a pipe creates its static
 * owner and registers that owner's provider. Core owns old_fb; this owner
 * retains independent old/new references. */
int poulsback_display_psb_set_config(struct drm_mode_set *, bool *handled);
struct drm_file;
/* DRM_IOCTL_PB_DISPLAY_INFO (render nodes too): the flip geometry. */
int poulsback_display_info_ioctl(struct drm_device *, void *data, struct drm_file *);
/* drm_driver.debugfs_init: the read-only dri/N/poulsback_display,
 * poulsback_position and poulsback_flips files. */
void poulsback_display_debugfs_init(struct drm_minor *);

/* Pipe timing records, in drm_psb_private under dev->event_lock: the
 * frame and pixel counters of one PIPExFRAMEPIXEL read and the scanline read
 * right after it (EMGD's PIPEx_SCANLINE_COUNT, PIPExCONF - 8), at a vblank
 * interrupt or right after a BASE store. */
enum poulsback_display_flip_kind { POULSBACK_DISPLAY_FLIP_VBLANK, POULSBACK_DISPLAY_FLIP_STORE };
struct poulsback_display_flip_record {
	u64 ns;
	u32 kind, pipe, scanline, frame, pixel;
};
struct work_struct;
struct poulsback_display_flips {
	struct poulsback_display_flip_record record[64];
	u32 count;
	/* Per pipe, the last flip: the replaced framebuffer's release, queued
	 * once DRM's vblank count reaches target (the flip's event). */
	struct {
		u64 target;
		struct work_struct *release;
	} pending[2];
};
/* gma500's vblank interrupt, after drm_handle_vblank: records the pipe's
 * position if it is enabled (PIPExCONF is read first, HW-DISP-001) and queues
 * the release of a flip whose vblank has come. */
void poulsback_display_psb_vblank(struct drm_device *, u32 pipe);
struct drm_framebuffer;
struct drm_pending_vblank_event;
struct drm_modeset_acquire_ctx;
/* gma500's page_flip, before its own: handles every flip that involves a
 * typed framebuffer. Typed to typed only, with the plane's mode and route;
 * -EBUSY while the last flip's vblank has not come. */
int poulsback_display_psb_page_flip(struct drm_crtc *, struct drm_framebuffer *,
				    struct drm_pending_vblank_event *,
				    struct drm_modeset_acquire_ctx *, bool *handled);
/* gma500's page_flip for its legacy framebuffers (after the typed hook): the
 * same pending rule and event; the replaced buffer stays pinned until then. */
int poulsback_display_psb_legacy_flip(struct drm_crtc *, struct drm_framebuffer *,
				      struct drm_pending_vblank_event *);
#endif
