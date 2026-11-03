// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_background_object.h"
#include "sgx535_background_internal.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_limm_fields.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t value)
{
    uint32_t i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8u * i));
}
#define BACKGROUND_NORMALIZED_TILE16_POLICY 2u
#define BACKGROUND_WINDOW_MESH_POLICY 3u
#define BACKGROUND_CLIP_VIEWPORT_POLICY 4u
static enum sgx535_task_status construct(const struct sgx535_background_request *r, uint32_t policy,
                                         uint32_t window_use_offset,
                                         struct sgx535_background_image *out)
{
    uint32_t use[2], i, entry_offset, use_offset = 64, pds_offset = 768;
    switch (policy) {
    case SGX535_BACKGROUND_TASK_OWN_CONSTANT:
        entry_offset = 64;
        break;
    case SGX535_BACKGROUND_TASK_CANONICAL_FOREGROUND:
        if (r->width != 16 || r->height != 16)
            return SGX535_TASK_UNSUPPORTED;
        entry_offset = 0;
        break;
    case BACKGROUND_NORMALIZED_TILE16_POLICY:
        if (r->width != 16 || r->height != 16)
            return SGX535_TASK_UNSUPPORTED;
        entry_offset = use_offset = SGX535_BACKGROUND_NORMALIZED_USE_OFFSET;
        break;
    case BACKGROUND_WINDOW_MESH_POLICY:
        if (!r->width || !r->height || r->width >= 2048 || r->height >= 2048 ||
            window_use_offset < SGX535_BACKGROUND_NORMALIZED_USE_OFFSET ||
            (window_use_offset & 31u) || window_use_offset > 0xffffffffu - 8u)
            return SGX535_TASK_UNSUPPORTED;
        entry_offset = use_offset = window_use_offset;
        break;
    case BACKGROUND_CLIP_VIEWPORT_POLICY:
        if (r->width != 64 || r->height != 64)
            return SGX535_TASK_UNSUPPORTED;
        entry_offset = use_offset = SGX535_BACKGROUND_NORMALIZED_USE_OFFSET;
        pds_offset = 1024;
        break;
    default:
        return SGX535_TASK_INVALID;
    }
    if (r->version !=
            (policy == BACKGROUND_CLIP_VIEWPORT_POLICY ? SGX535_BACKGROUND_CLIP_VIEWPORT_VERSION
             : policy == BACKGROUND_WINDOW_MESH_POLICY ? SGX535_BACKGROUND_WINDOW_MESH_VERSION
             : policy == BACKGROUND_NORMALIZED_TILE16_POLICY
                 ? SGX535_BACKGROUND_NORMALIZED_TILE16_VERSION
                 : SGX535_BACKGROUND_VERSION) ||
        r->core_id != SGX535_ENGINE121_CORE_ID || r->revision != SGX535_ENGINE121_REVISION ||
        r->reserved[0] || r->reserved[1] ||
        (policy != BACKGROUND_WINDOW_MESH_POLICY &&
         (r->width != r->height || (r->width != 16 && r->width != 64))))
        return SGX535_TASK_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    out->version = r->version;
    out->core_id = r->core_id;
    out->revision = r->revision;
    out->width = r->width;
    out->height = r->height;
    out->packed_argb = r->packed_argb;
    out->missing_contracts = SGX535_BACKGROUND_MISSING_ALL;
    out->byte_count = 180;
    out->section_count = SGX535_BACKGROUND_SECTIONS;
    out->relocation_count = SGX535_BACKGROUND_RELOCS;
    out->sections[0] = (struct sgx535_background_section){
        SGX535_BACKGROUND_USE_SECTION, 0, 8, SGX535_BACKGROUND_USE, use_offset, 32};
    out->sections[1] = (struct sgx535_background_section){
        SGX535_BACKGROUND_PRIMARY_SECTION, 32, 56, SGX535_BACKGROUND_PDS, pds_offset, 32};
    out->sections[2] = (struct sgx535_background_section){
        SGX535_BACKGROUND_SECONDARY_SECTION, 96, 4, SGX535_BACKGROUND_PDS, pds_offset + 64u, 32};
    out->sections[3] = (struct sgx535_background_section){
        SGX535_BACKGROUND_OBJECT_SECTION, 112, 68, SGX535_BACKGROUND_RASTGEOM, 0, 16};
    out->relocations[0] = (struct sgx535_background_reloc){
        SGX535_BACKGROUND_USE_BANK, 1, 0, SGX535_BACKGROUND_USE, entry_offset, 8, 32, 0xf, 0};
    /* Bank and both entry fields share the first task DWORD. The source
     * relocation shifts select bits within that word, not adjacent words. */
    out->relocations[1] = (struct sgx535_background_reloc){
        SGX535_BACKGROUND_USE_ENTRY, 1, 0, SGX535_BACKGROUND_USE, entry_offset, 8, 32, 0xf0, 0};
    out->relocations[2] = (struct sgx535_background_reloc){
        SGX535_BACKGROUND_USE_ENTRY, 1, 0, SGX535_BACKGROUND_USE, entry_offset, 8, 32, 0x7ff00, 0};
    out->relocations[3] = (struct sgx535_background_reloc){SGX535_BACKGROUND_OFFSET,
                                                           3,
                                                           0,
                                                           SGX535_BACKGROUND_PDS,
                                                           pds_offset + 64u,
                                                           4,
                                                           32,
                                                           0xffffff,
                                                           4};
    out->relocations[4] = (struct sgx535_background_reloc){
        SGX535_BACKGROUND_OFFSET, 3, 8, SGX535_BACKGROUND_PDS, pds_offset, 56, 32, 0xffffff, 4};
    sgx535_limm_compose(1, 0, r->packed_argb, 1, use);
    put(out->bytes, use[0]);
    put(out->bytes + 4, use[1]);
    /* Matched constant dispatch reads data0/1/8. Its bank/entry relocations
     * all patch data0; data1 and data2 remain zero. */
    put(out->bytes + 32 + 8 * 4, 0x20);
    put(out->bytes + 32 + 12 * 4, 0x07000345);
    put(out->bytes + 32 + 13 * 4, 0xaf000000);
    put(out->bytes + 96, 0xaf000000);
    put(out->bytes + 112 + 4, 0x00030000);
    put(out->bytes + 112 + 8, 0x0c000000);
    put(out->bytes + 112 + 0x24, 2);
    put(out->bytes + 112 + 0x2c, 0x40004000);
    put(out->bytes + 112 + 0x34, ((r->width + 0x400u) << 20) | 0x4000u);
    put(out->bytes + 112 + 0x3c, 0x40000000u | ((r->height + 0x400u) << 4));
    for (i = 0x30; i <= 0x40; i += 8)
        put(out->bytes + 112 + i, 0x3f800000);
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status
sgx535_background_normalized_tile16_seed(const struct sgx535_background_request *r,
                                         struct sgx535_background_image *out)
{
    return construct(r, BACKGROUND_NORMALIZED_TILE16_POLICY, 0, out);
}

enum sgx535_task_status
sgx535_background_window_mesh_layout_seed(const struct sgx535_background_request *r,
                                          uint32_t use_offset, struct sgx535_background_image *out)
{
    return construct(r, BACKGROUND_WINDOW_MESH_POLICY, use_offset, out);
}
enum sgx535_task_status
sgx535_background_clip_viewport_seed(const struct sgx535_background_request *r,
                                     struct sgx535_background_image *out)
{
    return construct(r, BACKGROUND_CLIP_VIEWPORT_POLICY, 0, out);
}
enum sgx535_task_status sgx535_background_build(const struct sgx535_background_request *r,
                                                struct sgx535_background_image *out,
                                                struct sgx535_background_image *work)
{
    enum sgx535_task_status status;
    if (!r || !out || !work || overlap(r, sizeof(*r), out, sizeof(*out)) ||
        overlap(r, sizeof(*r), work, sizeof(*work)) ||
        overlap(out, sizeof(*out), work, sizeof(*work)))
        return SGX535_TASK_INVALID;
    status = construct(r, SGX535_BACKGROUND_SELECTED_TASK_POLICY, 0, work);
    if (status == SGX535_TASK_BLOCKED)
        memcpy(out, work, sizeof(*out));
    return status;
}
enum sgx535_task_status sgx535_background_validate(const struct sgx535_background_request *r,
                                                   const struct sgx535_background_image *image,
                                                   struct sgx535_background_image *work)
{
    enum sgx535_task_status status;
    if (!r || !image || !work || overlap(r, sizeof(*r), image, sizeof(*image)) ||
        overlap(r, sizeof(*r), work, sizeof(*work)) ||
        overlap(image, sizeof(*image), work, sizeof(*work)))
        return SGX535_TASK_INVALID;
    status = construct(r, SGX535_BACKGROUND_SELECTED_TASK_POLICY, 0, work);
    return status == SGX535_TASK_BLOCKED && memcmp(image, work, sizeof(*work)) ? SGX535_TASK_INVALID
                                                                               : status;
}
static int binding_valid(const struct sgx535_background_binding *b, int full_owner)
{
    uint32_t i, j;
    if (!b->epoch || !b->parameter_storage_id || b->reserved || b->fragment_bank_index < 3 ||
        b->fragment_bank_index > 15 || b->pds_exec_base != 0x20000000 ||
        b->bif_3d_req_base != 0x30000000 || b->parameter_va != 0x30000000 ||
        b->parameter_bytes != 0x02000000)
        return 0;
    for (i = 0; i < SGX535_BACKGROUND_ROLES; ++i) {
        const struct sgx535_background_resource *r = &b->resources[i];
        uint64_t low = i == 0 ? 0x40000000u : i == 1 ? 0x20000000u : 0x32000000u;
        uint64_t high = i == 0 ? 0x50000000u : i == 1 ? 0x30000000u : 0x40000000u;
        if (r->role != i + 1 || !r->storage_id || r->storage_id == b->parameter_storage_id ||
            r->access != SGX535_TASK_ACCESS_READ || r->reserved[0] || r->reserved[1] ||
            (full_owner ? r->allocation_bytes < 4096 || (r->allocation_bytes & 4095u)
                        : r->allocation_bytes != 4096) ||
            (r->gpu_va_bytes & 4095u) || r->gpu_va_bytes < low || r->gpu_va_bytes >= high ||
            r->allocation_bytes > high - r->gpu_va_bytes)
            return 0;
        for (j = 0; j < i; ++j) {
            const struct sgx535_background_resource *p = &b->resources[j];
            if (p->storage_id == r->storage_id ||
                (p->gpu_va_bytes < r->gpu_va_bytes + r->allocation_bytes &&
                 r->gpu_va_bytes < p->gpu_va_bytes + p->allocation_bytes))
                return 0;
        }
    }
    return 1;
}
static enum sgx535_task_status bind_construct(const struct sgx535_background_request *r,
                                              const struct sgx535_background_image *image,
                                              const struct sgx535_background_binding *b,
                                              uint32_t policy, uint32_t window_use_offset,
                                              int full_owner, struct sgx535_background_bound *work)
{
    struct sgx535_use_code_bank_input code;
    struct sgx535_pds_use_link_projection link;
    enum sgx535_task_status status;
    uint32_t i;
    status = construct(r, policy, window_use_offset, &work->image);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(image, &work->image, sizeof(*image)) || !binding_valid(b, full_owner))
        return SGX535_TASK_INVALID;
    code = (struct sgx535_use_code_bank_input){
        (uint32_t)b->resources[0].gpu_va_bytes, (uint32_t)b->resources[0].allocation_bytes,
        (uint32_t)b->resources[0].gpu_va_bytes + work->image.relocations[0].resource_offset,
        b->fragment_bank_index, 1};
    if (sgx535_use_code_bank_project(&code, &work->bank) != SGX535_USE_CODE_BANK_OK ||
        sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < image->section_count; ++i) {
        const struct sgx535_background_section *s = &image->sections[i];
        const struct sgx535_background_resource *owner = &b->resources[s->role - 1];
        if (s->resource_offset > owner->allocation_bytes ||
            s->byte_count > owner->allocation_bytes - s->resource_offset ||
            ((owner->gpu_va_bytes + s->resource_offset) & (s->alignment - 1)))
            return SGX535_TASK_INVALID;
    }
    for (i = 0; i < image->relocation_count; ++i) {
        const struct sgx535_background_reloc *rel = &image->relocations[i];
        const struct sgx535_background_section *s = &image->sections[rel->section];
        const struct sgx535_background_resource *owner = &b->resources[rel->role - 1];
        uint64_t va = owner->gpu_va_bytes + rel->resource_offset;
        uint32_t value;
        uint8_t *dst = work->image.bytes + s->byte_offset + rel->section_offset;
        if (rel->resource_offset > owner->allocation_bytes ||
            rel->span_bytes > owner->allocation_bytes - rel->resource_offset ||
            (va & (rel->alignment - 1)))
            return SGX535_TASK_INVALID;
        value = rel->kind == SGX535_BACKGROUND_OFFSET     ? (uint32_t)(va >> rel->right_shift)
                : rel->kind == SGX535_BACKGROUND_USE_BANK ? link.bank_selector_fields
                                                          : link.address_fields;
        put(dst, (word(dst) & ~rel->mask) | (value & rel->mask));
    }
    memcpy(&work->binding, b, sizeof(*b));
    work->bgobjtag_word = 0x02000000 | ((uint32_t)b->resources[2].gpu_va_bytes >> 4 & 0xffffff);
    work->bgobj_word = 0x200;
    work->bgdepth_word = 0x3f800000;
    work->resolved_relocations = SGX535_BACKGROUND_RELOCS;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_background_normalized_tile16_bind_seed(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, struct sgx535_background_bound *work)
{
    memset(work, 0, sizeof(*work));
    return bind_construct(r, image, b, BACKGROUND_NORMALIZED_TILE16_POLICY, 0, 0, work);
}
enum sgx535_task_status sgx535_background_window_mesh_bind_seed(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, struct sgx535_background_bound *work)
{
    memset(work, 0, sizeof(*work));
    return bind_construct(r, image, b, BACKGROUND_WINDOW_MESH_POLICY,
                          SGX535_BACKGROUND_NORMALIZED_USE_OFFSET, 0, work);
}
enum sgx535_task_status sgx535_background_window_mesh_bind_full_owner_seed(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, struct sgx535_background_bound *work)
{
    memset(work, 0, sizeof(*work));
    return bind_construct(r, image, b, BACKGROUND_WINDOW_MESH_POLICY,
                          SGX535_BACKGROUND_NORMALIZED_USE_OFFSET, 1, work);
}
enum sgx535_task_status sgx535_background_window_mesh_bind_layout_seed(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, uint32_t use_offset, int full_owner,
    struct sgx535_background_bound *work)
{
    memset(work, 0, sizeof(*work));
    return bind_construct(r, image, b, BACKGROUND_WINDOW_MESH_POLICY, use_offset, full_owner, work);
}
enum sgx535_task_status sgx535_background_clip_viewport_bind_seed(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, struct sgx535_background_bound *work)
{
    memset(work, 0, sizeof(*work));
    return bind_construct(r, image, b, BACKGROUND_CLIP_VIEWPORT_POLICY, 0, 0, work);
}
static int arguments_valid(const struct sgx535_background_request *r,
                           const struct sgx535_background_image *image,
                           const struct sgx535_background_binding *b,
                           const struct sgx535_background_bound *out,
                           const struct sgx535_background_bound *work)
{
    if (!r || !image || !b || !out || !work)
        return 0;
    return !overlap(r, sizeof(*r), image, sizeof(*image)) &&
           !overlap(r, sizeof(*r), b, sizeof(*b)) && !overlap(r, sizeof(*r), out, sizeof(*out)) &&
           !overlap(r, sizeof(*r), work, sizeof(*work)) &&
           !overlap(image, sizeof(*image), b, sizeof(*b)) &&
           !overlap(image, sizeof(*image), out, sizeof(*out)) &&
           !overlap(image, sizeof(*image), work, sizeof(*work)) &&
           !overlap(b, sizeof(*b), out, sizeof(*out)) &&
           !overlap(b, sizeof(*b), work, sizeof(*work)) &&
           !overlap(out, sizeof(*out), work, sizeof(*work));
}
enum sgx535_task_status sgx535_background_bind(const struct sgx535_background_request *r,
                                               const struct sgx535_background_image *image,
                                               const struct sgx535_background_binding *b,
                                               struct sgx535_background_bound *out,
                                               struct sgx535_background_bound *work)
{
    enum sgx535_task_status status;
    if (!arguments_valid(r, image, b, out, work))
        return SGX535_TASK_INVALID;
    memset(work, 0, sizeof(*work));
    status = bind_construct(r, image, b, SGX535_BACKGROUND_SELECTED_TASK_POLICY, 0, 0, work);
    if (status == SGX535_TASK_BLOCKED)
        memcpy(out, work, sizeof(*out));
    return status;
}
enum sgx535_task_status sgx535_background_bound_validate(
    const struct sgx535_background_request *r, const struct sgx535_background_image *image,
    const struct sgx535_background_binding *b, const struct sgx535_background_bound *out,
    struct sgx535_background_bound *work)
{
    enum sgx535_task_status status;
    if (!arguments_valid(r, image, b, out, work))
        return SGX535_TASK_INVALID;
    memset(work, 0, sizeof(*work));
    status = bind_construct(r, image, b, SGX535_BACKGROUND_SELECTED_TASK_POLICY, 0, 0, work);
    return status == SGX535_TASK_BLOCKED && memcmp(out, work, sizeof(*work)) ? SGX535_TASK_INVALID
                                                                             : status;
}
