// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* CPU substitutes for the kernel interfaces kernel/engine/poulsback_context.c
 * uses beyond the vm suite's (vm-mocks/), for the `context` host suite: the
 * device and file, an id table, and scheduler entities that count their
 * initialisations and destructions (the Nth init from now fails, 0: none).
 * Nothing here models a device or a scheduler's queue. */
#ifndef CONTEXT_KERNEL_MOCKS_H
#define CONTEXT_KERNEL_MOCKS_H

#include "vm_kernel_mocks.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))

struct drm_device {
    int unused;
};
struct drm_file {
    int unused;
};
static bool drm_dev_enter(struct drm_device *dev, int *idx)
{
    (void)dev;
    *idx = 0;
    return true;
}
static void drm_dev_exit(int idx)
{
    (void)idx;
}

enum drm_sched_priority { DRM_SCHED_PRIORITY_NORMAL };
struct drm_gpu_scheduler {
    int unused;
};
struct drm_sched_entity {
    int live;
};
struct drm_sched_job {
    struct drm_sched_entity *entity;
};
static unsigned mock_entities_live, mock_entity_fail_in;
static int drm_sched_entity_init(struct drm_sched_entity *entity, enum drm_sched_priority priority,
                                 struct drm_gpu_scheduler **list, unsigned count, void *guilty)
{
    (void)priority, (void)list, (void)count, (void)guilty;
    if (mock_entity_fail_in && !--mock_entity_fail_in)
        return -ENOMEM;
    entity->live = 1;
    mock_entities_live++;
    return 0;
}
static void drm_sched_entity_destroy(struct drm_sched_entity *entity)
{
    if (!entity->live)
        abort();
    entity->live = 0;
    mock_entities_live--;
}

#define MOCK_IDR_IDS 128
struct idr {
    void *slot[MOCK_IDR_IDS];
};
static void idr_init(struct idr *idr)
{
    memset(idr, 0, sizeof(*idr));
}
static void idr_destroy(struct idr *idr)
{
    (void)idr;
}
static int idr_alloc(struct idr *idr, void *p, int start, int end, unsigned gfp)
{
    (void)gfp;
    for (int id = start; id < (end > 0 ? end : MOCK_IDR_IDS) && id < MOCK_IDR_IDS; id++)
        if (!idr->slot[id]) {
            idr->slot[id] = p;
            return id;
        }
    return -ENOSPC;
}
static void *idr_find(struct idr *idr, unsigned long id)
{
    return id < MOCK_IDR_IDS ? idr->slot[id] : NULL;
}
static void *idr_remove(struct idr *idr, unsigned long id)
{
    void *p = idr_find(idr, id);
    if (p)
        idr->slot[id] = NULL;
    return p;
}
#define idr_for_each_entry(idr, entry, id)                                                         \
    for ((id) = 0; (id) < MOCK_IDR_IDS; (id)++)                                                     \
        if (((entry) = (idr)->slot[id]) != NULL)
#endif
