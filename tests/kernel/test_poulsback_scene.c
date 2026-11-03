// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* A context's scene buffer (kernel/engine/poulsback_scene.c) on the vm suite's
 * CPU stand-ins (vm-mocks/), with the real address space under it: every
 * allocation and mapping failure of poulsback_scene_create unwinds without a
 * leak, a NULL access or a lost VM reference, a created scene maps its pages
 * at its slot and its last put frees them, and a frozen space keeps them.
 * The pages-array failure is the public review's reproducer: it loaded a
 * page pointer from the NULL array. CPU evidence only. */
#include "../../kernel/memory/poulsback_vm.c"
#include "../../kernel/engine/poulsback_scene.c"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "scene check %u line %d: %s\n", checks, __LINE__, #x);                 \
        }                                                                                          \
    } while (0)

static u32 pte_at(const struct poulsback_vm *vm, u64 va)
{
    u32 pde, pte;
    poulsback_vm_peek(vm, va, &pde, &pte);
    return pte;
}

/* A failed create leaves the address space, its references and every
 * allocation as they were. */
static void fails_clean(struct poulsback_vm *vm, u32 handle)
{
    u64 va = POULSBACK_VM_SCENE_VA + (u64)(handle - 1) * POULSBACK_VM_SCENE_SLOT;
    unsigned pages = mock_pages_live, kv = mock_kv_live;
    int ref = vm->ref.refcount;
    u32 pte = pte_at(vm, va);
    CHECK(!poulsback_scene_create(vm, handle));
    CHECK(mock_pages_live == pages && mock_kv_live == kv && vm->ref.refcount == ref);
    CHECK(pte_at(vm, va) == pte);
}

int main(void)
{
    struct poulsback_vm *vm = poulsback_vm_create(), *held;
    struct poulsback_scene *scene, *other;
    struct sgx535_engine121_scene sizes;
    u32 count, i;
    unsigned pages;

    CHECK(vm && !sgx535_engine121_scene_build(POULSBACK_SUBMIT_MAX_SIZE, POULSBACK_SUBMIT_MAX_SIZE,
                                               &sizes));
    count = (u32)DIV_ROUND_UP(sgx535_engine121_scene_bytes(&sizes, 2), PAGE_SIZE);
    CHECK(count > 2);

    /* The pages array: the review's reproducer. */
    mock_kvcalloc_fail = true;
    fails_clean(vm, 1);
    mock_kvcalloc_fail = false;
    /* Each page, first to last, then the page table the mapping needs. */
    for (i = 1; i <= count + 1; i++) {
        mock_page_fail_in = i;
        fails_clean(vm, 1);
        mock_page_fail_in = 0;
    }

    /* A scene maps its pages at its slot; its last put unmaps and frees. */
    pages = mock_pages_live;
    scene = poulsback_scene_create(vm, 2);
    CHECK(scene && scene->count == count && scene->vm == vm && vm->ref.refcount == 2);
    CHECK(scene && drm_mm_node_allocated(&scene->node) &&
          scene->node.start == POULSBACK_VM_SCENE_VA + POULSBACK_VM_SCENE_SLOT);
    for (i = 0; scene && i < count; i++) {
        u32 word;
        CHECK(!poulsback_vm_pte(page_to_phys(scene->pages[i]), 0, &word) &&
              pte_at(vm, scene->node.start + (u64)i * PAGE_SIZE) == word);
    }
    CHECK(mock_pages_live == pages + count + 1); /* its pages and their table */
    /* A second scene at the same slot fails its mapping and unwinds. */
    fails_clean(vm, 2);
    other = poulsback_scene_create(vm, 3);
    CHECK(other && vm->ref.refcount == 3);
    poulsback_scene_put(scene);
    CHECK(vm->ref.refcount == 2 && !pte_at(vm, POULSBACK_VM_SCENE_VA + POULSBACK_VM_SCENE_SLOT));
    poulsback_scene_put(other);
    CHECK(vm->ref.refcount == 1 && mock_pages_live == pages && !mock_kv_live);

    /* A frozen address space keeps a scene's pages, node and PTEs (HOLD); the
     * scene drops only its address-space reference. */
    held = poulsback_vm_create();
    scene = poulsback_scene_create(held, 1);
    CHECK(scene && held->ref.refcount == 2);
    poulsback_vm_freeze();
    pages = mock_pages_live;
    poulsback_scene_put(scene);
    CHECK(held->ref.refcount == 1 && mock_pages_live == pages);
    CHECK(drm_mm_node_allocated(&scene->node) && pte_at(held, POULSBACK_VM_SCENE_VA));

    poulsback_vm_destroy(vm);
    printf("scene: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
