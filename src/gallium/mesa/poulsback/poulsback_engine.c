// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_engine.h"
#include "poulsback_bo_uapi.h"
#include "poulsback_exec_uapi.h"
#include "poulsback_frame.h"
#include "poulsback_lost.h"
#include "poulsback_refuse.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

/* Frame slots: one frame's PDS words (TA stream, vertex PDS programs, pixel
 * event) and vertices. A slot is rewritten only after its last job's fence
 * has signalled. Everything else a frame reads is the writer's and built once
 * (poulsback_frame.h). */
#define SLOTS 3u
#define SLOT_WAIT_NS INT64_C(2000000000)
enum slot_buffer { PDS, VERTICES, SLOT_BUFFERS };
static const uint32_t slot_class[SLOT_BUFFERS] = {POULSBACK_BO_CLASS_PDS,
                                                  POULSBACK_BO_CLASS_GENERAL};
static const uint64_t slot_bytes[SLOT_BUFFERS] = {POULSBACK_FRAME_PDS_BYTES,
                                                  POULSBACK_FRAME_VERTEX_BYTES};

struct slot {
    struct poulsback_frame_bo bo[SLOT_BUFFERS];
    uint32_t syncobj;
    bool used;
};
struct poulsback_engine {
    int fd;
    uint32_t context;
    unsigned next;
    struct slot slots[SLOTS];
    /* The last copy's fence; the context's entity runs copies in order. */
    uint32_t copy_syncobj;
    bool copied;
    struct poulsback_frame_writer *writer;
    struct poulsback_frame_bo depth; /* For frames without a depth attachment. */
};

static void bo_free(int fd, struct poulsback_frame_bo *bo)
{
    struct drm_gem_close close_request = {.handle = bo->handle};
    if (bo->map)
        munmap(bo->map, (size_t)bo->size);
    if (bo->handle)
        drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    memset(bo, 0, sizeof(*bo));
}

int poulsback_bo_create(int fd, uint32_t bo_class, uint32_t flags, uint64_t bytes, int prot,
                        struct poulsback_frame_bo *out, int *cleanup)
{
    struct poulsback_bo_create c = {
        .abi = POULSBACK_BO_ABI, .flags = flags, .bo_class = bo_class, .size = bytes};
    void *map = NULL;
    int ret;
    if (drmIoctl(fd, DRM_IOCTL_PB_BO_CREATE, &c))
        return poulsback_lost_note(-errno);
    ret = c.size < bytes || c.size > SIZE_MAX ? -EPROTO : 0;
    if (!ret && prot != PROT_NONE) {
        map = mmap(NULL, (size_t)c.size, prot, MAP_SHARED, fd, (off_t)c.mmap_offset);
        if (map == MAP_FAILED)
            ret = -errno;
    }
    if (ret) {
        struct drm_gem_close close_request = {.handle = c.handle};
        if (drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_request) && cleanup)
            *cleanup = -errno;
        return ret;
    }
    *out = (struct poulsback_frame_bo){c.handle, c.gpu_va, c.size, map};
    return 0;
}

/* Write-combined: the CPU only writes these, the GPU reads them. */
static int bo_alloc(int fd, uint32_t bo_class, uint64_t bytes, struct poulsback_frame_bo *bo)
{
    uint64_t size = 4096;
    int ret;
    while (size < bytes) /* Code BOs are aligned to their size anyway. */
        size <<= 1;
    ret = poulsback_bo_create(fd, bo_class, POULSBACK_BO_CPU_WC, size, PROT_WRITE, bo, NULL);
    return ret ? POULSBACK_REFUSED(ret) : 0;
}

/* A deadline of 0 has passed: the first wait only checks, and the clock is
 * read only for a job that is still running. */
static int syncobj_wait(int fd, uint32_t syncobj, bool *used)
{
    struct timespec now;
    struct drm_syncobj_wait wait = {.handles = (uintptr_t)&syncobj, .count_handles = 1};
    if (!*used)
        return 0;
    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait)) {
        if (errno != ETIME)
            return -errno;
        clock_gettime(CLOCK_MONOTONIC, &now);
        wait.timeout_nsec = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + SLOT_WAIT_NS;
        if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait))
            return -errno;
    }
    *used = false;
    return 0;
}

static int slot_wait(int fd, struct slot *s)
{
    return syncobj_wait(fd, s->syncobj, &s->used);
}

/* The syncobj's fence as a sync_file (the kernel creates it O_CLOEXEC). */
static int export_fence(int fd, uint32_t syncobj, int *sync_file)
{
    struct drm_syncobj_handle exported = {
        .handle = syncobj, .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, .fd = -1};
    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &exported))
        return -errno;
    *sync_file = exported.fd;
    return 0;
}

static int writer_alloc(void *data, uint32_t bo_class, uint64_t bytes,
                        struct poulsback_frame_bo *out)
{
    return bo_alloc(((struct poulsback_engine *)data)->fd, bo_class, bytes, out);
}
static void writer_free(void *data, struct poulsback_frame_bo *bo)
{
    bo_free(((struct poulsback_engine *)data)->fd, bo);
}
static int writer_idle(void *data)
{
    struct poulsback_engine *e = data;
    int ret = 0;
    for (unsigned i = 0; !ret && i < SLOTS; i++)
        ret = slot_wait(e->fd, &e->slots[i]);
    return ret;
}

int poulsback_engine_create(int fd, struct poulsback_engine **out)
{
    struct poulsback_frame_bo_ops ops = {NULL, writer_alloc, writer_free, writer_idle};
    struct poulsback_context_create request = {.abi = POULSBACK_EXEC_ABI};
    struct poulsback_engine *e = calloc(1, sizeof(*e));
    int ret = -ENOMEM;
    if (!e)
        return ret;
    e->fd = fd;
    ops.data = e;
    ret = poulsback_frame_writer_create(&ops, &e->writer);
    if (ret)
        goto fail;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct drm_syncobj_create sync = {0};
        if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
            ret = POULSBACK_REFUSED(-errno);
            goto fail;
        }
        e->slots[i].syncobj = sync.handle;
    }
    {
        struct drm_syncobj_create sync = {0};
        if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sync)) {
            ret = POULSBACK_REFUSED(-errno);
            goto fail;
        }
        e->copy_syncobj = sync.handle;
    }
    if (drmIoctl(fd, DRM_IOCTL_PB_CONTEXT_CREATE, &request)) {
        ret = POULSBACK_REFUSED(poulsback_lost_note(-errno));
        goto fail;
    }
    e->context = request.handle;
    *out = e;
    return 0;
fail:
    poulsback_engine_destroy(e);
    return ret;
}

void poulsback_engine_destroy(struct poulsback_engine *e)
{
    if (!e)
        return;
    /* The writer's buffers only once no slot's job can read them. */
    if (e->writer && !writer_idle(e))
        poulsback_frame_writer_destroy(e->writer);
    for (unsigned i = 0; i < SLOTS; i++) {
        struct slot *s = &e->slots[i];
        struct drm_syncobj_destroy destroy = {.handle = s->syncobj};
        /* A wedged job completes with an error; never free under the GPU. */
        if (slot_wait(e->fd, s))
            continue;
        for (unsigned b = 0; b < SLOT_BUFFERS; b++)
            bo_free(e->fd, &s->bo[b]);
        if (s->syncobj)
            drmIoctl(e->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    if (e->copy_syncobj && !syncobj_wait(e->fd, e->copy_syncobj, &e->copied)) {
        struct drm_syncobj_destroy destroy = {.handle = e->copy_syncobj};
        drmIoctl(e->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
    bo_free(e->fd, &e->depth);
    if (e->context) {
        struct poulsback_context_destroy request = {.abi = POULSBACK_EXEC_ABI,
                                                    .handle = e->context};
        drmIoctl(e->fd, DRM_IOCTL_PB_CONTEXT_DESTROY, &request);
    }
    free(e);
}

static int slot_prepare(struct poulsback_engine *e, struct slot *s)
{
    int ret = slot_wait(e->fd, s);
    for (unsigned b = 0; !ret && b < SLOT_BUFFERS; b++)
        if (!s->bo[b].handle)
            ret = bo_alloc(e->fd, slot_class[b], slot_bytes[b], &s->bo[b]);
    return ret;
}

static int submit(struct poulsback_engine *e, struct slot *s, const struct poulsback_frame_job *j,
                  int *sync_file)
{
    struct poulsback_submit job = {.abi = POULSBACK_EXEC_ABI,
                                   .context = e->context,
                                   .width = j->width,
                                   .height = j->height,
                                   .bo_count = j->bo_count,
                                   .ta_reg_count = j->ta_count,
                                   .raster_reg_count = j->raster_count,
                                   .bos = (uintptr_t)j->bos,
                                   .ta_regs = (uintptr_t)j->ta,
                                   .raster_regs = (uintptr_t)j->raster,
                                   .out_syncobj = s->syncobj};
    if (drmIoctl(e->fd, DRM_IOCTL_PB_SUBMIT, &job))
        return POULSBACK_REFUSED(poulsback_lost_note(-errno));
    s->used = true;
    return export_fence(e->fd, s->syncobj, sync_file);
}

/* A frame without a depth attachment still binds a depth buffer: the ZLS
 * stores it after the scene. One per engine, replaced only when every slot's
 * job (which may use the old one) has finished. */
static int scratch_depth(struct poulsback_engine *e, uint64_t bytes,
                         struct poulsback_engine_buffer *out)
{
    int ret = 0;
    if (e->depth.size < bytes) {
        for (unsigned i = 0; !ret && i < SLOTS; i++)
            ret = slot_wait(e->fd, &e->slots[i]);
        if (!ret) {
            bo_free(e->fd, &e->depth);
            ret = bo_alloc(e->fd, POULSBACK_BO_CLASS_DEPTH, bytes, &e->depth);
        }
    }
    *out = (struct poulsback_engine_buffer){e->depth.handle, 0, e->depth.gpu_va, e->depth.size};
    return ret;
}

/* Opens a frame in the next slot, after that slot's last job. */
static int frame_open(struct poulsback_engine *e)
{
    if (!poulsback_frame_open(e->writer)) {
        struct slot *s = &e->slots[e->next];
        int ret = slot_prepare(e, s);
        if (ret)
            return ret;
        poulsback_frame_begin(e->writer, &s->bo[PDS], &s->bo[VERTICES]);
    }
    return 0;
}
int poulsback_engine_draw(struct poulsback_engine *e, const poulsback_frame_shape *shape,
                          const struct poulsback_frame_vertices *vertices)
{
    int ret = frame_open(e);
    return ret ? ret : poulsback_frame_draw(e->writer, shape, vertices);
}
int poulsback_engine_draw_gpu(struct poulsback_engine *e, const poulsback_frame_shape *shape,
                              const struct poulsback_frame_gpu_draw *draw)
{
    int ret = frame_open(e);
    return ret ? ret : poulsback_frame_draw_gpu(e->writer, shape, draw);
}
int poulsback_engine_stream(struct poulsback_engine *e, uint32_t bytes, uint32_t alignment,
                            void **cpu, uint64_t *address, uint32_t *handle)
{
    int ret = frame_open(e);
    return ret ? ret : poulsback_frame_stream(e->writer, bytes, alignment, cpu, address, handle);
}
void poulsback_engine_sequence(struct poulsback_engine *e, uint64_t *address, uint32_t *handle)
{
    poulsback_frame_sequence(e->writer, address, handle);
}

void poulsback_engine_discard(struct poulsback_engine *e)
{
    poulsback_frame_discard(e->writer);
}
void poulsback_engine_load(struct poulsback_engine *e)
{
    poulsback_frame_load(e->writer);
}
uint32_t poulsback_engine_draws(const struct poulsback_engine *e)
{
    return poulsback_frame_draws(e->writer);
}

int poulsback_engine_submit(struct poulsback_engine *e,
                            const struct poulsback_engine_buffer *target,
                            const struct poulsback_engine_buffer *depth, int *sync_file)
{
    struct poulsback_engine_buffer no_depth;
    struct poulsback_frame_job job;
    struct slot *s = &e->slots[e->next];
    uint32_t width, height;
    uint64_t depth_bytes;
    int ret = poulsback_frame_extent(e->writer, &width, &height, &depth_bytes);
    if (!ret && !depth) {
        ret = scratch_depth(e, depth_bytes, &no_depth);
        depth = &no_depth;
    }
    if (ret) {
        poulsback_frame_abort(e->writer);
        return ret == -EINVAL ? POULSBACK_REFUSED(ret) : ret;
    }
    ret = poulsback_frame_end(
        e->writer,
        &(struct poulsback_frame_buffer){target->handle, target->stride, target->gpu_va,
                                         target->size},
        &(struct poulsback_frame_buffer){depth->handle, depth->stride, depth->gpu_va, depth->size},
        &job);
    if (!ret)
        ret = submit(e, s, &job, sync_file);
    if (!ret)
        e->next = (e->next + 1u) % SLOTS;
    return ret;
}

static struct poulsback_2d_surface copy_surface(const struct poulsback_engine_image *image,
                                                uint32_t bo)
{
    return (struct poulsback_2d_surface){
        bo, 0, image->stride, image->width, image->height, image->format, {0, 0}};
}

int poulsback_engine_copy(struct poulsback_engine *e, const struct poulsback_engine_image *dst,
                          const struct poulsback_engine_image *src,
                          const struct poulsback_engine_copy *copy, int *sync_file)
{
    /* One image is one BO and one surface: the kernel refuses two views of
     * one BO, and copies a surface onto itself as if through a temporary. */
    bool same = dst->handle == src->handle;
    struct poulsback_submit_bo bos[2] = {{dst->handle, 0}, {src->handle, 0}};
    struct poulsback_2d_surface surfaces[2] = {copy_surface(dst, 0), copy_surface(src, 1)};
    struct poulsback_2d_op op = {.kind = POULSBACK_2D_COPY,
                                 .rop = 3, /* GXcopy */
                                 .dst = 0,
                                 .src = same ? 0u : 1u,
                                 .rect = {copy->x, copy->y, copy->width, copy->height},
                                 .src_x = copy->src_x,
                                 .src_y = copy->src_y};
    struct poulsback_submit_2d job = {.abi = POULSBACK_EXEC_ABI,
                                      .context = e->context,
                                      .bo_count = same ? 1u : 2u,
                                      .surface_count = same ? 1u : 2u,
                                      .op_count = 1,
                                      .bos = (uintptr_t)bos,
                                      .surfaces = (uintptr_t)surfaces,
                                      .ops = (uintptr_t)&op,
                                      .out_syncobj = e->copy_syncobj};
    int ret;
    if (drmIoctl(e->fd, DRM_IOCTL_PB_SUBMIT_2D, &job))
        return POULSBACK_REFUSED(poulsback_lost_note(-errno));
    e->copied = true;
    ret = export_fence(e->fd, e->copy_syncobj, sync_file);
    /* Without its fence the caller cannot track the job: it ends here. */
    if (ret) {
        int wait = syncobj_wait(e->fd, e->copy_syncobj, &e->copied);
        if (wait)
            ret = wait;
    }
    return ret;
}
