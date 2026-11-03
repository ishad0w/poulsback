// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Buffer storage (poulsback_slab.h) on fake BOs: placement, zeroing, slab
 * reuse, the empty-slab bound, busy storage, renames and their failure, and a
 * seeded random run against a byte model of every buffer, including storage
 * kept for jobs. Every BO the allocator took is freed at the end.
 *
 * usage: poulsback-slab */
#include "poulsback_slab.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define SLAB POULSBACK_SLAB_BYTES
#define ALIGN POULSBACK_SLAB_ALIGN

/* ---- fake BOs: zeroed host memory at disjoint fake GPU addresses ---- */

#define FAKES 256
static struct fake {
    uint64_t va, size;
    uint8_t *bytes;
    bool live;
} fakes[FAKES];
static unsigned fake_live, fake_allocs, fake_frees;
static int fail_alloc; /* The next alloc returns it. */
static uint64_t next_va = 0x10000000u;

static int fake_alloc(void *data, uint64_t bytes, struct poulsback_slab_bo *out)
{
    unsigned i;
    CHECK(data == fakes && bytes && !(bytes % 4096u));
    if (fail_alloc) {
        int ret = fail_alloc;
        fail_alloc = 0;
        return ret;
    }
    for (i = 0; i < FAKES && fakes[i].live; i++) {
    }
    CHECK(i < FAKES);
    fakes[i] = (struct fake){next_va, bytes, calloc(1, (size_t)bytes), true};
    CHECK(fakes[i].bytes);
    next_va += bytes + SLAB;
    fake_live++;
    fake_allocs++;
    *out = (struct poulsback_slab_bo){i + 1u, fakes[i].va, bytes, fakes[i].bytes};
    return 0;
}
static void fake_free(void *data, struct poulsback_slab_bo *bo)
{
    struct fake *f;
    CHECK(data == fakes && bo->handle && bo->handle <= FAKES);
    f = &fakes[bo->handle - 1u];
    CHECK(f->live && bo->map == f->bytes && bo->size == f->size && bo->gpu_va == f->va);
    memset(f->bytes, 0xa5, (size_t)f->size); /* A use after free reads poison. */
    free(f->bytes);
    f->bytes = NULL;
    f->live = false;
    fake_live--;
    fake_frees++;
}
static const struct poulsback_slab_ops ops = {fakes, fake_alloc, fake_free};

/* Each case starts without a BO and counts its own. */
static struct poulsback_slab *create(void)
{
    struct poulsback_slab *s;
    CHECK(!fake_live);
    fake_allocs = fake_frees = 0;
    CHECK(!poulsback_slab_create(&ops, &s));
    return s;
}
static void destroy(struct poulsback_slab *s)
{
    CHECK(!poulsback_slab_destroy(s));
    CHECK(!fake_live && fake_allocs == fake_frees);
}
static struct fake *bo_of(const struct poulsback_slab_entry *e)
{
    CHECK(e->handle && e->handle <= FAKES && fakes[e->handle - 1u].live);
    return &fakes[e->handle - 1u];
}
/* E describes BYTES of its BO, consistently. */
static void described(const struct poulsback_slab_entry *e, uint64_t bytes)
{
    const struct fake *f = bo_of(e);
    CHECK(e->size == bytes && !(e->offset % ALIGN) && e->offset + bytes <= f->size);
    CHECK(e->cpu == f->bytes + e->offset && e->gpu_va == f->va + e->offset);
}
static struct poulsback_slab_entry *alloc(struct poulsback_slab *s, uint32_t bytes)
{
    struct poulsback_slab_entry *e = NULL;
    CHECK(!poulsback_slab_alloc(s, bytes, &e) && e);
    described(e, bytes);
    return e;
}
static bool filled(const uint8_t *p, uint64_t bytes, uint8_t value)
{
    for (uint64_t i = 0; i < bytes; i++)
        if (p[i] != value)
            return false;
    return true;
}
static uint64_t round_up(uint64_t bytes, uint64_t to)
{
    return (bytes + to - 1u) / to * to;
}

/* ---- fixed cases ---- */

static void placement(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *a = alloc(s, 1), *b = alloc(s, 300), *c = alloc(s, 256), *big,
                                *whole, *d;
    /* First fit at the alignment, in one slab. */
    CHECK(fake_live == 1 && a->handle == b->handle && b->handle == c->handle);
    CHECK(bo_of(a)->size == SLAB && !a->offset && b->offset == ALIGN && c->offset == 3u * ALIGN);
    CHECK(filled(a->cpu, 1, 0) && filled(b->cpu, 300, 0) && filled(c->cpu, 256, 0));
    /* Larger than a slab: its own BO, in pages. */
    big = alloc(s, SLAB + 1u);
    CHECK(fake_live == 2 && !big->offset && bo_of(big)->size == SLAB + 4096u);
    /* A whole slab does not fit beside a: a new slab. */
    whole = alloc(s, SLAB);
    CHECK(fake_live == 3 && !whole->offset && bo_of(whole)->size == SLAB);
    /* Freed space is reused first fit; reused bytes are zero again. */
    memset(b->cpu, 0x5a, 300);
    poulsback_slab_free(s, b);
    d = alloc(s, 2u * ALIGN);
    CHECK(d->handle == a->handle && d->offset == ALIGN && filled(d->cpu, 2u * ALIGN, 0));
    /* A large BO goes with its buffer; emptied slabs stay (two). */
    poulsback_slab_free(s, big);
    CHECK(fake_live == 2);
    poulsback_slab_free(s, a);
    poulsback_slab_free(s, c);
    poulsback_slab_free(s, d);
    poulsback_slab_free(s, whole);
    CHECK(fake_live == 2);
    destroy(s);
}

static uint64_t seed = 0x9e3779b97f4a7c15u;
static uint32_t random32(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (uint32_t)(seed >> 16);
}

/* Fill a slab with buffers of random sizes and free them in random order:
 * every byte of space returns, so a whole slab then fits in it again. */
static void reuse(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *e[SLAB / ALIGN], *one;
    unsigned n = 0;
    uint64_t used = 0;
    uint32_t handle = 0;
    while (used < SLAB) {
        uint32_t bytes = 1u + random32() % 6000u;
        if (used + round_up(bytes, ALIGN) > SLAB)
            bytes = (uint32_t)(SLAB - used);
        e[n] = alloc(s, bytes);
        CHECK(!n || e[n]->handle == handle);
        CHECK(e[n]->offset == used); /* Packed: no space is lost to placement. */
        handle = e[n]->handle;
        memset(e[n]->cpu, 0x77, bytes);
        used += round_up(bytes, ALIGN);
        n++;
    }
    CHECK(fake_live == 1 && n > 100);
    for (unsigned i = n; i > 1; i--) {
        unsigned j = random32() % i;
        struct poulsback_slab_entry *t = e[i - 1u];
        e[i - 1u] = e[j];
        e[j] = t;
    }
    for (unsigned i = 0; i < n; i++)
        poulsback_slab_free(s, e[i]);
    CHECK(fake_live == 1);
    one = alloc(s, SLAB);
    CHECK(fake_allocs == 1 && one->handle == handle && !one->offset && filled(one->cpu, SLAB, 0));
    poulsback_slab_free(s, one);
    destroy(s);
}

/* A slab that holds buffers is filled before an empty one; at most
 * POULSBACK_SLAB_KEEP empty slabs stay. */
static void retention(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *w = alloc(s, SLAB), *y = alloc(s, 1000), *z, *e[5], *big;
    CHECK(fake_live == 2 && y->handle != w->handle);
    poulsback_slab_free(s, w);
    z = alloc(s, 1000);
    CHECK(z->handle == y->handle && fake_live == 2);
    w = alloc(s, SLAB); /* The empty slab, no new BO. */
    CHECK(fake_allocs == 2 && w->handle != y->handle);
    poulsback_slab_free(s, w);
    poulsback_slab_free(s, y);
    poulsback_slab_free(s, z);
    CHECK(fake_live == 2);
    for (unsigned i = 0; i < 5; i++)
        e[i] = alloc(s, SLAB);
    CHECK(fake_live == 5 && fake_allocs == 5);
    for (unsigned i = 0; i < 5; i++)
        poulsback_slab_free(s, e[i]);
    CHECK(fake_live == POULSBACK_SLAB_KEEP);
    big = alloc(s, 3u * SLAB);
    CHECK(fake_live == POULSBACK_SLAB_KEEP + 1u);
    poulsback_slab_free(s, big);
    CHECK(fake_live == POULSBACK_SLAB_KEEP);
    destroy(s);
}

/* Busy until every job up to the last use completed; freed busy storage
 * keeps its space and bytes until then, and blocks destruction. */
static void busy(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *e = alloc(s, 4096), *f, *g, *big;
    uint64_t offset = e->offset;
    uint8_t *bytes = e->cpu;
    CHECK(!poulsback_slab_busy(s, e));
    memset(e->cpu, 0x3c, 4096);
    poulsback_slab_use(e, 3);
    CHECK(poulsback_slab_busy(s, e) && e->last_use == 3);
    poulsback_slab_use(e, 2); /* An older job changes nothing. */
    CHECK(e->last_use == 3);
    poulsback_slab_complete(s, 2);
    CHECK(poulsback_slab_busy(s, e));
    poulsback_slab_complete(s, 3);
    CHECK(!poulsback_slab_busy(s, e));
    poulsback_slab_complete(s, 1);
    CHECK(!poulsback_slab_busy(s, e));
    poulsback_slab_use(e, 3);
    CHECK(!poulsback_slab_busy(s, e));
    poulsback_slab_use(e, 5);
    poulsback_slab_free(s, e);
    f = alloc(s, 4096);
    CHECK(f->offset >= offset + 4096u && filled(bytes, 4096, 0x3c));
    poulsback_slab_free(s, f);
    CHECK(poulsback_slab_destroy(s) == -EBUSY && fake_live == 1);
    poulsback_slab_complete(s, 4);
    CHECK(filled(bytes, 4096, 0x3c));
    g = alloc(s, 4096);
    CHECK(g->offset != offset);
    poulsback_slab_free(s, g);
    poulsback_slab_complete(s, 5);
    g = alloc(s, 4096); /* First fit: the space came back. */
    CHECK(g->offset == offset && filled(g->cpu, 4096, 0));
    poulsback_slab_free(s, g);
    /* A busy large BO stays until its job completes. */
    big = alloc(s, 3u * SLAB);
    poulsback_slab_use(big, 6);
    poulsback_slab_free(s, big);
    CHECK(fake_live == 2);
    poulsback_slab_complete(s, 6);
    CHECK(fake_live == 1);
    destroy(s);
}

static void rename_cases(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *e = alloc(s, 10000), *old, *rest;
    uint8_t *old_bytes;
    unsigned allocs;
    memset(e->cpu, 0x11, 10000);
    /* Idle storage is written in place. */
    old = e;
    CHECK(!poulsback_slab_rename(s, &e, 100, 50) && e == old && fake_allocs == 1);
    /* A range write into busy storage: new storage with every other byte;
     * the old storage stays readable until its job completes. */
    poulsback_slab_use(e, 7);
    old_bytes = e->cpu;
    CHECK(!poulsback_slab_rename(s, &e, 100, 50) && e != old);
    described(e, 10000);
    CHECK(!poulsback_slab_busy(s, e) && e->handle == old->handle);
    CHECK(filled(e->cpu, 100, 0x11) && filled(e->cpu + 150, 10000 - 150, 0x11));
    CHECK(filled(old_bytes, 10000, 0x11));
    /* A write that reaches the end, and a whole discard. */
    memset(e->cpu, 0x22, 10000);
    poulsback_slab_use(e, 8);
    CHECK(!poulsback_slab_rename(s, &e, 9990, 10) && filled(e->cpu, 9990, 0x22));
    poulsback_slab_use(e, 8);
    old = e;
    CHECK(!poulsback_slab_rename(s, &e, 0, 10000) && e != old);
    /* The three old storages and e fill the slab from 0; the rest is taken,
     * so a rename needs a new BO. When that fails the busy storage stays. */
    CHECK(fake_live == 1 && e->offset == 3u * round_up(10000, ALIGN));
    rest = alloc(s, (uint32_t)(SLAB - e->offset - round_up(10000, ALIGN)));
    CHECK(rest->handle == e->handle);
    memset(e->cpu, 0x33, 10000);
    poulsback_slab_use(e, 9);
    old = e;
    allocs = fake_allocs;
    fail_alloc = -ENOMEM;
    CHECK(poulsback_slab_rename(s, &e, 0, 1) == -ENOMEM && e == old);
    CHECK(fake_allocs == allocs && fake_live == 1);
    CHECK(poulsback_slab_busy(s, e) && filled(e->cpu, 10000, 0x33));
    /* With BOs to be had, the rename takes a new slab. */
    CHECK(!poulsback_slab_rename(s, &e, 0, 1) && e != old && fake_live == 2);
    CHECK(filled(e->cpu + 1, 9999, 0x33));
    /* Everything kept for jobs 7 to 9 goes once they completed: the first
     * slab then holds rest alone. */
    poulsback_slab_complete(s, 9);
    poulsback_slab_free(s, e);
    e = alloc(s, (uint32_t)(4u * round_up(10000, ALIGN)));
    CHECK(e->handle == rest->handle && !e->offset);
    poulsback_slab_free(s, e);
    poulsback_slab_free(s, rest);
    CHECK(fake_live == 2);
    destroy(s);
}

/* A failed BO allocation changes nothing. */
static void failure(void)
{
    struct poulsback_slab *s = create();
    struct poulsback_slab_entry *e = NULL;
    fail_alloc = -ENOSPC;
    CHECK(poulsback_slab_alloc(s, 100, &e) == -ENOSPC && !e && !fake_live);
    fail_alloc = -ENOMEM;
    CHECK(poulsback_slab_alloc(s, 2u * SLAB, &e) == -ENOMEM && !e && !fake_live);
    e = alloc(s, 100);
    poulsback_slab_free(s, e);
    destroy(s);
}

/* ---- random run against a byte model ---- */

#define BUFFERS 160
#define KEPT 512
struct model {
    struct poulsback_slab_entry *e;
    uint8_t *bytes; /* What the entry must hold. */
    uint64_t last_use;
};
static struct model live[BUFFERS], kept[KEPT];
static unsigned live_count, kept_count;

static uint32_t random_size(void)
{
    uint32_t r = random32() % 100u;
    if (r < 70)
        return 1u + random32() % 4096u;
    if (r < 95)
        return 1u + random32() % (SLAB / 2u);
    return 1u + random32() % (3u * SLAB);
}
/* No two pieces of storage the allocator holds overlap. */
static void disjoint(const struct poulsback_slab_entry *e)
{
    const struct model *sets[2] = {live, kept};
    unsigned counts[2] = {live_count, kept_count};
    for (unsigned k = 0; k < 2; k++)
        for (unsigned i = 0; i < counts[k]; i++) {
            const struct poulsback_slab_entry *o = sets[k][i].e;
            if (o == e || o->handle != e->handle)
                continue;
            CHECK(o->offset + o->size <= e->offset || e->offset + e->size <= o->offset);
        }
}
static void verify(void)
{
    for (unsigned i = 0; i < live_count; i++)
        CHECK(!memcmp(live[i].e->cpu, live[i].bytes, (size_t)live[i].e->size));
    for (unsigned i = 0; i < kept_count; i++)
        CHECK(!memcmp(kept[i].e->cpu, kept[i].bytes, (size_t)kept[i].e->size));
}
static void keep(struct model *m)
{
    CHECK(kept_count < KEPT);
    kept[kept_count++] = *m;
}
static void random_run(void)
{
    struct poulsback_slab *s = create();
    uint64_t submitted = 0, completed = 0;
    unsigned renames = 0, kept_max = 0, allocations = 0;
    for (unsigned step = 0; step < 40000; step++) {
        uint32_t op = random32() % 100u;
        struct model *m = live_count ? &live[random32() % live_count] : NULL;
        if (op < 20 && live_count < BUFFERS) {
            uint32_t bytes = random_size();
            m = &live[live_count++];
            m->e = alloc(s, bytes);
            CHECK(filled(m->e->cpu, bytes, 0));
            m->bytes = calloc(1, bytes);
            CHECK(m->bytes);
            m->last_use = 0;
            disjoint(m->e);
            allocations++;
        } else if (op < 45 && m) {
            /* A write through a rename, as the resource domain does it. */
            struct poulsback_slab_entry *before = m->e;
            uint64_t offset = random32() % m->e->size;
            uint64_t bytes = 1u + random32() % (m->e->size - offset);
            uint8_t value = (uint8_t)random32();
            bool was_busy = poulsback_slab_busy(s, m->e);
            if (was_busy) {
                struct model old = {before, malloc((size_t)before->size), m->last_use};
                CHECK(old.bytes);
                memcpy(old.bytes, m->bytes, (size_t)before->size);
                keep(&old);
            }
            CHECK(!poulsback_slab_rename(s, &m->e, offset, bytes));
            CHECK((m->e != before) == was_busy && !poulsback_slab_busy(s, m->e));
            if (was_busy) {
                described(m->e, before->size);
                CHECK(!memcmp(m->e->cpu, m->bytes, (size_t)offset) &&
                      !memcmp(m->e->cpu + offset + bytes, m->bytes + offset + bytes,
                              (size_t)(m->e->size - offset - bytes)));
                disjoint(m->e);
                m->last_use = 0;
                renames++;
            }
            memset(m->e->cpu + offset, value, (size_t)bytes);
            memset(m->bytes + offset, value, (size_t)bytes);
        } else if (op < 65 && m) {
            /* The open frame reads it. */
            poulsback_slab_use(m->e, submitted + 1u);
            m->last_use = submitted + 1u;
        } else if (op < 80 && m) {
            if (poulsback_slab_busy(s, m->e))
                keep(m);
            else
                free(m->bytes);
            CHECK(poulsback_slab_busy(s, m->e) == (m->last_use > completed));
            poulsback_slab_free(s, m->e);
            *m = live[--live_count];
        } else if (op < 95) {
            submitted++;
        } else if (submitted > completed) {
            completed += 1u + random32() % (unsigned)(submitted - completed);
            verify();
            poulsback_slab_complete(s, completed);
            for (unsigned i = 0; i < kept_count;)
                if (kept[i].last_use <= completed) {
                    free(kept[i].bytes);
                    kept[i] = kept[--kept_count];
                } else
                    i++;
        }
        if (kept_count > kept_max)
            kept_max = kept_count;
        if (!(step % 256))
            verify();
    }
    verify();
    while (live_count) {
        free(live[--live_count].bytes);
        poulsback_slab_free(s, live[live_count].e);
    }
    if (kept_count)
        CHECK(poulsback_slab_destroy(s) == -EBUSY);
    poulsback_slab_complete(s, submitted + 1u);
    while (kept_count)
        free(kept[--kept_count].bytes);
    CHECK(fake_live <= POULSBACK_SLAB_KEEP);
    destroy(s);
    printf("poulsback-slab: random run: %u allocations, %u renames, %u kept at most, %u BOs\n",
           allocations, renames, kept_max, fake_allocs);
}

int main(void)
{
    placement();
    reuse();
    retention();
    busy();
    rename_cases();
    failure();
    random_run();
    printf("poulsback-slab: placement, reuse, retention, busy, rename, failure, random run "
           "PASS\n");
    return 0;
}
