// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_scene.h"
#include "poulsback_exec_uapi.h"
#include "poulsback_vm.h"
#include "poulsback_vm_layout.h"
#include "sgx535_engine121.h"

#include <asm/cacheflush.h>
#include <linux/mm.h>
#include <linux/slab.h>

/* A held address space keeps the scene's PTEs (HOLD): the scene then keeps
 * its pages and its node for good, and drops only its address space. */
static void scene_release(struct kref *ref)
{
	struct poulsback_scene *scene = container_of(ref, struct poulsback_scene, ref);
	u32 i;
	if (drm_mm_node_allocated(&scene->node) && !poulsback_vm_unmap(scene->vm, &scene->node)) {
		poulsback_vm_put(scene->vm);
		return;
	}
	for (i = 0; i < scene->count; i++)
		if (scene->pages[i])
			__free_page(scene->pages[i]);
	kvfree(scene->pages);
	poulsback_vm_put(scene->vm);
	kfree(scene);
}

void poulsback_scene_put(struct poulsback_scene *scene)
{
	if (scene)
		kref_put(&scene->ref, scene_release);
}

struct poulsback_scene *poulsback_scene_create(struct poulsback_vm *vm, u32 handle)
{
	struct sgx535_engine121_scene sizes;
	struct poulsback_scene *scene;
	u32 i, count;
	u64 va;
	if (sgx535_engine121_scene_build(POULSBACK_SUBMIT_MAX_SIZE, POULSBACK_SUBMIT_MAX_SIZE,
					 &sizes))
		return NULL;
	scene = kzalloc(sizeof(*scene), GFP_KERNEL);
	if (!scene)
		return NULL;
	kref_init(&scene->ref);
	scene->vm = poulsback_vm_get(vm);
	/* The count is published only with its array: release walks count
	 * entries, so a failed array allocation leaves no count over a NULL
	 * array. */
	count = (u32)DIV_ROUND_UP(sgx535_engine121_scene_bytes(&sizes, 2), PAGE_SIZE);
	scene->pages = kvcalloc(count, sizeof(*scene->pages), GFP_KERNEL);
	if (!scene->pages)
		goto put;
	scene->count = count;
	for (i = 0; i < count; i++) {
		scene->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!scene->pages[i])
			goto put;
		clflush_cache_range(page_address(scene->pages[i]), PAGE_SIZE);
	}
	va = POULSBACK_VM_SCENE_VA + (u64)(handle - 1) * POULSBACK_VM_SCENE_SLOT;
	if (poulsback_vm_map_fixed(vm, va, scene->pages, count, false, &scene->node))
		goto put;
	return scene;
put:
	poulsback_scene_put(scene);
	return NULL;
}
