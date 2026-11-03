// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* RAM edges for the exact locked/integration-patched Linux bodies. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t resource_size_t;
typedef uint32_t u32;
typedef uint16_t u16;
#define __iomem
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PSB_PTE_VALID 1U
#define PSB_PTE_WO 2U
#define PSB_PTE_RO 4U
#define PSB_PTE_CACHED 8U
#define PSB_MMU_CACHED_MEMORY 1U
#define PSB_MMU_RO_MEMORY 2U
#define PSB_MMU_WO_MEMORY 4U
#define PSB_GTT_RESOURCE 3
#define PSB_GATT_RESOURCE 2
#define PSB_GMCH_CTRL 0x52
#define _PSB_GMCH_ENABLED 4U
#define PSB_PGETBL_CTL 0x2020
#define _PSB_PGETBL_ENABLED 1U
#define PSB_BSM 0x5c
#define IORESOURCE_MEM 0x200U
#define BUG_ON(v) assert(!(v))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#ifndef GTT_EXPECT_LOCK
#define GTT_EXPECT_LOCK 1
#endif

struct pci_dev;
struct device {
    struct pci_dev *pci;
};
struct drm_device {
    struct device *dev;
};
struct resource {
    resource_size_t start, end;
    const char *name;
    unsigned long flags;
    struct resource *child, *sibling;
};
struct pci_dev {
    struct device dev;
    struct drm_device *drm;
    struct resource resource[6];
    resource_size_t length[6];
};
struct page {
    uint32_t pfn;
};
struct mutex {
    unsigned int held, initialized;
};
#include "gtt-struct.current.inc"
struct psb_test_ops {
    int (*power_up)(struct drm_device *dev);
    int (*restore_regs)(struct drm_device *dev);
};
struct drm_psb_private {
    struct drm_device dev;
    struct psb_gtt gtt;
    struct page *scratch_page;
    u32 *gtt_map;
    struct mutex gtt_mutex;
    struct resource *gtt_mem;
    u16 gmch_ctrl;
    u32 pge_ctl;
    unsigned long stolen_base, vram_stolen_size;
    const struct psb_test_ops *ops;
};

static struct drm_psb_private owner;
static struct pci_dev pci;
static struct page scratch = {0x12345U};
static u32 *allocation;
static size_t capacity, writes, reads, map_calls, unmaps;
static size_t last_index, final_read_index;
static unsigned int config_read_error, config_write_error, map_error;
static unsigned int config_reads, config_writes, mmio_reads, mmio_writes;
static u16 gmch;
static u32 pgetbl;
static bool post_read, table_locked, expect_clear_lock;
static unsigned int checks;
static unsigned int gem_calls, restore_calls, irq_calls, power_calls;
static unsigned int pm_busy;
static int gem_error, restore_error, irq_error, pci_error;
static bool expect_resume_order = true;

#define CHECK(v)                                                                                   \
    do {                                                                                           \
        assert(v);                                                                                 \
        checks++;                                                                                  \
    } while (0)
static struct drm_psb_private *to_drm_psb_private(struct drm_device *dev)
{
    return container_of(dev, struct drm_psb_private, dev);
}
static struct pci_dev *to_pci_dev(struct device *dev)
{
    return dev->pci;
}
static uint32_t page_to_pfn(const struct page *p)
{
    return p->pfn;
}
static resource_size_t resource_size(const struct resource *r)
{
    return r->end - r->start + 1;
}
static resource_size_t pci_resource_start(struct pci_dev *p, unsigned int index)
{
    return p->resource[index].start;
}
static resource_size_t pci_resource_len(struct pci_dev *p, unsigned int index)
{
    return p->length[index];
}
static int allocate_resource(struct resource *root, struct resource *r, resource_size_t size,
                             resource_size_t begin, resource_size_t end, resource_size_t align,
                             void *a, void *b)
{
    (void)root;
    (void)align;
    (void)a;
    (void)b;
    if (!size || size - 1 > end - begin)
        return -ENOSPC;
    r->start = begin;
    r->end = begin + size - 1;
    return 0;
}
static void mutex_init(struct mutex *m)
{
    CHECK(!m->initialized);
    m->initialized = 1;
}
static void mutex_destroy(struct mutex *m)
{
    CHECK(m->initialized && !m->held);
    m->initialized = 0;
}
static void mutex_lock(struct mutex *m)
{
    CHECK(m->initialized && !m->held);
    m->held = 1;
    table_locked = true;
}
static void mutex_unlock(struct mutex *m)
{
    CHECK(m->held);
    m->held = 0;
    table_locked = false;
}
static void message(const void *dev, const char *format, ...)
{
    (void)dev;
    (void)format;
}
#define dev_dbg(...) message(__VA_ARGS__)
#define dev_err(...) message(__VA_ARGS__)
#define drm_dbg(...) message(__VA_ARGS__)
static int pcibios_err_to_errno(int ret)
{
    return ret ? -EIO : 0;
}
static int pci_read_config_word(struct pci_dev *p, int where, u16 *out)
{
    CHECK(p == &pci && where == PSB_GMCH_CTRL);
    config_reads++;
    if (config_read_error)
        return (int)config_read_error;
    *out = gmch;
    return 0;
}
static int pci_write_config_word(struct pci_dev *p, int where, u16 value)
{
    CHECK(p == &pci && where == PSB_GMCH_CTRL);
    config_writes++;
    if (config_write_error)
        return (int)config_write_error;
    gmch = value;
    return 0;
}
static u32 register_read(struct drm_psb_private *p, u32 where)
{
    CHECK(p == &owner && where == PSB_PGETBL_CTL);
    mmio_reads++;
    return pgetbl;
}
static void register_write(struct drm_psb_private *p, u32 value, u32 where)
{
    CHECK(p == &owner && where == PSB_PGETBL_CTL);
    mmio_writes++;
    pgetbl = value;
}
#define PSB_RVDC32(where) register_read(dev_priv, (where))
#define PSB_WVDC32(value, where) register_write(dev_priv, (value), (where))
static void *ioremap(resource_size_t start, unsigned long bytes)
{
    size_t i;
    map_calls++;
    CHECK(start == (pgetbl & PAGE_MASK));
    CHECK(bytes && bytes / sizeof(u32) <= 262144);
    if (map_error)
        return NULL;
    capacity = bytes / sizeof(u32);
    allocation = malloc((capacity + 2) * sizeof(u32));
    CHECK(allocation);
    for (i = 0; i < capacity + 2; i++)
        allocation[i] = 0xdeadbeefU;
    return allocation + 1;
}
static void iounmap(void *ptr)
{
    CHECK(ptr == allocation + 1);
    unmaps++;
    free(allocation);
    allocation = NULL;
}
static void iowrite32(u32 value, u32 *where)
{
    CHECK(allocation && where >= allocation + 1 && where <= allocation + capacity);
    CHECK(!post_read);
    CHECK(table_locked || !expect_clear_lock);
    *where = value;
    last_index = (size_t)(where - (allocation + 1));
    writes++;
}
static u32 ioread32(const u32 *where)
{
    CHECK(allocation && where >= allocation + 1 && where <= allocation + capacity);
    CHECK(writes && (size_t)(where - (allocation + 1)) == last_index);
    CHECK(table_locked || !expect_clear_lock);
    final_read_index = last_index;
    reads++;
    post_read = true;
    return *where;
}

#include "gtt.current.inc"
#include "gem-stolen.current.inc"

static void reset(unsigned int aperture_mb, unsigned int storage_pages)
{
    CHECK(!allocation);
    memset(&owner, 0, sizeof(owner));
    memset(&pci, 0, sizeof(pci));
    owner.dev.dev = &pci.dev;
    owner.scratch_page = &scratch;
    pci.dev.pci = &pci;
    pci.drm = &owner.dev;
    pci.resource[PSB_GTT_RESOURCE].start = 0x30000000;
    pci.length[PSB_GTT_RESOURCE] = (resource_size_t)storage_pages << PAGE_SHIFT;
    pci.resource[PSB_GATT_RESOURCE].start = 0x40000000;
    pci.length[PSB_GATT_RESOURCE] = (resource_size_t)aperture_mb << 20;
    pci.resource[PSB_GATT_RESOURCE].end = 0x40000000 + pci.length[PSB_GATT_RESOURCE] - 1;
    gmch = 0x50;
    pgetbl = 0x1f000000U;
    capacity = writes = reads = map_calls = unmaps = 0;
    config_reads = config_writes = mmio_reads = mmio_writes = 0;
    config_read_error = config_write_error = map_error = 0;
    post_read = table_locked = false;
    expect_clear_lock = GTT_EXPECT_LOCK;
}
static void release_owner(void)
{
    psb_gtt_fini(&owner.dev);
    CHECK(unmaps == 1 && !allocation && !owner.gtt_mutex.initialized);
}
static void full_table(unsigned int aperture_mb, unsigned int storage_pages)
{
    size_t i, active;
    u32 expected;
    reset(aperture_mb, storage_pages);
    CHECK(psb_gtt_init(&owner.dev) == 0);
    CHECK(capacity == (size_t)storage_pages * PAGE_SIZE / sizeof(u32));
    active = (size_t)aperture_mb << (20 - PAGE_SHIFT);
    CHECK(writes == active && reads == 1 && final_read_index == active - 1);
    CHECK(!table_locked && !owner.gtt_mutex.held);
    expected = psb_gtt_mask_pte(scratch.pfn, PSB_MMU_CACHED_MEMORY);
    for (i = 0; i < active; i++)
        CHECK(owner.gtt_map[i] == expected);
    for (i = active; i < capacity; i++)
        CHECK(owner.gtt_map[i] == 0xdeadbeefU);
    CHECK(allocation[0] == 0xdeadbeefU && allocation[capacity + 1] == 0xdeadbeefU);
    printf("{\"aperture_bytes\":%llu,\"storage_pages\":%u,\"mapped_bytes\":%zu,"
           "\"table_words\":%zu,\"scratch_writes\":%zu,\"posted_reads\":%zu}\n",
           (unsigned long long)pci.length[PSB_GATT_RESOURCE], storage_pages, capacity * sizeof(u32),
           capacity, writes, reads);
    release_owner();
}
static void invalid_ranges(void)
{
    reset(256, 32); /* Table capacity is smaller than the selected aperture. */
    CHECK(psb_gtt_init(&owner.dev) == -EINVAL);
    CHECK(!map_calls && !writes && !reads && !owner.gtt_mutex.initialized);
    reset(128, 32);
    pci.length[PSB_GTT_RESOURCE] = ((resource_size_t)UINT_MAX + 1) << PAGE_SHIFT;
    CHECK(psb_gtt_init(&owner.dev) == -EINVAL);
    CHECK(!map_calls && !writes && !owner.gtt_mutex.initialized);
    reset(128, 32);
    pci.length[PSB_GATT_RESOURCE] = ((resource_size_t)UINT_MAX + 1) << PAGE_SHIFT;
    CHECK(psb_gtt_init(&owner.dev) == -EINVAL);
    CHECK(!map_calls && !writes && !owner.gtt_mutex.initialized);
#if ULONG_MAX == UINT_MAX
    reset(128, 32);
    pci.length[PSB_GTT_RESOURCE] = ((resource_size_t)(ULONG_MAX >> PAGE_SHIFT) + 1) << PAGE_SHIFT;
    CHECK(psb_gtt_init(&owner.dev) == -EINVAL);
    CHECK(!map_calls && !writes && !owner.gtt_mutex.initialized);
#endif
}
static void stolen_prefix_and_tail(void)
{
    size_t i, stolen_pages = 2048;
    u32 scratch_pte;
    reset(256, 64);
    CHECK(psb_gtt_init(&owner.dev) == 0);
    owner.stolen_base = 0x1e000000U;
    owner.vram_stolen_size = stolen_pages << PAGE_SHIFT;
    writes = reads = 0;
    post_read = false;
    expect_clear_lock = false; /* Actual GEM population is separately serialized. */
    psb_gem_mm_populate_stolen(&owner);
    CHECK(writes == stolen_pages && reads == 1 && final_read_index == stolen_pages - 1);
    for (i = 0; i < stolen_pages; i++)
        CHECK(owner.gtt_map[i] == psb_gtt_mask_pte((u32)(owner.stolen_base >> PAGE_SHIFT) + (u32)i,
                                                   PSB_MMU_CACHED_MEMORY));
    scratch_pte = psb_gtt_mask_pte(scratch.pfn, PSB_MMU_CACHED_MEMORY);
    for (i = stolen_pages; i < capacity; i++)
        CHECK(owner.gtt_map[i] == scratch_pte);
    release_owner();
}
static void fallback_ranges(void)
{
    size_t i;
    reset(0, 0);
    pci.resource[PSB_GTT_RESOURCE].start = 0;
    pci.resource[PSB_GATT_RESOURCE].start = 0;
    CHECK(psb_gtt_init(&owner.dev) == 0);
    CHECK(owner.gtt.gtt_pages == 64 && owner.gtt.gatt_pages == 32768);
    CHECK(capacity == 65536 && writes == 32768 && reads == 1 && final_read_index == 32767);
    for (i = 0; i < owner.gtt.gatt_pages; i++)
        CHECK(owner.gtt_map[i] == psb_gtt_mask_pte(scratch.pfn, PSB_MMU_CACHED_MEMORY));
    for (; i < capacity; i++)
        CHECK(owner.gtt_map[i] == 0xdeadbeefU);
    release_owner();
}
static void error_paths(void)
{
    reset(128, 32);
    map_error = 1;
    CHECK(psb_gtt_init(&owner.dev) == -ENOMEM);
    CHECK(map_calls == 1 && !writes && !reads && !owner.gtt_mutex.initialized);
    CHECK(config_writes == 2); /* enable followed by the existing disable path */
    reset(128, 32);
    config_read_error = 1;
    CHECK(psb_gtt_init(&owner.dev) == -EIO);
    CHECK(!map_calls && !writes && !config_writes && !owner.gtt_mutex.initialized);
    reset(128, 32);
    config_write_error = 1;
    CHECK(psb_gtt_init(&owner.dev) == -EIO);
    CHECK(!map_calls && !writes && config_writes == 1 && !owner.gtt_mutex.initialized);
}
static void resume_rebuild(void)
{
    size_t i;
    reset(256, 64);
    CHECK(psb_gtt_init(&owner.dev) == 0);
    /* These sentinel live bindings are deliberately discarded by the existing
	 * PM rebuild callsite, never by an ordinary per-object update.
	 */
    for (i = 0; i < capacity; i++)
        owner.gtt_map[i] = 0xbeef0001U;
    writes = reads = 0;
    post_read = false;
    CHECK(psb_gtt_resume(&owner.dev) == 0);
    CHECK(map_calls == 1 && writes == capacity && reads == 1);
    for (i = 0; i < capacity; i++)
        CHECK(owner.gtt_map[i] == psb_gtt_mask_pte(scratch.pfn, PSB_MMU_CACHED_MEMORY));
    /* Invalid new aperture extent must return before touching the old map. */
    pci.length[PSB_GATT_RESOURCE] <<= 1;
    writes = reads = 0;
    post_read = false;
    CHECK(psb_gtt_resume(&owner.dev) == -EINVAL);
    CHECK(!writes && !reads && map_calls == 1);
    release_owner();
}
static void reset_log(void)
{
    writes = reads = 0;
    post_read = false;
    gem_calls = restore_calls = irq_calls = power_calls = 0;
    gem_error = restore_error = irq_error = pci_error = 0;
    pm_busy = 0;
}
static int power_up(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    power_calls++;
    return 0;
}
static int restore_regs(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    CHECK(!expect_resume_order || (gem_calls == 1 && post_read));
    restore_calls++;
    return restore_error;
}
static const struct psb_test_ops ops = {power_up, restore_regs};
static struct drm_device *pci_get_drvdata(struct pci_dev *p)
{
    CHECK(p == &pci);
    return p->drm;
}
static int gma_resume_pci(struct pci_dev *p)
{
    CHECK(p == &pci);
    return pci_error;
}
static int psb_gem_mm_resume(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    CHECK(!expect_resume_order || (post_read && writes == owner.gtt.gatt_pages));
    gem_calls++;
    return gem_error;
}
static int gma_irq_install(struct drm_device *dev)
{
    CHECK(dev == &owner.dev && restore_calls == 1 && !restore_error);
    irq_calls++;
    return irq_error;
}
static bool poulsback_bootstrap_cold_dms_retained(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return pm_busy;
}
static void poulsback_bootstrap_observe_lifecycle(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
}
static bool psb_gem_retained(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return false;
}
static int sgx535_epoch_pm_begin(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return 0;
}
static bool sgx535_gma500_nativecopy_pm_busy(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return false;
}
static bool sgx535_gma500_twocopy_pm_busy(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return false;
}
static bool sgx535_gma500_ownedcopy_pm_busy(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return false;
}
static bool sgx535_gma500_ownedxor_pm_busy(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
    return false;
}
static void sgx535_gma500_nativecopy_lifecycle(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
}
static void sgx535_gma500_twocopy_lifecycle(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
}
static void sgx535_gma500_ownedcopy_lifecycle(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
}
static void sgx535_gma500_ownedxor_lifecycle(struct drm_device *dev)
{
    CHECK(dev == &owner.dev);
}
#include "power-resume.current.inc"

static void prepared(void)
{
    reset(128, 64); /* Active aperture smaller than mapped capacity. */
    CHECK(psb_gtt_init(&owner.dev) == 0);
    owner.ops = &ops;
    reset_log();
}
static void changed_geometry(unsigned int mode)
{
    struct psb_gtt saved;
    struct resource *saved_mem;
    prepared();
    saved = owner.gtt;
    saved_mem = owner.gtt_mem;
    switch (mode) {
    case 0:
        pgetbl += 0x100000U;
        break;
    case 1:
        pci.length[PSB_GTT_RESOURCE] <<= 1;
        break;
    case 2:
        pci.resource[PSB_GATT_RESOURCE].start += 0x100000U;
        break;
    case 3:
        pci.length[PSB_GATT_RESOURCE] <<= 1;
        break;
    case 4:
        pci.resource[PSB_GTT_RESOURCE].start += 0x100000U;
        break;
    default:
        abort();
    }
    CHECK(psb_gtt_resume(&owner.dev) == -ENODEV);
    CHECK(!writes && !reads && map_calls == 1);
    CHECK(!memcmp(&saved, &owner.gtt, sizeof(saved)) && owner.gtt_mem == saved_mem);
    release_owner();
}
static void caller_gtt_error(void)
{
    prepared();
    pci.length[PSB_GATT_RESOURCE] <<= 2; /* More active entries than mapped capacity. */
    CHECK(gma_power_resume(&pci.dev) == -EINVAL);
    CHECK(!writes && !reads && !gem_calls && !restore_calls && !irq_calls);
    release_owner();
}
static void caller_errors_and_success(void)
{
    prepared();
    gem_error = -EIO;
    CHECK(gma_power_resume(&pci.dev) == -EIO);
    CHECK(gem_calls == 1 && !restore_calls && !irq_calls && writes == 32768);
    release_owner();
    prepared();
    restore_error = -EFAULT;
    CHECK(gma_power_resume(&pci.dev) == -EFAULT);
    CHECK(gem_calls == 1 && restore_calls == 1 && !irq_calls);
    release_owner();
    prepared();
    CHECK(gma_power_resume(&pci.dev) == 0);
    CHECK(gem_calls == 1 && restore_calls == 1 && irq_calls == 1 && power_calls == 1);
    release_owner();
    prepared();
    pm_busy = 1;
    CHECK(gma_power_resume(&pci.dev) == -EBUSY);
    CHECK(!writes && !reads && !gem_calls && !restore_calls && !irq_calls && !power_calls);
    release_owner();
    prepared();
    pci_error = -EIO;
    CHECK(gma_power_resume(&pci.dev) == -EIO);
    CHECK(!writes && !reads && !gem_calls && !restore_calls && !irq_calls && !power_calls);
    release_owner();
}
static void caller_fallback(unsigned int mode)
{
    reset(128, 64);
    if (mode != 3) {
        pci.resource[PSB_GTT_RESOURCE].start = 0;
        pci.length[PSB_GTT_RESOURCE] = 0;
    }
    if (mode != 2) {
        pci.resource[PSB_GATT_RESOURCE].start = 0;
        pci.length[PSB_GATT_RESOURCE] = 0;
    }
    if (mode == 1)
        pgetbl |= _PSB_PGETBL_ENABLED;
    CHECK(psb_gtt_init(&owner.dev) == 0);
    owner.ops = &ops;
    if (mode != 3)
        CHECK(owner.gtt.gtt_start == owner.gtt.gtt_phys_start);
    reset_log();
    /* Actual power caller selects its saved root with ENABLE before the
	 * actual gtt_resume rereads it. A flag is not a changed physical base.
	 */
    CHECK(gma_power_resume(&pci.dev) == 0);
    CHECK(writes == 32768 && reads == 1 && final_read_index == 32767);
    CHECK(gem_calls == 1 && restore_calls == 1 && irq_calls == 1);
    if (mode != 3)
        CHECK(owner.gtt.gtt_start == owner.gtt.gtt_phys_start);
    release_owner();
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        expect_resume_order = false;
        if (!strcmp(argv[1], "--fallback-before"))
            caller_fallback(0);
        else if (!strcmp(argv[1], "--root-before"))
            changed_geometry(0);
        else {
            CHECK(!strcmp(argv[1], "--caller-before"));
            caller_gtt_error();
        }
        return 0;
    }
    CHECK(argc == 1);
    full_table(128, 32);
    full_table(256, 64);
    full_table(512, 128);
    fallback_ranges();
    stolen_prefix_and_tail();
    invalid_ranges();
    error_paths();
    resume_rebuild();
    for (unsigned int mode = 0; mode < 5; mode++)
        changed_geometry(mode);
    caller_gtt_error();
    caller_errors_and_success();
    for (unsigned int mode = 0; mode < 4; mode++)
        caller_fallback(mode);
    printf(
        "{\"scope\":\"RAM_SOURCE_BOUND_GTT_RESUME_IDENTITY_CALLER_ERRORS_AND_FALLBACK_PHYSICAL_BASE\",\"checks\":%u,"
        "\"unsigned_long_bits\":%zu,\"hardware\":false,\"SGX\":false}\n",
        checks, sizeof(unsigned long) * CHAR_BIT);
    return 0;
}
