/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* A file's SGX address space: one page directory and a VA allocator
 * (docs/kernel/memory.md, "Buffer objects and per-file address spaces"). */
#ifndef POULSBACK_VM_H
#define POULSBACK_VM_H

#include <drm/drm_mm.h>
#include <linux/types.h>

struct page;
struct poulsback_vm;

struct poulsback_vm *poulsback_vm_create(void);
/* Drops the creator's reference; the VM goes when the last job holding it
 * is freed, and by then every mapping must be gone. A held VM (below) never
 * goes. */
void poulsback_vm_destroy(struct poulsback_vm *vm);
struct poulsback_vm *poulsback_vm_get(struct poulsback_vm *vm);
void poulsback_vm_put(struct poulsback_vm *vm);

/* Allocates `count` pages of VA in the class range with `alignment` and maps
 * `pages` there; snooped PTEs for write-back CPU pages. */
int poulsback_vm_map(struct poulsback_vm *vm, u32 vm_class, u64 alignment, struct page **pages,
		     u32 count, bool snooped, struct drm_mm_node *node);
/* False when the VM is held: the node and its PTEs stay, so the caller keeps
 * the pages behind them and the node for good. */
bool poulsback_vm_unmap(struct poulsback_vm *vm, struct drm_mm_node *node);
/* Maps kernel pages at a fixed VA outside every class (a device range). */
int poulsback_vm_map_fixed(struct poulsback_vm *vm, u64 va, struct page **pages, u32 count,
			   bool snooped, struct drm_mm_node *node);

/* Writes fixed PDEs (device ranges outside every class) into the directory. */
void poulsback_vm_link(struct poulsback_vm *vm, const u32 *index, const u32 *pde, u32 count);

/* The loss (HOLD, docs/kernel/memory.md): the engine freezes the VM layer
 * once, when it stops on an unknown outcome. Every VM that existed then is
 * held: unmaps and releases keep their PTEs, tables and pages, and its file
 * creates no BO. poulsback_vm_kept() counts what the freeze kept. */
void poulsback_vm_freeze(void);
bool poulsback_vm_held(const struct poulsback_vm *vm);
void poulsback_vm_kept(u32 *maps, u64 *bytes, u32 *spaces);

/* Physical address of the page directory, for a BIF directory slot. */
u32 poulsback_vm_directory(const struct poulsback_vm *vm);
/* The PDE and PTE that translate va, read without the lock: a diagnostic for
 * a GPU fault, which may race with a map or unmap. */
void poulsback_vm_peek(const struct poulsback_vm *vm, u64 va, u32 *pde, u32 *pte);
#endif
