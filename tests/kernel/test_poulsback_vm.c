// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The per-file address space (kernel/memory/poulsback_vm.c) on CPU stand-ins
 * (vm-mocks/): maps write PTEs and tables, unmaps clear and free them, and
 * after the loss's freeze an address space that existed before keeps every
 * PTE, table, page and its directory (HOLD), while one created after it
 * releases normally. CPU evidence only, never a GPU result. */
#include "../../kernel/memory/poulsback_vm.c"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "vm check %u line %d: %s\n", checks, __LINE__, #x);                    \
        }                                                                                          \
    } while (0)

#define BO_VA 0x60000000ULL

/* The pages of one BO, owned by the test. */
struct bo {
    struct page *pages[2048];
    u32 count;
    struct drm_mm_node node;
};

static void bo_init(struct bo *bo, u32 count)
{
    memset(bo, 0, sizeof(*bo));
    bo->count = count;
    for (u32 i = 0; i < count; i++)
        bo->pages[i] = alloc_page(GFP_KERNEL);
}

static void bo_fini(struct bo *bo)
{
    for (u32 i = 0; i < bo->count; i++)
        __free_page(bo->pages[i]);
}

static u32 pte_of(const struct poulsback_vm *vm, u64 va)
{
    u32 pde, pte;
    poulsback_vm_peek(vm, va, &pde, &pte);
    return pte;
}

static u32 pde_of(const struct poulsback_vm *vm, u64 va)
{
    u32 pde, pte;
    poulsback_vm_peek(vm, va, &pde, &pte);
    return pde;
}

/* Every page of the BO mapped at its node with a valid PTE for its page. */
static int mapped(const struct poulsback_vm *vm, const struct bo *bo, int snooped)
{
    for (u32 i = 0; i < bo->count; i++) {
        u32 word;
        if (poulsback_vm_pte(page_to_phys(bo->pages[i]), snooped, &word) ||
            pte_of(vm, bo->node.start + (u64)i * PAGE_SIZE) != word)
            return 0;
    }
    return 1;
}

static void kept(u32 *maps, u64 *bytes, u32 *spaces)
{
    poulsback_vm_kept(maps, bytes, spaces);
}

int main(void)
{
    static struct bo a, b, c;
    struct drm_mm_node busy, scene, import, failed, late_node;
    struct poulsback_vm *vm, *held, *late;
    u32 maps, spaces, base_pages, warnings;
    u64 bytes;

    /* Healthy: a map writes PTEs and a table, an unmap clears both. */
    vm = poulsback_vm_create();
    CHECK(vm && !vm_is_frozen() && !poulsback_vm_held(vm));
    CHECK(pde_of(vm, POULSBACK_VM_2D_SCRATCH_VA) && pte_of(vm, POULSBACK_VM_2D_SCRATCH_VA));
    bo_init(&a, 3);
    base_pages = mock_pages_live;
    CHECK(poulsback_vm_map(vm, POULSBACK_VM_GENERAL, PAGE_SIZE, a.pages, 0, true, &a.node) ==
          -EINVAL);
    CHECK(!poulsback_vm_map(vm, POULSBACK_VM_GENERAL, PAGE_SIZE, a.pages, a.count, true, &a.node));
    CHECK(a.node.start == BO_VA && drm_mm_node_allocated(&a.node) && mapped(vm, &a, 1));
    CHECK(mock_pages_live == base_pages + 1); /* the table for BO_VA's 4 MiB */
    CHECK(poulsback_vm_unmap(vm, &a.node));
    CHECK(!drm_mm_node_allocated(&a.node) && !pde_of(vm, BO_VA) && !pte_of(vm, BO_VA));
    CHECK(mock_pages_live == base_pages);
    poulsback_vm_destroy(vm);
    CHECK(mock_pages_live == base_pages - 3); /* directory, scratch page, its table */

    /* A failed map clears what it wrote and frees the table it allocated:
     * 1025 pages need two tables, and the second allocation fails. */
    vm = poulsback_vm_create();
    bo_init(&b, 1025);
    base_pages = mock_pages_live;
    mock_page_fail_in = 2;
    CHECK(poulsback_vm_map(vm, POULSBACK_VM_GENERAL, PAGE_SIZE, b.pages, b.count, false,
                           &b.node) == -ENOMEM);
    mock_page_fail_in = 0;
    CHECK(!drm_mm_node_allocated(&b.node) && !pde_of(vm, BO_VA) && !pte_of(vm, BO_VA));
    CHECK(mock_pages_live == base_pages);
    poulsback_vm_destroy(vm);

    /* A mapping left at release (a close that found its BO busy) keeps its
     * table and the directory, with a warning; the scratch page goes. */
    vm = poulsback_vm_create();
    base_pages = mock_pages_live;
    warnings = mock_warnings;
    CHECK(!poulsback_vm_map(vm, POULSBACK_VM_GENERAL, PAGE_SIZE, a.pages, a.count, true, &busy));
    poulsback_vm_destroy(vm);
    kept(&maps, &bytes, &spaces);
    CHECK(mock_warnings == warnings + 1 && spaces == 1 && !maps && !bytes);
    CHECK(mock_pages_live == base_pages - 1 && pte_of(vm, BO_VA) && pde_of(vm, BO_VA));

    /* The freeze: an address space created before it keeps every PTE, table
     * and node its unmaps would have cleared, and counts them. */
    held = poulsback_vm_create();
    bo_init(&c, 2);
    CHECK(!poulsback_vm_map(held, POULSBACK_VM_GENERAL, PAGE_SIZE, c.pages, c.count, true,
                            &c.node));
    CHECK(!poulsback_vm_map_fixed(held, POULSBACK_VM_SCENE_VA, a.pages, a.count, false, &scene));
    poulsback_vm_freeze();
    CHECK(vm_is_frozen() && poulsback_vm_held(held));
    base_pages = mock_pages_live;
    warnings = mock_warnings;
    CHECK(!poulsback_vm_unmap(held, &c.node));
    CHECK(drm_mm_node_allocated(&c.node) && mapped(held, &c, 1) && pde_of(held, BO_VA));
    CHECK(!poulsback_vm_unmap(held, &scene));
    CHECK(drm_mm_node_allocated(&scene) && pte_of(held, POULSBACK_VM_SCENE_VA));
    kept(&maps, &bytes, &spaces);
    CHECK(maps == 2 && bytes == 5 * PAGE_SIZE && spaces == 1);
    CHECK(mock_pages_live == base_pages);

    /* A map into a held space still works (a DRI3 import); a failed one
     * clears its PTEs but keeps the table it linked. */
    CHECK(!poulsback_vm_map(held, POULSBACK_VM_2D, PAGE_SIZE, a.pages, 1, true, &import));
    CHECK(mock_pages_live == base_pages);
    mock_page_fail_in = 2;
    CHECK(poulsback_vm_map(held, POULSBACK_VM_DEPTH, PAGE_SIZE, b.pages, b.count, false,
                           &failed) == -ENOMEM);
    mock_page_fail_in = 0;
    CHECK(!drm_mm_node_allocated(&failed) && failed.start == 0x50000000ULL);
    CHECK(!pte_of(held, 0x50000000ULL) && pde_of(held, 0x50000000ULL));
    CHECK(mock_pages_live == base_pages + 1);

    /* A held space's release keeps its directory, tables, scratch page and
     * mappings, without a warning. */
    base_pages = mock_pages_live;
    poulsback_vm_destroy(held);
    kept(&maps, &bytes, &spaces);
    CHECK(spaces == 2 && mock_pages_live == base_pages && mock_warnings == warnings);
    CHECK(mapped(held, &c, 1) && pte_of(held, POULSBACK_VM_SCENE_VA) &&
          pte_of(held, POULSBACK_VM_2D_SCRATCH_VA) && pte_of(held, import.start));

    /* An address space created after the freeze is never named by a BIF slot:
     * it maps, unmaps and releases normally. */
    late = poulsback_vm_create();
    CHECK(!poulsback_vm_held(late));
    base_pages = mock_pages_live;
    CHECK(!poulsback_vm_map(late, POULSBACK_VM_GENERAL, PAGE_SIZE, c.pages, c.count, true,
                            &late_node));
    CHECK(poulsback_vm_unmap(late, &late_node) && !pte_of(late, BO_VA) && !pde_of(late, BO_VA));
    CHECK(mock_pages_live == base_pages);
    poulsback_vm_destroy(late);
    CHECK(mock_pages_live == base_pages - 3);
    kept(&maps, &bytes, &spaces);
    CHECK(maps == 2 && bytes == 5 * PAGE_SIZE && spaces == 2 && mock_warnings == warnings);

    bo_fini(&b);
    printf("vm: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
