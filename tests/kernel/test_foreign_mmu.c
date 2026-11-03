// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Execute the production wrapper include with raw-writer/epoch boundary mocks.
 * This is CPU refusal/transaction evidence, never physical MMU qualification.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

struct drm_device {
    unsigned id;
};
struct psb_mmu_driver {
    struct drm_device *dev;
    unsigned destroyed;
};
struct psb_mmu_pd {
    struct psb_mmu_driver *driver;
    unsigned destroyed;
};
typedef unsigned atomic_t;
enum boundary { NONE, FOREIGN, EPISODE, LEGACY };
static struct drm_device device = {1}, other = {2};
static struct psb_mmu_driver fixture_driver = {&device, 0};
static struct psb_mmu_pd directory = {&fixture_driver, 0};
static enum boundary boundary;
static struct drm_device *owner_device;
static unsigned owner_task, current_task = 1, begins, ends, writes, warnings, invalid;
static unsigned nested, depth, lose_owner;
static unsigned long checks;
static unsigned scenarios;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(value)) {                                                                            \
            fprintf(stderr, "check %s:%d: %s\n", __FILE__, __LINE__, #value);                      \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define WARN_ON(value)                                                                             \
    do {                                                                                           \
        if (value)                                                                                 \
            warnings++;                                                                            \
    } while (0)

static bool sgx535_epoch_external_active(struct drm_device *dev)
{
    return boundary == FOREIGN && owner_device == dev && owner_task == current_task;
}
static int sgx535_epoch_external_begin(struct drm_device *dev)
{
    begins++;
    if (boundary != NONE)
        return -EBUSY;
    boundary = FOREIGN;
    owner_device = dev;
    owner_task = current_task;
    return 0;
}
static int sgx535_epoch_external_end(struct drm_device *dev)
{
    ends++;
    if (!sgx535_epoch_external_active(dev))
        return -EPERM;
    boundary = NONE;
    owner_device = NULL;
    owner_task = 0;
    return 0;
}
void psb_mmu_flush(struct psb_mmu_driver *);
static void write_action(struct drm_device *dev)
{
    if (!sgx535_epoch_external_active(dev))
        invalid++;
    writes++;
    if (nested && !depth) {
        depth++;
        psb_mmu_flush(&fixture_driver);
        depth--;
    }
    if (lose_owner)
        owner_task = 99;
}
static void psb_mmu_flush_unguarded(struct psb_mmu_driver *d)
{
    write_action(d->dev);
}
static void psb_mmu_set_pd_context_unguarded(struct psb_mmu_pd *pd, int context)
{
    CHECK(context == 7);
    write_action(pd->driver->dev);
}
static void psb_mmu_free_pagedir_unguarded(struct psb_mmu_pd *pd)
{
    write_action(pd->driver->dev);
    pd->destroyed++;
}
static void psb_mmu_driver_takedown_unguarded(struct psb_mmu_driver *d)
{
    write_action(d->dev);
    d->destroyed++;
}
static struct psb_mmu_driver *psb_mmu_driver_init_unguarded(struct drm_device *dev, int trap,
                                                            int type, atomic_t *counter)
{
    CHECK(trap == 1 && type == 2 && counter && *counter == 3);
    write_action(dev);
    return &fixture_driver;
}
#include "poulsback_foreign_mmu.inc"

static int call(unsigned entry)
{
    atomic_t counter = 3;
    scenarios++;

    switch (entry) {
    case 0:
        psb_mmu_flush(&fixture_driver);
        return 0;
    case 1:
        psb_mmu_set_pd_context(&directory, 7);
        return 0;
    case 2:
        psb_mmu_free_pagedir(&directory);
        return 0;
    case 3:
        psb_mmu_driver_takedown(&fixture_driver);
        return 0;
    case 4:
        return psb_mmu_driver_init(&device, 1, 2, &counter) ? 0 : -EBUSY;
    default:
        abort();
    }
}
static void reset(void)
{
    boundary = NONE;
    owner_device = NULL;
    owner_task = 0;
    begins = ends = writes = warnings = invalid = nested = depth = lose_owner = 0;
    fixture_driver.destroyed = directory.destroyed = 0;
}
int main(void)
{
    unsigned entry, kind;

    for (entry = 0; entry < 5; entry++) {
        reset();
        CHECK(call(entry) == 0);
        CHECK(begins == 1 && ends == 1 && writes == 1 && !invalid && boundary == NONE && !warnings);
        reset();
        nested = 1;
        CHECK(call(entry) == 0);
        CHECK(begins == 1 && ends == 1 && writes == 2 && !invalid && boundary == NONE && !warnings);
        reset();
        boundary = FOREIGN;
        owner_device = &device;
        owner_task = current_task;
        nested = 1;
        CHECK(call(entry) == 0);
        CHECK(!begins && !ends && writes == 2 && !invalid && boundary == FOREIGN && !warnings);
        for (kind = 0; kind < 4; kind++) {
            int result;

            reset();
            boundary = kind < 2 ? FOREIGN : kind == 2 ? EPISODE : LEGACY;
            owner_device = kind == 0 ? &other : &device;
            owner_task = kind == 1 ? 99 : current_task;
            result = call(entry);
            CHECK(result == (entry == 4 ? -EBUSY : 0));
            CHECK(begins == 1 && !ends && !writes && !fixture_driver.destroyed &&
                  !directory.destroyed && !warnings);
        }
        reset();
        lose_owner = 1;
        CHECK(call(entry) == 0);
        CHECK(begins == 1 && ends == 1 && writes == 1 && warnings == 1 && boundary == FOREIGN &&
              owner_task == 99);
    }
    printf(
        "PASS production foreign-MMU wrappers: %lu CPU assertions / %u entry scenarios; no hardware\n",
        checks, scenarios);
    return 0;
}
