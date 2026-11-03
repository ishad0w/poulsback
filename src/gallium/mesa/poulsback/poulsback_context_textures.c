// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The textures a backend draw samples (docs/native-3d.md#texture-memory):
 * each unit's bound view and state as the backend reads them, and the open
 * frame's hold on their images until it is submitted, and the target's as
 * the load draw samples it. A texture is a 2D
 * B8G8R8A8 or B8G8R8X8 resource of one level, stored as a stride surface
 * (rows of 32 texels): the hardware clamps to its edge; REPEAT and
 * MIRRORED_REPEAT wrap in USE before a dependent read, under nearest
 * filtering. */

#include "poulsback_context_private.h"
#include "sgx535_texture_words.h"
#include <errno.h>

/* GL's component from a view swizzle on a U8X4 read; an X8 format's alpha is
 * one. */
static uint8_t swizzle(unsigned s, bool x8)
{
    switch (s) {
    case PIPE_SWIZZLE_X:
        return SGX535_FS_SWIZZLE_R;
    case PIPE_SWIZZLE_Y:
        return SGX535_FS_SWIZZLE_G;
    case PIPE_SWIZZLE_Z:
        return SGX535_FS_SWIZZLE_B;
    case PIPE_SWIZZLE_W:
        return x8 ? SGX535_FS_SWIZZLE_ONE : SGX535_FS_SWIZZLE_A;
    case PIPE_SWIZZLE_0:
        return SGX535_FS_SWIZZLE_ZERO;
    default:
        return SGX535_FS_SWIZZLE_ONE;
    }
}

/* An axis' wrap: the hardware's (WORDS) and the USE's (FORM); false without
 * a form. GL_CLAMP under nearest filtering is CLAMP_TO_EDGE; REPEAT and
 * MIRRORED_REPEAT wrap in USE before a dependent read. */
static bool wrap(unsigned mode, bool nearest, uint8_t *words, uint8_t *form)
{
    *words = SGX535_WRAP_CLAMP_TO_EDGE;
    *form = SGX535_FS_WRAP_HW;
    switch (mode) {
    case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
        return true;
    case PIPE_TEX_WRAP_CLAMP:
        return nearest;
    case PIPE_TEX_WRAP_REPEAT:
        *form = SGX535_FS_WRAP_REPEAT;
        return nearest;
    case PIPE_TEX_WRAP_MIRROR_REPEAT:
        *form = SGX535_FS_WRAP_MIRRORED;
        return nearest;
    default:
        return false;
    }
}

/* The frame's hold on IMAGE (R's), once per image; false when full. */
static bool hold(struct poulsback_context *c, struct pipe_resource *r,
                 struct poulsback_ws_image *image)
{
    for (uint32_t i = 0; i < c->frame_texture_count; i++)
        if (c->frame_image[i] == image) {
            poulsback_ws_image_unref(image);
            return true;
        }
    if (c->frame_texture_count == POULSBACK_CONTEXT_FRAME_TEXTURES)
        return false;
    c->frame_image[c->frame_texture_count] = image;
    c->frame_texture[c->frame_texture_count] = NULL;
    pipe_resource_reference(&c->frame_texture[c->frame_texture_count++], r);
    return true;
}

/* R's stride surface under SAMPLER into T (words, size, BO), the frame
 * holding its image. */
static int stride_texture(struct poulsback_context *c, struct pipe_resource *r,
                          const struct sgx535_texture_sampler *sampler,
                          struct poulsback_fragment_texture *t, const char **refusal)
{
    const struct sgx535_texture_view view = {0, 0, 0};
    struct sgx535_texture_layout layout;
    struct poulsback_ws_image *image = NULL;
    uint32_t stride;
    uint64_t size, va;
    int e;
    *refusal = "a texture's rows apart from the stride layout's";
    if (sgx535_texture_layout_init(&layout, SGX535_TEXTURE_2D,
                                   r->format == PIPE_FORMAT_B8G8R8X8_UNORM
                                       ? SGX535_TEXEL_B8G8R8X8_UNORM
                                       : SGX535_TEXEL_B8G8R8A8_UNORM,
                                   r->width0, r->height0, 1, 0, true) != SGX535_TEXTURE_OK ||
        poulsback_resource_layout(r, &stride, &size) || layout.level_pitch[0] != stride)
        return -EOPNOTSUPP;
    e = poulsback_resource_image_acquire(r, &image);
    if (!e)
        e = poulsback_ws_image_gpu(image, &t->handle, &va);
    *refusal = "a texture's words";
    if (!e && (va > UINT32_MAX || sgx535_texture_words(&layout, sampler, &view, (uint32_t)va,
                                                       t->words) != SGX535_TEXTURE_OK))
        e = -EOPNOTSUPP;
    *refusal = "more textures in a frame than it holds";
    if (!e && !hold(c, r, image))
        e = -EOPNOTSUPP;
    if (e) {
        if (image)
            poulsback_ws_image_unref(image);
        return e;
    }
    t->form.result = SGX535_FS_RESULT_U8X4;
    t->width = r->width0;
    t->height = r->height0;
    return 0;
}

static int texture(struct poulsback_context *c, const struct poulsback_sampler_binding *b,
                   struct poulsback_fragment_texture *t, const char **refusal)
{
    const struct pipe_sampler_state *s = &b->sampler;
    struct pipe_resource *r = b->resource;
    const bool x8 = b->format == PIPE_FORMAT_B8G8R8X8_UNORM,
               nearest = s->min_img_filter == PIPE_TEX_FILTER_NEAREST &&
                         s->mag_img_filter == PIPE_TEX_FILTER_NEAREST;
    struct sgx535_texture_sampler sampler = {
        .min_filter = s->min_img_filter == PIPE_TEX_FILTER_LINEAR ? SGX535_FILTER_LINEAR
                                                                  : SGX535_FILTER_NEAREST,
        .mag_filter = s->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? SGX535_FILTER_LINEAR
                                                                  : SGX535_FILTER_NEAREST,
        .mip_filter = SGX535_MIP_NONE,
        .lod_bias = s->lod_bias,
        .min_lod = s->min_lod};
    int e;
    *refusal = "a texture other than a 2D B8G8R8A8 or B8G8R8X8 image of one level";
    if (b->target != PIPE_TEXTURE_2D || b->first_level || b->last_level || b->first_layer ||
        b->last_layer || !r || r->target != PIPE_TEXTURE_2D || r->last_level ||
        b->format != r->format ||
        (b->format != PIPE_FORMAT_B8G8R8A8_UNORM && b->format != PIPE_FORMAT_B8G8R8X8_UNORM))
        return -EOPNOTSUPP;
    *refusal = "a sampler without a form (shadow, border, anisotropy, unnormalized, or a "
               "repeat under linear filtering)";
    if (s->compare_mode != PIPE_TEX_COMPARE_NONE || s->unnormalized_coords ||
        s->max_anisotropy > 1 || !wrap(s->wrap_s, nearest, &sampler.wrap[0], &t->form.wrap[0]) ||
        !wrap(s->wrap_t, nearest, &sampler.wrap[1], &t->form.wrap[1]))
        return -EOPNOTSUPP;
    sampler.wrap[2] = SGX535_WRAP_CLAMP_TO_EDGE;
    e = stride_texture(c, r, &sampler, t, refusal);
    for (unsigned k = 0; !e && k < 4; k++)
        t->form.swizzle[k] = swizzle(b->swizzle[k], x8);
    return e;
}

int poulsback_context_target_texture(struct poulsback_context *c,
                                     struct poulsback_fragment_texture *t, const char **refusal)
{
    const struct sgx535_texture_sampler nearest = {
        {SGX535_WRAP_CLAMP_TO_EDGE, SGX535_WRAP_CLAMP_TO_EDGE, SGX535_WRAP_CLAMP_TO_EDGE},
        SGX535_FILTER_NEAREST,
        SGX535_FILTER_NEAREST,
        SGX535_MIP_NONE,
        0,
        0,
        0.0f,
        0.0f};
    memset(t, 0, sizeof(*t));
    return stride_texture(c, c->scene_target, &nearest, t, refusal);
}

int poulsback_context_textures(struct poulsback_context *c, uint32_t mask,
                               struct poulsback_fragment_texture t[SGX535_FS_MAX_SAMPLERS],
                               const char **refusal)
{
    for (uint32_t u = 0; u < SGX535_FS_MAX_SAMPLERS; u++) {
        struct poulsback_sampler_binding b;
        int e;
        if (!(mask >> u & 1u))
            continue;
        memset(&t[u], 0, sizeof(t[u]));
        e = poulsback_sampler_fragment_binding(c->samplers, u, &b);
        if (e) {
            *refusal = "a sampler unit without a state and view";
            return e == -ENOENT ? -EOPNOTSUPP : e;
        }
        e = texture(c, &b, &t[u], refusal);
        pipe_resource_reference(&b.resource, NULL);
        if (e)
            return e;
    }
    return 0;
}

bool poulsback_context_samples(const struct poulsback_context *c, const struct pipe_resource *r)
{
    for (uint32_t i = 0; r && i < c->frame_texture_count; i++)
        if (c->frame_texture[i] == r)
            return true;
    return false;
}

int poulsback_context_textures_release(struct poulsback_context *c)
{
    int e = 0;
    while (c->frame_texture_count) {
        uint32_t i = --c->frame_texture_count;
        int cleanup = poulsback_ws_image_unref(c->frame_image[i]);
        if (!e)
            e = cleanup;
        pipe_resource_reference(&c->frame_texture[i], NULL);
    }
    return e;
}
