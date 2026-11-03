/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* GEM buffer objects with a VA in every file that holds a handle
 * (docs/kernel/memory.md, "Buffer objects and per-file address spaces"). */
#ifndef POULSBACK_BO_H
#define POULSBACK_BO_H

#include <linux/types.h>

struct dma_buf;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct page;
struct poulsback_vm;

int poulsback_bo_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int poulsback_bo_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
/* The BO behind a file's handle, with its VA in that file, for a job: takes a
 * reference and holds the mapping (even past the last handle's close) until
 * poulsback_bo_release(obj, the file's address space). */
int poulsback_bo_resolve(struct drm_file *file, u32 handle, struct drm_gem_object **obj, u64 *va,
			 u64 *size);
void poulsback_bo_release(struct drm_gem_object *obj, struct poulsback_vm *vm);
/* Type identity only: a poulsback BO is never a gma500 psb_gem_object. */
bool poulsback_bo_is_object(const struct drm_gem_object *obj);
/* A BO created with POULSBACK_BO_SCANOUT (always CPU_WC). */
bool poulsback_bo_is_scanout(const struct drm_gem_object *obj);
/* Display pin of a SCANOUT BO: one GEM reference and one display pin count.
 * The BO's pages stay pinned (and WC) for its life, so the returned array is
 * stable while pinned. There is no alias refusal: a pinned BO may be mapped,
 * exported or used by the GPU. unpin drops both. */
int poulsback_bo_display_pin(struct drm_gem_object *obj, struct page ***pages, u32 *count);
void poulsback_bo_display_unpin(struct drm_gem_object *obj);
/* Whether pages/count are this display-pinned SCANOUT BO's own. */
int poulsback_bo_display_check(struct drm_gem_object *obj, struct page **pages, u32 count);

/* The driver's PRIME hooks: BOs only. */
int poulsback_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file, u32 handle,
				 u32 flags, int *fd);
int poulsback_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file, int fd,
				 u32 *handle);
struct drm_gem_object *poulsback_prime_import(struct drm_device *dev, struct dma_buf *buf);
#endif
