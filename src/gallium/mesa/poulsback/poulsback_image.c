// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "poulsback_image.h"
#include "poulsback_bo_uapi.h"
#include "poulsback_engine.h"
#include "poulsback_exec_uapi.h"
#include "poulsback_public.h"
#include "poulsback_refuse.h"
#include "poulsback_slab.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

/* Images and depth are BOs; renders run on the engine. */
struct poulsback_ws {
    pthread_mutex_t lock;
    struct poulsback_ws_image *images;
    struct poulsback_ws_render *renders;
    struct poulsback_ws_depth *depths;
    struct poulsback_ws_layout display;
    int fd;
    int cleanup_error;
    /* Job numbers for buffer storage (poulsback_slab.h): one sequence for the
     * client's renders; a render's open frame reserves the number its job
     * will get. */
    uint64_t sequence;
};

struct poulsback_ws_image {
    struct poulsback_ws *ws;
    struct poulsback_ws_image *next;
    struct poulsback_image_info info;
    unsigned refs, gpu_refs;
    void *mapping;
    uint64_t sync_flags;
    int dma_fd;
    bool mapped, sync_ended, private_closed;
    uint64_t gpu_va, mmap_offset;
};

static int checked_layout(const struct poulsback_ws_layout *layout)
{
    uint64_t extent;
    if (!layout || !layout->width || !layout->height ||
        layout->modifier != POULSBACK_IMAGE_MOD_LINEAR || layout->offset ||
        (layout->format != POULSBACK_IMAGE_FORMAT_XRGB8888 &&
         layout->format != POULSBACK_IMAGE_FORMAT_ARGB8888) ||
        !layout->stride || (layout->stride & 127U) || (uint64_t)layout->width * 4U > layout->stride)
        return -EINVAL;
    extent = (uint64_t)layout->stride * layout->height;
    return extent <= POULSBACK_IMAGE_MAX_BYTES ? 0 : -E2BIG;
}

static int close_handle(struct poulsback_ws *ws, uint32_t handle)
{
    struct drm_gem_close request = {.handle = handle};
    return drmIoctl(ws->fd, DRM_IOCTL_GEM_CLOSE, &request) ? -errno : 0;
}

static int close_image_handles(struct poulsback_ws_image *image)
{
    int ret;
    if (!image->private_closed && image->info.handle) {
        ret = close_handle(image->ws, image->info.handle);
        if (ret)
            return ret;
    }
    image->private_closed = true;
    return 0;
}

static int open_namespace(int fd)
{
    struct stat original, reopened;
    char path[64];
    int anchor, owned, ret;
    anchor = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (anchor < 0)
        return -errno;
    if (fstat(anchor, &original)) {
        ret = -errno;
        close(anchor);
        return ret;
    }
    if (!S_ISCHR(original.st_mode)) {
        close(anchor);
        return -ENODEV;
    }
    /* dup shares a DRM file's GEM namespace. Reopen its stable /proc anchor
     * to create a new drm_file; never close a loader/caller-owned GEM handle.
     * Opening can fail when the caller lacks device access: refuse rather
     * than silently falling back to the shared namespace, except below. */
    (void)snprintf(path, sizeof(path), "/proc/self/fd/%d", anchor);
    owned = open(path, O_RDWR | O_CLOEXEC);
    ret = owned < 0 ? -errno : 0;
    /* A DRI3 client renders on a render-node fd the X server opened for it;
     * without device access of its own it cannot reopen it. That fd belongs to
     * this client alone and only this driver creates GEM handles on it, so its
     * namespace is shared. */
    if (ret == -EACCES && drmGetNodeTypeFromFd(anchor) == DRM_NODE_RENDER) {
        owned = fcntl(anchor, F_DUPFD_CLOEXEC, 0);
        ret = owned < 0 ? -errno : 0;
    }
    if (owned >= 0) {
        if (fstat(owned, &reopened))
            ret = -errno;
        else if (!S_ISCHR(reopened.st_mode) || reopened.st_rdev != original.st_rdev ||
                 poulsback_drm_identify(owned) != POULSBACK_DRM_NATIVE_UNQUALIFIED)
            ret = -ENODEV;
        if (ret)
            close(owned);
    }
    close(anchor);
    return ret ? ret : owned;
}

/* An image is a 2D-class BO (older clients' GENERAL ones still import) and its
 * layout is the caller's. */
static int image_info(struct poulsback_ws *ws, uint32_t handle,
                      const struct poulsback_ws_layout *layout, struct poulsback_ws_image *image)
{
    struct poulsback_bo_info bo = {.abi = POULSBACK_BO_ABI, .handle = handle};
    if (drmIoctl(ws->fd, DRM_IOCTL_PB_BO_INFO, &bo))
        return -errno;
    if ((bo.bo_class != POULSBACK_BO_CLASS_2D && bo.bo_class != POULSBACK_BO_CLASS_GENERAL) ||
        bo.size < (uint64_t)layout->stride * layout->height || bo.size > SIZE_MAX)
        return -EPROTO;
    image->info = (struct poulsback_image_info){.handle = handle,
                                                .size = bo.size,
                                                .modifier = layout->modifier,
                                                .width = layout->width,
                                                .height = layout->height,
                                                .stride = layout->stride,
                                                .format = layout->format};
    image->gpu_va = bo.gpu_va;
    image->mmap_offset = bo.mmap_offset;
    return 0;
}

int poulsback_ws_create(int fd, struct poulsback_ws **out)
{
    struct poulsback_ws *ws;
    int owned, ret;
    if (!out || fd < 0)
        return -EINVAL;
    if (poulsback_drm_identify(fd) != POULSBACK_DRM_NATIVE_UNQUALIFIED)
        return -ENODEV;
    owned = open_namespace(fd);
    if (owned < 0)
        return owned;
    ws = calloc(1, sizeof(*ws));
    if (!ws) {
        close(owned);
        return -ENOMEM;
    }
    ret = pthread_mutex_init(&ws->lock, NULL);
    if (ret) {
        free(ws);
        close(owned);
        return -ret;
    }
    ws->fd = owned;
    {
        struct poulsback_display_info info = {.abi = POULSBACK_BO_ABI};
        if (!drmIoctl(owned, DRM_IOCTL_PB_DISPLAY_INFO, &info))
            ws->display = (struct poulsback_ws_layout){.width = info.width,
                                                       .height = info.height,
                                                       .stride = info.stride,
                                                       .format = info.format};
    }
    *out = ws;
    return 0;
}

void poulsback_ws_display(const struct poulsback_ws *ws, struct poulsback_ws_layout *out)
{
    *out = ws->display;
}

int poulsback_ws_destroy(struct poulsback_ws *ws)
{
    if (!ws)
        return -EINVAL;
    pthread_mutex_lock(&ws->lock);
    if (ws->images || ws->renders || ws->depths) {
        pthread_mutex_unlock(&ws->lock);
        return -EBUSY;
    }
    pthread_mutex_unlock(&ws->lock);
    /* Linux close consumes the descriptor even on EINTR. Never retry it. */
    close(ws->fd);
    pthread_mutex_destroy(&ws->lock);
    free(ws);
    return 0;
}

static int publish_image(struct poulsback_ws *ws, uint32_t handle,
                         const struct poulsback_ws_layout *layout, struct poulsback_ws_image *image,
                         struct poulsback_ws_image **out)
{
    struct poulsback_ws_image *existing;
    int ret;
    for (existing = ws->images; existing; existing = existing->next) {
        if (existing->private_closed || existing->info.handle != handle)
            continue;
        if (!existing->refs) {
            free(image);
            return -EBUSY;
        }
        ret = image_info(ws, handle, layout, image);
        if (!ret && memcmp(&existing->info, &image->info, sizeof(image->info)))
            ret = -EPROTO;
        if (!ret && existing->refs == UINT_MAX)
            ret = -EOVERFLOW;
        if (!ret) {
            existing->refs++;
            *out = existing;
        }
        free(image);
        return ret;
    }
    ret = image_info(ws, handle, layout, image);
    /* Retain a failed unpublished handle internally if GEM_CLOSE itself fails:
     * destroy returns EBUSY, preventing silent namespace abandonment. The
     * public reap operation below makes cleanup failures explicitly retryable. */
    if (ret) {
        int cleanup = close_handle(ws, handle);
        if (!cleanup) {
            free(image);
            return ret;
        }
        ws->cleanup_error = cleanup;
        image->info.handle = handle;
        image->refs = 0;
    } else {
        image->refs = 1;
    }
    image->ws = ws;
    image->dma_fd = -1;
    image->next = ws->images;
    ws->images = image;
    if (!ret)
        *out = image;
    return ret;
}

int poulsback_ws_image_create(struct poulsback_ws *ws, const struct poulsback_ws_layout *layout,
                              bool scanout, struct poulsback_ws_image **out)
{
    struct poulsback_ws_image *image;
    int ret;
    if (!ws || !out)
        return -EINVAL;
    ret = checked_layout(layout);
    if (ret)
        return ret;
    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;
    /* Write-back, in the 2D class: the display server copies from it with the
     * 2D core in its own file. A SCANOUT image is shown by a page flip
     * instead, from write-combined pages. */
    const uint32_t flags = scanout ? POULSBACK_BO_SCANOUT | POULSBACK_BO_CPU_WC : 0;
    const uint64_t bytes = ((uint64_t)layout->stride * layout->height + 4095u) & ~UINT64_C(4095);
    struct poulsback_frame_bo bo;
    pthread_mutex_lock(&ws->lock);
    ret = poulsback_bo_create(ws->fd, POULSBACK_BO_CLASS_2D, flags, bytes, PROT_NONE, &bo,
                              &ws->cleanup_error);
    if (!ret)
        ret = publish_image(ws, bo.handle, layout, image, out);
    else
        free(image);
    pthread_mutex_unlock(&ws->lock);
    return ret;
}

int poulsback_ws_image_import(struct poulsback_ws *ws, int fd,
                              const struct poulsback_ws_layout *layout,
                              struct poulsback_ws_image **out)
{
    struct poulsback_ws_image *image;
    uint32_t handle;
    int ret;
    if (!ws || !out || fd < 0)
        return -EINVAL;
    ret = checked_layout(layout);
    if (ret)
        return ret;
    image = calloc(1, sizeof(*image));
    if (!image)
        return -ENOMEM;
    pthread_mutex_lock(&ws->lock);
    ret = drmPrimeFDToHandle(ws->fd, fd, &handle) ? -errno : 0;
    if (!ret)
        ret = publish_image(ws, handle, layout, image, out);
    else
        free(image);
    pthread_mutex_unlock(&ws->lock);
    return ret;
}

int poulsback_ws_image_ref(struct poulsback_ws_image *image)
{
    int ret = 0;
    if (!image)
        return -EINVAL;
    pthread_mutex_lock(&image->ws->lock);
    if (image->private_closed)
        ret = -EBUSY;
    else if (image->refs == UINT_MAX)
        ret = -EOVERFLOW;
    else
        image->refs++;
    pthread_mutex_unlock(&image->ws->lock);
    return ret;
}

static int image_unref_locked(struct poulsback_ws_image *image)
{
    struct poulsback_ws_image **link;
    struct poulsback_ws *ws;
    int ret = 0;
    if (!image)
        return -EINVAL;
    ws = image->ws;
    if (image->mapped && image->refs <= 2) {
        /* Keep the final caller reference alongside the internal CPU hold;
         * otherwise END would release it with no owner left to close GEM. */
        ret = -EBUSY;
    } else if (image->refs > 1) {
        image->refs--;
    } else if (image->mapped) {
        /* The internal CPU reference is not a caller reference. */
        ret = -EBUSY;
    } else if (!(ret = close_image_handles(image))) {
        for (link = &ws->images; *link != image; link = &(*link)->next) {
        }
        *link = image->next;
        free(image);
    }
    return ret;
}

int poulsback_ws_image_unref(struct poulsback_ws_image *image)
{
    struct poulsback_ws *ws;
    int ret;
    if (!image)
        return -EINVAL;
    ws = image->ws;
    pthread_mutex_lock(&ws->lock);
    ret = image_unref_locked(image);
    pthread_mutex_unlock(&ws->lock);
    return ret;
}

int poulsback_ws_image_info(struct poulsback_ws_image *image, struct poulsback_image_info *out)
{
    if (!image || !out)
        return -EINVAL;
    *out = image->info;
    return 0;
}

int poulsback_ws_image_gpu(struct poulsback_ws_image *image, uint32_t *handle, uint64_t *gpu_va)
{
    if (!image || !handle || !gpu_va || !image->gpu_va)
        return -EINVAL;
    *handle = image->info.handle;
    *gpu_va = image->gpu_va;
    return 0;
}

int poulsback_ws_image_export(struct poulsback_ws_image *image, int *fd)
{
    int exported, ret;
    if (!image || !fd)
        return -EINVAL;
    pthread_mutex_lock(&image->ws->lock);
    ret = -EBUSY;
    if (!image->private_closed && !image->gpu_refs && !image->mapped)
        ret =
            drmPrimeHandleToFD(image->ws->fd, image->info.handle, DRM_CLOEXEC | DRM_RDWR, &exported)
                ? -errno
                : 0;
    if (!ret)
        *fd = exported;
    pthread_mutex_unlock(&image->ws->lock);
    return ret;
}

/* Whether GPU_REFS (an image's or a depth's, under WS's lock) counts a job. */
static bool gpu_busy(struct poulsback_ws *ws, const unsigned *gpu_refs)
{
    bool busy;
    pthread_mutex_lock(&ws->lock);
    busy = *gpu_refs != 0;
    pthread_mutex_unlock(&ws->lock);
    return busy;
}
bool poulsback_ws_image_gpu_busy(struct poulsback_ws_image *image)
{
    return image && gpu_busy(image->ws, &image->gpu_refs);
}
int poulsback_ws_image_cpu_begin(struct poulsback_ws_image *image,
                                 enum poulsback_ws_cpu_access access, void **address,
                                 uint64_t *size)
{
    struct dma_buf_sync sync;
    void *mapping;
    int exported = -1, ret;
    if (!image || !address || !size || access < POULSBACK_WS_CPU_READ ||
        access > POULSBACK_WS_CPU_READ_WRITE)
        return -EINVAL;
    pthread_mutex_lock(&image->ws->lock);
    ret = -EBUSY;
    if (image->private_closed || image->mapped || image->gpu_refs)
        goto done;
    ret = -EOVERFLOW;
    if (image->refs == UINT_MAX)
        goto done;
    ret = drmPrimeHandleToFD(image->ws->fd, image->info.handle, DRM_CLOEXEC | DRM_RDWR, &exported)
              ? -errno
              : 0;
    if (ret)
        goto done;
    mapping = mmap(NULL, (size_t)image->info.size,
                   PROT_READ | ((access & POULSBACK_WS_CPU_WRITE) ? PROT_WRITE : 0), MAP_SHARED,
                   image->ws->fd, (off_t)image->mmap_offset);
    if (mapping == MAP_FAILED) {
        ret = -errno;
        close(exported);
        goto done;
    }
    sync.flags = (uint64_t)access | DMA_BUF_SYNC_START;
    if (ioctl(exported, DMA_BUF_IOCTL_SYNC, &sync)) {
        ret = -errno;
        /* No CPU access has begun; keep an orphan if unmapping fails. */
        if (munmap(mapping, (size_t)image->info.size)) {
            image->ws->cleanup_error = -errno;
            image->mapping = mapping;
            image->mapped = true;
            image->dma_fd = exported;
            image->sync_ended = true;
            image->refs++;
        } else {
            close(exported);
        }
        goto done;
    }
    image->mapping = mapping;
    image->mapped = true;
    image->dma_fd = exported;
    image->sync_flags = (uint64_t)access;
    image->sync_ended = false;
    image->refs++;
    *address = mapping;
    *size = image->info.size;
done:
    pthread_mutex_unlock(&image->ws->lock);
    return ret;
}

int poulsback_ws_image_cpu_end(struct poulsback_ws_image *image)
{
    struct dma_buf_sync sync;
    int ret = 0;
    if (!image)
        return -EINVAL;
    pthread_mutex_lock(&image->ws->lock);
    if (!image->mapped) {
        ret = -EINVAL;
        goto done;
    }
    if (!image->sync_ended) {
        sync.flags = image->sync_flags | DMA_BUF_SYNC_END;
        if (ioctl(image->dma_fd, DMA_BUF_IOCTL_SYNC, &sync)) {
            ret = -errno;
            goto done;
        }
        image->sync_ended = true;
    }
    if (munmap(image->mapping, (size_t)image->info.size)) {
        ret = -errno;
        goto done;
    }
    close(image->dma_fd);
    image->dma_fd = -1;
    image->mapping = NULL;
    image->mapped = false;
    image->refs--;
done:
    pthread_mutex_unlock(&image->ws->lock);
    return ret;
}

static void buffer_cleanup_error(struct poulsback_ws *ws, int ret)
{
    pthread_mutex_lock(&ws->lock);
    ws->cleanup_error = ret;
    pthread_mutex_unlock(&ws->lock);
}
static int buffer_alloc(void *data, uint64_t bytes, struct poulsback_slab_bo *out)
{
    struct poulsback_ws *ws = data;
    struct poulsback_frame_bo bo;
    int cleanup = 0;
    /* No CPU_WC flag: write-back pages, snooped GPU access (HW-CACHE-002). */
    int ret = poulsback_bo_create(ws->fd, POULSBACK_BO_CLASS_GENERAL, 0, bytes,
                                  PROT_READ | PROT_WRITE, &bo, &cleanup);
    if (cleanup)
        buffer_cleanup_error(ws, cleanup);
    if (ret)
        return POULSBACK_REFUSED(ret);
    *out = (struct poulsback_slab_bo){bo.handle, bo.gpu_va, bo.size, bo.map};
    return 0;
}
static void buffer_free(void *data, struct poulsback_slab_bo *bo)
{
    struct poulsback_ws *ws = data;
    int ret = munmap(bo->map, (size_t)bo->size) ? -errno : 0,
        cleanup = close_handle(ws, bo->handle);
    if (ret || cleanup)
        buffer_cleanup_error(ws, ret ? ret : cleanup);
}
void poulsback_ws_buffer_ops(struct poulsback_ws *ws, struct poulsback_slab_ops *out)
{
    *out = (struct poulsback_slab_ops){ws, buffer_alloc, buffer_free};
}

struct poulsback_ws_depth {
    struct poulsback_ws *ws;
    struct poulsback_ws_depth *next;
    uint32_t handle, width, height;
    unsigned refs, gpu_refs;
    int error;
    bool held;
    uint64_t gpu_va, size; /* A DEPTH BO. */
    void *mapping;         /* the CPU's, between cpu_begin and cpu_end */
};
struct render_job {
    struct render_job *next;
    struct poulsback_ws_image *target, *source; /* source: a copy's other image */
    struct poulsback_ws_depth *depth;
    uint64_t id, sequence; /* sequence: the frame's buffer job number, or 0 */
    bool retired;
    int fence; /* The job's sync_file, readable once it completed. */
};
struct poulsback_ws_render {
    struct poulsback_ws_render *next;
    struct poulsback_ws *ws;
    struct render_job *jobs;
    int error;
    bool held;
    struct poulsback_engine *engine;
    uint64_t engine_jobs, open_sequence;
};
static int render_hold(struct poulsback_ws_render *r, int error)
{
    if (!r->error)
        r->error = error;
    r->held = true;
    for (struct render_job *job = r->jobs; job; job = job->next)
        if (job->depth) {
            job->depth->held = true;
            if (!job->depth->error)
                job->depth->error = error;
        }
    return error;
}

/* A render is one engine context; it takes depth scenes only. */
int poulsback_ws_render_create(struct poulsback_ws *ws, struct poulsback_ws_render **out)
{
    struct poulsback_ws_render *r;
    int ret;
    if (!ws || !out)
        return -EINVAL;
    r = calloc(1, sizeof(*r));
    if (!r)
        return -ENOMEM;
    pthread_mutex_lock(&ws->lock);
    ret = poulsback_engine_create(ws->fd, &r->engine);
    if (ret) {
        free(r);
    } else {
        r->ws = ws;
        r->next = ws->renders;
        ws->renders = r;
        *out = r;
    }
    pthread_mutex_unlock(&ws->lock);
    return ret;
}
static bool canonical_depth(struct poulsback_ws *ws, struct poulsback_ws_depth *depth)
{
    for (struct poulsback_ws_depth *p = ws->depths; p; p = p->next)
        if (p == depth)
            return p->refs && p->refs < UINT_MAX && p->gpu_refs < UINT_MAX && p->handle && !p->held;
    return false;
}
int poulsback_ws_depth_create(struct poulsback_ws_render *r, uint32_t width, uint32_t height,
                              struct poulsback_ws_depth **out)
{
    struct poulsback_ws_depth *d;
    int ret;
    if (!r || !out || *out || !width || !height || width > 2048 || height > 2048)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    if (r->held || r->error) {
        ret = r->error ? r->error : -EIO;
        goto done;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        ret = -ENOMEM;
        goto done;
    }
    d->ws = r->ws;
    d->width = width;
    d->height = height;
    d->refs = 1;
    d->next = r->ws->depths;
    r->ws->depths = d;
    /* Write-combined: only the GPU touches depth. */
    struct poulsback_frame_bo bo;
    ret = poulsback_bo_create(r->ws->fd, POULSBACK_BO_CLASS_DEPTH, POULSBACK_BO_CPU_WC,
                              poulsback_frame_depth_bytes(width, height), PROT_NONE, &bo,
                              &r->ws->cleanup_error);
    if (ret) {
        d->held = true;
        d->error = ret;
        render_hold(r, ret);
    } else {
        d->handle = bo.handle;
        d->gpu_va = bo.gpu_va;
        d->size = bo.size;
        *out = d;
    }
done:
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
int poulsback_ws_depth_ref(struct poulsback_ws_depth *d)
{
    int ret = 0;
    if (!d)
        return -EINVAL;
    pthread_mutex_lock(&d->ws->lock);
    if (!canonical_depth(d->ws, d))
        ret = d->error ? d->error : -EINVAL;
    else
        ++d->refs;
    pthread_mutex_unlock(&d->ws->lock);
    return ret;
}
static int depth_unref_locked(struct poulsback_ws_depth *d)
{
    struct drm_gem_close close_request = {.handle = d->handle};
    struct poulsback_ws_depth **link;
    int ret;
    if (d->refs > 1) {
        --d->refs;
        return 0;
    }
    if (!d->refs || d->gpu_refs || d->held || d->mapping)
        return d->error ? d->error : -EBUSY;
    ret = drmIoctl(d->ws->fd, DRM_IOCTL_GEM_CLOSE, &close_request) ? -errno : 0;
    if (ret) {
        d->held = true;
        d->error = ret;
        return ret;
    }
    for (link = &d->ws->depths; *link != d; link = &(*link)->next) {
    }
    *link = d->next;
    free(d);
    return 0;
}
int poulsback_ws_depth_unref(struct poulsback_ws_depth *d)
{
    struct poulsback_ws *ws;
    int ret;
    if (!d)
        return -EINVAL;
    ws = d->ws;
    pthread_mutex_lock(&ws->lock);
    ret = depth_unref_locked(d);
    pthread_mutex_unlock(&ws->lock);
    return ret;
}
bool poulsback_ws_depth_gpu_busy(struct poulsback_ws_depth *d)
{
    return d && gpu_busy(d->ws, &d->gpu_refs);
}
int poulsback_ws_depth_cpu_begin(struct poulsback_ws_depth *d, void **address, uint32_t *stride)
{
    struct poulsback_bo_info bo = {.abi = POULSBACK_BO_ABI};
    void *mapping;
    int ret;
    if (!d || !address || !stride)
        return -EINVAL;
    pthread_mutex_lock(&d->ws->lock);
    ret = d->held ? (d->error ? d->error : -EIO) : d->gpu_refs || d->mapping ? -EBUSY : 0;
    bo.handle = d->handle;
    if (!ret && drmIoctl(d->ws->fd, DRM_IOCTL_PB_BO_INFO, &bo))
        ret = -errno;
    if (!ret) {
        mapping = mmap(NULL, (size_t)d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->ws->fd,
                       (off_t)bo.mmap_offset);
        if (mapping == MAP_FAILED)
            ret = -errno;
        else {
            d->mapping = *address = mapping;
            *stride = ((d->width + 31u) & ~31u) * 4u;
        }
    }
    pthread_mutex_unlock(&d->ws->lock);
    return ret;
}
int poulsback_ws_depth_cpu_end(struct poulsback_ws_depth *d)
{
    int ret;
    if (!d)
        return -EINVAL;
    pthread_mutex_lock(&d->ws->lock);
    ret = !d->mapping ? -EINVAL : munmap(d->mapping, (size_t)d->size) ? -errno : 0;
    if (!ret)
        d->mapping = NULL;
    pthread_mutex_unlock(&d->ws->lock);
    return ret;
}
uint32_t poulsback_ws_render_depth_scene_version(struct poulsback_ws_render *r)
{
    uint32_t version = 0;
    if (!r)
        return 0;
    pthread_mutex_lock(&r->ws->lock);
    if (!r->held && !r->error)
        version = POULSBACK_RENDER_DEPTH_SCENE_ABI;
    pthread_mutex_unlock(&r->ws->lock);
    return version;
}
static int reap_locked(struct poulsback_ws_render *r)
{
    struct render_job **link, *job;
    int ret, first = 0;
    if (r->held)
        return r->error ? r->error : -EIO;
    for (link = &r->jobs; (job = *link);) {
        if (!job->retired) {
            struct pollfd fence = {.fd = job->fence, .events = POLLIN};
            ret = poll(&fence, 1, 0);
            if (ret < 0)
                return render_hold(r, -errno);
            if (!ret) {
                first = -EBUSY;
                link = &job->next;
                continue;
            }
            close(job->fence);
            job->fence = -1;
            job->retired = true;
        }
        /* Keep each pointer until its independently checked cleanup succeeds. */
        if (job->target) {
            --job->target->gpu_refs;
            ret = image_unref_locked(job->target);
            if (ret) {
                ++job->target->gpu_refs;
                r->ws->cleanup_error = ret;
                return ret;
            }
            job->target = NULL;
        }
        if (job->source) {
            --job->source->gpu_refs;
            ret = image_unref_locked(job->source);
            if (ret) {
                ++job->source->gpu_refs;
                r->ws->cleanup_error = ret;
                return ret;
            }
            job->source = NULL;
        }
        if (job->depth) {
            --job->depth->gpu_refs;
            ret = depth_unref_locked(job->depth);
            if (ret) {
                ++job->depth->gpu_refs;
                r->ws->cleanup_error = ret;
                return ret;
            }
            job->depth = NULL;
        }
        *link = job->next;
        free(job);
    }
    return first;
}
int poulsback_ws_render_reap(struct poulsback_ws_render *r)
{
    int ret;
    if (!r)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    ret = reap_locked(r);
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
int poulsback_ws_render_draw(struct poulsback_ws_render *r, const poulsback_frame_shape *shape,
                             const struct poulsback_frame_vertices *vertices)
{
    int ret;
    if (!r || !r->engine || !shape || !vertices)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    ret =
        r->held ? (r->error ? r->error : -EIO) : poulsback_engine_draw(r->engine, shape, vertices);
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
int poulsback_ws_render_draw_gpu(struct poulsback_ws_render *r, const poulsback_frame_shape *shape,
                                 const struct poulsback_frame_gpu_draw *draw)
{
    int ret;
    if (!r || !r->engine || !shape || !draw)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    ret = r->held ? (r->error ? r->error : -EIO) : poulsback_engine_draw_gpu(r->engine, shape, draw);
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
int poulsback_ws_render_stream(struct poulsback_ws_render *r, uint32_t bytes, uint32_t alignment,
                               void **cpu, uint64_t *address, uint32_t *handle)
{
    int ret;
    if (!r || !r->engine || !cpu || !address || !handle)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    ret = r->held ? (r->error ? r->error : -EIO)
                  : poulsback_engine_stream(r->engine, bytes, alignment, cpu, address, handle);
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
void poulsback_ws_render_sequence(struct poulsback_ws_render *r, uint64_t *address,
                                  uint32_t *handle)
{
    pthread_mutex_lock(&r->ws->lock);
    poulsback_engine_sequence(r->engine, address, handle);
    pthread_mutex_unlock(&r->ws->lock);
}
uint64_t poulsback_ws_render_frame_sequence(struct poulsback_ws_render *r)
{
    uint64_t sequence;
    pthread_mutex_lock(&r->ws->lock);
    if (!r->open_sequence)
        r->open_sequence = ++r->ws->sequence;
    sequence = r->open_sequence;
    pthread_mutex_unlock(&r->ws->lock);
    return sequence;
}
uint64_t poulsback_ws_completed(struct poulsback_ws *ws)
{
    uint64_t low = UINT64_MAX, done;
    pthread_mutex_lock(&ws->lock);
    for (struct poulsback_ws_render *r = ws->renders; r; r = r->next) {
        (void)reap_locked(r);
        if (r->open_sequence && r->open_sequence < low)
            low = r->open_sequence;
        for (struct render_job *job = r->jobs; job; job = job->next)
            if (job->sequence && !job->retired && job->sequence < low)
                low = job->sequence;
    }
    done = low == UINT64_MAX ? ws->sequence : low - 1u;
    pthread_mutex_unlock(&ws->lock);
    return done;
}
void poulsback_ws_render_discard(struct poulsback_ws_render *r)
{
    if (!r || !r->engine)
        return;
    pthread_mutex_lock(&r->ws->lock);
    poulsback_engine_discard(r->engine);
    pthread_mutex_unlock(&r->ws->lock);
}
void poulsback_ws_render_load(struct poulsback_ws_render *r)
{
    if (!r || !r->engine)
        return;
    pthread_mutex_lock(&r->ws->lock);
    poulsback_engine_load(r->engine);
    pthread_mutex_unlock(&r->ws->lock);
}
uint32_t poulsback_ws_render_draws(struct poulsback_ws_render *r)
{
    uint32_t n;
    if (!r || !r->engine)
        return 0;
    pthread_mutex_lock(&r->ws->lock);
    n = poulsback_engine_draws(r->engine);
    pthread_mutex_unlock(&r->ws->lock);
    return n;
}
int poulsback_ws_render_submit_engine(struct poulsback_ws_render *r,
                                      struct poulsback_ws_image *target,
                                      struct poulsback_ws_depth *depth, int *sync_file,
                                      uint64_t *job_id)
{
    struct poulsback_engine_buffer t, d;
    struct render_job *job;
    int ret, fence = -1;
    if (!r || !r->engine || !target || !sync_file || !job_id || target->ws != r->ws ||
        (depth && depth->ws != r->ws))
        return -EINVAL;
    job = calloc(1, sizeof(*job));
    if (!job)
        return -ENOMEM;
    pthread_mutex_lock(&r->ws->lock);
    if (r->held) {
        ret = r->error ? r->error : -EIO;
        goto failed;
    }
    if (target->private_closed || target->mapped || target->refs == UINT_MAX ||
        (depth && (depth->held || depth->refs == UINT_MAX))) {
        ret = -EBUSY;
        goto failed;
    }
    t = (struct poulsback_engine_buffer){target->info.handle, target->info.stride, target->gpu_va,
                                         target->info.size};
    if (depth)
        d = (struct poulsback_engine_buffer){depth->handle, 0, depth->gpu_va, depth->size};
    /* A refused or failed frame may have reached the GPU: hold the render. */
    ret = poulsback_engine_submit(r->engine, &t, depth ? &d : NULL, &fence);
    if (!ret && (*sync_file = fcntl(fence, F_DUPFD_CLOEXEC, 0)) < 0)
        ret = -errno;
    if (ret) {
        if (fence >= 0)
            close(fence);
        render_hold(r, ret);
        goto failed;
    }
    job->fence = fence;
    job->target = target;
    ++target->refs;
    ++target->gpu_refs;
    job->depth = depth;
    if (depth) {
        ++depth->refs;
        ++depth->gpu_refs;
    }
    job->id = ++r->engine_jobs;
    job->sequence = r->open_sequence;
    r->open_sequence = 0;
    job->next = r->jobs;
    r->jobs = job;
    *job_id = job->id;
    pthread_mutex_unlock(&r->ws->lock);
    return 0;
failed:
    pthread_mutex_unlock(&r->ws->lock);
    free(job);
    return ret;
}
static bool copyable(const struct poulsback_ws_image *image)
{
    return !image->private_closed && !image->mapped && image->refs < UINT_MAX;
}
static struct poulsback_engine_image engine_image(const struct poulsback_ws_image *image)
{
    return (struct poulsback_engine_image){
        image->info.handle, image->info.stride, image->info.width, image->info.height,
        image->info.format == POULSBACK_IMAGE_FORMAT_ARGB8888 ? POULSBACK_2D_ARGB8888
                                                              : POULSBACK_2D_XRGB8888};
}
int poulsback_ws_render_copy(struct poulsback_ws_render *r, struct poulsback_ws_image *dst,
                             struct poulsback_ws_image *src,
                             const struct poulsback_engine_copy *copy, int *sync_file)
{
    struct poulsback_engine_image d, s;
    struct render_job *job;
    int ret, fence = -1;
    if (!r || !r->engine || !dst || !src || !copy || !sync_file || dst->ws != r->ws ||
        src->ws != r->ws)
        return -EINVAL;
    job = calloc(1, sizeof(*job));
    if (!job)
        return -ENOMEM;
    pthread_mutex_lock(&r->ws->lock);
    if (r->held) {
        ret = r->error ? r->error : -EIO;
        goto failed;
    }
    if (!copyable(dst) || !copyable(src)) {
        ret = -EBUSY;
        goto failed;
    }
    d = engine_image(dst);
    s = engine_image(src);
    /* A failed copy left nothing running: the render stays healthy. */
    ret = poulsback_engine_copy(r->engine, &d, &s, copy, &fence);
    if (ret)
        goto failed;
    POULSBACK_TRACE("copy: %ux%u at %d,%d from %d,%d, va %llx from %llx\n", copy->width,
                    copy->height, copy->x, copy->y, copy->src_x, copy->src_y,
                    (unsigned long long)dst->gpu_va, (unsigned long long)src->gpu_va);
    job->fence = fence;
    job->target = dst;
    ++dst->refs;
    ++dst->gpu_refs;
    if (src != dst) {
        job->source = src;
        ++src->refs;
        ++src->gpu_refs;
    }
    job->id = ++r->engine_jobs;
    job->next = r->jobs;
    r->jobs = job;
    /* The job is tracked by its own fence even if the caller gets none. */
    *sync_file = fcntl(fence, F_DUPFD_CLOEXEC, 0);
    ret = *sync_file < 0 ? -errno : 0;
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
failed:
    pthread_mutex_unlock(&r->ws->lock);
    free(job);
    return ret;
}
int poulsback_ws_render_error(struct poulsback_ws_render *r)
{
    int ret;
    if (!r)
        return -EINVAL;
    pthread_mutex_lock(&r->ws->lock);
    ret = r->error;
    pthread_mutex_unlock(&r->ws->lock);
    return ret;
}
int poulsback_ws_render_check_owner(struct poulsback_ws_render *r, struct poulsback_ws *ws)
{
    struct poulsback_ws_render *candidate;
    int ret = -EXDEV;
    if (!r || !ws)
        return -EINVAL;
    pthread_mutex_lock(&ws->lock);
    for (candidate = ws->renders; candidate; candidate = candidate->next)
        if (candidate == r) {
            ret = candidate->ws != ws ? -ESTALE
                  : candidate->error  ? candidate->error
                  : candidate->held   ? -EIO
                                      : 0;
            break;
        }
    pthread_mutex_unlock(&ws->lock);
    return ret;
}
int poulsback_ws_render_destroy(struct poulsback_ws_render *r)
{
    struct poulsback_ws_render **link;
    struct poulsback_ws *ws;
    int ret;
    if (!r)
        return -EINVAL;
    ws = r->ws;
    pthread_mutex_lock(&ws->lock);
    /* Destroying the engine waits for every frame it submitted. */
    poulsback_engine_destroy(r->engine);
    r->engine = NULL;
    ret = reap_locked(r);
    if (!ret) {
        for (link = &ws->renders; *link != r; link = &(*link)->next) {
        }
        *link = r->next;
        free(r);
    }
    pthread_mutex_unlock(&ws->lock);
    return ret;
}
