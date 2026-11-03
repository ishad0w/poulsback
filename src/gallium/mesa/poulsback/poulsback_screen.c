// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_public.h"
#include "poulsback_context.h"
#include "poulsback_resource.h"
#include "poulsback_fence.h"
#include "poulsback_nir.h"
#include "pipe/p_screen.h"
#include "draw/draw_context.h"
#include "util/u_screen.h"
#include "util/format/u_format.h"
#include "sgx535_linear.h"
#include "poulsback_render_ir.h"
#include "poulsback_refuse.h"
#include <stdio.h>
#include <xf86drm.h>
#include <stdlib.h>
#include <string.h>

struct poulsback_screen {
    struct pipe_screen base;
    struct poulsback_resource_domain *resources;
    struct pipe_resource *(*resource_create)(struct pipe_screen *, const struct pipe_resource *);
    struct pipe_resource *(*resource_create_with_modifiers)(struct pipe_screen *,
                                                            const struct pipe_resource *,
                                                            const uint64_t *, int);
    int fd;
};
static const char *name(struct pipe_screen *s)
{
    (void)s;
    return "poulsback";
}
static const char *vendor(struct pipe_screen *s)
{
    (void)s;
    return "poulsback contributors";
}
static int screen_fd(struct pipe_screen *base)
{
    return ((struct poulsback_screen *)base)->fd;
}
static void destroy(struct pipe_screen *base)
{
    struct poulsback_screen *s = (struct poulsback_screen *)base;
    /* Domain refuses live resources/contexts/HOLD. Keep the entire owner. */
    if (poulsback_resource_domain_destroy(s->resources))
        return;
    free(s);
}
/* What the frame writer renders: BGRA8/BGRX8 colour targets (also sampled by
 * textured draws), Z24X8 and Z24S8 depth (the hardware's D24S8 either way) and CPU buffers; no
 * multisampling. */
static bool is_format_supported(struct pipe_screen *s, enum pipe_format format,
                                enum pipe_texture_target target, unsigned samples,
                                unsigned storage_samples, unsigned bind)
{
    const unsigned color = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                           PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_LINEAR |
                           PIPE_BIND_SAMPLER_VIEW;
    const unsigned buffer = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_INDEX_BUFFER |
                            PIPE_BIND_CONSTANT_BUFFER | PIPE_BIND_VERTEX_STATE;
    (void)s;
    if (samples > 1 || storage_samples > 1)
        return false;
    if (target == PIPE_BUFFER)
        return !(bind & ~buffer);
    /* rectangle textures have no resource or coordinate form yet */
    if (target != PIPE_TEXTURE_2D)
        return false;
    if (format == PIPE_FORMAT_B8G8R8A8_UNORM || format == PIPE_FORMAT_B8G8R8X8_UNORM)
        return !(bind & ~color);
    if (format == PIPE_FORMAT_Z24X8_UNORM || format == PIPE_FORMAT_Z24_UNORM_S8_UINT)
        return !(bind & ~PIPE_BIND_DEPTH_STENCIL);
    return false;
}
/* Gallium caps of the published screen. Accepted vertex shaders run on the
 * USSE, with Mesa's draw module handling refused shaders and draws. Fragment
 * shader and uniform limits are described in docs/compiler.md. */
static void init_caps(struct pipe_screen *screen)
{
    struct pipe_caps *c = (struct pipe_caps *)&screen->caps;
    struct pipe_shader_caps *fs =
        (struct pipe_shader_caps *)&screen->shader_caps[MESA_SHADER_FRAGMENT];
    struct pipe_shader_caps *vs =
        (struct pipe_shader_caps *)&screen->shader_caps[MESA_SHADER_VERTEX];
    u_init_pipe_screen_caps(screen, 1);
    c->packed_uniforms = true;
    /* GL 2.0 / ES 2.0 need these; draws with state the frame domain does not
     * render (blending, for one) are refused by the context, not faked. */
    c->npot_textures = true;
    /* Images are dma-bufs (kernel image_dmabuf_ops, userspace import_resource):
     * GBM and the DRI image loader allocate window buffers through them. */
    c->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
    /* CPU buffers map per transfer; persistent mapping is not implemented. */
    c->buffer_map_persistent_coherent = false;
    /* The GPU's loss reaches robust contexts as an unknown reset
     * (poulsback_lost.h); robust buffer access stays refused. */
    c->device_reset_status_query = true;
    c->blend_equation_separate = true;
    /* gl_FragCoord as the fragment backend delivers it: GL's lower-left origin
     * (y = height - window y) at pixel centres; without a convention st/mesa
     * stops on any shader that reads it. */
    c->fs_coord_origin_lower_left = true;
    c->fs_coord_pixel_center_half_integer = true;
    /* A scissored clear is a quad of the context's own, not
     * st/mesa's draw with its clear shaders. */
    c->clear_scissored = true;
    c->max_texture_2d_size = 2048;
    c->max_texture_3d_levels = 1;
    c->max_texture_cube_levels = 1;
    c->max_texture_array_layers = 0;
    c->rasterizer_subpixel_bits = 4;
    c->min_line_width = c->min_line_width_aa = c->max_line_width = c->max_line_width_aa = 1.0f;
    c->min_point_size = c->min_point_size_aa = c->max_point_size = c->max_point_size_aa = 1.0f;
    c->point_size_granularity = c->line_width_granularity = 0.1f;
    draw_init_shader_caps(vs);
    /* The context binds no shader buffers, images, atomics or vertex-stage
     * samplers; st/mesa calls the matching setters for every advertised slot. */
    vs->max_shader_buffers = vs->max_shader_images = 0;
    vs->max_hw_atomic_counters = vs->max_hw_atomic_counter_buffers = 0;
    vs->max_texture_samplers = vs->max_sampler_views = 0;
    vs->max_const_buffers = 1;
    *fs = (struct pipe_shader_caps){.max_instructions = SGX535_NIR_MAX_INSTRUCTIONS,
                                    .max_alu_instructions = SGX535_NIR_MAX_INSTRUCTIONS,
                                    .max_control_flow_depth = 0,
                                    .max_inputs = 8,
                                    .max_outputs = 1,
                                    .max_const_buffer0_size = POULSBACK_RENDER_UNIFORM_WORDS * 4,
                                    .max_const_buffers = 1,
                                    .max_temps = 64,
                                    /* A draw samples at most one texture. */
                                    .max_texture_samplers = 1,
                                    .max_sampler_views = 1,
                                    /* Mesa pairs packed (dword) uniforms with native
                                     * integers; the compiler has bool32/bitwise ops
                                     * and refuses the rest at compile time. */
                                    .integers = true,
                                    .supported_irs = 1 << PIPE_SHADER_IR_NIR};
}
/* The domain's resource_create; a refusal names the template on stderr. */
static struct pipe_resource *published_resource_create(struct pipe_screen *base,
                                                       const struct pipe_resource *t)
{
    struct poulsback_screen *s = (struct poulsback_screen *)base;
    struct pipe_resource *r = s->resource_create(base, t);
    if (r && t->target != PIPE_BUFFER)
        POULSBACK_TRACE("resource %s %ux%u bind 0x%x\n", util_format_name(t->format), t->width0,
                        t->height0, t->bind);
    if (!r)
        fprintf(stderr,
                "poulsback: resource: create target %u %s %ux%u bind 0x%x usage %u flags 0x%x "
                "failed; domain error %d\n",
                t->target, util_format_name(t->format), t->width0, t->height0, t->bind, t->usage,
                t->flags, poulsback_resource_domain_error(s->resources));
    return r;
}
static struct pipe_resource *published_resource_create_with_modifiers(
    struct pipe_screen *base, const struct pipe_resource *t, const uint64_t *modifiers, int count)
{
    struct poulsback_screen *s = (struct poulsback_screen *)base;
    struct pipe_resource *r = s->resource_create_with_modifiers(base, t, modifiers, count);
    if (r)
        POULSBACK_TRACE("resource %s %ux%u bind 0x%x (%d modifiers)\n", util_format_name(t->format),
                        t->width0, t->height0, t->bind, count);
    if (!r)
        fprintf(stderr,
                "poulsback: resource: create %s %ux%u bind 0x%x with %d modifiers (first 0x%llx) "
                "failed; domain error %d\n",
                util_format_name(t->format), t->width0, t->height0, t->bind, count,
                count > 0 ? (unsigned long long)modifiers[0] : 0ull,
                poulsback_resource_domain_error(s->resources));
    return r;
}
/* Mesa's context hints change nothing here; requirements are still refused. */
static struct pipe_context *published_context_create(struct pipe_screen *base, void *priv,
                                                     unsigned flags)
{
    const unsigned hints = PIPE_CONTEXT_PREFER_THREADED | PIPE_CONTEXT_HIGH_PRIORITY |
                           PIPE_CONTEXT_LOW_PRIORITY | PIPE_CONTEXT_REALTIME_PRIORITY |
                           PIPE_CONTEXT_NO_LOD_BIAS;
    struct pipe_context *c = poulsback_context_create(base, priv, flags & ~hints);
    if (!c)
        fprintf(stderr, "poulsback: context: create with flags 0x%x failed; domain error %d\n", flags,
                poulsback_resource_domain_error(((struct poulsback_screen *)base)->resources));
    return c;
}
static struct pipe_screen *screen_create(int fd)
{
    struct poulsback_screen *s = calloc(1, sizeof(*s));
    int ret;
    if (!s)
        return NULL;
    s->fd = fd;
    poulsback_nir_screen_init(&s->base);
    init_caps(&s->base);
    ret = poulsback_resource_domain_create(&s->base, fd, &s->resources);
    if (ret) {
        fprintf(stderr, "poulsback: screen: resource domain: %s (%d)\n", strerror(-ret), ret);
        free(s);
        return NULL;
    }
    ret = poulsback_fence_screen_init(&s->base);
    if (ret) {
        fprintf(stderr, "poulsback: screen: fences: %s (%d)\n", strerror(-ret), ret);
        if (!poulsback_resource_domain_destroy(s->resources))
            free(s);
        return NULL;
    }
    s->base.destroy = destroy;
    s->base.get_name = name;
    s->base.get_vendor = vendor;
    s->base.get_device_vendor = vendor;
    s->base.context_create = published_context_create;
    s->resource_create = s->base.resource_create;
    s->base.resource_create = published_resource_create;
    s->resource_create_with_modifiers = s->base.resource_create_with_modifiers;
    s->base.resource_create_with_modifiers = published_resource_create_with_modifiers;
    s->base.get_screen_fd = screen_fd;
    s->base.is_format_supported = is_format_supported;
    return &s->base;
}

/* The published screen: frames the frame writer builds (poulsback_frame.h) run
 * on the kernel's engine (poulsback_engine.c). */
struct pipe_screen *poulsback_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
    enum poulsback_drm_identity identity = poulsback_drm_identify(fd);
    (void)config;
    if (identity != POULSBACK_DRM_NATIVE_UNQUALIFIED) {
        fprintf(stderr, "poulsback: screen: fd %d is not a poulsback device (identity %d)\n", fd,
                identity);
        return NULL;
    }
    return screen_create(fd);
}
