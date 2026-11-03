// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_normalized_color0_mask_list.h"
#include "sgx535_clip_w1.h"
#include "sgx535_normalized_color0_window_list.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
static int span(const void *p, unsigned long n)
{
    return p && n <= ~(unsigned long)0 - (unsigned long)p;
}
static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int arguments(const struct sgx535_normalized_color0_mask_list_request *r,
                     const struct sgx535_normalized_color0_mask_list_task *out,
                     const struct sgx535_normalized_color0_mask_list_workspace *w)
{
    unsigned long geometry, ir;
    if (!span(r, sizeof(*r)) || !span(out, sizeof(*out)) || !span(w, sizeof(*w)) ||
        !r->triangle_count ||
        r->triangle_count > SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES ||
        r->fragment_count < 5 || r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    geometry = (unsigned long)r->triangle_count * sizeof(*r->triangles);
    ir = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r->triangles, geometry) && span(r->fragment, ir) &&
           !overlap(r, sizeof(*r), out, sizeof(*out)) && !overlap(r, sizeof(*r), w, sizeof(*w)) &&
           !overlap(out, sizeof(*out), w, sizeof(*w)) &&
           !overlap(r->triangles, geometry, out, sizeof(*out)) &&
           !overlap(r->triangles, geometry, w, sizeof(*w)) &&
           !overlap(r->fragment, ir, out, sizeof(*out)) && !overlap(r->fragment, ir, w, sizeof(*w));
}
static int same_request(const struct sgx535_normalized_color0_mask_list_request *a,
                        const struct sgx535_normalized_color0_mask_list_request *b)
{
    return a->version == b->version && a->profile == b->profile && a->core_id == b->core_id &&
           a->revision == b->revision && a->triangles == b->triangles &&
           a->triangle_count == b->triangle_count && a->interpolation == b->interpolation &&
           a->fragment == b->fragment && a->fragment_count == b->fragment_count &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved));
}
static void put(uint8_t *p, uint32_t x)
{
    uint32_t i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> (i * 8u));
}
static void put_index(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
}
#include "sgx535_normalized_color0_list_internal.h"
static enum sgx535_task_status construct(const struct sgx535_normalized_color0_mask_list_request *r,
                                         struct sgx535_normalized_color0_mask_list_workspace *w)
{
    struct sgx535_normalized_color0_tile16_request first = {0};
    struct sgx535_normalized_color0_mask_list_task *out = &w->result;
    enum sgx535_task_status status;
    uint32_t i, j;
    if (r->version != SGX535_NORMALIZED_COLOR0_MASK_LIST_VERSION ||
        r->profile != SGX535_NORMALIZED_COLOR0_MASK_LIST_PROFILE ||
        (r->interpolation != SGX535_VARYING_SMOOTH &&
         r->interpolation != SGX535_VARYING_FLAT_REPLICATED) ||
        r->reserved[0] || r->reserved[1] || r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    memcpy(w->triangles, r->triangles, (unsigned long)r->triangle_count * sizeof(*r->triangles));
    memcpy(w->fragment, r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment));
    for (i = 0; i < r->triangle_count; i++)
        for (j = 0; j < 3; j++) {
            const struct sgx535_normalized_color0_mask_list_triangle *t = &w->triangles[i];
            uint32_t c, *v = w->vertex_words + i * 24u + j * 8u;
            if (sgx535_clip_w1_encode(t->clip_position[j], 16, 16, v) != SGX535_CLIP_W1_OK)
                return SGX535_TASK_UNSUPPORTED;
            for (c = 0; c < 4; c++) {
                if (t->rgba[j][c] > 0x3f800000u ||
                    (r->interpolation == SGX535_VARYING_FLAT_REPLICATED &&
                     t->rgba[j][c] != t->rgba[0][c]))
                    return SGX535_TASK_UNSUPPORTED;
                v[4u + c] = t->rgba[j][c];
            }
        }
    first.version = SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION;
    first.profile = SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE;
    first.core_id = r->core_id;
    first.revision = r->revision;
    first.interpolation = r->interpolation;
    first.fragment = w->fragment;
    first.fragment_count = r->fragment_count;
    memcpy(first.clip_position, w->triangles[0].clip_position, sizeof(first.clip_position));
    memcpy(first.rgba, w->triangles[0].rgba, sizeof(first.rgba));
    /* Compile exactly once. The seed is unpublished workspace, not an accepted
     * caller packet. The list is then constructed canonically as one task. */
    status = sgx535_normalized_color0_mask_tile16_task_build(&first, &w->seed, &w->single);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = normalized_color0_list_repack(&w->seed, r->triangle_count, w->vertex_words, out);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    out->task.packet.version = r->version;
    out->task.packet.profile = r->profile;
    if (memcmp(r->triangles, w->triangles,
               (unsigned long)r->triangle_count * sizeof(*r->triangles)) ||
        memcmp(r->fragment, w->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment)))
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status
sgx535_normalized_color0_mask_list_build(const struct sgx535_normalized_color0_mask_list_request *r,
                                         struct sgx535_normalized_color0_mask_list_task *out,
                                         struct sgx535_normalized_color0_mask_list_workspace *w)
{
    struct sgx535_normalized_color0_mask_list_request frozen;
    enum sgx535_task_status status;
    if (!arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !same_request(r, &frozen))
        status = SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_list_validate(
    const struct sgx535_normalized_color0_mask_list_request *r,
    const struct sgx535_normalized_color0_mask_list_task *out,
    struct sgx535_normalized_color0_mask_list_workspace *w)
{
    struct sgx535_normalized_color0_mask_list_request frozen;
    enum sgx535_task_status status;
    if (!arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !same_request(r, &frozen))
        status = SGX535_TASK_UNSUPPORTED;
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}

static int window_list_arguments(const struct sgx535_normalized_color0_window_list_request *r,
                                 const struct sgx535_normalized_color0_window_list_task *out,
                                 const struct sgx535_normalized_color0_window_list_workspace *w)
{
    unsigned long geometry, ir;
    if (!span(r, sizeof(*r)) || !span(out, sizeof(*out)) || !span(w, sizeof(*w)) ||
        !r->triangle_count ||
        r->triangle_count > SGX535_NORMALIZED_COLOR0_WINDOW_LIST_INPUT_TRIANGLES ||
        r->fragment_count < 5 || r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    geometry = (unsigned long)r->triangle_count * sizeof(*r->triangles);
    ir = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r->triangles, geometry) && span(r->fragment, ir) &&
           !overlap(r, sizeof(*r), out, sizeof(*out)) && !overlap(r, sizeof(*r), w, sizeof(*w)) &&
           !overlap(out, sizeof(*out), w, sizeof(*w)) &&
           !overlap(r->triangles, geometry, out, sizeof(*out)) &&
           !overlap(r->triangles, geometry, w, sizeof(*w)) &&
           !overlap(r->fragment, ir, out, sizeof(*out)) && !overlap(r->fragment, ir, w, sizeof(*w));
}
static int window_list_same_request(const struct sgx535_normalized_color0_window_list_request *a,
                                    const struct sgx535_normalized_color0_window_list_request *b)
{
    return a->version == b->version && a->profile == b->profile && a->core_id == b->core_id &&
           a->revision == b->revision && a->triangles == b->triangles &&
           a->triangle_count == b->triangle_count && a->interpolation == b->interpolation &&
           a->fragment == b->fragment && a->fragment_count == b->fragment_count &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved));
}
static int window_list_xy(uint32_t word)
{
    uint32_t magnitude = word & 0x7fffffffu;
    return magnitude <= 0x41800000u && (!(word >> 31) || !magnitude);
}
static enum sgx535_task_status
window_list_construct(const struct sgx535_normalized_color0_window_list_request *r,
                      struct sgx535_normalized_color0_window_list_workspace *w)
{
    /* These positions produce only a private shared program/state/mask seed.
     * Foreground geometry comes exclusively from the copied window tuples. */
    static const uint32_t seed_clip[3][4] = {{0xbf400000u, 0x3f400000u, 0, 0x3f800000u},
                                             {0x3f400000u, 0x3f400000u, 0, 0x3f800000u},
                                             {0x3f200000u, 0xbf000000u, 0, 0x3f800000u}};
    struct sgx535_normalized_color0_tile16_request first = {0};
    enum sgx535_task_status status;
    uint32_t i, j, flat = r->interpolation == SGX535_VARYING_FLAT_REPLICATED;
    if (r->version != SGX535_NORMALIZED_COLOR0_WINDOW_LIST_VERSION ||
        r->profile != SGX535_NORMALIZED_COLOR0_WINDOW_LIST_PROFILE ||
        (r->interpolation != SGX535_VARYING_SMOOTH && !flat) || r->reserved[0] || r->reserved[1] ||
        r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    memcpy(w->triangles, r->triangles, (unsigned long)r->triangle_count * sizeof(*r->triangles));
    memcpy(w->fragment, r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment));
    for (i = 0; i < r->triangle_count; i++) {
        const struct sgx535_window_color0_triangle *t = &w->triangles[i];
        if (t->replicated_flat != flat ||
            sgx535_window_color0_encode(t, w->vertex_words + i * 24u) != SGX535_WINDOW_VERTEX_OK)
            return SGX535_TASK_UNSUPPORTED;
        for (j = 0; j < 3; j++)
            if (!window_list_xy(t->window_xyz_reciprocal_w[j][0]) ||
                !window_list_xy(t->window_xyz_reciprocal_w[j][1]))
                return SGX535_TASK_UNSUPPORTED;
    }
    first.version = SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION;
    first.profile = SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE;
    first.core_id = r->core_id;
    first.revision = r->revision;
    first.interpolation = r->interpolation;
    first.fragment = w->fragment;
    first.fragment_count = r->fragment_count;
    memcpy(first.clip_position, seed_clip, sizeof(seed_clip));
    memcpy(first.rgba, w->triangles[0].rgba, sizeof(first.rgba));
    status = sgx535_normalized_color0_mask_tile16_task_build(&first, &w->seed, &w->single);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    memset(&w->result, 0, sizeof(w->result));
    status = normalized_color0_list_repack(&w->seed, r->triangle_count, w->vertex_words,
                                           &w->result.list);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    w->result.list.task.packet.version = r->version;
    w->result.list.task.packet.profile = r->profile;
    w->result.missing_window_contracts = SGX535_WINDOW_LIST_MISSING_ALL;
    if (memcmp(r->triangles, w->triangles,
               (unsigned long)r->triangle_count * sizeof(*r->triangles)) ||
        memcmp(r->fragment, w->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment)))
        return SGX535_TASK_UNSUPPORTED;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_build(
    const struct sgx535_normalized_color0_window_list_request *r,
    struct sgx535_normalized_color0_window_list_task *out,
    struct sgx535_normalized_color0_window_list_workspace *w)
{
    struct sgx535_normalized_color0_window_list_request frozen;
    enum sgx535_task_status status;
    if (!window_list_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_same_request(r, &frozen))
        status = SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_validate(
    const struct sgx535_normalized_color0_window_list_request *r,
    const struct sgx535_normalized_color0_window_list_task *out,
    struct sgx535_normalized_color0_window_list_workspace *w)
{
    struct sgx535_normalized_color0_window_list_request frozen;
    enum sgx535_task_status status;
    if (!window_list_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_same_request(r, &frozen))
        status = SGX535_TASK_UNSUPPORTED;
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
