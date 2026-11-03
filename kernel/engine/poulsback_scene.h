/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* A context's scene buffer: kernel pages at a fixed VA of the file's space
 * (0x4C000000 + 1 MiB per context slot). Jobs hold references, so it lives
 * until the last job on it is freed. */
#ifndef POULSBACK_SCENE_H
#define POULSBACK_SCENE_H

#include <drm/drm_mm.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/types.h>

struct page;
struct poulsback_vm;

struct poulsback_scene {
	struct kref ref;
	struct poulsback_vm *vm;
	/* count is set only once pages exists; unallocated slots are NULL. */
	struct page **pages;
	u32 count;
	struct drm_mm_node node;
	/* Jobs take the two region-header arrays in turn. */
	atomic_t arrays;
};

/* Zeroed, flushed pages for the largest scene with both region-header arrays,
 * mapped at context slot handle's VA in vm; NULL when any allocation or the
 * mapping fails, with everything it took released. */
struct poulsback_scene *poulsback_scene_create(struct poulsback_vm *vm, u32 handle);
void poulsback_scene_put(struct poulsback_scene *scene);
#endif
