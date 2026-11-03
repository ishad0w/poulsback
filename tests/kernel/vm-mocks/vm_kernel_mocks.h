// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* CPU substitutes for the kernel interfaces kernel/memory/poulsback_vm.c and
 * kernel/engine/poulsback_scene.c use, for the `vm` and `scene` host suites.
 * Every kernel header the files include names this one. Pages carry a fake
 * physical address below 4 GiB and count their allocations, which can fail
 * on demand; drm_mm is a first-fit list. Nothing here models a device. */
#ifndef VM_KERNEL_MOCKS_H
#define VM_KERNEL_MOCKS_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
/* The UAPI headers' types (include/uapi/poulsback, libdrm's drm.h). */
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t __s32;
typedef int64_t __s64;
typedef size_t __kernel_size_t;

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define GFP_KERNEL 0U
#define __GFP_ZERO 1U

#define container_of(ptr, type, member) ((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define smp_store_release(p, v) (*(p) = (v))
#define smp_load_acquire(p) (*(p))

static unsigned mock_warnings;
static int mock_warn(int condition)
{
    mock_warnings += condition != 0;
    return condition;
}
#define WARN_ON(x) mock_warn(!!(x))
#define WARN_ON_ONCE(x) mock_warn(!!(x))

/* Pages: live count, a fake physical address per page, and an optional
 * failure of the Nth allocation from now (0: none). */
struct page {
    void *data;
    u64 phys;
};
static unsigned mock_pages_live, mock_page_serial, mock_page_fail_in;

static struct page *alloc_page(unsigned gfp)
{
    struct page *page;
    (void)gfp;
    if (mock_page_fail_in && !--mock_page_fail_in)
        return NULL;
    page = calloc(1, sizeof(*page));
    if (!page)
        abort();
    page->data = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (!page->data)
        abort();
    memset(page->data, 0, PAGE_SIZE);
    page->phys = 0x10000000ULL + (u64)++mock_page_serial * PAGE_SIZE;
    mock_pages_live++;
    return page;
}

static void __free_page(struct page *page)
{
    free(page->data);
    free(page);
    mock_pages_live--;
}

static void *page_address(const struct page *page)
{
    return page->data;
}

static u64 page_to_phys(const struct page *page)
{
    return page->phys;
}

static void clflush_cache_range(void *address, unsigned int size)
{
    (void)address;
    (void)size;
}

/* kvcalloc fails while mock_kvcalloc_fail is set; kvfree takes NULL. */
static bool mock_kvcalloc_fail;
static unsigned mock_kv_live;

static void *kzalloc(size_t size, unsigned gfp)
{
    (void)gfp;
    return calloc(1, size);
}

static void kfree(const void *p)
{
    free((void *)p);
}

static inline void *kvcalloc(size_t n, size_t size, unsigned gfp)
{
    void *p;
    (void)gfp;
    if (mock_kvcalloc_fail)
        return NULL;
    p = calloc(n, size);
    if (!p)
        abort();
    mock_kv_live++;
    return p;
}

static inline void kvfree(const void *p)
{
    if (p)
        mock_kv_live--;
    free((void *)p);
}

struct mutex {
    int held;
};
static void mutex_init(struct mutex *m)
{
    m->held = 0;
}
static void mutex_lock(struct mutex *m)
{
    if (m->held++)
        abort();
}
static void mutex_unlock(struct mutex *m)
{
    if (!m->held--)
        abort();
}
static void mutex_destroy(struct mutex *m)
{
    if (m->held)
        abort();
}

typedef struct {
    int counter;
} atomic_t;
static int atomic_read(const atomic_t *v)
{
    return v->counter;
}
static void atomic_inc(atomic_t *v)
{
    v->counter++;
}
static void atomic_add(u32 i, atomic_t *v)
{
    v->counter += (int)i;
}

struct kref {
    int refcount;
};
static void kref_init(struct kref *k)
{
    k->refcount = 1;
}
static void kref_get(struct kref *k)
{
    k->refcount++;
}
static int kref_put(struct kref *k, void (*release)(struct kref *))
{
    if (--k->refcount)
        return 0;
    release(k);
    return 1;
}

/* drm_mm: up to MOCK_MM_NODES nodes, first fit. */
#define MOCK_MM_NODES 64
#define DRM_MM_INSERT_BEST 0
struct drm_mm;
struct drm_mm_node {
    u64 start, size;
    bool allocated;
    struct drm_mm *mm;
};
struct drm_mm {
    u64 start, size;
    struct drm_mm_node *nodes[MOCK_MM_NODES];
};

static void drm_mm_init(struct drm_mm *mm, u64 start, u64 size)
{
    memset(mm, 0, sizeof(*mm));
    mm->start = start;
    mm->size = size;
}

static bool drm_mm_node_allocated(const struct drm_mm_node *node)
{
    return node->allocated;
}

static bool drm_mm_clean(const struct drm_mm *mm)
{
    for (unsigned i = 0; i < MOCK_MM_NODES; i++)
        if (mm->nodes[i])
            return false;
    return true;
}

static void drm_mm_takedown(struct drm_mm *mm)
{
    if (!drm_mm_clean(mm))
        abort();
}

static bool mock_mm_free(const struct drm_mm *mm, u64 start, u64 size)
{
    if (start < mm->start || start + size > mm->start + mm->size)
        return false;
    for (unsigned i = 0; i < MOCK_MM_NODES; i++) {
        const struct drm_mm_node *n = mm->nodes[i];
        if (n && start < n->start + n->size && n->start < start + size)
            return false;
    }
    return true;
}

static int mock_mm_add(struct drm_mm *mm, struct drm_mm_node *node)
{
    for (unsigned i = 0; i < MOCK_MM_NODES; i++)
        if (!mm->nodes[i]) {
            mm->nodes[i] = node;
            node->allocated = true;
            node->mm = mm;
            return 0;
        }
    return -ENOSPC;
}

static int drm_mm_reserve_node(struct drm_mm *mm, struct drm_mm_node *node)
{
    return mock_mm_free(mm, node->start, node->size) ? mock_mm_add(mm, node) : -ENOSPC;
}

static int drm_mm_insert_node_in_range(struct drm_mm *mm, struct drm_mm_node *node, u64 size,
                                       u64 alignment, unsigned long color, u64 start, u64 end,
                                       int mode)
{
    (void)color;
    (void)mode;
    for (u64 va = start; va + size <= end; va += alignment ? alignment : PAGE_SIZE)
        if (mock_mm_free(mm, va, size)) {
            node->start = va;
            node->size = size;
            return mock_mm_add(mm, node);
        }
    return -ENOSPC;
}

static void drm_mm_remove_node(struct drm_mm_node *node)
{
    for (unsigned i = 0; i < MOCK_MM_NODES; i++)
        if (node->mm->nodes[i] == node)
            node->mm->nodes[i] = NULL;
    node->allocated = false;
}
#endif
