// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Deterministic read-only ioctl fixture; no DRM node or GL context is opened. */
#include "poulsback_public.h"
#include "poulsback_context.h"
#include "poulsback_resource.h"
#include "pipe/p_screen.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <xf86drm.h>

static drmVersion fixture;
static unsigned query_count, free_count;
static int query_fails;
static unsigned domain_creates, domain_destroys;
static bool domain_fails;
static struct pipe_screen *domain_screen;

/* Keep constructor ownership checks independent of opening a DRM namespace.
 * Actual context/uniform/draw consumers have their separate Mesa fixtures. */
int poulsback_resource_domain_create(struct pipe_screen *screen, int fd,
                                     struct poulsback_resource_domain **out)
{
    assert(screen && fd == 7 && out);
    domain_creates++;
    if (domain_fails)
        return -ENOMEM;
    assert(!domain_screen);
    domain_screen = screen;
    *out = (struct poulsback_resource_domain *)&domain_screen;
    return 0;
}

int poulsback_resource_domain_error(struct poulsback_resource_domain *domain)
{
    (void)domain;
    return 0;
}

int poulsback_resource_domain_destroy(struct poulsback_resource_domain *domain)
{
    assert(domain == (struct poulsback_resource_domain *)&domain_screen && domain_screen);
    domain_screen = NULL;
    domain_destroys++;
    return 0;
}

struct pipe_context *poulsback_context_create(struct pipe_screen *screen, void *private,
                                              unsigned flags)
{
    (void)screen;
    (void)private;
    (void)flags;
    assert(!"loader construction must not create a context");
    return NULL;
}

drmVersionPtr drmGetVersion(int fd)
{
    assert(fd == 7);
    query_count++;
    return query_fails ? NULL : &fixture;
}

void drmFreeVersion(drmVersionPtr version)
{
    assert(version == &fixture);
    free_count++;
}

int main(void)
{
    static char exact[] = "poulsback";
    static char prefix[] = "poulsback-extra";
    static char nul_name[] = "pouls\0ack";
    static char wrong[] = "i915";

    assert(poulsback_drm_identify(-1) == POULSBACK_DRM_INVALID_FD);
    assert(query_count == 0 && free_count == 0);
    query_fails = 1;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_QUERY_FAILED);
    assert(query_count == 1 && free_count == 0);
    query_fails = 0;
    fixture.name = NULL;
    fixture.name_len = 9;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_OTHER_DRIVER);
    fixture.name = wrong;
    fixture.name_len = 4;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_OTHER_DRIVER);
    fixture.name = prefix;
    fixture.name_len = (int)strlen(prefix);
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_OTHER_DRIVER);
    fixture.name = nul_name;
    fixture.name_len = 9;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_OTHER_DRIVER);
    fixture.name = exact;
    fixture.name_len = 8;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_OTHER_DRIVER);
    fixture.name_len = 9;
    assert(poulsback_drm_identify(7) == POULSBACK_DRM_NATIVE_UNQUALIFIED);
    domain_fails = true;
    assert(!poulsback_drm_screen_create(7, NULL));
    assert(domain_creates == 1 && !domain_destroys && !domain_screen);
    assert(query_count == 8 && free_count == 7);
    domain_fails = false;
    struct pipe_screen *screen = poulsback_drm_screen_create(7, NULL);
    assert(screen && screen == domain_screen && domain_creates == 2);
    /* The published screen wraps context creation to drop Mesa's hints. */
    assert(screen->caps.packed_uniforms && screen->caps.accelerated &&
           screen->context_create != poulsback_context_create);
    assert(screen->shader_caps[MESA_SHADER_FRAGMENT].max_const_buffer0_size &&
           screen->get_screen_fd(screen) == 7 && screen->is_format_supported);
    /* a 2D colour format is admitted; a rectangle texture, which no resource
     * or view takes, is not */
    assert(screen->is_format_supported(screen, PIPE_FORMAT_B8G8R8A8_UNORM, PIPE_TEXTURE_2D, 0, 0,
                                       PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET));
    assert(!screen->is_format_supported(screen, PIPE_FORMAT_B8G8R8A8_UNORM, PIPE_TEXTURE_RECT, 0, 0,
                                        PIPE_BIND_SAMPLER_VIEW) &&
           !screen->is_format_supported(screen, PIPE_FORMAT_B8G8R8X8_UNORM, PIPE_TEXTURE_RECT, 0, 0,
                                        PIPE_BIND_RENDER_TARGET));
    assert(screen->fence_reference && screen->fence_finish && screen->fence_get_fd);
    screen->destroy(screen);
    assert(!domain_screen && domain_destroys == 1 && query_count == 9 && free_count == 8);
    return 0;
}
