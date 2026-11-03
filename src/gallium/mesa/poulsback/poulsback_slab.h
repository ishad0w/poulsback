// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Buffer storage (docs/graphics.md#buffer-storage): PIPE_BUFFER resources
 * suballocated from slab BOs, so a frame lists slabs, not buffers.
 *
 * - Placement: a buffer of at most POULSBACK_SLAB_BYTES goes first fit, at a
 *   multiple of POULSBACK_SLAB_ALIGN, into a slab that already holds entries,
 *   else into an empty slab, else into a new one. A larger buffer gets its own
 *   BO (pages).
 * - An entry's bytes are zero when it is allocated.
 * - Freed space returns to its slab. At most POULSBACK_SLAB_KEEP empty slabs
 *   stay for reuse; another emptied slab and a large buffer's BO are freed.
 * - Busy storage: the owner numbers its jobs from 1 upwards. A job N reads an
 *   entry once the owner calls poulsback_slab_use(N) (the open frame's N is
 *   the one its submit will get), and the entry is busy until the owner
 *   reports every job up to N complete. Storage freed or renamed while busy
 *   keeps its space and bytes until then.
 *
 * BOs come from the owner's ops; the allocator issues no ioctl. It is not
 * thread-safe: the owner serialises every call. */
#ifndef POULSBACK_SLAB_H
#define POULSBACK_SLAB_H
#include <stdbool.h>
#include <stdint.h>

#define POULSBACK_SLAB_BYTES (1u << 20)
#define POULSBACK_SLAB_ALIGN 256u
#define POULSBACK_SLAB_KEEP 2u

/* A CPU-mapped BO at a fixed GPU address. */
struct poulsback_slab_bo {
    uint32_t handle;
    uint64_t gpu_va, size;
    uint8_t *map;
};
struct poulsback_slab_ops {
    void *data;
    /* A mapped BO of at least BYTES (a multiple of 4096) whose bytes are zero. */
    int (*alloc)(void *data, uint64_t bytes, struct poulsback_slab_bo *out);
    void (*free)(void *data, struct poulsback_slab_bo *);
};

/* One buffer's storage: SIZE bytes at OFFSET in BO HANDLE, at GPU_VA and CPU.
 * Read-only for the owner. LAST_USE is the newest job that reads it (0: none). */
struct poulsback_slab_entry {
    uint32_t handle;
    uint64_t gpu_va, offset, size;
    uint8_t *cpu;
    uint64_t last_use;
};

struct poulsback_slab;
int poulsback_slab_create(const struct poulsback_slab_ops *, struct poulsback_slab **);
/* Frees every BO. -EBUSY, changing nothing, while an entry is allocated or
 * busy storage is kept. */
int poulsback_slab_destroy(struct poulsback_slab *);
/* BYTES is at least 1. A failure changes nothing. */
int poulsback_slab_alloc(struct poulsback_slab *, uint32_t bytes,
                         struct poulsback_slab_entry **out);
/* The entry's space returns once no job that reads it is outstanding. */
void poulsback_slab_free(struct poulsback_slab *, struct poulsback_slab_entry *);
/* Job SEQUENCE reads the entry. */
void poulsback_slab_use(struct poulsback_slab_entry *, uint64_t sequence);
/* Every job numbered at most SEQUENCE has completed or will never run; the
 * storage only they kept is freed. An older SEQUENCE changes nothing. */
void poulsback_slab_complete(struct poulsback_slab *, uint64_t sequence);
bool poulsback_slab_busy(const struct poulsback_slab *, const struct poulsback_slab_entry *);
/* Before a CPU write of [offset, offset + bytes), which lies in the entry:
 * busy storage is replaced by new storage of the same size holding every byte
 * outside that range (those inside are undefined), and the old storage is
 * freed as by poulsback_slab_free (it stays readable until then). Idle
 * storage is kept. A failure keeps *entry. */
int poulsback_slab_rename(struct poulsback_slab *, struct poulsback_slab_entry **entry,
                          uint64_t offset, uint64_t bytes);
#endif
