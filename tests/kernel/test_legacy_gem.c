// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Exact compiled GEM bodies; explicit CPU policy mocks, no MMIO/PAT/page table.
 * Caller/module, full-owner retention and cleanup order are separate witnesses. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

typedef uint32_t u32;
struct page {
    unsigned int identity;
};
struct kref {
    unsigned int refs;
};
struct resource {
    bool live;
};
struct drm_psb_private {
    struct {
        u32 gatt_start;
    } gtt;
    void *mmu;
};
struct drm_device {
    void *dev;
    struct drm_psb_private *private;
    unsigned int refs;
};
struct drm_gem_object {
    struct drm_device *dev;
    void *resv;
    struct kref refcount;
    size_t size;
};
struct list_head {
    struct list_head *next, *prev;
};
struct psb_gem_object {
    struct drm_gem_object base;
    struct resource resource;
    unsigned long offset;
    int in_gart;
    bool stolen, mmapping;
    struct page **pages;
    struct list_head poulsback_hold_link;
    bool poulsback_held, poulsback_module_ref;
    bool poulsback_pages_module_ref, poulsback_freeing, poulsback_put_after_free;
    struct psb_gem_display_pin *poulsback_display_pins[2];
};
#define LIST_HEAD(name) struct list_head name = {&name, &name}
#define DEFINE_MUTEX(name) int name
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define list_for_each_entry(p, h, m)                                                               \
    for ((p) = container_of((h)->next, __typeof__(*(p)), m); &(p)->m != (h);                       \
         (p) = container_of((p)->m.next, __typeof__(*(p)), m))
static void list_add_tail(struct list_head *p, struct list_head *head)
{
    p->prev = head->prev;
    p->next = head;
    head->prev->next = p;
    head->prev = p;
}
static void mutex_lock(int *p)
{
    assert(!*p);
    *p = 1;
}
static void mutex_unlock(int *p)
{
    assert(*p);
    *p = 0;
}

static struct {
    unsigned int refs, gets, puts, pages_get, pages_put, wc, wb;
    unsigned int gtt_insert, gtt_remove, gem_release, resource_release, frees, kref_get;
    unsigned int losses, warnings, begins, ends;
    bool going, external, outer_free, ever_published, race_hold, resv_locked;
    bool delayed_unpin;
    int begin_error, resv_error, pages_error, wc_error, wb_error;
    struct psb_gem_object *owner;
} model;
static unsigned int checks, scenarios;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        assert(x);                                                                                 \
    } while (0)
#define PAGE_SIZE 4096U
#define PSB_MMU_CACHED_MEMORY 1
#define SGX535_EPOCH_FAULT 8
#define THIS_MODULE ((void *)1)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define WARN_ON(x) ((x) ? (++model.warnings, true) : false)
#define drm_WARN_ONCE(d, x, ...) WARN_ON(x)
/* Arguments are type-checked and read, never printed. */
#define dev_err(d, ...) ((void)(d), (void)sizeof(printf(__VA_ARGS__)))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define dma_resv_assert_held(r) assert(model.resv_locked)
static struct drm_psb_private *to_drm_psb_private(struct drm_device *d)
{
    return d->private;
}
static struct psb_gem_object *to_psb_gem_object(struct drm_gem_object *o)
{
    return container_of(o, struct psb_gem_object, base);
}
static bool try_module_get(void *p)
{
    (void)p;
    model.gets++;
    if (model.going)
        return false;
    model.refs++;
    return true;
}
static void module_put(void *p)
{
    (void)p;
    assert(model.refs);
    if (model.outer_free && model.ever_published)
        assert(model.frees);
    model.refs--;
    model.puts++;
}
static void drm_dev_get(struct drm_device *d)
{
    d->refs++;
}
static bool kref_get_unless_zero(struct kref *k)
{
    model.kref_get++;
    if (!k->refs)
        return false;
    k->refs++;
    return true;
}
static void sgx535_epoch_lost(struct drm_device *d, unsigned int reason)
{
    (void)d;
    assert(reason == SGX535_EPOCH_FAULT);
    model.losses++;
}
static int poulsback_gem_hold(struct psb_gem_object *, int);
void psb_gem_unpin(struct psb_gem_object *);
static int sgx535_epoch_external_begin(struct drm_device *d)
{
    (void)d;
    model.begins++;
    if (model.begin_error)
        return model.begin_error;
    assert(!model.external);
    model.external = true;
    if (model.race_hold) {
        model.race_hold = false;
        (void)poulsback_gem_hold(model.owner, -ECANCELED);
    }
    return 0;
}
static int sgx535_epoch_external_end(struct drm_device *d)
{
    (void)d;
    assert(model.external);
    model.external = false;
    model.ends++;
    return 0;
}
static int dma_resv_lock(void *r, void *context)
{
    (void)r;
    (void)context;
    assert(!model.resv_locked);
    if (model.delayed_unpin) {
        int error = model.begin_error;
        model.delayed_unpin = false;
        model.begin_error = 0;
        psb_gem_unpin(model.owner); /* Another valid pin retires before HOLD. */
        model.begin_error = error;
    }
    if (model.resv_error)
        return model.resv_error;
    model.resv_locked = true;
    return 0;
}
static void dma_resv_unlock(void *r)
{
    (void)r;
    assert(model.resv_locked);
    model.resv_locked = false;
}
static struct page **drm_gem_get_pages(struct drm_gem_object *o)
{
    struct page **p;
    size_t count = o->size / PAGE_SIZE;
    assert(model.refs && model.external);
    model.pages_get++;
    if (model.pages_error)
        return (void *)(intptr_t)model.pages_error;
    p = calloc(count, sizeof(*p));
    assert(p);
    for (size_t i = 0; i < count; i++) {
        p[i] = calloc(1, sizeof(**p));
        assert(p[i]);
        p[i]->identity = (unsigned int)i + 1;
    }
    return p;
}
static void drm_gem_put_pages(struct drm_gem_object *o, struct page **p, bool dirty, bool accessed)
{
    (void)dirty;
    (void)accessed;
    assert(model.refs && p && model.external);
    model.pages_put++;
    for (size_t i = 0; i < o->size / PAGE_SIZE; i++)
        free(p[i]);
    free(p);
}
static int set_pages_array_wc(struct page **p, unsigned int n)
{
    assert(p && n && model.refs && model.external && model.owner->poulsback_pages_module_ref);
    model.ever_published = true;
    model.wc++;
    return model.wc_error;
}
static int set_pages_array_wb(struct page **p, unsigned int n)
{
    assert(p && n && model.refs && model.external && model.owner->poulsback_pages_module_ref);
    model.wb++;
    return model.wb_error;
}
static void psb_gtt_insert_pages(struct drm_psb_private *d, struct resource *r, struct page **p)
{
    (void)d;
    assert(r->live && p && model.refs && model.external);
    model.gtt_insert++;
}
static void psb_gtt_remove_pages(struct drm_psb_private *d, struct resource *r)
{
    (void)d;
    assert(r->live && model.refs && model.external);
    model.gtt_remove++;
}
/* No scenario creates a display pin; the pin owners are tested elsewhere. */
static int psb_gem_display_reserved(struct psb_gem_object *p)
{
    return !!p->poulsback_display_pins[0] + !!p->poulsback_display_pins[1];
}
static void drm_gem_object_release(struct drm_gem_object *o)
{
    (void)o;
    if (model.ever_published)
        assert(model.refs);
    model.gem_release++;
}
static void release_resource(struct resource *r)
{
    assert(r->live);
    if (model.ever_published)
        assert(model.refs);
    r->live = false;
    model.resource_release++;
}
static void mock_kfree(struct psb_gem_object *p)
{
    if (model.ever_published)
        assert(model.refs);
    model.frees++;
    free(p);
}
#define kfree(p) mock_kfree(p)
#include "poulsback_gem_hold.inc"
#include "gem-entrypoints.inc"

static struct drm_psb_private private;
static struct drm_device device;
static struct psb_gem_object *new_owner(bool stolen)
{
    struct psb_gem_object *p;
    model = (__typeof__(model)){.refs = 1};
    device = (struct drm_device){.dev = &device, .private = &private, .refs = 1};
    poulsback_gem_holds.next = poulsback_gem_holds.prev = &poulsback_gem_holds;
    scenarios++;
    p = calloc(1, sizeof(*p));
    assert(p);
    p->base = (struct drm_gem_object){.dev = &device, .size = 8192, .refcount = {1}};
    p->resource.live = true;
    p->stolen = stolen;
    p->in_gart = stolen ? 1 : 0;
    p->poulsback_hold_link.next = p->poulsback_hold_link.prev = &p->poulsback_hold_link;
    model.owner = p;
    return p;
}
static void final_free(struct psb_gem_object *p)
{
    p->base.refcount.refs = 0;
    model.outer_free = true;
    psb_gem_free_object(&p->base);
    model.outer_free = false;
}
static void discard_cpu_hold(struct psb_gem_object *p)
{
    /* Explicit heap/mock state only; never a physical cleanup operation. */
    if (p->pages) {
        for (size_t i = 0; i < p->base.size / PAGE_SIZE; i++)
            free(p->pages[i]);
        free(p->pages);
    }
    free(p);
    poulsback_gem_holds.next = poulsback_gem_holds.prev = &poulsback_gem_holds;
}
static void check_held(struct psb_gem_object *p)
{
    CHECK(p->poulsback_held && p->poulsback_module_ref && !p->poulsback_pages_module_ref);
    CHECK(p->resource.live && psb_gem_retained(&device) && device.refs == 2);
    CHECK(model.refs == 2 && model.losses && !model.pages_put && !model.resource_release &&
          !model.frees);
}
int main(void)
{
    struct psb_gem_object *p = new_owner(false);
    CHECK(!psb_gem_pin(p) && p->in_gart == 1 && model.refs == 2 && p->poulsback_pages_module_ref);
    CHECK(!psb_gem_pin(p) && p->in_gart == 2 && model.gets == 1);
    psb_gem_unpin(p);
    CHECK(p->in_gart == 1 && !model.pages_put && model.refs == 2);
    psb_gem_unpin(p);
    CHECK(!p->pages && !p->poulsback_pages_module_ref && model.refs == 1);
    CHECK(model.wc == 1 && model.wb == 1 && model.pages_put == 1);
    CHECK(model.gtt_insert == 1 && model.gtt_remove == 1);
    final_free(p);
    CHECK(model.frees == 1 && model.resource_release == 1 && model.gem_release == 1);

    p = new_owner(false);
    model.going = true;
    model.refs = 0;
    CHECK(psb_gem_pin(p) == -ENODEV && !p->pages && !p->poulsback_pages_module_ref);
    CHECK(!model.pages_get && !model.wc && !model.gtt_insert && !model.losses);
    final_free(p);
    CHECK(model.frees == 1 && !model.puts);

    p = new_owner(false);
    model.pages_error = -ENOMEM;
    CHECK(psb_gem_pin(p) == -ENOMEM && model.refs == 1 && model.puts == 1 && !p->poulsback_held);
    CHECK(!model.wc && !model.gtt_insert && !p->poulsback_pages_module_ref);
    final_free(p);

    p = new_owner(false);
    model.wc_error = -EIO;
    CHECK(psb_gem_pin(p) == -EIO);
    check_held(p);
    CHECK(model.gets == 1 && !model.gtt_insert);
    final_free(p);
    CHECK(!model.frees && !model.gem_release);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    model.wb_error = -EIO;
    psb_gem_unpin(p);
    check_held(p);
    CHECK(model.gets == 1 && model.gtt_remove == 1 && model.wb == 1);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    model.begin_error = -EBUSY;
    psb_gem_unpin(p);
    check_held(p);
    CHECK(model.gets == 1 && !model.gtt_remove && !model.wb);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    model.resv_error = -EDEADLK;
    psb_gem_unpin(p);
    /* Explicit unexpected-lock-error model, not a reachable NULL-context
     * ww-mutex failure claim: retain both the untouched page ref and a new
     * independent full-object ref, with no cache/PTE cleanup. */
    CHECK(p->poulsback_held && p->poulsback_module_ref && p->poulsback_pages_module_ref);
    CHECK(model.refs == 3 && model.gets == 2 && !model.gtt_remove && !model.wb && !model.external);
    final_free(p);
    CHECK(!model.frees && !model.pages_put && p->resource.live);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    p->mmapping = true;
    final_free(p);
    CHECK(model.refs == 1 && model.puts == 1 && model.pages_put == 1);
    CHECK(model.frees == 1 && model.resource_release == 1 && model.gem_release == 1 &&
          !model.kref_get);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    final_free(p);
    check_held(p);
    CHECK(!p->base.refcount.refs && model.kref_get == 1);
    CHECK(!model.frees && !model.gem_release);
    discard_cpu_hold(p);

    p = new_owner(true);
    model.going = true;
    model.refs = 0;
    psb_gem_unpin(p);
    CHECK(!p->in_gart && !model.gets && !model.wc && !model.gtt_remove);
    final_free(p);
    CHECK(model.frees == 1 && model.resource_release == 1 && !model.puts);

    p = new_owner(true);
    model.begin_error = -EBUSY;
    psb_gem_unpin(p);
    check_held(p);
    CHECK(model.gets == 1 && !model.wc && !model.gtt_remove);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p));
    model.race_hold = true;
    CHECK(psb_gem_pin(p) == -EBUSY);
    check_held(p);
    CHECK(model.gets == 1 && model.wc == 1 && model.gtt_insert == 1);
    discard_cpu_hold(p);

    p = new_owner(false);
    CHECK(!psb_gem_pin(p) && !psb_gem_pin(p));
    model.begin_error = -EBUSY;
    model.delayed_unpin = true;
    psb_gem_unpin(p); /* Failed admission, valid other unpin, locked latch. */
    CHECK(p->in_gart == 1 && !model.delayed_unpin && !model.resv_locked);
    check_held(p);
    CHECK(!model.gtt_remove && !model.wb && model.gets == 1);
    CHECK(psb_gem_pin(p) == -EBUSY && p->in_gart == 1);
    final_free(p);
    CHECK(!model.frees && !model.pages_put && p->resource.live);
    discard_cpu_hold(p);
    printf(
        "PASS exact production GEM: %u checks / %u CPU scenarios; no hardware or module-manager proof\n",
        checks, scenarios);
    return 0;
}
