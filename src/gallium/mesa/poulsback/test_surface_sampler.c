// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa26 descriptors/references. Canonical image-storage lookup is an
 * explicit CPU boundary; there are no pixels, descriptors or GPU operations. */
#ifdef NDEBUG
#error "Reference validation required"
#endif
#include "poulsback_surface.h"
#include "poulsback_sampler.h"
#include "poulsback_resource.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_framebuffer.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pipe_resource *images[3];
static unsigned destroyed, view_destroys;
int poulsback_resource_depth_capture_description(struct pipe_resource *resource,
                                                 struct poulsback_depth_capture_description *out)
{
    (void)resource;
    (void)out;
    return -EOPNOTSUPP; /* This fixture supplies color resources only. */
}
static void destroy_resource(struct pipe_screen *s, struct pipe_resource *r)
{
    unsigned i;
    assert(s == r->screen && !r->reference.count);
    for (i = 0; i < 3 && images[i] != r; i++) {
    }
    assert(i < 3);
    images[i] = NULL;
    destroyed++;
    free(r);
}
int poulsback_resource_layout(struct pipe_resource *r, uint32_t *stride, uint64_t *bytes)
{
    unsigned i;
    for (i = 0; i < 3 && images[i] != r; i++) {
    }
    if (i == 3)
        return -EINVAL;
    *stride = 128;
    *bytes = 4096;
    return 0;
}
static struct pipe_resource *image(struct pipe_screen *s, unsigned slot)
{
    assert(!images[slot]);
    struct pipe_resource *r = calloc(1, sizeof(*r));
    assert(r);
    *r = (struct pipe_resource){.screen = s,
                                .target = PIPE_TEXTURE_2D,
                                .format = PIPE_FORMAT_B8G8R8A8_UNORM,
                                .width0 = 32,
                                .height0 = 32,
                                .depth0 = 1,
                                .array_size = 1,
                                .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW};
    pipe_reference_init(&r->reference, 1);
    images[slot] = r;
    return r;
}
static void destroy_view(struct pipe_context *p, struct pipe_sampler_view *v)
{
    assert(!poulsback_sampler_view_destroy(p, v));
    view_destroys++;
}
static void lifetime(struct pipe_screen *screen)
{
    enum { LIMIT = 4096, CHURN = LIMIT * 2 + 1 };
    struct pipe_context a = {.screen = screen, .sampler_view_destroy = destroy_view},
                        b = {.screen = screen, .sampler_view_destroy = destroy_view};
    struct poulsback_sampler_bank *ba = NULL, *bb = NULL;
    struct pipe_resource *r = image(screen, 2);
    struct pipe_sampler_state descriptor = {.min_mip_filter = PIPE_TEX_MIPFILTER_NONE}, snapshot;
    struct pipe_sampler_view templ = {.target = PIPE_TEXTURE_2D,
                                      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
                                      .swizzle_r = PIPE_SWIZZLE_X,
                                      .swizzle_g = PIPE_SWIZZLE_Y,
                                      .swizzle_b = PIPE_SWIZZLE_Z,
                                      .swizzle_a = PIPE_SWIZZLE_W};
    void *states[LIMIT], *extra = NULL;
    struct pipe_sampler_view *views[LIMIT], *overflow = NULL;
    assert(!poulsback_sampler_bank_create(&a, &ba) && !poulsback_sampler_bank_create(&b, &bb));
    for (unsigned i = 0; i < LIMIT; i++) {
        states[i] = NULL;
        assert(!poulsback_sampler_state_create(ba, &descriptor, &states[i]));
    }
    assert(poulsback_sampler_state_create(ba, &descriptor, &extra) == -EBUSY && !extra);
    assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, states));
    assert(!poulsback_sampler_state_delete(ba, states[0]));
    assert(!poulsback_sampler_state_snapshot(ba, states[0], &snapshot));
    assert(poulsback_sampler_state_create(ba, &descriptor, &extra) == -EBUSY && !extra);
    assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, NULL));
    assert(poulsback_sampler_state_snapshot(ba, states[0], &snapshot) == -EINVAL);
    states[0] = NULL;
    assert(!poulsback_sampler_state_create(ba, &descriptor, &states[0]));
    for (unsigned i = 0; i < LIMIT; i++)
        assert(!poulsback_sampler_state_delete(ba, states[i]));
    for (unsigned i = 0; i < LIMIT; i++) {
        views[i] = NULL;
        assert(!poulsback_sampler_view_create(ba, r, &templ, &views[i]));
    }
    assert(r->reference.count == LIMIT + 1);
    assert(poulsback_sampler_view_create(ba, r, &templ, &overflow) == -EINVAL && !overflow);
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, 0, views));
    pipe_sampler_view_reference(&views[0], NULL);
    assert(poulsback_sampler_view_create(ba, r, &templ, &overflow) == -EINVAL && !overflow);
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL));
    assert(!poulsback_sampler_view_create(ba, r, &templ, &views[0]));
    for (unsigned i = 0; i < LIMIT; i++)
        pipe_sampler_view_reference(&views[i], NULL);
    assert(r->reference.count == 1);
    /* Each iteration exceeds neither live budget nor ownership bounds. The
     * total creation count exceeds the limit for both kinds independently. */
    for (unsigned i = 0; i < CHURN; i++) {
        void *state = NULL, *bindings[2];
        struct pipe_sampler_view *view = NULL, *bound[2];
        assert(!poulsback_sampler_state_create(ba, &descriptor, &state));
        assert(!poulsback_sampler_view_create(ba, r, &templ, &view));
        bindings[0] = bindings[1] = state;
        bound[0] = bound[1] = view;
        assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, bindings));
        assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, 0, bound));
        assert(!poulsback_sampler_state_delete(ba, state));
        pipe_sampler_view_reference(&view, NULL);
        assert(!poulsback_sampler_state_snapshot(ba, state, &snapshot) &&
               bound[0]->reference.count == 2);
        assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, NULL));
        assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 0, 2, NULL));
        assert(r->reference.count == 1 && !poulsback_sampler_draw_admit(ba));
    }
    struct pipe_sampler_view *own = NULL, *other = NULL;
    assert(!poulsback_sampler_view_create(ba, r, &templ, &own));
    assert(!poulsback_sampler_view_create(bb, r, &templ, &other));
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, 0, &own));
    struct pipe_sampler_view *mixed[2] = {own, other};
    assert(poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, 0, mixed) == -EINVAL);
    assert(own->reference.count == 2 && other->reference.count == 1 && r->reference.count == 3);
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL));
    pipe_sampler_view_reference(&own, NULL);
    assert(pipe_reference_described_nonatomic(
        &other->reference, NULL, (debug_reference_descriptor)debug_describe_sampler_view));
    assert(!poulsback_sampler_view_destroy(&a, other)); /* final drop belongs to bb */
    other = NULL;
    assert(r->reference.count == 1);
    assert(!poulsback_sampler_bank_close(ba) && !poulsback_sampler_bank_destroy(ba));
    assert(!poulsback_sampler_bank_close(bb) && !poulsback_sampler_bank_destroy(bb));
    pipe_resource_reference(&r, NULL);
    printf(
        "PASS sampler/view live limit=%u; final bound/external release, %u churn cycles, transactional foreign view; no GPU sampling\n",
        (unsigned)LIMIT, (unsigned)CHURN);
}
int main(void)
{
    struct pipe_screen screen = {.resource_destroy = destroy_resource}, foreign_screen = {0};
    struct pipe_context a = {.screen = &screen, .sampler_view_destroy = destroy_view},
                        b = {.screen = &screen, .sampler_view_destroy = destroy_view},
                        foreign = {.screen = &foreign_screen, .sampler_view_destroy = destroy_view};
    struct poulsback_sampler_bank *ba = NULL, *bb = NULL, *bf = NULL, *duplicate = NULL;
    struct pipe_resource *r = image(&screen, 0), *borrowed = r;
    struct pipe_framebuffer_state fb = {0}, input = {.width = 32, .height = 32, .nr_cbufs = 1},
                                  before;
    input.cbufs[0] = (struct pipe_surface){.texture = r, .format = r->format};
    assert(!poulsback_surface_framebuffer_set(&screen, &fb, &input) && r->reference.count == 2);
    pipe_resource_reference(&r, NULL);
    assert(borrowed->reference.count == 1);
    assert(!poulsback_surface_framebuffer_set(&screen, &fb, &fb) && borrowed->reference.count == 1);
    before = fb;
    input = fb;
    input.cbufs[0].level = 1;
    assert(poulsback_surface_framebuffer_set(&screen, &fb, &input) == -EOPNOTSUPP &&
           !memcmp(&fb, &before, sizeof(fb)) && borrowed->reference.count == 1);
    input = fb;
    input.width = 33;
    assert(poulsback_surface_framebuffer_set(&screen, &fb, &input) == -EINVAL &&
           !memcmp(&fb, &before, sizeof(fb)));
    input = (struct pipe_framebuffer_state){0};
    assert(!poulsback_surface_framebuffer_set(&screen, &fb, &input) && destroyed == 1 &&
           !fb.cbufs[0].texture);

    assert(!poulsback_sampler_bank_create(&a, &ba) && !poulsback_sampler_bank_create(&b, &bb) &&
           !poulsback_sampler_bank_create(&foreign, &bf));
    assert(poulsback_sampler_bank_create(&a, &duplicate) == -EEXIST && !duplicate);
    struct pipe_sampler_state state = {.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                                       .wrap_t = PIPE_TEX_WRAP_REPEAT,
                                       .min_img_filter = PIPE_TEX_FILTER_LINEAR,
                                       .mag_img_filter = PIPE_TEX_FILTER_LINEAR,
                                       .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
                                       .min_lod = 0,
                                       .max_lod = 4,
                                       .lod_bias = .5f},
                              snapshot;
    void *sa = NULL, *sb = NULL, *bad = NULL;
    assert(!poulsback_sampler_state_create(ba, &state, &sa) &&
           !poulsback_sampler_state_create(bb, &state, &sb));
    state.lod_bias = 2;
    assert(!poulsback_sampler_state_snapshot(ba, sa, &snapshot) && snapshot.lod_bias == .5f);
    state.min_lod = NAN;
    assert(poulsback_sampler_state_create(ba, &state, &bad) == -EINVAL && !bad);
    void *pair[2] = {sa, sa};
    assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, pair));
    assert(poulsback_sampler_draw_admit(ba) == -EOPNOTSUPP);
    pair[1] = sb;
    assert(poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, pair) == -EINVAL);
    assert(!poulsback_sampler_state_delete(ba, sa) &&
           !poulsback_sampler_state_snapshot(ba, sa, &snapshot));
    assert(poulsback_sampler_state_delete(ba, sa) == -EINVAL);
    assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, NULL));
    assert(poulsback_sampler_state_snapshot(ba, sa, &snapshot) == -EINVAL &&
           !poulsback_sampler_draw_admit(ba));
    assert(poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, PIPE_MAX_SAMPLERS, 1, NULL) ==
           -EINVAL);
    assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_COMPUTE, 0, 0, NULL));

    r = image(&screen, 1);
    struct pipe_sampler_view templ = {.target = PIPE_TEXTURE_2D,
                                      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
                                      .swizzle_r = PIPE_SWIZZLE_X,
                                      .swizzle_g = PIPE_SWIZZLE_Y,
                                      .swizzle_b = PIPE_SWIZZLE_Z,
                                      .swizzle_a = PIPE_SWIZZLE_W};
    struct pipe_sampler_view *v = NULL, *alien = NULL;
    assert(!poulsback_sampler_view_create(ba, r, &templ, &v) && r->reference.count == 2);
    assert(poulsback_sampler_view_create(bf, r, &templ, &alien) == -EINVAL && !alien);
    assert(poulsback_sampler_views_bind(bb, MESA_SHADER_FRAGMENT, 0, 1, 0, &v) == -EINVAL);
    struct pipe_sampler_view *views[2] = {v, v};
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 2, 0, views) &&
           v->reference.count == 3);
    /* A backend draw's lease of unit 0: none without a state, then the
     * state's description and the view's, its resource referenced. */
    {
        struct poulsback_sampler_binding lease;
        void *s0 = NULL;
        assert(poulsback_sampler_fragment_binding(ba, 0, &lease) == -ENOENT);
        state.min_lod = 0;
        assert(!poulsback_sampler_state_create(ba, &state, &s0) &&
               !poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, &s0));
        assert(!poulsback_sampler_fragment_binding(ba, 0, &lease) && lease.resource == r &&
               r->reference.count == 3 && lease.target == PIPE_TEXTURE_2D &&
               lease.format == PIPE_FORMAT_B8G8R8A8_UNORM && lease.swizzle[3] == PIPE_SWIZZLE_W &&
               lease.sampler.mag_img_filter == PIPE_TEX_FILTER_LINEAR);
        pipe_resource_reference(&lease.resource, NULL);
        assert(poulsback_sampler_fragment_binding(ba, PIPE_MAX_SAMPLERS, &lease) == -EINVAL);
        assert(!poulsback_sampler_states_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, NULL) &&
               !poulsback_sampler_state_delete(ba, s0));
    }
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, 1, views) &&
           v->reference.count == 2);
    assert(poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, PIPE_MAX_SHADER_SAMPLER_VIEWS, 0,
                                        1, NULL) == -EINVAL);
    struct pipe_sampler_view *held = v;
    pipe_sampler_view_reference(&v, NULL);
    assert(held->reference.count == 1);
    /* Incoming is borrowed from the old slot's sole reference. */
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, 0, &held) &&
           held->reference.count == 1);
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL));
    assert(r->reference.count == 1 && !poulsback_sampler_draw_admit(ba));
    assert(poulsback_sampler_view_destroy(&a, held) == -EINVAL); /* stale opaque handle */

    assert(!poulsback_sampler_view_create(bb, r, &templ, &v));
    assert(pipe_reference_described_nonatomic(
        &v->reference, NULL, (debug_reference_descriptor)debug_describe_sampler_view));
    assert(poulsback_sampler_view_destroy(&foreign, v) == -EINVAL);
    assert(!poulsback_sampler_view_destroy(&a, v) &&
           r->reference.count == 1); /* valid cross-context */
    v = NULL; /* The direct destructor does not own the caller's pointer slot. */
    assert(poulsback_sampler_view_destroy(&a, (void *)(uintptr_t)1) == -EINVAL);

    assert(!poulsback_sampler_view_create(ba, r, &templ, &v));
    assert(!poulsback_sampler_views_bind(ba, MESA_SHADER_FRAGMENT, 0, 1, 0, &v));
    assert(poulsback_sampler_bank_close(ba) == -EBUSY &&
           poulsback_sampler_draw_admit(ba) == -EBUSY);
    assert(poulsback_sampler_bank_destroy(ba) == -EBUSY);
    pipe_sampler_view_reference(&v, NULL); /* last external ref, bound ref remains */
    assert(!poulsback_sampler_bank_close(ba) && !poulsback_sampler_bank_destroy(ba));
    ba = NULL;
    assert(r->reference.count == 1);
    pipe_resource_reference(&r, NULL);
    assert(destroyed == 2);
    assert(!poulsback_sampler_bank_close(bb) && !poulsback_sampler_bank_destroy(bb));
    assert(!poulsback_sampler_bank_close(bf) && !poulsback_sampler_bank_destroy(bf));
    assert(view_destroys == 0); /* final external refs here were bound; bank owns final drops */
    lifetime(&screen);
    assert(destroyed == 3 && !images[0] && !images[1] && !images[2]);
    puts(
        "PASS actual Mesa26 inline surfaces/samplers/views; transactional refs, foreign/stale handles, terminal close; no GPU sampling");
    return 0;
}
