// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_surface.h"
#include "poulsback_resource.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_framebuffer.h"
#include <errno.h>
#include <stdint.h>

int poulsback_surface_framebuffer_set(struct pipe_screen *screen,
                                      struct pipe_framebuffer_state *dst,
                                      const struct pipe_framebuffer_state *src)
{
    struct pipe_framebuffer_state candidate;
    uint32_t stride;
    uint64_t bytes;
    if (!screen || !dst || !src)
        return -EINVAL;
    if (src->nr_cbufs > 1 || src->resolve || src->viewmask || src->pls_enabled ||
        src->samples > 1 || src->layers > 1)
        return -EOPNOTSUPP;
    for (unsigned i = src->nr_cbufs; i < PIPE_MAX_COLOR_BUFS; i++)
        if (src->cbufs[i].texture)
            return -EINVAL;
    if (src->nr_cbufs) {
        const struct pipe_surface *s = &src->cbufs[0];
        struct pipe_resource *r = s->texture;
        if (!r || r->screen != screen || !src->width || !src->height || s->format != r->format ||
            r->target != PIPE_TEXTURE_2D || src->width > r->width0 || src->height > r->height0)
            return -EINVAL;
        if (s->level || s->first_layer || s->last_layer || s->nr_samples > 1 || r->last_level ||
            r->array_size != 1 || r->depth0 != 1 || !(r->bind & PIPE_BIND_RENDER_TARGET))
            return -EOPNOTSUPP;
        int error = poulsback_resource_layout(r, &stride, &bytes);
        if (error)
            return error;
        if ((uint64_t)stride < (uint64_t)r->width0 * 4 || (uint64_t)stride * r->height0 > bytes)
            return -EPROTO;
    } else if (src->width || src->height) {
        return -EOPNOTSUPP; /* No-attachment rendering is not implemented. */
    }
    if (src->zsbuf.texture) {
        const struct pipe_surface *s = &src->zsbuf;
        struct pipe_resource *r = s->texture;
        struct poulsback_depth_capture_description depth;
        if (src->nr_cbufs != 1 || r == src->cbufs[0].texture || r->screen != screen ||
            s->format != r->format || s->level || s->first_layer || s->last_layer ||
            s->nr_samples > 1 || poulsback_resource_depth_capture_description(r, &depth) ||
            /* Mesa surface0 inherits the resource's sample count. Keep the
             * original descriptor; only explicit overrides must match. */
            (s->nr_samples && s->nr_samples != depth.samples) || depth.width != src->width ||
            depth.height != src->height)
            return -EOPNOTSUPP;
    }
    candidate = *src;
    /* Acquire all incoming refs first, including a borrowed final reference
     * held by dst itself. Inline reference counters are not heap surfaces. */
    for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
        candidate.cbufs[i].texture = NULL;
        if (i < src->nr_cbufs)
            pipe_resource_reference(&candidate.cbufs[i].texture, src->cbufs[i].texture);
    }
    candidate.zsbuf.texture = NULL;
    if (src->zsbuf.texture)
        pipe_resource_reference(&candidate.zsbuf.texture, src->zsbuf.texture);
    candidate.resolve = NULL;
    util_unreference_framebuffer_state(dst);
    *dst = candidate;
    return 0;
}
