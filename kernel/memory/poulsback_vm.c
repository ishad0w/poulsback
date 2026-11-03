// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_vm.h"
#include "poulsback_vm_layout.h"
#include "poulsback_errsite.h"

#include <asm/cacheflush.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#define VM_TABLES 1024U

/* Directory and page tables are lowmem pages written through their linear
 * mapping and flushed with clflush. The engine installs a job's directory and
 * invalidates the BIF cache (INVALDC) before every job, and a running job holds
 * its BOs, so changes here need no invalidation of their own. */
struct poulsback_vm {
	struct kref ref;
	struct mutex lock;
	struct page *directory, *scratch;
	struct drm_mm_node scratch_node;
	struct page *tables[VM_TABLES];
	u16 used[VM_TABLES];
	struct drm_mm mm;
	/* Created before the freeze: a BIF slot may still name its directory. */
	bool reachable;
};

/* Set once, when the engine stops on an unknown outcome (HOLD), before any of
 * its lost fences signal. From then on an address space created before keeps
 * every PTE, table and page it maps, and its directory: the GPU may still walk
 * it. One SGX per system. */
static bool vm_frozen;
/* What the freeze kept: mappings and their pages, and whole address spaces. */
static atomic_t vm_kept_maps, vm_kept_pages, vm_kept_spaces;

void poulsback_vm_freeze(void)
{
	smp_store_release(&vm_frozen, true);
}

static bool vm_is_frozen(void)
{
	return smp_load_acquire(&vm_frozen);
}

void poulsback_vm_kept(u32 *maps, u64 *bytes, u32 *spaces)
{
	*maps = (u32)atomic_read(&vm_kept_maps);
	*bytes = (u64)(u32)atomic_read(&vm_kept_pages) << PAGE_SHIFT;
	*spaces = (u32)atomic_read(&vm_kept_spaces);
}

bool poulsback_vm_held(const struct poulsback_vm *vm)
{
	return vm->reachable && vm_is_frozen();
}

static u32 *table_words(struct page *page)
{
	return page_address(page);
}

static int vm_fill(struct poulsback_vm *vm, struct page **pages, u32 count, bool snooped,
		   struct drm_mm_node *node);
static void vm_clear(struct poulsback_vm *vm, u64 va, u32 count);

/* The file's 2D scratch page at POULSBACK_VM_2D_SCRATCH_VA: only the 2D core
 * writes it, and nothing reads it. */
static int vm_scratch(struct poulsback_vm *vm)
{
	int ret;
	vm->scratch = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!vm->scratch)
		return -ENOMEM;
	clflush_cache_range(page_address(vm->scratch), PAGE_SIZE);
	vm->scratch_node.start = POULSBACK_VM_2D_SCRATCH_VA;
	vm->scratch_node.size = PAGE_SIZE;
	mutex_lock(&vm->lock);
	ret = drm_mm_reserve_node(&vm->mm, &vm->scratch_node);
	if (!ret)
		ret = vm_fill(vm, &vm->scratch, 1, false, &vm->scratch_node);
	mutex_unlock(&vm->lock);
	return ret;
}

struct poulsback_vm *poulsback_vm_create(void)
{
	struct poulsback_vm *vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return NULL;
	vm->directory = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!vm->directory) {
		kfree(vm);
		return NULL;
	}
	clflush_cache_range(table_words(vm->directory), PAGE_SIZE);
	/* After the freeze no job runs, so no BIF slot will ever name it. */
	vm->reachable = !vm_is_frozen();
	kref_init(&vm->ref);
	mutex_init(&vm->lock);
	drm_mm_init(&vm->mm, POULSBACK_VM_START, POULSBACK_VM_END - POULSBACK_VM_START);
	if (vm_scratch(vm)) {
		poulsback_vm_put(vm);
		return NULL;
	}
	return vm;
}

/* A held address space keeps everything: directory, tables, scratch page and
 * every mapping left in it. So does one that still maps a BO whose close found
 * it busy (bo_close): its tables stay under that mapping (HOLD). */
static void vm_release(struct kref *ref)
{
	struct poulsback_vm *vm = container_of(ref, struct poulsback_vm, ref);
	u32 i;
	if (poulsback_vm_held(vm)) {
		atomic_inc(&vm_kept_spaces);
		return;
	}
	if (drm_mm_node_allocated(&vm->scratch_node)) {
		vm_clear(vm, vm->scratch_node.start, 1);
		drm_mm_remove_node(&vm->scratch_node);
	}
	if (vm->scratch)
		__free_page(vm->scratch);
	if (WARN_ON(!drm_mm_clean(&vm->mm))) {
		atomic_inc(&vm_kept_spaces);
		return;
	}
	for (i = 0; i < VM_TABLES; i++)
		if (vm->tables[i])
			__free_page(vm->tables[i]);
	drm_mm_takedown(&vm->mm);
	__free_page(vm->directory);
	mutex_destroy(&vm->lock);
	kfree(vm);
}

struct poulsback_vm *poulsback_vm_get(struct poulsback_vm *vm)
{
	kref_get(&vm->ref);
	return vm;
}

void poulsback_vm_put(struct poulsback_vm *vm)
{
	if (vm)
		kref_put(&vm->ref, vm_release);
}

void poulsback_vm_destroy(struct poulsback_vm *vm)
{
	poulsback_vm_put(vm);
}

void poulsback_vm_link(struct poulsback_vm *vm, const u32 *index, const u32 *pde, u32 count)
{
	u32 *dir = table_words(vm->directory), i;
	mutex_lock(&vm->lock);
	for (i = 0; i < count; i++) {
		WARN_ON(vm->tables[index[i]]);
		dir[index[i]] = pde[i];
		clflush_cache_range(&dir[index[i]], sizeof(*dir));
	}
	mutex_unlock(&vm->lock);
}

u32 poulsback_vm_directory(const struct poulsback_vm *vm)
{
	return (u32)page_to_phys(vm->directory);
}

void poulsback_vm_peek(const struct poulsback_vm *vm, u64 va, u32 *pde, u32 *pte)
{
	u32 index = poulsback_vm_pde_index(va);
	struct page *table = READ_ONCE(vm->tables[index]);
	*pde = READ_ONCE(table_words(vm->directory)[index]);
	*pte = table ? READ_ONCE(table_words(table)[poulsback_vm_pte_index(va)]) : 0;
}

/* The page table for va's 4 MiB, allocated and linked on first use. */
static u32 *vm_table(struct poulsback_vm *vm, u64 va)
{
	u32 index = poulsback_vm_pde_index(va), *dir = table_words(vm->directory), pde;
	struct page *table = vm->tables[index];
	if (table)
		return table_words(table);
	/* A linked device PDE is never replaced. */
	if (WARN_ON_ONCE(dir[index]))
		return NULL;
	table = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!table)
		return NULL;
	if (poulsback_vm_pde(page_to_phys(table), &pde)) {
		__free_page(table);
		return NULL;
	}
	clflush_cache_range(table_words(table), PAGE_SIZE);
	vm->tables[index] = table;
	dir[index] = pde;
	clflush_cache_range(&dir[index], sizeof(dir[index]));
	return table_words(table);
}

/* Frees the table at a directory index once it maps nothing, unless the
 * address space is held: the GPU may still walk a table it was linked to. */
static void vm_drop_table(struct poulsback_vm *vm, u32 index)
{
	u32 *dir = table_words(vm->directory);
	if (!vm->tables[index] || vm->used[index] || poulsback_vm_held(vm))
		return;
	dir[index] = 0;
	clflush_cache_range(&dir[index], sizeof(dir[index]));
	__free_page(vm->tables[index]);
	vm->tables[index] = NULL;
}

/* Clears `count` PTEs from va and drops tables left empty. */
static void vm_clear(struct poulsback_vm *vm, u64 va, u32 count)
{
	for (; count; count--, va += PAGE_SIZE) {
		u32 index = poulsback_vm_pde_index(va), *table;
		if (WARN_ON(!vm->tables[index]))
			continue;
		table = table_words(vm->tables[index]);
		table[poulsback_vm_pte_index(va)] = 0;
		clflush_cache_range(&table[poulsback_vm_pte_index(va)], sizeof(*table));
		vm->used[index]--;
		vm_drop_table(vm, index);
	}
}

/* Writes the PTEs of a node just allocated; on failure clears what it wrote
 * and removes the node. Called with the VM lock held. */
static int vm_fill(struct poulsback_vm *vm, struct page **pages, u32 count, bool snooped,
		   struct drm_mm_node *node)
{
	u64 va;
	u32 i;
	int ret = 0;
	for (i = 0, va = node->start; i < count; i++, va += PAGE_SIZE) {
		u32 *table = vm_table(vm, va), word;
		if (!table) {
			ret = POULSBACK_ERRSITE(-ENOMEM);
			break;
		}
		if (table[poulsback_vm_pte_index(va)]) {
			ret = POULSBACK_ERRSITE(-EBUSY);
			break;
		}
		if (poulsback_vm_pte(page_to_phys(pages[i]), snooped, &word)) {
			ret = POULSBACK_ERRSITE(-EFAULT);
			break;
		}
		table[poulsback_vm_pte_index(va)] = word;
		clflush_cache_range(&table[poulsback_vm_pte_index(va)], sizeof(*table));
		vm->used[poulsback_vm_pde_index(va)]++;
	}
	if (!ret)
		return 0;
	vm_clear(vm, node->start, i);
	/* A table allocated for the failed page holds no entry. */
	vm_drop_table(vm, poulsback_vm_pde_index(va));
	drm_mm_remove_node(node);
	return ret;
}

int poulsback_vm_map(struct poulsback_vm *vm, u32 vm_class, u64 alignment, struct page **pages,
		     u32 count, bool snooped, struct drm_mm_node *node)
{
	u64 start, end;
	int ret;
	if (poulsback_vm_class_range(vm_class, &start, &end) || !count)
		return POULSBACK_ERRSITE(-EINVAL);
	mutex_lock(&vm->lock);
	ret = drm_mm_insert_node_in_range(&vm->mm, node, (u64)count << PAGE_SHIFT, alignment, 0,
					  start, end, DRM_MM_INSERT_BEST);
	if (!ret && !poulsback_vm_span_ok(vm_class, node->start, node->size)) {
		drm_mm_remove_node(node);
		ret = POULSBACK_ERRSITE(-EFAULT);
	}
	if (!ret)
		ret = vm_fill(vm, pages, count, snooped, node);
	mutex_unlock(&vm->lock);
	return ret;
}

int poulsback_vm_map_fixed(struct poulsback_vm *vm, u64 va, struct page **pages, u32 count,
			   bool snooped, struct drm_mm_node *node)
{
	u64 end = va + ((u64)count << PAGE_SHIFT);
	int ret;
	/* Only the scene slots: never a class, never a device PDE. */
	if (!count || (va & ~PAGE_MASK) || va < POULSBACK_VM_SCENE_VA ||
	    end > POULSBACK_VM_SCENE_END)
		return POULSBACK_ERRSITE(-EINVAL);
	memset(node, 0, sizeof(*node));
	node->start = va;
	node->size = (u64)count << PAGE_SHIFT;
	mutex_lock(&vm->lock);
	ret = drm_mm_reserve_node(&vm->mm, node);
	if (!ret)
		ret = vm_fill(vm, pages, count, snooped, node);
	mutex_unlock(&vm->lock);
	return ret;
}

bool poulsback_vm_unmap(struct poulsback_vm *vm, struct drm_mm_node *node)
{
	u32 count = (u32)(node->size >> PAGE_SHIFT);
	bool held;
	mutex_lock(&vm->lock);
	held = poulsback_vm_held(vm);
	if (held) {
		atomic_inc(&vm_kept_maps);
		atomic_add(count, &vm_kept_pages);
	} else {
		vm_clear(vm, node->start, count);
		drm_mm_remove_node(node);
	}
	mutex_unlock(&vm->lock);
	return !held;
}
