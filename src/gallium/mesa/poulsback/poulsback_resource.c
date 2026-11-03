// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_resource.h"
#include "poulsback_image.h"
#include "poulsback_refuse.h"
#include "poulsback_selfcheck.h"
#include "poulsback_slab.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "frontend/winsys_handle.h"
#include "util/u_inlines.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

struct poulsback_resource {
    struct pipe_resource base;
    struct poulsback_resource *next;
    struct poulsback_ws_image *image;
    struct poulsback_ws_depth *native_depth;
    int native_depth_error;
    struct poulsback_image_info info;
    struct poulsback_depth_capture_description depth_capture;
    struct poulsback_slab_entry *storage; /* A buffer's; cpu is its CPU address. */
    uint8_t *cpu;
    size_t cpu_bytes;
    unsigned mapped; /* Transfers open on it. */
    bool destroy_pending;
};
struct poulsback_transfer {
    struct pipe_transfer base;
    struct poulsback_transfer *next;
    struct poulsback_resource *owner;
    bool held;
};
struct poulsback_resource_domain {
    struct pipe_screen *screen;
    struct poulsback_ws *ws;
    struct poulsback_slab *slab; /* Buffer storage, BOs of ws. */
    struct poulsback_resource *resources;
    struct poulsback_transfer *transfers;
    pthread_mutex_t lock;
    int error, cleanup_error;
    size_t cpu_bytes;
};
static void *map_buffer(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                        const struct pipe_box *, struct pipe_transfer **);
static void flush_buffer(struct pipe_context *, struct pipe_transfer *, const struct pipe_box *);
static void buffer_subdata(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                           unsigned, const void *);
static void clear_buffer(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                         const void *, int);
static void copy_region(struct pipe_context *, struct pipe_resource *, unsigned, unsigned, unsigned,
                        unsigned, struct pipe_resource *, unsigned, const struct pipe_box *);

static struct poulsback_resource_domain *domain(struct pipe_screen *screen)
{
    return screen ? screen->winsys_priv : NULL;
}
/* Gallium passes a context only resources its screen created, so the screen
 * check admits the cast; the self-check build also walks the canonical list. */
static struct poulsback_resource *find_resource(struct poulsback_resource_domain *d,
                                                struct pipe_resource *base)
{
    struct poulsback_resource *r = (struct poulsback_resource *)base;
    if (!base || base->screen != d->screen)
        return NULL;
    if (POULSBACK_SELFCHECK) {
        struct poulsback_resource *p = d->resources;
        while (p && p != r)
            p = p->next;
        if (!p)
            return NULL;
    }
    return r->destroy_pending ? NULL : r;
}
static void error(struct poulsback_resource_domain *d, int ret)
{
    if (ret && !d->error)
        d->error = ret;
}
static uint32_t image_format(enum pipe_format format)
{
    switch (format) {
    case PIPE_FORMAT_B8G8R8A8_UNORM:
        return POULSBACK_IMAGE_FORMAT_ARGB8888;
    case PIPE_FORMAT_B8G8R8X8_UNORM:
        return POULSBACK_IMAGE_FORMAT_XRGB8888;
    default:
        return 0;
    }
}
static bool depth_capture_template(const struct pipe_resource *t,
                                   struct poulsback_depth_capture_description *out)
{
    uint32_t format;
    if (!t || t->target != PIPE_TEXTURE_2D || !t->width0 || !t->height0 || t->width0 > 2048 ||
        t->height0 > 2048 || t->depth0 != 1 || t->array_size != 1 || t->last_level ||
        t->nr_samples > 1 || t->nr_storage_samples > 1 || t->nr_sparse_levels ||
        t->compression_rate || t->flags || t->next || t->usage != PIPE_USAGE_DEFAULT ||
        /* Mesa adds SAMPLER_VIEW to every renderbuffer; sampling depth is
         * still refused at draw time. */
        (t->bind & ~PIPE_BIND_SAMPLER_VIEW) != PIPE_BIND_DEPTH_STENCIL)
        return false;
    if (t->format == PIPE_FORMAT_Z16_UNORM)
        format = POULSBACK_DEPTH_CAPTURE_D16;
    else if (t->format == PIPE_FORMAT_Z24_UNORM_S8_UINT)
        format = POULSBACK_DEPTH_CAPTURE_D24S8;
    else if (t->format == PIPE_FORMAT_Z24X8_UNORM)
        format = POULSBACK_DEPTH_CAPTURE_D24X8;
    else
        return false;
    *out = (struct poulsback_depth_capture_description){POULSBACK_DEPTH_CAPTURE_VERSION,
                                                        format,
                                                        t->width0,
                                                        t->height0,
                                                        t->nr_samples,
                                                        t->nr_storage_samples};
    return true;
}
static bool depth_capture_spans_overlap(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return !a || !b || na > UINTPTR_MAX - x || nb > UINTPTR_MAX - y || (x < y + nb && y < x + na);
}
/* Explicit internal construction; format/capability callbacks stay unchanged.
 * screen.resource_create also uses this metadata constructor for the normal
 * class: single-sample Z24X8 or Z24S8 up to 2048x2048. D16 and other
 * descriptors stay explicit.
 * Only canonical single-level D16/D24S8/D24X8 descriptors
 * with DEPTH_STENCIL binding are accepted. No backing allocation, import,
 * mapping, export, clear or GPU effect occurs. The returned pipe_resource has
 * one ordinary caller reference and participates in domain lifetime checks.
 * Errors preserve output. The template is stable during this call and output
 * storage is disjoint from it and from prior owned objects. */
static int poulsback_resource_depth_capture_create(struct pipe_screen *screen,
                                                   const struct pipe_resource *template,
                                                   struct pipe_resource **out)
{
    struct poulsback_resource_domain *d = domain(screen);
    struct poulsback_depth_capture_description description;
    struct pipe_resource frozen;
    struct poulsback_resource *r;
    if (!d || d->screen != screen ||
        depth_capture_spans_overlap(template, sizeof(*template), out, sizeof(*out)))
        return -EINVAL;
    memcpy(&frozen, template, sizeof(frozen));
    if (!depth_capture_template(&frozen, &description) ||
        (frozen.screen && frozen.screen != screen))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    r = CALLOC_STRUCT_CL(poulsback_resource);
    if (!r) {
        pthread_mutex_unlock(&d->lock);
        return -ENOMEM;
    }
    if (memcmp(template, &frozen, sizeof(frozen))) {
        FREE_CL(r);
        pthread_mutex_unlock(&d->lock);
        return -ESTALE;
    }
    r->base = frozen;
    pipe_reference_init(&r->base.reference, 1);
    r->base.screen = screen;
    r->depth_capture = description;
    r->next = d->resources;
    d->resources = r;
    pthread_mutex_unlock(&d->lock);
    *out = &r->base;
    return 0;
}
int poulsback_resource_depth_capture_description(struct pipe_resource *base,
                                                 struct poulsback_depth_capture_description *out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    struct poulsback_depth_capture_description description;
    int ret = -EINVAL;
    if (depth_capture_spans_overlap(base, sizeof(*base), out, sizeof(*out)) ||
        !(d = domain(base->screen)) || d->screen != base->screen)
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (r && r->depth_capture.version == POULSBACK_DEPTH_CAPTURE_VERSION && !r->image && !r->cpu &&
        !r->cpu_bytes && depth_capture_template(&r->base, &description) &&
        !memcmp(&description, &r->depth_capture, sizeof(description))) {
        *out = r->depth_capture;
        ret = 0;
    }
    pthread_mutex_unlock(&d->lock);
    return ret;
}
static bool depth_matches(struct poulsback_resource_domain *d, struct poulsback_resource *r,
                          const struct poulsback_depth_capture_description *before)
{
    struct poulsback_depth_capture_description after;
    return r->base.screen == d->screen && !r->image && !r->cpu && !r->cpu_bytes &&
           depth_capture_template(&r->base, &after) && !memcmp(before, &after, sizeof(after)) &&
           !memcmp(before, &r->depth_capture, sizeof(*before));
}
int poulsback_resource_depth_acquire(struct pipe_resource *base, struct poulsback_ws_render *render,
                                     struct poulsback_ws_depth **out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    struct poulsback_depth_capture_description before;
    int ret = -EINVAL;
    if (!render || !out || *out ||
        depth_capture_spans_overlap(base, sizeof(*base), out, sizeof(*out)) ||
        !(d = domain(base->screen)) || d->screen != base->screen)
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (!r)
        goto done;
    if (r->native_depth_error) {
        ret = r->native_depth_error;
        goto done;
    }
    before = r->depth_capture;
    if (!depth_matches(d, r, &before))
        goto done;
    if ((before.format != POULSBACK_DEPTH_CAPTURE_D24X8 &&
         before.format != POULSBACK_DEPTH_CAPTURE_D24S8) ||
        before.samples || before.storage_samples) {
        ret = -EOPNOTSUPP;
        goto done;
    }
    ret = poulsback_ws_render_check_owner(render, d->ws);
    if (ret)
        goto done;
    if (poulsback_ws_render_depth_scene_version(render) != POULSBACK_RENDER_DEPTH_SCENE_ABI) {
        ret = -EOPNOTSUPP;
        goto done;
    }
    if (!r->native_depth) {
        ret = poulsback_ws_depth_create(render, before.width, before.height, &r->native_depth);
        if (ret) {
            /* An allocation refusal has no kernel owner. An uncertain ioctl
             * retains the ws list entry even if no depth pointer was returned. */
            if (poulsback_ws_render_error(render)) {
                r->native_depth_error = ret;
                error(d, ret);
            }
            goto done;
        }
        ret =
            depth_matches(d, r, &before) ? poulsback_ws_render_check_owner(render, d->ws) : -ESTALE;
        if (ret) {
            r->native_depth_error = ret;
            error(d, ret);
            goto done;
        }
    }
    ret = poulsback_ws_depth_ref(r->native_depth);
    if (!ret) {
        ret =
            depth_matches(d, r, &before) ? poulsback_ws_render_check_owner(render, d->ws) : -ESTALE;
        if (ret) {
            /* Only the just-acquired unpublished borrow is dropped. The
             * resource anchor remains owned across this source/health loss. */
            int cleanup = poulsback_ws_depth_unref(r->native_depth);
            r->native_depth_error = ret;
            error(d, ret);
            if (cleanup)
                d->cleanup_error = cleanup;
        } else
            *out = r->native_depth;
    }
done:
    pthread_mutex_unlock(&d->lock);
    return ret;
}
static bool layout(const struct pipe_resource *t, uint32_t stride, struct poulsback_ws_layout *out)
{
    uint64_t row;
    /* LINEAR/shared colour storage and CPU transfers are implemented
     * independently of rendering; a texture is sampled as a stride surface
     * (st/mesa marks textures TEXTURING_MORE_LIKELY, a hint). */
    const unsigned allowed = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED | PIPE_BIND_LINEAR |
                             PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SAMPLER_VIEW;
    if (!t || t->target != PIPE_TEXTURE_2D || !image_format(t->format) || !t->width0 ||
        !t->height0 || t->depth0 != 1 || t->array_size != 1 || t->last_level || t->nr_samples > 1 ||
        t->nr_storage_samples > 1 || t->nr_sparse_levels || t->compression_rate ||
        (t->flags & ~PIPE_RESOURCE_FLAG_TEXTURING_MORE_LIKELY) ||
        (t->bind & ~allowed) || t->next)
        return false;
    row = ((uint64_t)t->width0 * 4u + 127u) & ~UINT64_C(127);
    if (!stride) {
        if (row > UINT32_MAX)
            return false;
        stride = (uint32_t)row;
    }
    if ((stride & 127u) || (uint64_t)t->width0 * 4u > stride ||
        (uint64_t)stride * t->height0 > POULSBACK_IMAGE_MAX_BYTES)
        return false;
    *out =
        (struct poulsback_ws_layout){t->width0, t->height0, stride, image_format(t->format), 0, 0};
    return true;
}
static void unlink_resource(struct poulsback_resource_domain *d, struct poulsback_resource *r)
{
    struct poulsback_resource **p;
    for (p = &d->resources; *p != r; p = &(*p)->next) {
    }
    *p = r->next;
    d->cpu_bytes -= r->cpu_bytes;
    if (r->storage)
        poulsback_slab_free(d->slab, r->storage);
    FREE_CL(r);
}
static bool same_storage(const struct poulsback_resource *a, const struct poulsback_resource *b)
{
    return a == b || (a->image && a->image == b->image);
}
static int release_storage(struct poulsback_resource *r)
{
    int ret;
    if (r->native_depth_error)
        return r->native_depth_error;
    if (!r->native_depth)
        return r->image ? poulsback_ws_image_unref(r->image) : 0;
    ret = poulsback_ws_depth_unref(r->native_depth);
    if (ret)
        r->native_depth_error = ret; /* A final depth destroy is never retried. */
    else
        r->native_depth = NULL;
    return ret;
}
static void destroy_resource(struct pipe_screen *screen, struct pipe_resource *base)
{
    struct poulsback_resource_domain *d = domain(screen);
    struct poulsback_resource *r;
    int ret;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (!r) {
        error(d, -EINVAL);
    } else {
        r->destroy_pending = true;
        ret = release_storage(r);
        if (!ret)
            unlink_resource(d, r);
        else {
            error(d, ret);
            d->cleanup_error = ret;
        }
    }
    pthread_mutex_unlock(&d->lock);
}
/* External uses an image accepts, exported or imported: FRAMEBUFFER_WRITE
 * (rendered to; EGL's dma-buf import asks it of every render target) and
 * EXPLICIT_FLUSH (DRI3 back buffers). */
#define SUPPORTED_HANDLE_USAGE                                                                     \
    (PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE | PIPE_HANDLE_USAGE_EXPLICIT_FLUSH)
/* A window's back buffer goes to a SCANOUT image only when the display could
 * flip it: XRGB in exactly the display's geometry. Present flips it at any
 * swap interval (the X driver takes async presents as vblank-latched flips);
 * other windows keep write-back images, which Present copies fast. */
static bool scanout(const struct poulsback_resource_domain *d, const struct pipe_resource *t,
                    const struct poulsback_ws_layout *l)
{
    struct poulsback_ws_layout display;
    poulsback_ws_display(d->ws, &display);
    return (t->bind & PIPE_BIND_SCANOUT) && t->format == PIPE_FORMAT_B8G8R8X8_UNORM &&
           display.width && l->width == display.width && l->height == display.height &&
           l->stride == display.stride && l->format == display.format;
}
static struct pipe_resource *construct(struct pipe_screen *screen, const struct pipe_resource *t,
                                       const struct winsys_handle *handle, unsigned usage)
{
    struct poulsback_resource_domain *d = domain(screen);
    struct poulsback_ws_layout image_layout;
    struct poulsback_resource *r;
    int ret;
    if (!d)
        return NULL;
    pthread_mutex_lock(&d->lock);
    if (t && t->target == PIPE_BUFFER) {
        /* VERTEX_STATE: display lists' combined vertex/index buffers, drawn as
         * ordinary buffers (no draw_vertex_state cap). */
        const unsigned allowed = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER |
                                 PIPE_BIND_CONSTANT_BUFFER | PIPE_BIND_VERTEX_STATE;
        if (handle || usage || t->format != PIPE_FORMAT_R8_UNORM || !t->width0 ||
            t->width0 > POULSBACK_IMAGE_MAX_BYTES || t->height0 != 1 || t->depth0 != 1 ||
            t->array_size != 1 || t->last_level || t->nr_samples > 1 || t->nr_storage_samples > 1 ||
            t->nr_sparse_levels || t->compression_rate ||
            (t->flags & ~(unsigned)PIPE_RESOURCE_FLAG_SINGLE_THREAD_USE) || t->next ||
            (t->bind & ~allowed) || t->width0 > POULSBACK_IMAGE_TOTAL_BYTES - d->cpu_bytes ||
            !d->slab) {
            error(d, -EINVAL);
            pthread_mutex_unlock(&d->lock);
            return NULL;
        }
        r = CALLOC_STRUCT_CL(poulsback_resource);
        if (!r) {
            error(d, -ENOMEM);
            pthread_mutex_unlock(&d->lock);
            return NULL;
        }
        ret = poulsback_slab_alloc(d->slab, t->width0, &r->storage);
        if (ret) {
            FREE_CL(r);
            error(d, ret);
            pthread_mutex_unlock(&d->lock);
            return NULL;
        }
        r->cpu = r->storage->cpu;
        r->cpu_bytes = t->width0;
        d->cpu_bytes += r->cpu_bytes;
        r->base = *t;
        pipe_reference_init(&r->base.reference, 1);
        r->base.screen = screen;
        r->next = d->resources;
        d->resources = r;
        pthread_mutex_unlock(&d->lock);
        return &r->base;
    }
    /* These flags describe image ownership only; no implicit GPU flush is
     * implemented. Imports retain the caller's FD and accept plane 0, LINEAR
     * or with no modifier given (DRM_FORMAT_MOD_INVALID: the implicit layout,
     * which is LINEAR). */
    if ((usage & ~SUPPORTED_HANDLE_USAGE) ||
        !layout(t, handle ? handle->stride : 0, &image_layout) ||
        (handle && (handle->type != WINSYS_HANDLE_TYPE_FD || handle->handle > INT_MAX ||
                    handle->layer || handle->plane || handle->offset ||
                    (handle->modifier != DRM_FORMAT_MOD_LINEAR &&
                     handle->modifier != DRM_FORMAT_MOD_INVALID) ||
                    handle->array_stride || handle->image_stride ||
                    (handle->format && handle->format != t->format)))) {
        error(d, -EINVAL);
        pthread_mutex_unlock(&d->lock);
        return NULL;
    }
    r = CALLOC_STRUCT_CL(poulsback_resource);
    if (!r) {
        error(d, -ENOMEM);
        pthread_mutex_unlock(&d->lock);
        return NULL;
    }
    r->base = *t;
    pipe_reference_init(&r->base.reference, 1);
    r->base.screen = screen;
    if (handle)
        ret = poulsback_ws_image_import(d->ws, (int)handle->handle, &image_layout, &r->image);
    else {
        bool flippable = scanout(d, t, &image_layout);
        ret = poulsback_ws_image_create(d->ws, &image_layout, flippable, &r->image);
        if (!ret && flippable)
            POULSBACK_TRACE("resource %ux%u in a SCANOUT image (write-combined)\n",
                            image_layout.width, image_layout.height);
    }
    if (ret) {
        error(d, ret);
        FREE_CL(r);
        pthread_mutex_unlock(&d->lock);
        return NULL;
    }
    r->next = d->resources;
    d->resources = r;
    ret = poulsback_ws_image_info(r->image, &r->info);
    if (!ret && handle && handle->size && handle->size != r->info.size)
        ret = -EPROTO;
    if (ret) {
        int cleanup;
        r->destroy_pending = true;
        error(d, ret);
        cleanup = poulsback_ws_image_unref(r->image);
        if (!cleanup)
            unlink_resource(d, r);
        else
            d->cleanup_error = cleanup;
        pthread_mutex_unlock(&d->lock);
        return NULL;
    }
    pthread_mutex_unlock(&d->lock);
    return &r->base;
}
static bool depth_format(enum pipe_format f)
{
    return f == PIPE_FORMAT_Z24X8_UNORM || f == PIPE_FORMAT_Z24_UNORM_S8_UINT;
}
static bool normal_depth_class(const struct pipe_resource *t)
{
    return depth_format(t->format) && !t->nr_samples && !t->nr_storage_samples && t->width0 &&
           t->height0 && t->width0 <= 2048 && t->height0 <= 2048;
}
static struct pipe_resource *create_resource(struct pipe_screen *screen,
                                             const struct pipe_resource *t)
{
    if (t && depth_format(t->format)) {
        struct poulsback_resource_domain *d = domain(screen);
        struct pipe_resource *depth = NULL;
        int ret;
        if (!d)
            return NULL;
        ret = normal_depth_class(t) ? poulsback_resource_depth_capture_create(screen, t, &depth)
                                    : -EOPNOTSUPP;
        /* The canonical constructor checks the complete copied template.
         * Keep this callback's narrower class even if its input changed
         * between dispatch and that copy. No native allocation exists yet. */
        if (!ret && !normal_depth_class(depth)) {
            pipe_resource_reference(&depth, NULL);
            ret = -ESTALE;
        }
        if (ret) {
            pthread_mutex_lock(&d->lock);
            error(d, ret);
            pthread_mutex_unlock(&d->lock);
        }
        return depth;
    }
    return construct(screen, t, NULL, 0);
}
static struct pipe_resource *create_resource_with_modifiers(struct pipe_screen *screen,
                                                            const struct pipe_resource *t,
                                                            const uint64_t *modifiers, int count)
{
    struct poulsback_resource_domain *d = domain(screen);
    int i;
    if (!d)
        return NULL;
    if (!t || t->target != PIPE_TEXTURE_2D || !modifiers || count <= 0)
        goto unsupported;
    /* Mesa's callback contract rejects a list containing an unsupported
     * entry. A LINEAR alternative cannot authorize another layout. */
    for (i = 0; i < count; i++)
        if (modifiers[i] != POULSBACK_IMAGE_MOD_LINEAR)
            goto unsupported;
    return construct(screen, t, NULL, 0);
unsupported:
    pthread_mutex_lock(&d->lock);
    error(d, -EINVAL);
    pthread_mutex_unlock(&d->lock);
    return NULL;
}
/* The one shareable layout, LINEAR, for the formats a window shows: Mesa's
 * DRI3 loader keeps a window's own (flippable) modifier list only when the
 * driver names it here. */
static void query_dmabuf_modifiers(struct pipe_screen *screen, enum pipe_format format, int max,
                                   uint64_t *modifiers, unsigned int *external_only, int *count)
{
    (void)screen;
    *count = format == PIPE_FORMAT_B8G8R8X8_UNORM || format == PIPE_FORMAT_B8G8R8A8_UNORM;
    if (*count && max > 0) {
        modifiers[0] = POULSBACK_IMAGE_MOD_LINEAR;
        if (external_only)
            external_only[0] = 0;
    }
}
static bool is_dmabuf_modifier_supported(struct pipe_screen *screen, uint64_t modifier,
                                         enum pipe_format format, bool *external_only)
{
    uint64_t linear;
    int count;
    query_dmabuf_modifiers(screen, format, 1, &linear, NULL, &count);
    if (external_only)
        *external_only = false;
    return count && modifier == linear;
}
static struct pipe_resource *import_resource(struct pipe_screen *screen,
                                             const struct pipe_resource *t,
                                             struct winsys_handle *handle, unsigned usage)
{
    return handle ? construct(screen, t, handle, usage) : NULL;
}
static bool export_resource(struct pipe_screen *screen, struct pipe_context *ctx,
                            struct pipe_resource *base, struct winsys_handle *handle,
                            unsigned usage)
{
    struct poulsback_resource_domain *d = domain(screen);
    struct poulsback_resource *r;
    struct winsys_handle result;
    int fd, ret = -EINVAL;
    (void)ctx;
    if (!d || !handle)
        return false;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    /* FRAMEBUFFER_WRITE describes expected external use. Native LINEAR
     * storage needs no layout conversion; the existing map/GPU exclusions
     * still govern export. EXPLICIT_FLUSH (DRI3 back buffers) needs no cache
     * handoff in flush_resource: the GPU writes images snooped and the fence
     * orders them before any external read. KMS handles are never returned. */
    if (r && r->image && !(usage & ~SUPPORTED_HANDLE_USAGE) && !handle->layer && !handle->plane &&
        handle->type == WINSYS_HANDLE_TYPE_FD) {
        struct poulsback_transfer *t;
        bool mapped = false;
        for (t = d->transfers; t; t = t->next)
            mapped |= same_storage(t->owner, r);
        ret = mapped ? -EBUSY : poulsback_ws_image_export(r->image, &fd);
        if (!ret) {
            result = (struct winsys_handle){0};
            result.type = handle->type;
            result.handle = (uint32_t)fd;
            result.stride = r->info.stride;
            /* winsys_handle uses enum pipe_format, not DRM/UAPI fourcc. */
            result.format = r->base.format;
            result.modifier = r->info.modifier;
            result.size = r->info.size;
            *handle = result;
        }
    }
    if (ret)
        fprintf(stderr, "poulsback: resource export (type %u usage 0x%x) refused (%d)\n",
                handle->type, usage, ret);
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
    return !ret;
}
static bool resource_get_param(struct pipe_screen *screen, struct pipe_context *ctx,
                               struct pipe_resource *base, unsigned plane, unsigned layer,
                               unsigned level, enum pipe_resource_param param, unsigned usage,
                               uint64_t *value)
{
    struct poulsback_resource_domain *d = domain(screen);
    struct poulsback_resource *r;
    const bool supported_usage = !(usage & ~SUPPORTED_HANDLE_USAGE);
    uint64_t result = 0;
    int ret = -EINVAL;
    if (!d || !value)
        return false;
    if (!plane && !layer && !level && supported_usage &&
        (param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD ||
         param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS)) {
        struct winsys_handle handle = {.type = param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD
                                                   ? WINSYS_HANDLE_TYPE_FD
                                                   : WINSYS_HANDLE_TYPE_KMS};
        /* Keep the existing alias/map/GPU exclusions. FD export owns a
         * CLOEXEC duplicate; KMS is refused. Never return the private
         * drm_file's GEM handles. */
        if (!export_resource(screen, ctx, base, &handle, usage))
            return false;
        *value = handle.handle;
        return true;
    }
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (r && r->image && !plane && !layer && !level && supported_usage) {
        switch (param) {
        case PIPE_RESOURCE_PARAM_NPLANES:
            result = 1;
            break;
        case PIPE_RESOURCE_PARAM_STRIDE:
            result = r->info.stride;
            break;
        case PIPE_RESOURCE_PARAM_OFFSET:
            result = 0;
            break;
        case PIPE_RESOURCE_PARAM_MODIFIER:
            result = r->info.modifier;
            break;
        default:
            goto done;
        }
        ret = 0;
    }
done:
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
    if (!ret)
        *value = result;
    return !ret;
}
static bool valid_box(const struct poulsback_resource *r, const struct pipe_box *b)
{
    return b && b->x >= 0 && b->y >= 0 && b->z == 0 && b->width > 0 && b->height > 0 &&
           b->depth == 1 && (uint64_t)(unsigned)b->x + (unsigned)b->width <= r->base.width0 &&
           (uint64_t)(unsigned)b->y + (unsigned)b->height <= r->base.height0;
}
/* Storage only completed jobs read is idle again (poulsback_ws_completed). */
static void buffers_completed(struct poulsback_resource_domain *d)
{
    poulsback_slab_complete(d->slab, poulsback_ws_completed(d->ws));
}
/* Whether a submitted job uses R's storage. */
static bool storage_busy(const struct poulsback_resource_domain *d,
                         const struct poulsback_resource *r)
{
    return r->image          ? poulsback_ws_image_gpu_busy(r->image)
           : r->native_depth ? poulsback_ws_depth_gpu_busy(r->native_depth)
                             : poulsback_slab_busy(d->slab, r->storage);
}
/* Waits until no job uses R's storage: an idle flush submits the open frame
 * and reaps retired jobs (the kernel retires shortly after the fence). The
 * lock is dropped meanwhile, so *R is looked up again. */
static int wait_idle(struct pipe_context *ctx, struct poulsback_resource_domain *d,
                     struct pipe_resource *base, struct poulsback_resource **r)
{
    for (unsigned waited = 0; (buffers_completed(d), storage_busy(d, *r)); waited++) {
        if (waited >= 2000)
            return -EBUSY;
        pthread_mutex_unlock(&d->lock);
        ctx->flush(ctx, NULL, 0);
        if (waited)
            usleep(1000);
        pthread_mutex_lock(&d->lock);
        *r = find_resource(d, base);
        if (!*r)
            return -EINVAL;
    }
    return 0;
}
static void *map_texture(struct pipe_context *ctx, struct pipe_resource *base, unsigned level,
                         unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct poulsback_resource *r;
    struct poulsback_transfer *t, *p;
    enum poulsback_ws_cpu_access access;
    void *address, *result = NULL;
    uint64_t size, offset, end;
    int ret = -EINVAL;
    if (!d || !out) {
        fprintf(stderr, "poulsback: map: no domain (%d) or transfer (%d)\n", !d, !out);
        return NULL;
    }
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    /* the discard hints need nothing: the contents are kept either way */
    if (!r || (!r->image && !r->native_depth) || level || !valid_box(r, box) ||
        !(usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) ||
        (usage & ~(unsigned)(PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE |
                             PIPE_MAP_DISCARD_WHOLE_RESOURCE)) ||
        (usage & UINT32_C(0xff000000)))
        goto done;
    /* A blocking map waits for the image's queued GPU use and for the jobs
     * that still hold it. */
    ret = wait_idle(ctx, d, base, &r);
    if (ret)
        goto done;
    for (p = d->transfers; p; p = p->next)
        if (same_storage(p->owner, r)) {
            ret = -EBUSY;
            goto done;
        }
    t = calloc(1, sizeof(*t));
    if (!t) {
        ret = -ENOMEM;
        goto done;
    }
    t->owner = r;
    t->base.level = (uint8_t)level;               /* The guard above requires level==0. */
    t->base.usage = usage & UINT32_C(0x00ffffff); /* High bits already refused. */
    t->base.box = *box;
    /* a depth attachment: its stored planes (poulsback_ws_depth_cpu_begin) */
    if (!r->image) {
        uint32_t stride = 0;
        ret = poulsback_ws_depth_cpu_begin(r->native_depth, &address, &stride);
        if (ret) {
            free(t);
            goto done;
        }
        t->base.stride = stride;
        t->base.layer_stride = (uintptr_t)stride * ((r->base.height0 + 31u) & ~31u);
        pipe_resource_reference(&t->base.resource, base);
        result = (uint8_t *)address + (size_t)(unsigned)box->y * stride +
                 (size_t)(unsigned)box->x * 4u;
        goto linked;
    }
    t->base.stride = r->info.stride;
    t->base.layer_stride = (uintptr_t)r->info.stride * r->info.height;
    pipe_resource_reference(&t->base.resource, base);
    access =
        (enum poulsback_ws_cpu_access)(((usage & PIPE_MAP_READ) ? POULSBACK_WS_CPU_READ : 0) |
                                       ((usage & PIPE_MAP_WRITE) ? POULSBACK_WS_CPU_WRITE : 0));
    ret = poulsback_ws_image_cpu_begin(r->image, access, &address, &size);
    if (!ret) {
        offset = (uint64_t)(unsigned)box->y * r->info.stride + (unsigned)box->x * 4u;
        end = offset + (uint64_t)(unsigned)(box->height - 1) * r->info.stride +
              (unsigned)box->width * 4u;
        if (size < r->info.size || end > size || size > SIZE_MAX)
            ret = -EPROTO;
        else
            result = (uint8_t *)address + (size_t)offset;
    }
    if (ret) {
        fprintf(stderr, "poulsback: map: image CPU access refused (%d) usage 0x%x\n", ret, usage);
        /* This domain exclusively owns its image client. No other mapping
         * exists after the transfer-list check. END can therefore release
         * only this begin's CPU orphan; EINVAL means no mapping was created. */
        int cleanup = poulsback_ws_image_cpu_end(r->image);
        if (!cleanup || cleanup == -EINVAL) {
            struct pipe_resource *reference = t->base.resource;
            free(t);
            error(d, ret);
            pthread_mutex_unlock(&d->lock);
            pipe_resource_reference(&reference, NULL);
            return NULL;
        }
        t->held = true;
        d->cleanup_error = cleanup;
    }
linked:
    t->owner->mapped++;
    t->next = d->transfers;
    d->transfers = t;
    if (!ret)
        *out = &t->base;
done:
    if (!result)
        fprintf(stderr, "poulsback: map: refused (%d) usage 0x%x box %d,%d %dx%d image %d\n", ret,
                usage, box ? box->x : -1, box ? box->y : -1, box ? box->width : -1,
                box ? box->height : -1, r && r->image ? 1 : 0);
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
    return result;
}
static void unmap_texture(struct pipe_context *ctx, struct pipe_transfer *base)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct poulsback_transfer **p, *t;
    struct pipe_resource *reference = NULL;
    int ret;
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    for (p = &d->transfers; *p && &(*p)->base != base; p = &(*p)->next) {
    }
    if (!*p || (*p)->held) {
        error(d, -EINVAL);
    } else {
        t = *p;
        ret = t->owner->image          ? poulsback_ws_image_cpu_end(t->owner->image)
              : t->owner->native_depth ? poulsback_ws_depth_cpu_end(t->owner->native_depth)
                                       : 0;
        if (ret) {
            t->held = true;
            error(d, ret);
            d->cleanup_error = ret;
        } else {
            *p = t->next;
            t->owner->mapped--;
            reference = t->base.resource;
            free(t);
        }
    }
    pthread_mutex_unlock(&d->lock);
    pipe_resource_reference(&reference, NULL);
}

static void texture_subdata(struct pipe_context *ctx, struct pipe_resource *base, unsigned level,
                            unsigned usage, const struct pipe_box *box, const void *data,
                            unsigned stride, uintptr_t layer_stride)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct poulsback_resource *r;
    struct pipe_transfer *transfer = NULL;
    struct pipe_box area;
    uint64_t row, source_span, destination_span;
    uintptr_t source, destination;
    unsigned y;
    uint8_t *map;
    bool valid;
    const unsigned allowed =
        PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE | PIPE_MAP_DISCARD_WHOLE_RESOURCE;
    if (!d)
        return;
    /* Gallium's source rows remain readable/immutable for this synchronous
     * call. Check their extent without dereferencing an incoming resource. */
    if (!box || !data || (usage & ~allowed))
        goto invalid;
    area = *box;
    if (area.x < 0 || area.y < 0 || area.z || area.width <= 0 || area.height <= 0 ||
        area.depth != 1)
        goto invalid;
    row = (uint64_t)(unsigned)area.width * 4u;
    source_span = (uint64_t)(unsigned)(area.height - 1) * stride + row;
    source = (uintptr_t)data;
    if (row > stride || source_span > SIZE_MAX ||
        source > UINTPTR_MAX - (uintptr_t)(source_span - 1) ||
        (layer_stride && layer_stride < source_span))
        goto invalid;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    valid = r && r->image && !level && valid_box(r, &area) && r->base.target == PIPE_TEXTURE_2D &&
            image_format(r->base.format) == r->info.format;
    if (!valid)
        error(d, -EINVAL);
    pthread_mutex_unlock(&d->lock);
    if (!valid)
        return;
    /* Reuse the canonical transfer/alias exclusion and CPU BEGIN/END path.
     * Discard is advisory; a partial upload preserves every other byte. */
    map = map_texture(ctx, base, 0, PIPE_MAP_WRITE, &area, &transfer);
    if (!map)
        return;
    destination = (uintptr_t)map;
    destination_span = (uint64_t)(unsigned)(area.height - 1) * transfer->stride + row;
    if (destination_span > SIZE_MAX ||
        destination > UINTPTR_MAX - (uintptr_t)(destination_span - 1) ||
        (source <= destination ? destination - source < source_span
                               : source - destination < destination_span)) {
        /* A readable pointer is a caller precondition, but an overlapping
         * source/destination cannot be copied row-wise without corruption.
         * No image bytes have changed; END failure still retains HOLD. */
        pthread_mutex_lock(&d->lock);
        error(d, -EINVAL);
        pthread_mutex_unlock(&d->lock);
    } else {
        for (y = 0; y < (unsigned)area.height; y++)
            memcpy(map + (size_t)y * transfer->stride, (const uint8_t *)data + (size_t)y * stride,
                   (size_t)row);
    }
    unmap_texture(ctx, transfer);
    return;
invalid:
    pthread_mutex_lock(&d->lock);
    error(d, -EINVAL);
    pthread_mutex_unlock(&d->lock);
}

int poulsback_resource_domain_create(struct pipe_screen *screen, int fd,
                                     struct poulsback_resource_domain **out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_slab_ops ops;
    int ret;
    if (!screen || !out || screen->winsys_priv || screen->resource_create ||
        screen->resource_create_with_modifiers || screen->resource_get_param ||
        screen->resource_from_handle || screen->resource_get_handle || screen->resource_destroy ||
        screen->query_dmabuf_modifiers || screen->is_dmabuf_modifier_supported)
        return -EINVAL;
    d = calloc(1, sizeof(*d));
    if (!d)
        return -ENOMEM;
    ret = pthread_mutex_init(&d->lock, NULL);
    if (ret) {
        free(d);
        return -ret;
    }
    ret = poulsback_ws_create(fd, &d->ws);
    if (ret) {
        pthread_mutex_destroy(&d->lock);
        free(d);
        return ret;
    }
    poulsback_ws_buffer_ops(d->ws, &ops);
    ret = poulsback_slab_create(&ops, &d->slab);
    if (ret) {
        poulsback_ws_destroy(d->ws);
        pthread_mutex_destroy(&d->lock);
        free(d);
        return ret;
    }
    d->screen = screen;
    screen->winsys_priv = d;
    screen->resource_create = create_resource;
    screen->resource_create_with_modifiers = create_resource_with_modifiers;
    screen->resource_get_param = resource_get_param;
    screen->resource_from_handle = import_resource;
    screen->resource_get_handle = export_resource;
    screen->resource_destroy = destroy_resource;
    screen->query_dmabuf_modifiers = query_dmabuf_modifiers;
    screen->is_dmabuf_modifier_supported = is_dmabuf_modifier_supported;
    *out = d;
    return 0;
}
int poulsback_resource_domain_destroy(struct poulsback_resource_domain *d)
{
    int ret;
    if (!d)
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    ret = d->resources || d->transfers ? -EBUSY : 0;
    /* Buffer BOs close before the client's DRM file; a domain whose client
     * stays creates no buffer after that. */
    if (!ret && d->slab) {
        ret = poulsback_slab_destroy(d->slab);
        if (!ret)
            d->slab = NULL;
    }
    if (!ret)
        ret = poulsback_ws_destroy(d->ws);
    pthread_mutex_unlock(&d->lock);
    if (ret)
        return ret;
    d->screen->winsys_priv = NULL;
    d->screen->resource_create = NULL;
    d->screen->resource_create_with_modifiers = NULL;
    d->screen->resource_get_param = NULL;
    d->screen->resource_from_handle = NULL;
    d->screen->resource_get_handle = NULL;
    d->screen->resource_destroy = NULL;
    d->screen->query_dmabuf_modifiers = NULL;
    d->screen->is_dmabuf_modifier_supported = NULL;
    pthread_mutex_destroy(&d->lock);
    free(d);
    return 0;
}
int poulsback_resource_domain_error(struct poulsback_resource_domain *d)
{
    int ret;
    if (!d)
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    ret = d->error;
    pthread_mutex_unlock(&d->lock);
    return ret;
}
void poulsback_resource_context_init(struct pipe_context *ctx)
{
    ctx->texture_map = map_texture;
    ctx->texture_unmap = unmap_texture;
    ctx->texture_subdata = texture_subdata;
    ctx->buffer_map = map_buffer;
    ctx->buffer_unmap = unmap_texture;
    ctx->buffer_subdata = buffer_subdata;
    ctx->clear_buffer = clear_buffer;
    ctx->resource_copy_region = copy_region;
    ctx->transfer_flush_region = flush_buffer;
    ctx->resource_release = u_default_resource_release;
}
int poulsback_resource_layout(struct pipe_resource *base, uint32_t *stride, uint64_t *size)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    if (!base || !stride || !size || !(d = domain(base->screen)))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (r && r->image) {
        *stride = r->info.stride;
        *size = r->info.size;
    }
    pthread_mutex_unlock(&d->lock);
    return r && r->image ? 0 : -EINVAL;
}

/* Before a CPU write of [offset, offset + bytes) into buffer R: storage that a
 * job may still read is replaced, keeping every other byte, so the write
 * neither waits for the GPU nor flushes the open frame. */
static int buffer_write(struct poulsback_resource_domain *d, struct poulsback_resource *r,
                        uint64_t offset, uint64_t bytes)
{
    int ret;
    buffers_completed(d);
    ret = poulsback_slab_rename(d->slab, &r->storage, offset, bytes);
    r->cpu = r->storage->cpu;
    return ret;
}
static void *map_buffer(struct pipe_context *ctx, struct pipe_resource *base, unsigned level,
                        unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct poulsback_resource *r;
    struct poulsback_transfer *t, *p;
    void *result = NULL;
    int ret = -EINVAL;
    const unsigned allowed = PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE |
                             PIPE_MAP_DISCARD_WHOLE_RESOURCE | PIPE_MAP_UNSYNCHRONIZED |
                             PIPE_MAP_DONTBLOCK | PIPE_MAP_FLUSH_EXPLICIT;
    const unsigned discard = PIPE_MAP_DISCARD_RANGE | PIPE_MAP_DISCARD_WHOLE_RESOURCE;
    if (!d || !out)
        return NULL;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (!r || !r->cpu || level || !box || box->x < 0 || box->y || box->z || box->width <= 0 ||
        box->height != 1 || box->depth != 1 ||
        (uint64_t)(unsigned)box->x + (unsigned)box->width > r->cpu_bytes ||
        !(usage & (PIPE_MAP_READ | PIPE_MAP_WRITE)) || (usage & ~allowed) ||
        (usage & UINT32_C(0xff000000)))
        goto done;
    /* The GPU only reads buffers. A synchronised write that keeps the bytes
     * waits for the buffer's last job; a discarding one renames below. */
    if ((usage & PIPE_MAP_WRITE) && !(usage & (PIPE_MAP_UNSYNCHRONIZED | discard))) {
        if (usage & PIPE_MAP_DONTBLOCK)
            buffers_completed(d);
        ret = (usage & PIPE_MAP_DONTBLOCK) && storage_busy(d, r) ? -EBUSY
                                                                 : wait_idle(ctx, d, base, &r);
        if (ret)
            goto done;
    }
    for (p = d->transfers; p; p = p->next)
        if (same_storage(p->owner, r)) {
            ret = -EBUSY;
            goto done;
        }
    t = calloc(1, sizeof(*t));
    if (!t) {
        ret = -ENOMEM;
        goto done;
    }
    /* An UNSYNCHRONIZED range write goes in place (the caller's promise); a
     * whole-resource discard keeps no byte. */
    ret = 0;
    if ((usage & PIPE_MAP_WRITE) && (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE))
        ret = buffer_write(d, r, 0, r->cpu_bytes);
    else if ((usage & PIPE_MAP_WRITE) && (usage & PIPE_MAP_DISCARD_RANGE) &&
             !(usage & PIPE_MAP_UNSYNCHRONIZED))
        ret = buffer_write(d, r, (unsigned)box->x, (unsigned)box->width);
    if (ret) {
        free(t);
        goto done;
    }
    t->owner = r;
    t->base.level = 0;
    t->base.usage = usage & UINT32_C(0x00ffffff);
    t->base.box = *box;
    pipe_resource_reference(&t->base.resource, base);
    r->mapped++;
    t->next = d->transfers;
    d->transfers = t;
    *out = &t->base;
    result = r->cpu + box->x;
    ret = 0;
done:
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
    return result;
}
static void buffer_subdata(struct pipe_context *ctx, struct pipe_resource *base, unsigned usage,
                           unsigned offset, unsigned size, const void *data)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct pipe_transfer *transfer = NULL;
    struct pipe_box box;
    void *map;
    const unsigned allowed = PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE |
                             PIPE_MAP_DISCARD_WHOLE_RESOURCE | PIPE_MAP_UNSYNCHRONIZED |
                             PIPE_MAP_DIRECTLY;
    if (!d || !size)
        return;
    if (!data || offset > INT_MAX || size > INT_MAX || (usage & ~allowed) ||
        (uintptr_t)data > UINTPTR_MAX - (size - 1u)) {
        pthread_mutex_lock(&d->lock);
        error(d, -EINVAL);
        pthread_mutex_unlock(&d->lock);
        return;
    }
    box = (struct pipe_box){.x = (int)offset, .width = (int)size, .height = 1, .depth = 1};
    /* The written range is replaced, so it is mapped as a discarded range:
     * busy storage is renamed with every other byte, never waited for. The
     * caller's discard permissions invalidate no other byte (DIRECTLY keeps
     * them all); UNSYNCHRONIZED writes in place. Transfer exclusion stays. */
    map = map_buffer(ctx, base, 0,
                     PIPE_MAP_WRITE | ((usage & PIPE_MAP_UNSYNCHRONIZED) ? PIPE_MAP_UNSYNCHRONIZED
                                                                         : PIPE_MAP_DISCARD_RANGE),
                     &box, &transfer);
    if (map) {
        memmove(map, data, size);
        unmap_texture(ctx, transfer);
    }
}
static void copy_region(struct pipe_context *ctx, struct pipe_resource *dst, unsigned dst_level,
                        unsigned dstx, unsigned dsty, unsigned dstz, struct pipe_resource *src,
                        unsigned src_level, const struct pipe_box *box)
{
    struct poulsback_resource_domain *d = ctx ? domain(ctx->screen) : NULL;
    struct poulsback_resource *s, *r;
    struct pipe_box b;
    uintptr_t input, output;
    const uint8_t *source;
    int ret = -EINVAL;
    if (!d)
        return;
    if (box)
        b = *box;
    pthread_mutex_lock(&d->lock);
    if (d->screen != ctx->screen || !box || dst_level || src_level || dsty || dstz || b.x < 0 ||
        b.y || b.z || b.width < 0 || b.height != 1 || b.depth != 1)
        goto done;
    if (box->x != b.x || box->y != b.y || box->z != b.z || box->width != b.width ||
        box->height != b.height || box->depth != b.depth) {
        ret = -ESTALE;
        goto done;
    }
    /* An empty 1D copy, like buffer_subdata, inspects no resource or storage. */
    if (!b.width) {
        ret = 0;
        goto done;
    }
    /* Canonical membership precedes every resource dereference. Only the
     * driver's private CPU buffers participate; GEM images never reach here. */
    s = find_resource(d, src);
    r = find_resource(d, dst);
    if (!s || !r)
        goto done;
    if (!s->cpu || !r->cpu || s->image || r->image || s->base.target != PIPE_BUFFER ||
        r->base.target != PIPE_BUFFER || s->base.format != PIPE_FORMAT_R8_UNORM ||
        r->base.format != PIPE_FORMAT_R8_UNORM || s->base.height0 != 1 || r->base.height0 != 1 ||
        s->base.depth0 != 1 || r->base.depth0 != 1 || s->base.array_size != 1 ||
        r->base.array_size != 1 || s->base.last_level || r->base.last_level ||
        s->base.nr_samples > 1 || r->base.nr_samples > 1 || s->base.nr_storage_samples > 1 ||
        r->base.nr_storage_samples > 1) {
        ret = -EOPNOTSUPP;
        goto done;
    }
    if (!s->cpu_bytes || !r->cpu_bytes || s->cpu_bytes != s->base.width0 ||
        r->cpu_bytes != r->base.width0 || s->cpu_bytes > POULSBACK_IMAGE_MAX_BYTES ||
        r->cpu_bytes > POULSBACK_IMAGE_MAX_BYTES ||
        (uint64_t)(unsigned)b.x + (unsigned)b.width > s->cpu_bytes ||
        (uint64_t)dstx + (unsigned)b.width > r->cpu_bytes)
        goto done;
    if (s->mapped || r->mapped) {
        ret = -EBUSY;
        goto done;
    }
    input = (uintptr_t)box;
    output = (uintptr_t)(r->cpu + dstx);
    if (input > UINTPTR_MAX - sizeof(b) || output > UINTPTR_MAX - (unsigned)b.width ||
        (input < output + (unsigned)b.width && output < input + sizeof(b)))
        goto done;
    /* Both owners and all transfers remain locked through the one bounded
     * CPU copy. A busy destination is renamed first; a same-buffer source is
     * read from the old storage, which stays until its last job completes.
     * memmove also defines same-buffer overlap. No image CPU_BEGIN/END,
     * cache/fence operation or GPU authority is acquired. */
    source = s->cpu + (unsigned)b.x;
    ret = buffer_write(d, r, dstx, (unsigned)b.width);
    if (!ret)
        memmove(r->cpu + dstx, source, (unsigned)b.width);
done:
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
}
static void clear_buffer(struct pipe_context *ctx, struct pipe_resource *base, unsigned offset,
                         unsigned size, const void *value, int value_size)
{
    struct poulsback_resource_domain *d = ctx ? domain(ctx->screen) : NULL;
    struct poulsback_resource *r;
    uint8_t pattern[16];
    unsigned copied = 0;
    int ret = -EINVAL;
    if (!d || !size)
        return;
    pthread_mutex_lock(&d->lock);
    /* Membership is checked before any incoming resource dereference. This
     * callback clears private host storage only; images/depth never map here. */
    r = find_resource(d, base);
    if (!r || !r->cpu || r->image || r->base.target != PIPE_BUFFER ||
        r->base.format != PIPE_FORMAT_R8_UNORM || r->base.height0 != 1 || r->base.depth0 != 1 ||
        r->base.array_size != 1 || r->base.last_level || r->base.nr_samples > 1 ||
        r->base.nr_storage_samples > 1 || !r->cpu_bytes || r->cpu_bytes != r->base.width0 ||
        r->cpu_bytes > POULSBACK_IMAGE_MAX_BYTES || (uint64_t)offset + size > r->cpu_bytes ||
        !value || value_size <= 0 || (unsigned)value_size > sizeof(pattern) ||
        (uintptr_t)value > UINTPTR_MAX - ((unsigned)value_size - 1u))
        goto done;
    if (r->mapped) {
        ret = -EBUSY;
        goto done;
    }
    /* Own the pattern before changing storage, including a readable pattern
     * alias inside this buffer. Repeat from its first byte at offset; Mesa's
     * clear_buffer contract permits a final partial element. */
    memcpy(pattern, value, (unsigned)value_size);
    ret = buffer_write(d, r, offset, size);
    if (ret)
        goto done;
    while (copied < size) {
        unsigned count = size - copied;
        if (count > (unsigned)value_size)
            count = (unsigned)value_size;
        memcpy(r->cpu + offset + copied, pattern, count);
        copied += count;
    }
    ret = 0;
done:
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
}
static void flush_buffer(struct pipe_context *ctx, struct pipe_transfer *base,
                         const struct pipe_box *box)
{
    struct poulsback_resource_domain *d = domain(ctx->screen);
    struct poulsback_transfer *t;
    int ret = -EINVAL;
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    for (t = d->transfers; t && &t->base != base; t = t->next) {
    }
    /* Flush coordinates are relative to the mapped range. These bytes are
     * private host storage, never a GEM image or an SGX/display mapping.
     * Writes are already in this storage; this validates the explicit CPU
     * upload boundary, not a GPU cache or visibility operation. */
    if (t && !t->held && t->owner->cpu && (t->base.usage & PIPE_MAP_WRITE) && box && box->x >= 0 &&
        !box->y && !box->z && box->width > 0 && box->height == 1 && box->depth == 1 &&
        (uint64_t)(unsigned)box->x + (unsigned)box->width <= (unsigned)t->base.box.width)
        ret = 0;
    error(d, ret);
    pthread_mutex_unlock(&d->lock);
}
static int buffer_cpu_locked(struct poulsback_resource_domain *d, struct pipe_resource *base,
                             struct poulsback_resource **out)
{
    struct poulsback_resource *r = find_resource(d, base);
    if (!r || !r->cpu)
        return -EINVAL;
    if (r->mapped)
        return -EBUSY;
    *out = r;
    return 0;
}
int poulsback_resource_buffer_gpu(struct pipe_resource *base, uint64_t sequence,
                                  struct poulsback_resource_gpu *out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    int ret;
    if (!base || !out || !sequence || !(d = domain(base->screen)))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    ret = buffer_cpu_locked(d, base, &r);
    if (!ret && !r->storage)
        ret = -EINVAL;
    if (!ret) {
        poulsback_slab_use(r->storage, sequence);
        *out = (struct poulsback_resource_gpu){r->storage->handle, r->storage->gpu_va,
                                               r->cpu_bytes, r->cpu};
    }
    pthread_mutex_unlock(&d->lock);
    return ret;
}
int poulsback_resource_buffer_borrow(struct pipe_resource *base, const void **out, size_t *bytes)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    int ret;
    if (!base || !out || !bytes || !(d = domain(base->screen)))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    ret = buffer_cpu_locked(d, base, &r);
    if (!ret) {
        *out = r->cpu;
        *bytes = r->cpu_bytes;
    }
    pthread_mutex_unlock(&d->lock);
    return ret;
}
int poulsback_resource_buffer_snapshot_range(struct pipe_resource *base, size_t offset,
                                             size_t bytes, void **out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    void *copy = NULL;
    int ret;
    if (!base || !out || !(d = domain(base->screen)))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    ret = buffer_cpu_locked(d, base, &r);
    if (!ret && (offset > r->cpu_bytes || bytes > r->cpu_bytes - offset))
        ret = -EINVAL;
    if (!ret && !(copy = malloc(bytes ? bytes : 1)))
        ret = -ENOMEM;
    if (!ret) {
        memcpy(copy, (const uint8_t *)r->cpu + offset, bytes);
        *out = copy;
    }
    pthread_mutex_unlock(&d->lock);
    return ret;
}
int poulsback_resource_image_acquire(struct pipe_resource *base, struct poulsback_ws_image **out)
{
    struct poulsback_resource_domain *d;
    struct poulsback_resource *r;
    struct poulsback_transfer *t;
    int ret = -EINVAL;
    if (!base || !out || !(d = domain(base->screen)))
        return -EINVAL;
    pthread_mutex_lock(&d->lock);
    r = find_resource(d, base);
    if (!r || !r->image)
        goto done;
    for (t = d->transfers; t; t = t->next)
        if (same_storage(t->owner, r)) {
            ret = -EBUSY;
            goto done;
        }
    ret = poulsback_ws_image_ref(r->image);
    if (!ret)
        *out = r->image;
done:
    pthread_mutex_unlock(&d->lock);
    return ret;
}
int poulsback_resource_render_create(struct pipe_screen *screen, struct poulsback_ws_render **out)
{
    struct poulsback_resource_domain *d = domain(screen);
    if (!d)
        return -EINVAL;
    return poulsback_ws_render_create(d->ws, out);
}
