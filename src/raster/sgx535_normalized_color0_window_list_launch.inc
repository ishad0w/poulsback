// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_normalized_color0_window_list.h"
static int window_list_arguments(const struct sgx535_normalized_color0_window_list_request *r,
                                 const void *out, unsigned long on, const void *w, unsigned long wn)
{
    if (!normalized_tile16_span(r, sizeof(*r)) || !normalized_tile16_span(out, on) ||
        !normalized_tile16_span(w, wn) || overlap(out, on, w, wn) || !r->triangle_count ||
        r->triangle_count > SGX535_NORMALIZED_COLOR0_WINDOW_LIST_INPUT_TRIANGLES ||
        r->fragment_count < 5 || r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    return mask_list_extra(r, sizeof(*r), out, on, w, wn) &&
           mask_list_extra(r->triangles, (unsigned long)r->triangle_count * sizeof(*r->triangles),
                           out, on, w, wn) &&
           mask_list_extra(r->fragment, (unsigned long)r->fragment_count * sizeof(*r->fragment),
                           out, on, w, wn);
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
static int window_list_copied_equal(const struct sgx535_normalized_color0_window_list_request *r,
                                    const struct sgx535_normalized_color0_window_list_workspace *w)
{
    return !memcmp(r->triangles, w->triangles,
                   (unsigned long)r->triangle_count * sizeof(*r->triangles)) &&
           !memcmp(r->fragment, w->fragment,
                   (unsigned long)r->fragment_count * sizeof(*r->fragment));
}
static int
window_list_launch_arguments(const struct sgx535_normalized_color0_window_list_request *r,
                             const struct sgx535_normalized_color0_window_list_task *p,
                             const struct sgx535_task_launch_binding *b,
                             const struct sgx535_normalized_color0_window_list_launch *out,
                             struct sgx535_normalized_color0_window_list_launch_workspace *w)
{
    return window_list_arguments(r, out, sizeof(*out), w, sizeof(*w)) &&
           mask_list_extra(p, sizeof(*p), out, sizeof(*out), w, sizeof(*w)) &&
           mask_list_extra(b, sizeof(*b), out, sizeof(*out), w, sizeof(*w));
}
static enum sgx535_task_status
window_list_launch_construct(const struct sgx535_normalized_color0_window_list_request *r,
                             const struct sgx535_normalized_color0_window_list_task *p,
                             const struct sgx535_task_launch_binding *b,
                             struct sgx535_normalized_color0_window_list_launch_workspace *w)
{
    struct sgx535_task_launch_binding frozen = *b;
    enum sgx535_task_status status =
        sgx535_normalized_color0_window_list_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status =
        construct_validated_packet(&w->packet.result.list.task.packet, &frozen, &w->projection);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(b, &frozen, sizeof(frozen)))
        return SGX535_TASK_INVALID;
    memset(&w->result, 0, sizeof(w->result));
    w->result.normalized.launch = w->projection.result;
    w->result.normalized.fragment_missing_contracts =
        w->packet.result.list.task.fragment.missing_contracts;
    w->result.normalized.reference_policy = w->packet.result.list.task.fragment.reference_policy;
    w->result.missing_window_contracts = SGX535_WINDOW_LIST_MISSING_ALL;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_launch_build(
    const struct sgx535_normalized_color0_window_list_request *r,
    const struct sgx535_normalized_color0_window_list_task *p,
    const struct sgx535_task_launch_binding *b,
    struct sgx535_normalized_color0_window_list_launch *out,
    struct sgx535_normalized_color0_window_list_launch_workspace *w)
{
    struct sgx535_normalized_color0_window_list_request frozen;
    enum sgx535_task_status status;
    if (!window_list_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_launch_construct(&frozen, p, b, w);
    if (status == SGX535_TASK_BLOCKED &&
        (!window_list_same_request(r, &frozen) || !window_list_copied_equal(r, &w->packet)))
        status = SGX535_TASK_INVALID;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_launch_validate(
    const struct sgx535_normalized_color0_window_list_request *r,
    const struct sgx535_normalized_color0_window_list_task *p,
    const struct sgx535_task_launch_binding *b,
    const struct sgx535_normalized_color0_window_list_launch *out,
    struct sgx535_normalized_color0_window_list_launch_workspace *w)
{
    struct sgx535_normalized_color0_window_list_request frozen;
    enum sgx535_task_status status;
    if (!window_list_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_launch_construct(&frozen, p, b, w);
    if (status == SGX535_TASK_BLOCKED &&
        (!window_list_same_request(r, &frozen) || !window_list_copied_equal(r, &w->packet)))
        status = SGX535_TASK_INVALID;
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
static int window_list_background_arguments(
    const struct sgx535_normalized_color0_window_list_background_request *r, const void *out,
    unsigned long on, struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    return normalized_tile16_span(r, sizeof(*r)) &&
           window_list_arguments(&r->foreground, out, on, w, sizeof(*w)) &&
           mask_list_extra(r, sizeof(*r), out, on, w, sizeof(*w));
}
static struct sgx535_background_request window_list_background_request(
    const struct sgx535_normalized_color0_window_list_background_request *r)
{
    struct sgx535_background_request b = {0};
    b.version = SGX535_BACKGROUND_NORMALIZED_TILE16_VERSION;
    b.core_id = r->foreground.core_id;
    b.revision = r->foreground.revision;
    b.width = b.height = 16;
    b.packed_argb = r->packed_background_argb;
    return b;
}
static int
window_list_background_same(const struct sgx535_normalized_color0_window_list_background_request *a,
                            const struct sgx535_normalized_color0_window_list_background_request *b,
                            const struct sgx535_normalized_color0_window_list_workspace *w)
{
    return a->version == b->version && a->packed_background_argb == b->packed_background_argb &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved)) &&
           window_list_same_request(&a->foreground, &b->foreground) &&
           window_list_copied_equal(&a->foreground, w);
}
static enum sgx535_task_status window_list_background_construct(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_background_request bg = window_list_background_request(r);
    struct sgx535_normalized_color0_window_list_background_image *p = &w->image;
    enum sgx535_task_status status;
    if (r->version != SGX535_NORMALIZED_COLOR0_WINDOW_LIST_BACKGROUND_VERSION || r->reserved[0] ||
        r->reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    memset(p, 0, sizeof(*p));
    status =
        sgx535_normalized_color0_window_list_build(&r->foreground, &p->foreground, &w->foreground);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = sgx535_background_normalized_tile16_seed(&bg, &p->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = normalized_list_background_disjoint(&p->foreground.list.task, &p->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    p->version = r->version;
    p->missing_background_contracts = SGX535_BACKGROUND_MISSING_ALL;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_background_build(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    struct sgx535_normalized_color0_window_list_background_image *out,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_normalized_color0_window_list_background_request frozen;
    enum sgx535_task_status status;
    if (!window_list_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_background_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_background_same(r, &frozen, &w->foreground))
        status = SGX535_TASK_INVALID;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->image;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_background_validate(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    const struct sgx535_normalized_color0_window_list_background_image *out,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_normalized_color0_window_list_background_request frozen;
    enum sgx535_task_status status;
    if (!window_list_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_background_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_background_same(r, &frozen, &w->foreground))
        status = SGX535_TASK_INVALID;
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->image, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
static int window_list_background_bind_arguments(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    const struct sgx535_normalized_color0_window_list_background_image *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    const struct sgx535_normalized_color0_window_list_background_bound *out,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    return window_list_background_arguments(r, out, sizeof(*out), w) &&
           mask_list_extra(p, sizeof(*p), out, sizeof(*out), w, sizeof(*w)) &&
           mask_list_extra(b, sizeof(*b), out, sizeof(*out), w, sizeof(*w)) &&
           mask_list_extra(bb, sizeof(*bb), out, sizeof(*out), w, sizeof(*w));
}
static enum sgx535_task_status window_list_background_bind_construct(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    const struct sgx535_normalized_color0_window_list_background_image *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_task_launch_binding frozen = *b;
    struct sgx535_background_binding frozen_bg = *bb;
    struct sgx535_background_request bg = window_list_background_request(r);
    enum sgx535_task_status status = window_list_background_construct(r, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(p, &w->image, sizeof(*p)))
        return SGX535_TASK_INVALID;
    status = sgx535_background_normalized_tile16_bind_seed(&bg, &w->image.background, &frozen_bg,
                                                           &w->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = construct_validated_packet_policy(
        &w->image.foreground.list.task.packet, &frozen, &w->projection,
        SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, &w->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (!color0_background_binding_matches(&frozen, &frozen_bg) ||
        memcmp(b, &frozen, sizeof(frozen)) || memcmp(bb, &frozen_bg, sizeof(frozen_bg)))
        return SGX535_TASK_INVALID;
    memset(&w->result, 0, sizeof(w->result));
    w->result.normalized.version = r->version;
    w->result.normalized.missing_background_contracts = SGX535_BACKGROUND_MISSING_ALL;
    w->result.normalized.launch.launch = w->projection.result;
    w->result.normalized.launch.fragment_missing_contracts =
        w->image.foreground.list.task.fragment.missing_contracts;
    w->result.normalized.launch.reference_policy =
        w->image.foreground.list.task.fragment.reference_policy;
    w->result.normalized.background = w->background;
    w->result.missing_window_contracts = SGX535_WINDOW_LIST_MISSING_ALL;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_background_bind(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    const struct sgx535_normalized_color0_window_list_background_image *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    struct sgx535_normalized_color0_window_list_background_bound *out,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_normalized_color0_window_list_background_request frozen;
    enum sgx535_task_status status;
    if (!window_list_background_bind_arguments(r, p, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_background_bind_construct(&frozen, p, b, bb, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_background_same(r, &frozen, &w->foreground))
        status = SGX535_TASK_INVALID;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_window_list_background_bound_validate(
    const struct sgx535_normalized_color0_window_list_background_request *r,
    const struct sgx535_normalized_color0_window_list_background_image *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    const struct sgx535_normalized_color0_window_list_background_bound *out,
    struct sgx535_normalized_color0_window_list_background_workspace *w)
{
    struct sgx535_normalized_color0_window_list_background_request frozen;
    enum sgx535_task_status status;
    if (!window_list_background_bind_arguments(r, p, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_list_background_bind_construct(&frozen, p, b, bb, w);
    if (status == SGX535_TASK_BLOCKED && !window_list_background_same(r, &frozen, &w->foreground))
        status = SGX535_TASK_INVALID;
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
