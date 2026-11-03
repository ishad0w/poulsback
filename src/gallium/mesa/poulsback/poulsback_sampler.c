// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_sampler.h"
#include "poulsback_resource.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PB_SAMPLER_OBJECTS 4096u
struct sampler {
    struct sampler *next;
    struct pipe_sampler_state descriptor;
    unsigned refs;
    bool caller;
};
struct view {
    struct pipe_sampler_view base;
    struct pipe_sampler_view description;
    struct view *next;
    struct pipe_resource *resource;
    bool live;
};
struct poulsback_sampler_bank {
    struct poulsback_sampler_bank *next;
    struct pipe_context *context;
    struct pipe_screen *screen;
    struct sampler *states, *bound[MESA_SHADER_STAGES][PIPE_MAX_SAMPLERS];
    struct view *views, *bound_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
    unsigned objects;
    unsigned bound_count; /* nonempty entries of bound and bound_views */
    bool closing;
};
/* This host-only registry permits Gallium's cross-context view destructor to
 * find the actual creator without first casting/dereferencing an opaque view.
 * Resource callbacks do not reenter this registry. Gallium view references
 * themselves are non-atomic and still require normal external serialization. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct poulsback_sampler_bank *banks;
static struct poulsback_sampler_bank *find_bank(struct poulsback_sampler_bank *key)
{
    for (struct poulsback_sampler_bank *b = banks; b; b = b->next)
        if (b == key)
            return b;
    return NULL;
}
static struct sampler *find_state(struct poulsback_sampler_bank *b, void *key)
{
    for (struct sampler *s = b->states; s; s = s->next)
        if (s == key)
            return s;
    return NULL;
}
static struct view *find_view(struct poulsback_sampler_bank *b, const struct pipe_sampler_view *key)
{
    for (struct view *v = b->views; v; v = v->next)
        if (&v->base == key && v->live)
            return v;
    return NULL;
}
static void release_state(struct poulsback_sampler_bank *b, struct sampler *s)
{
    struct sampler **link;
    if (!s || --s->refs)
        return;
    for (link = &b->states; *link != s; link = &(*link)->next) {
    }
    *link = s->next;
    b->objects--;
    free(s);
}
static void release_view(struct poulsback_sampler_bank *b, struct view *v)
{
    struct view **link;
    v->live = false;
    pipe_resource_reference(&v->resource, NULL);
    v->base.texture = NULL;
    for (link = &b->views; *link != v; link = &(*link)->next) {
    }
    *link = v->next;
    b->objects--;
    free(v);
}
static void view_reference(struct poulsback_sampler_bank *b, struct view **dst, struct view *src)
{
    struct view *old = *dst;
    if (pipe_reference_described_nonatomic(old ? &old->base.reference : NULL,
                                           src ? &src->base.reference : NULL,
                                           (debug_reference_descriptor)debug_describe_sampler_view))
        release_view(b, old);
    *dst = src;
}
static bool stage_valid(mesa_shader_stage stage)
{
    return (unsigned)stage < MESA_SHADER_STAGES;
}
static bool stage_supported(mesa_shader_stage stage)
{
    return stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_FRAGMENT;
}
int poulsback_sampler_bank_create(struct pipe_context *p, struct poulsback_sampler_bank **out)
{
    struct poulsback_sampler_bank *b;
    if (!p || !p->screen || !out || *out)
        return -EINVAL;
    b = calloc(1, sizeof(*b));
    if (!b)
        return -ENOMEM;
    pthread_mutex_lock(&lock);
    for (struct poulsback_sampler_bank *other = banks; other; other = other->next)
        if (other->context == p) {
            pthread_mutex_unlock(&lock);
            free(b);
            return -EEXIST;
        }
    b->context = p;
    b->screen = p->screen;
    b->next = banks;
    banks = b;
    *out = b;
    pthread_mutex_unlock(&lock);
    return 0;
}
static bool external_views(struct poulsback_sampler_bank *b)
{
    for (struct view *v = b->views; v; v = v->next)
        if (v->live) {
            unsigned bound = 0;
            for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++)
                for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; slot++)
                    bound += b->bound_views[stage][slot] == v;
            if (v->base.reference.count != (int)bound)
                return true;
        }
    return false;
}
int poulsback_sampler_bank_close(struct poulsback_sampler_bank *key)
{
    int ret = -EINVAL;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    if (b) {
        b->closing = true;
        ret = external_views(b) ? -EBUSY : 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}
int poulsback_sampler_bank_destroy(struct poulsback_sampler_bank *key)
{
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key), **link;
    if (!b || !b->closing || external_views(b)) {
        pthread_mutex_unlock(&lock);
        return -EBUSY;
    }
    for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++)
        for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; slot++)
            view_reference(b, &b->bound_views[stage][slot], NULL);
    while (b->views)
        release_view(b, b->views);
    while (b->states) {
        struct sampler *s = b->states;
        b->states = s->next;
        free(s);
    }
    for (link = &banks; *link != b; link = &(*link)->next) {
    }
    *link = b->next;
    free(b);
    pthread_mutex_unlock(&lock);
    return 0;
}
static bool descriptor_valid(const struct pipe_sampler_state *s)
{
    return s && s->min_mip_filter <= PIPE_TEX_MIPFILTER_NONE &&
           s->reduction_mode <= PIPE_TEX_REDUCTION_MAX && !s->pad && isfinite(s->lod_bias) &&
           isfinite(s->min_lod) && isfinite(s->max_lod) && s->min_lod <= s->max_lod;
}
int poulsback_sampler_state_create(struct poulsback_sampler_bank *key,
                                   const struct pipe_sampler_state *descriptor, void **out)
{
    struct sampler *s;
    if (!out || *out || !descriptor_valid(descriptor))
        return -EINVAL;
    s = calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    memcpy(&s->descriptor, descriptor, sizeof(s->descriptor));
    s->caller = true;
    s->refs = 1;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    if (!b || b->closing || b->objects == PB_SAMPLER_OBJECTS) {
        pthread_mutex_unlock(&lock);
        free(s);
        return b ? -EBUSY : -EINVAL;
    }
    b->objects++;
    s->next = b->states;
    b->states = s;
    *out = s;
    pthread_mutex_unlock(&lock);
    return 0;
}
int poulsback_sampler_state_delete(struct poulsback_sampler_bank *key, void *handle)
{
    int ret = -EINVAL;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    struct sampler *s = b ? find_state(b, handle) : NULL;
    if (s && s->caller) {
        s->caller = false;
        release_state(b, s);
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}
int poulsback_sampler_states_bind(struct poulsback_sampler_bank *key, mesa_shader_stage stage,
                                  unsigned start, unsigned count, void **handles)
{
    struct sampler *incoming[PIPE_MAX_SAMPLERS] = {0};
    if (!stage_valid(stage) || start > PIPE_MAX_SAMPLERS || count > PIPE_MAX_SAMPLERS - start)
        return -EINVAL;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    if (!b || b->closing) {
        pthread_mutex_unlock(&lock);
        return -EBUSY;
    }
    for (unsigned i = 0; i < count; i++)
        if (handles && handles[i]) {
            incoming[i] = find_state(b, handles[i]);
            if (!incoming[i] || !incoming[i]->caller || !stage_supported(stage)) {
                pthread_mutex_unlock(&lock);
                return -EINVAL;
            }
        }
    for (unsigned i = 0; i < count; i++)
        if (incoming[i])
            incoming[i]->refs++;
    for (unsigned i = 0; i < count; i++) {
        struct sampler *old = b->bound[stage][start + i];
        b->bound[stage][start + i] = incoming[i];
        b->bound_count = b->bound_count - !!old + !!incoming[i];
        release_state(b, old);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
int poulsback_sampler_state_snapshot(struct poulsback_sampler_bank *key, void *handle,
                                     struct pipe_sampler_state *out)
{
    int ret = -EINVAL;
    if (!out)
        return ret;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    struct sampler *s = b ? find_state(b, handle) : NULL;
    if (s && s->refs) {
        *out = s->descriptor;
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}
int poulsback_sampler_view_create(struct poulsback_sampler_bank *key, struct pipe_resource *r,
                                  const struct pipe_sampler_view *t, struct pipe_sampler_view **out)
{
    uint32_t stride;
    uint64_t bytes;
    int ret;
    if (!t || !r || !out || *out)
        return -EINVAL;
    if (t->target != PIPE_TEXTURE_2D || r->target != PIPE_TEXTURE_2D || t->format != r->format ||
        t->astc_decode_format || t->is_tex2d_from_buf || t->u.tex.first_layer ||
        t->u.tex.last_layer || t->u.tex.first_level || t->u.tex.last_level ||
        t->u.tex.min_lod_clamp != 0 || t->swizzle_r > PIPE_SWIZZLE_1 ||
        t->swizzle_g > PIPE_SWIZZLE_1 || t->swizzle_b > PIPE_SWIZZLE_1 ||
        t->swizzle_a > PIPE_SWIZZLE_1)
        return -EOPNOTSUPP;
    ret = poulsback_resource_layout(r, &stride, &bytes);
    if (ret)
        return ret;
    if (!stride || !bytes)
        return -EPROTO;
    struct view *v = calloc(1, sizeof(*v));
    if (!v)
        return -ENOMEM;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    if (!b || b->closing || b->screen != r->screen || b->objects == PB_SAMPLER_OBJECTS) {
        pthread_mutex_unlock(&lock);
        free(v);
        return -EINVAL;
    }
    v->base = *t;
    v->base.context = b->context;
    pipe_reference_init(&v->base.reference, 1);
    pipe_resource_reference(&v->resource, r);
    v->base.texture = v->resource;
    v->live = true;
    memcpy(&v->description, &v->base, sizeof(v->description));
    v->next = b->views;
    b->views = v;
    b->objects++;
    *out = &v->base;
    pthread_mutex_unlock(&lock);
    return 0;
}
int poulsback_sampler_views_bind(struct poulsback_sampler_bank *key, mesa_shader_stage stage,
                                 unsigned start, unsigned count, unsigned trailing,
                                 struct pipe_sampler_view **views)
{
    struct view *incoming[PIPE_MAX_SHADER_SAMPLER_VIEWS] = {0};
    if (!stage_valid(stage) || start > PIPE_MAX_SHADER_SAMPLER_VIEWS ||
        count > PIPE_MAX_SHADER_SAMPLER_VIEWS - start ||
        trailing > PIPE_MAX_SHADER_SAMPLER_VIEWS - start - count)
        return -EINVAL;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *b = find_bank(key);
    if (!b || b->closing) {
        pthread_mutex_unlock(&lock);
        return -EBUSY;
    }
    for (unsigned i = 0; i < count; i++)
        if (views && views[i]) {
            incoming[i] = find_view(b, views[i]);
            if (!incoming[i] || !stage_supported(stage)) {
                pthread_mutex_unlock(&lock);
                return -EINVAL;
            }
        }
    for (unsigned i = 0; i < count; i++)
        if (incoming[i]) {
            struct view *temporary = NULL;
            view_reference(b, &temporary, incoming[i]);
        }
    for (unsigned i = 0; i < count + trailing; i++) {
        struct view **slot = &b->bound_views[stage][start + i];
        b->bound_count -= !!*slot;
        view_reference(b, slot, NULL);
        *slot = i < count ? incoming[i] : NULL; /* Transfers the temporary ref. */
        b->bound_count += !!*slot;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
int poulsback_sampler_view_destroy(struct pipe_context *current, struct pipe_sampler_view *handle)
{
    int ret = -EINVAL;
    pthread_mutex_lock(&lock);
    struct poulsback_sampler_bank *caller = NULL, *owner = NULL;
    struct view *v = NULL;
    for (struct poulsback_sampler_bank *b = banks; b; b = b->next) {
        if (b->context == current)
            caller = b;
        struct view *found = find_view(b, handle);
        if (found) {
            owner = b;
            v = found;
        }
    }
    if (v && caller && owner->screen == caller->screen && !v->base.reference.count) {
        release_view(owner, v);
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}
int poulsback_sampler_fragment_binding(struct poulsback_sampler_bank *b, unsigned unit,
                                       struct poulsback_sampler_binding *out)
{
    const struct sampler *s;
    const struct view *v;
    const struct pipe_sampler_view *d;
    if (!b || b->closing || !out || unit >= PIPE_MAX_SAMPLERS ||
        unit >= PIPE_MAX_SHADER_SAMPLER_VIEWS)
        return -EINVAL;
    s = b->bound[MESA_SHADER_FRAGMENT][unit];
    v = b->bound_views[MESA_SHADER_FRAGMENT][unit];
    if (!s || !v || !v->live || !v->resource)
        return -ENOENT;
    d = &v->description;
    *out =
        (struct poulsback_sampler_binding){s->descriptor,
                                           unit,
                                           unit,
                                           d->target,
                                           d->format,
                                           {d->swizzle_r, d->swizzle_g, d->swizzle_b, d->swizzle_a},
                                           d->u.tex.first_level,
                                           d->u.tex.last_level,
                                           d->u.tex.first_layer,
                                           d->u.tex.last_layer,
                                           NULL};
    pipe_resource_reference(&out->resource, v->resource);
    return 0;
}
int poulsback_sampler_draw_admit(struct poulsback_sampler_bank *b)
{
    /* The context's own bank on its own thread: only its binds and close
     * write these fields, so no registry lookup or lock. */
    if (!b || b->closing)
        return -EBUSY;
    return b->bound_count ? -EOPNOTSUPP : 0;
}
