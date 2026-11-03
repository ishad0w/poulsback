// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_owned.h"

static int range(uintptr_t base, size_t bytes)
{
    return bytes != 0 && bytes - 1 <= UINTPTR_MAX - base;
}
static int overlap(uintptr_t a, size_t na, uintptr_t b, size_t nb)
{
    return a <= b ? b - a < na : a - b < nb;
}
static const struct msvdx_owned_buffer *find(const struct msvdx_owned_buffer *buffers, size_t count,
                                             uint32_t handle)
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (buffers[i].handle == handle)
            return &buffers[i];
    return NULL;
}
enum msvdx_owned_status msvdx_owned_validate(const struct msvdx_owned_buffer *buffers, size_t nb,
                                             const struct msvdx_owned_reloc *relocs, size_t nr,
                                             uint64_t owner, uint64_t epoch)
{
    size_t i, j;
    size_t bs, rs;
    if (!buffers || !relocs || !nb || !nr || !owner || !epoch)
        return MSVDX_OWNED_ARGUMENT;
    if (nb > MSVDX_OWNED_MAX_BUFFERS || nr > MSVDX_OWNED_MAX_RELOCS)
        return MSVDX_OWNED_LIMIT;
    bs = nb * sizeof(*buffers);
    rs = nr * sizeof(*relocs);
    if (!range((uintptr_t)buffers, bs) || !range((uintptr_t)relocs, rs) ||
        overlap((uintptr_t)buffers, bs, (uintptr_t)relocs, rs))
        return MSVDX_OWNED_ALIAS;
    for (i = 0; i < nb; ++i) {
        const struct msvdx_owned_buffer *b = &buffers[i];
        if (!b->handle || b->owner != owner || b->epoch != epoch)
            return MSVDX_OWNED_IDENTITY;
        if (!b->access || (b->access & ~(MSVDX_OWNED_READ | MSVDX_OWNED_PATCH)))
            return MSVDX_OWNED_ACCESS;
        if (!b->mapping || !range((uintptr_t)b->mapping, b->bytes))
            return MSVDX_OWNED_BOUNDS;
        if (b->gpu_base > UINT32_MAX || b->bytes - 1 > UINT32_MAX - b->gpu_base)
            return MSVDX_OWNED_ADDRESS;
        if (overlap((uintptr_t)b->mapping, b->bytes, (uintptr_t)buffers, bs) ||
            overlap((uintptr_t)b->mapping, b->bytes, (uintptr_t)relocs, rs))
            return MSVDX_OWNED_ALIAS;
        for (j = 0; j < i; ++j) {
            const struct msvdx_owned_buffer *p = &buffers[j];
            if (b->handle == p->handle)
                return MSVDX_OWNED_IDENTITY;
            if (overlap((uintptr_t)b->mapping, b->bytes, (uintptr_t)p->mapping, p->bytes) ||
                overlap((uintptr_t)b->gpu_base, b->bytes, (uintptr_t)p->gpu_base, p->bytes))
                return MSVDX_OWNED_ALIAS;
        }
    }
    for (i = 0; i < nr; ++i) {
        const struct msvdx_owned_reloc *r = &relocs[i];
        const struct msvdx_owned_buffer *d = find(buffers, nb, r->destination);
        const struct msvdx_owned_buffer *s = find(buffers, nb, r->source);
        if (!d || !s)
            return MSVDX_OWNED_IDENTITY;
        if (!(d->access & MSVDX_OWNED_PATCH) || !(s->access & MSVDX_OWNED_READ))
            return MSVDX_OWNED_ACCESS;
        if (r->destination_offset % 4 || r->destination_offset > d->bytes ||
            d->bytes - r->destination_offset < 4 || !r->source_bytes ||
            r->source_offset > s->bytes || r->source_bytes > s->bytes - r->source_offset)
            return MSVDX_OWNED_BOUNDS;
        if (!r->alignment || r->alignment > 4096 || (r->alignment & (r->alignment - 1)) ||
            ((s->gpu_base + r->source_offset) & (r->alignment - 1)))
            return MSVDX_OWNED_ADDRESS;
        for (j = 0; j < i; ++j)
            if (relocs[j].destination == r->destination &&
                relocs[j].destination_offset == r->destination_offset)
                return MSVDX_OWNED_ALIAS;
    }
    return MSVDX_OWNED_OK;
}
enum msvdx_owned_status msvdx_owned_patch(const struct msvdx_owned_buffer *buffers, size_t nb,
                                          const struct msvdx_owned_reloc *relocs, size_t nr,
                                          uint64_t owner, uint64_t epoch)
{
    size_t i;
    enum msvdx_owned_status status = msvdx_owned_validate(buffers, nb, relocs, nr, owner, epoch);
    if (status != MSVDX_OWNED_OK)
        return status;
    for (i = 0; i < nr; ++i) {
        const struct msvdx_owned_reloc *r = &relocs[i];
        const struct msvdx_owned_buffer *d = find(buffers, nb, r->destination);
        const struct msvdx_owned_buffer *s = find(buffers, nb, r->source);
        uint32_t address = (uint32_t)(s->gpu_base + r->source_offset);
        uint8_t *p = d->mapping + r->destination_offset;
        p[0] = (uint8_t)address;
        p[1] = (uint8_t)(address >> 8);
        p[2] = (uint8_t)(address >> 16);
        p[3] = (uint8_t)(address >> 24);
    }
    return MSVDX_OWNED_OK;
}
