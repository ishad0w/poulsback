// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_slab.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u

/* A slab, or a large buffer's own BO. */
struct chunk {
    struct chunk *next;
    struct poulsback_slab_bo bo;
    struct entry *entries; /* By offset: the owner's and the kept ones. */
    uint64_t clean;        /* No byte from here on was handed out: still zero. */
    bool large;
};
struct entry {
    struct poulsback_slab_entry base;
    struct chunk *chunk;
    struct entry *next; /* In the chunk. */
    struct entry *kept; /* In the allocator's kept list. */
};
struct poulsback_slab {
    struct poulsback_slab_ops ops;
    struct chunk *slabs, *large;
    struct entry *kept; /* Freed while busy. */
    uint64_t completed;
    unsigned live, empty; /* Entries the owner holds; slabs without an entry. */
};

static uint64_t extent(uint64_t bytes)
{
    return (bytes + POULSBACK_SLAB_ALIGN - 1u) & ~(uint64_t)(POULSBACK_SLAB_ALIGN - 1u);
}
int poulsback_slab_create(const struct poulsback_slab_ops *ops, struct poulsback_slab **out)
{
    struct poulsback_slab *s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->ops = *ops;
    *out = s;
    return 0;
}
/* Appended: older slabs fill first. */
static struct chunk *new_chunk(struct poulsback_slab *s, uint64_t bytes, bool large, int *ret)
{
    struct chunk *c = calloc(1, sizeof(*c)), **list = large ? &s->large : &s->slabs;
    if (!c) {
        *ret = -ENOMEM;
        return NULL;
    }
    *ret = s->ops.alloc(s->ops.data, bytes, &c->bo);
    if (*ret) {
        free(c);
        return NULL;
    }
    c->large = large;
    while (*list)
        list = &(*list)->next;
    *list = c;
    return c;
}
static void free_chunk(struct poulsback_slab *s, struct chunk *c)
{
    struct chunk **list = c->large ? &s->large : &s->slabs;
    while (*list != c)
        list = &(*list)->next;
    *list = c->next;
    s->ops.free(s->ops.data, &c->bo);
    free(c);
}
int poulsback_slab_destroy(struct poulsback_slab *s)
{
    if (s->live || s->kept)
        return -EBUSY;
    /* Large BOs went with their entries. */
    while (s->slabs)
        free_chunk(s, s->slabs);
    free(s);
    return 0;
}
/* First fit of NEED bytes among slab C's entries: the offset, and the link
 * the new entry goes in at. */
static bool fit(struct chunk *c, uint64_t need, uint64_t *at, struct entry ***link)
{
    uint64_t end = 0;
    struct entry **p;
    for (p = &c->entries; *p && (*p)->base.offset - end < need; p = &(*p)->next)
        end = (*p)->base.offset + extent((*p)->base.size);
    if (!*p && POULSBACK_SLAB_BYTES - end < need)
        return false;
    *at = end;
    *link = p;
    return true;
}
static struct chunk *place(struct poulsback_slab *s, uint32_t bytes, uint64_t *at,
                           struct entry ***link, int *ret)
{
    struct chunk *c, *empty = NULL;
    if (bytes > POULSBACK_SLAB_BYTES) {
        c = new_chunk(s, ((uint64_t)bytes + PAGE - 1u) & ~(uint64_t)(PAGE - 1u), true, ret);
    } else {
        for (c = s->slabs; c; c = c->next) {
            if (c->entries && fit(c, extent(bytes), at, link))
                return c;
            if (!c->entries && !empty)
                empty = c;
        }
        if (empty)
            s->empty--;
        c = empty ? empty : new_chunk(s, POULSBACK_SLAB_BYTES, false, ret);
    }
    if (c) {
        *at = 0;
        *link = &c->entries;
    }
    return c;
}
/* ZERO: bytes that an earlier entry held are cleared. */
static int allocate(struct poulsback_slab *s, uint32_t bytes, bool zero,
                    struct poulsback_slab_entry **out)
{
    struct entry *e = calloc(1, sizeof(*e)), **link = NULL;
    struct chunk *c;
    uint64_t at = 0;
    int ret = 0;
    if (!e)
        return -ENOMEM;
    c = place(s, bytes, &at, &link, &ret);
    if (!c) {
        free(e);
        return ret;
    }
    e->chunk = c;
    e->next = *link;
    *link = e;
    e->base = (struct poulsback_slab_entry){.handle = c->bo.handle,
                                            .gpu_va = c->bo.gpu_va + at,
                                            .offset = at,
                                            .size = bytes,
                                            .cpu = c->bo.map + at};
    if (zero && at < c->clean)
        memset(e->base.cpu, 0, (size_t)(c->clean - at < bytes ? c->clean - at : bytes));
    if (c->clean < at + bytes)
        c->clean = at + bytes;
    s->live++;
    *out = &e->base;
    return 0;
}
int poulsback_slab_alloc(struct poulsback_slab *s, uint32_t bytes,
                         struct poulsback_slab_entry **out)
{
    return allocate(s, bytes, true, out);
}
/* E's space returns to its chunk. */
static void release(struct poulsback_slab *s, struct entry *e)
{
    struct chunk *c = e->chunk;
    struct entry **p;
    for (p = &c->entries; *p != e; p = &(*p)->next) {
    }
    *p = e->next;
    free(e);
    if (c->entries)
        return;
    if (c->large || s->empty == POULSBACK_SLAB_KEEP)
        free_chunk(s, c);
    else
        s->empty++;
}
void poulsback_slab_free(struct poulsback_slab *s, struct poulsback_slab_entry *base)
{
    struct entry *e = (struct entry *)base;
    s->live--;
    if (poulsback_slab_busy(s, base)) {
        e->kept = s->kept;
        s->kept = e;
    } else
        release(s, e);
}
void poulsback_slab_use(struct poulsback_slab_entry *e, uint64_t sequence)
{
    if (e->last_use < sequence)
        e->last_use = sequence;
}
void poulsback_slab_complete(struct poulsback_slab *s, uint64_t sequence)
{
    struct entry **p, *e;
    if (sequence <= s->completed)
        return;
    s->completed = sequence;
    for (p = &s->kept; (e = *p);) {
        if (e->base.last_use > sequence) {
            p = &e->kept;
            continue;
        }
        *p = e->kept;
        release(s, e);
    }
}
bool poulsback_slab_busy(const struct poulsback_slab *s, const struct poulsback_slab_entry *e)
{
    return e->last_use > s->completed;
}
int poulsback_slab_rename(struct poulsback_slab *s, struct poulsback_slab_entry **entry,
                          uint64_t offset, uint64_t bytes)
{
    struct poulsback_slab_entry *old = *entry, *renamed;
    uint64_t end = offset + bytes;
    int ret;
    if (!poulsback_slab_busy(s, old))
        return 0;
    ret = allocate(s, (uint32_t)old->size, false, &renamed);
    if (ret)
        return ret;
    memcpy(renamed->cpu, old->cpu, (size_t)(offset < old->size ? offset : old->size));
    if (end < old->size)
        memcpy(renamed->cpu + end, old->cpu + end, (size_t)(old->size - end));
    poulsback_slab_free(s, old);
    *entry = renamed;
    return 0;
}
