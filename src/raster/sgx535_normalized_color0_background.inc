// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Typed composition reuses task_launch.c's private canonical projector. */
#include "sgx535_normalized_color0_background.h"
#include "sgx535_background_internal.h"
static int color0_background_arguments(const struct sgx535_normalized_color0_background_request *r,
                                       const void *out, unsigned long bytes,
                                       struct sgx535_normalized_color0_background_workspace *w)
{
    unsigned long ir_bytes;
    if (!r || r->foreground.fragment_count < 5 ||
        r->foreground.fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    ir_bytes = (unsigned long)r->foreground.fragment_count * sizeof(*r->foreground.fragment);
    return normalized_tile16_span(r, sizeof(*r)) && normalized_tile16_span(out, bytes) &&
           normalized_tile16_span(w, sizeof(*w)) &&
           normalized_tile16_span(r->foreground.fragment, ir_bytes) &&
           !overlap(r, sizeof(*r), out, bytes) && !overlap(r, sizeof(*r), w, sizeof(*w)) &&
           !overlap(out, bytes, w, sizeof(*w)) &&
           !overlap(r->foreground.fragment, ir_bytes, out, bytes) &&
           !overlap(r->foreground.fragment, ir_bytes, w, sizeof(*w));
}
static struct sgx535_background_request
color0_background_request(const struct sgx535_normalized_color0_background_request *r)
{
    struct sgx535_background_request b = {0};
    b.version = SGX535_BACKGROUND_NORMALIZED_TILE16_VERSION;
    b.core_id = r->foreground.core_id;
    b.revision = r->foreground.revision;
    b.width = b.height = 16;
    b.packed_argb = r->packed_background_argb;
    return b;
}
static enum sgx535_task_status
color0_background_construct(const struct sgx535_normalized_color0_background_request *r,
                            struct sgx535_normalized_color0_background_workspace *w, int mask)
{
    struct sgx535_background_request background = color0_background_request(r);
    struct sgx535_normalized_color0_background_image *p = &w->image;
    enum sgx535_task_status status;
    uint32_t i, j;
    if (r->version != (mask ? SGX535_NORMALIZED_COLOR0_MASK_BACKGROUND_VERSION
                            : SGX535_NORMALIZED_COLOR0_BACKGROUND_VERSION) ||
        r->reserved[0] || r->reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    memset(p, 0, sizeof(*p));
    status = mask ? sgx535_normalized_color0_mask_tile16_task_build(&r->foreground, &p->foreground,
                                                                    &w->foreground)
                  : sgx535_normalized_color0_tile16_task_build(&r->foreground, &p->foreground,
                                                               &w->foreground);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = sgx535_background_normalized_tile16_seed(&background, &p->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (p->foreground.fragment_use_extent_bytes > SGX535_BACKGROUND_NORMALIZED_USE_OFFSET ||
        p->background.sections[0].resource_offset != SGX535_BACKGROUND_NORMALIZED_USE_OFFSET ||
        p->background.sections[0].byte_count != 8 ||
        SGX535_BACKGROUND_NORMALIZED_USE_OFFSET + 8u > 4096u)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < SGX535_BACKGROUND_SECTIONS; i++) {
        const struct sgx535_background_section *a = &p->background.sections[i];
        uint32_t role;
        if (a->role == SGX535_BACKGROUND_RASTGEOM)
            continue;
        role = a->role == SGX535_BACKGROUND_USE ? SGX535_TASK_USE_FRAGMENT : SGX535_TASK_PDS;
        for (j = 0; j < p->foreground.packet.section_count; j++) {
            const struct sgx535_task_packet_section *b = &p->foreground.packet.sections[j];
            if (b->role == role && a->resource_offset < b->resource_byte_offset + b->byte_count &&
                b->resource_byte_offset < a->resource_offset + a->byte_count)
                return SGX535_TASK_UNSUPPORTED;
        }
    }
    p->version = r->version;
    p->missing_background_contracts = SGX535_BACKGROUND_MISSING_ALL;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_normalized_color0_background_build(
    const struct sgx535_normalized_color0_background_request *r,
    struct sgx535_normalized_color0_background_image *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_construct(&frozen, w, 0);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->image;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_background_validate(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_construct(&frozen, w, 0);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->image, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
static int
color0_background_bind_arguments(const struct sgx535_normalized_color0_background_request *r,
                                 const struct sgx535_normalized_color0_background_image *image,
                                 const struct sgx535_task_launch_binding *b,
                                 const struct sgx535_background_binding *bb,
                                 const struct sgx535_normalized_color0_background_bound *out,
                                 struct sgx535_normalized_color0_background_workspace *w)
{
    const void *inputs[3] = {image, b, bb};
    const unsigned long sizes[3] = {sizeof(*image), sizeof(*b), sizeof(*bb)};
    uint32_t i;
    if (!color0_background_arguments(r, out, sizeof(*out), w))
        return 0;
    for (i = 0; i < 3; i++)
        if (!normalized_tile16_span(inputs[i], sizes[i]) ||
            overlap(inputs[i], sizes[i], out, sizeof(*out)) ||
            overlap(inputs[i], sizes[i], w, sizeof(*w)))
            return 0;
    return 1;
}
static int color0_background_binding_matches(const struct sgx535_task_launch_binding *b,
                                             const struct sgx535_background_binding *bb)
{
    const struct sgx535_task_launch_role *use = &b->roles[SGX535_TASK_USE_FRAGMENT];
    const struct sgx535_task_launch_role *pds = &b->roles[SGX535_TASK_PDS];
    const struct sgx535_task_launch_role *parameters = &b->roles[SGX535_TASK_DPM_PARAMETERS];
    const struct sgx535_background_resource *bu = &bb->resources[0], *bp = &bb->resources[1];
    const struct sgx535_background_resource *geom = &bb->resources[2];
    uint32_t i;
    /* Both complete bindings have already passed their own range validators. */
    if (bb->epoch != b->epoch || bb->fragment_bank_index != b->bank_index[1] ||
        bb->parameter_storage_id != parameters->storage_id ||
        bb->parameter_va != parameters->gpu_va_bytes ||
        bb->parameter_bytes != parameters->allocation_bytes || bu->storage_id != use->storage_id ||
        bu->gpu_va_bytes != use->gpu_va_bytes || bu->allocation_bytes != use->allocation_bytes ||
        bu->access != use->access || bp->storage_id != pds->storage_id ||
        bp->gpu_va_bytes != pds->gpu_va_bytes || bp->allocation_bytes != pds->allocation_bytes ||
        bp->access != pds->access || geom->gpu_va_bytes != 0x32000000u ||
        geom->allocation_bytes != 4096u)
        return 0;
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
        const struct sgx535_task_launch_role *other = &b->roles[i];
        if (geom->storage_id == other->storage_id ||
            (geom->gpu_va_bytes < other->gpu_va_bytes + other->allocation_bytes &&
             other->gpu_va_bytes < geom->gpu_va_bytes + geom->allocation_bytes))
            return 0;
    }
    return 1;
}
static enum sgx535_task_status
color0_background_bind_construct(const struct sgx535_normalized_color0_background_request *r,
                                 const struct sgx535_normalized_color0_background_image *image,
                                 const struct sgx535_task_launch_binding *b,
                                 const struct sgx535_background_binding *bb,
                                 struct sgx535_normalized_color0_background_workspace *w, int mask)
{
    struct sgx535_background_request background = color0_background_request(r);
    enum sgx535_task_status status = color0_background_construct(r, w, mask);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(image, &w->image, sizeof(*image)))
        return SGX535_TASK_INVALID;
    status = sgx535_background_normalized_tile16_bind_seed(&background, &w->image.background, bb,
                                                           &w->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status =
        construct_validated_packet_policy(&w->image.foreground.packet, b, &w->projection,
                                          SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, &w->background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (!color0_background_binding_matches(b, bb))
        return SGX535_TASK_INVALID;
    memset(&w->result, 0, sizeof(w->result));
    w->result.version = r->version;
    w->result.missing_background_contracts = SGX535_BACKGROUND_MISSING_ALL;
    w->result.launch.launch = w->projection.result;
    w->result.launch.fragment_missing_contracts = w->image.foreground.fragment.missing_contracts;
    w->result.launch.reference_policy = w->image.foreground.fragment.reference_policy;
    w->result.background = w->background;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_normalized_color0_background_bind(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *image,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    struct sgx535_normalized_color0_background_bound *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_bind_arguments(r, image, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_bind_construct(&frozen, image, b, bb, w, 0);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_background_bound_validate(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *image,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    const struct sgx535_normalized_color0_background_bound *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_bind_arguments(r, image, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_bind_construct(&frozen, image, b, bb, w, 0);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_background_build(
    const struct sgx535_normalized_color0_background_request *r,
    struct sgx535_normalized_color0_background_image *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_construct(&frozen, w, 1);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->image;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_background_validate(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_arguments(r, out, sizeof(*out), w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_construct(&frozen, w, 1);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->image, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_background_bind(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *image,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    struct sgx535_normalized_color0_background_bound *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_bind_arguments(r, image, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_bind_construct(&frozen, image, b, bb, w, 1);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_background_bound_validate(
    const struct sgx535_normalized_color0_background_request *r,
    const struct sgx535_normalized_color0_background_image *image,
    const struct sgx535_task_launch_binding *b, const struct sgx535_background_binding *bb,
    const struct sgx535_normalized_color0_background_bound *out,
    struct sgx535_normalized_color0_background_workspace *w)
{
    struct sgx535_normalized_color0_background_request frozen;
    enum sgx535_task_status status;
    if (!color0_background_bind_arguments(r, image, b, bb, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_background_bind_construct(&frozen, image, b, bb, w, 1);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
