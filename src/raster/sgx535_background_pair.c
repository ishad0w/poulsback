// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_background_object.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int overlaps(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int arguments(const struct sgx535_task_pair_request *triangles,
                     const struct sgx535_background_request *background,
                     const struct sgx535_background_pair_image *out,
                     const struct sgx535_background_pair_workspace *work)
{
    return triangles && background && out && work &&
           !overlaps(triangles, sizeof(*triangles), background, sizeof(*background)) &&
           !overlaps(triangles, sizeof(*triangles), out, sizeof(*out)) &&
           !overlaps(triangles, sizeof(*triangles), work, sizeof(*work)) &&
           !overlaps(background, sizeof(*background), out, sizeof(*out)) &&
           !overlaps(background, sizeof(*background), work, sizeof(*work)) &&
           !overlaps(out, sizeof(*out), work, sizeof(*work));
}
static enum sgx535_task_status pair_construct(const struct sgx535_task_pair_request *triangles,
                                              const struct sgx535_background_request *background,
                                              struct sgx535_background_pair_workspace *work)
{
    struct sgx535_background_pair_image *result = &work->result;
    enum sgx535_task_status status;
    uint32_t i, j;
    memset(result, 0, sizeof(*result));
    status = sgx535_task_pair_packet_build(triangles, &result->triangles, &work->packet_scratch);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = sgx535_background_build(background, &result->background, &work->background_scratch);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (result->triangles.width != result->background.width ||
        result->triangles.height != result->background.height ||
        result->triangles.core_id != result->background.core_id ||
        result->triangles.revision != result->background.revision)
        return SGX535_TASK_UNSUPPORTED;
    if (SGX535_BACKGROUND_SELECTED_TASK_POLICY == SGX535_BACKGROUND_TASK_CANONICAL_FOREGROUND) {
        uint32_t found = 0;
        /* The entry is the generated foreground section in the SAME canonical
         * fragment owner. BG bytes remain at +64 and are still populated. */
        for (i = 0; i < result->triangles.section_count; ++i) {
            const struct sgx535_task_packet_section *s = &result->triangles.sections[i];
            if (s->kind != SGX535_PACKET_FRAGMENT_USE)
                continue;
            if (s->role != SGX535_TASK_USE_FRAGMENT || s->resource_byte_offset ||
                s->byte_count != 8 || s->alignment != 32 ||
                s->byte_offset > SGX535_TASK_PACKET_BYTES ||
                s->byte_count > SGX535_TASK_PACKET_BYTES - s->byte_offset)
                return SGX535_TASK_INVALID;
            ++found;
        }
        if (found != 1)
            return SGX535_TASK_INVALID;
    }
    for (i = 0; i < SGX535_BACKGROUND_SECTIONS; ++i) {
        const struct sgx535_background_section *a = &result->background.sections[i];
        uint32_t role;
        if (a->role == SGX535_BACKGROUND_RASTGEOM)
            continue; /* Separate new owner. */
        role = a->role == SGX535_BACKGROUND_USE ? SGX535_TASK_USE_FRAGMENT : SGX535_TASK_PDS;
        for (j = 0; j < result->triangles.section_count; ++j) {
            const struct sgx535_task_packet_section *b = &result->triangles.sections[j];
            if (b->role == role && a->resource_offset < b->resource_byte_offset + b->byte_count &&
                b->resource_byte_offset < a->resource_offset + a->byte_count)
                return SGX535_TASK_INVALID;
        }
    }
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status
sgx535_background_pair_build(const struct sgx535_task_pair_request *triangles,
                             const struct sgx535_background_request *background,
                             struct sgx535_background_pair_image *out,
                             struct sgx535_background_pair_workspace *work)
{
    enum sgx535_task_status status;
    if (!arguments(triangles, background, out, work))
        return SGX535_TASK_INVALID;
    status = pair_construct(triangles, background, work);
    if (status == SGX535_TASK_BLOCKED)
        memcpy(out, &work->result, sizeof(*out));
    return status;
}
enum sgx535_task_status
sgx535_background_pair_validate(const struct sgx535_task_pair_request *triangles,
                                const struct sgx535_background_request *background,
                                const struct sgx535_background_pair_image *out,
                                struct sgx535_background_pair_workspace *work)
{
    enum sgx535_task_status status;
    if (!arguments(triangles, background, out, work))
        return SGX535_TASK_INVALID;
    status = pair_construct(triangles, background, work);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &work->result, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
