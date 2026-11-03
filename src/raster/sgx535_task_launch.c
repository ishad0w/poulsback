// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_launch.h"
#include "sgx535_heap_layout.h"
#include "sgx535_use_code_bank.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_normalized_color0_task.h"
#include "sgx535_normalized_color0_mask.h"
#include "sgx535_normalized_color0_mask_list.h"
#include "sgx535_normalized_color0_window_list.h"
#include "sgx535_background_launch.h"
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
static uint32_t load_word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int matched_dri(uint32_t version)
{
    return version == SGX535_TASK_PACKET_DRI_VERSION ||
           version == SGX535_COMPILED_COLOR0_PACKET_VERSION ||
           version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
           version == SGX535_TASK_PAIR_PACKET_VERSION ||
           version == SGX535_NORMALIZED_COLOR0_TASK_VERSION ||
           version == SGX535_NORMALIZED_COLOR0_TILE16_VERSION ||
           version == SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION ||
           version == SGX535_NORMALIZED_COLOR0_MASK_LIST_VERSION ||
           version == SGX535_NORMALIZED_COLOR0_WINDOW_LIST_VERSION ||
           version == SGX535_NORMALIZED_PA0_TASK_VERSION;
}
static void store_word(uint8_t *p, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}
static void write_action(struct sgx535_task_launch_stage_plan *s, uint32_t offset, uint32_t value,
                         uint32_t unknown)
{
    uint32_t i = s->count++;
    s->actions[i] = (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, offset, value, 0};
    s->unknown_value_masks[i] = unknown;
}
static struct sgx535_heap_extent heap_role(const struct sgx535_task_launch_role *r)
{
    return (struct sgx535_heap_extent){r->storage_id, r->gpu_va_bytes, r->allocation_bytes};
}
static int bindings(const struct sgx535_task_packet *p, const struct sgx535_task_launch_binding *b,
                    struct sgx535_task_launch *out)
{
    struct sgx535_heap_layout_input heaps;
    struct sgx535_heap_layout_projection hp;
    struct sgx535_use_code_bank_input code;
    struct sgx535_use_code_bank_projection cp;
    struct sgx535_task_launch_stage_plan *s = &out->stages[SGX535_TASK_LAUNCH_CODE];
    uint32_t i, j;
    if (!b->epoch || b->reserved[0] || b->reserved[1] || b->bank_index[0] < 3 ||
        b->bank_index[0] > 15 || b->bank_index[1] < 3 || b->bank_index[1] > 15 ||
        b->bank_index[0] == b->bank_index[1])
        return -1;
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++) {
        const struct sgx535_task_launch_role *r = &b->roles[i];
        const struct sgx535_task_requirement *q = &p->requirements[i];
        if (!r->storage_id || r->reserved || r->access != q->access ||
            !sgx535_task_role_placed(i, r->gpu_va_bytes, r->allocation_bytes) ||
            r->allocation_bytes < q->minimum_bytes || (r->gpu_va_bytes & (q->alignment - 1u)) ||
            (r->allocation_bytes & 4095u))
            return -1;
        for (j = 0; j < i; j++) {
            const struct sgx535_task_launch_role *t = &b->roles[j];
            if (r->storage_id == t->storage_id ||
                (r->gpu_va_bytes < t->gpu_va_bytes + t->allocation_bytes &&
                 t->gpu_va_bytes < r->gpu_va_bytes + r->allocation_bytes))
                return -1;
        }
    }
    heaps.pds = heap_role(&b->roles[SGX535_TASK_PDS]);
    heaps.parameters = heap_role(&b->roles[SGX535_TASK_DPM_PARAMETERS]);
    heaps.tables = heap_role(&b->roles[SGX535_TASK_DPM_TABLES]);
    heaps.scene = heap_role(&b->roles[SGX535_TASK_SCENE]);
    if (sgx535_heap_layout_project(&heaps, &hp) != SGX535_HEAP_LAYOUT_OK)
        return -1;
    write_action(s, hp.pds_exec_base.register_offset_bytes, hp.pds_exec_base.word, 0);
    for (i = 0; i < 2; i++) {
        const struct sgx535_task_launch_role *r = &b->roles[SGX535_TASK_USE_VERTEX + i];
        code = (struct sgx535_use_code_bank_input){(uint32_t)r->gpu_va_bytes,
                                                   (uint32_t)r->allocation_bytes,
                                                   (uint32_t)r->gpu_va_bytes, b->bank_index[i], i};
        if (sgx535_use_code_bank_project(&code, &cp) != SGX535_USE_CODE_BANK_OK)
            return -1;
        out->bank_base[i] = cp.bank_base_va_bytes;
        out->bank_index[i] = b->bank_index[i];
        write_action(s, cp.register_offset_bytes, cp.register_word, 0);
    }
    memcpy(out->roles, b->roles, sizeof(out->roles));
    out->epoch = b->epoch;
    return 0;
}
static int project(const struct sgx535_task_packet *p, const struct sgx535_task_launch_binding *b,
                   struct sgx535_task_launch *out)
{
    uint32_t i;
    memcpy(out->bytes, p->bytes, sizeof(out->bytes));
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        const struct sgx535_task_launch_role *r = &b->roles[s->role];
        if (s->resource_byte_offset > r->allocation_bytes ||
            s->byte_count > r->allocation_bytes - s->resource_byte_offset ||
            ((r->gpu_va_bytes + s->resource_byte_offset) & (s->alignment - 1u)))
            return -1;
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        const struct sgx535_task_packet_section *s = &p->sections[r->section];
        const struct sgx535_task_launch_role *owner = &b->roles[r->role];
        struct sgx535_use_code_bank_input code;
        struct sgx535_pds_use_link_projection link;
        uint64_t va = owner->gpu_va_bytes + r->resource_byte_offset, v;
        uint8_t *dst = out->bytes + s->byte_offset + r->section_byte_offset;
        if (r->resource_byte_offset > owner->allocation_bytes ||
            r->span_bytes > owner->allocation_bytes - r->resource_byte_offset ||
            (va & (r->alignment - 1u)) || va > 0xffffffffu)
            return -1;
        if (r->kind == SGX535_PACKET_DRI_OFFSET) {
            /* Actual generic OFFSET producer. Do not subtract the PDS base
             * merely because a particular consumer field drops upper bits. */
            v = (va >> r->right_shift) << r->left_shift;
        } else {
            uint32_t index;
            if (r->role != SGX535_TASK_USE_VERTEX && r->role != SGX535_TASK_USE_FRAGMENT)
                return -1;
            index = r->role - SGX535_TASK_USE_VERTEX;
            code = (struct sgx535_use_code_bank_input){(uint32_t)owner->gpu_va_bytes,
                                                       (uint32_t)owner->allocation_bytes,
                                                       (uint32_t)va, b->bank_index[index], index};
            if (sgx535_pds_use_link_project(&code, &link) != SGX535_PDS_USE_LINK_OK)
                return -1;
            if (r->kind == SGX535_PACKET_USE_BANK)
                v = link.bank_selector_fields;
            else if (r->kind == SGX535_PACKET_USE_ENTRY)
                v = link.address_fields;
            else
                return -1;
        }
        store_word(dst, (load_word(dst) & ~r->mask) | ((uint32_t)v & r->mask));
        out->resolved_relocations++;
    }
    return 0;
}
#include "sgx535_task_launch_prefix_internal.h"
static void launch_stages(struct sgx535_task_launch *out, uint32_t background_policy,
                          const struct sgx535_background_bound *background)
{
    sgx535_launch_prefix_build(&out->stages[SGX535_TASK_LAUNCH_TA],
                               &out->stages[SGX535_TASK_LAUNCH_RASTER], out->ta_stream_va_bytes,
                               out->event_pds_va_bytes, matched_dri(out->packet_version),
                               background_policy, background);
}
static enum sgx535_task_status construct_validated_packet_policy(
    const struct sgx535_task_packet *packet, const struct sgx535_task_launch_binding *binding,
    struct sgx535_task_launch_workspace *work, uint32_t background_policy,
    const struct sgx535_background_bound *background)
{
    struct sgx535_task_launch *out = &work->result;
    const struct sgx535_task_packet_section *stream, *event;
    int mask_set = (packet->version == SGX535_TASK_PAIR_PACKET_VERSION &&
                    (packet->profile == SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT ||
                     packet->profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT)) ||
                   (packet->version == SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION &&
                    packet->profile == SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE) ||
                   (packet->version == SGX535_NORMALIZED_COLOR0_MASK_LIST_VERSION &&
                    packet->profile == SGX535_NORMALIZED_COLOR0_MASK_LIST_PROFILE) ||
                   (packet->version == SGX535_NORMALIZED_COLOR0_WINDOW_LIST_VERSION &&
                    packet->profile == SGX535_NORMALIZED_COLOR0_WINDOW_LIST_PROFILE);
    if (background_policy > SGX535_PAIR_BACKGROUND_DISABLED_FIXED16 ||
        (background_policy == SGX535_PAIR_BACKGROUND_DISABLED_FIXED16 &&
         (packet->version != SGX535_TASK_PAIR_PACKET_VERSION ||
          (packet->profile != SGX535_TASK_PAIR_W1_PACKED_CONSTANT && !mask_set) ||
          packet->width != 16 || packet->height != 16)))
        return SGX535_TASK_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    if (bindings(packet, binding, out) || project(packet, binding, out))
        return SGX535_TASK_INVALID;
    stream = &packet->sections[packet->ta_stream_section];
    event = &packet->sections[packet->event_pds_section];
    if (stream->kind != SGX535_PACKET_TA_STREAM || stream->role != SGX535_TASK_PDS ||
        stream->byte_count != (mask_set ? 68u : 40u) || event->kind != SGX535_PACKET_EVENT_PDS ||
        event->role != SGX535_TASK_PDS ||
        event->byte_count != (matched_dri(packet->version) ? 148u : 128u))
        return SGX535_TASK_INVALID;
    out->version = SGX535_TASK_LAUNCH_VERSION;
    out->packet_version = packet->version;
    out->core_id = packet->core_id;
    out->revision = packet->revision;
    out->missing_packet_contracts = packet->missing_contracts;
    out->missing_launch_contracts = SGX535_LAUNCH_MISSING_ALL;
    if (matched_dri(packet->version))
        out->missing_launch_contracts &=
            ~(SGX535_LAUNCH_MISSING_EVENT_PROFILE | SGX535_LAUNCH_MISSING_CALLER_SCALARS);
    out->byte_count = packet->byte_count;
    out->ta_stream_va_bytes =
        (uint32_t)(binding->roles[stream->role].gpu_va_bytes + stream->resource_byte_offset);
    out->event_pds_va_bytes =
        (uint32_t)(binding->roles[event->role].gpu_va_bytes + event->resource_byte_offset) & ~15u;
    launch_stages(out, background_policy, background);
    return SGX535_TASK_BLOCKED;
}
static enum sgx535_task_status
construct_validated_packet(const struct sgx535_task_packet *packet,
                           const struct sgx535_task_launch_binding *binding,
                           struct sgx535_task_launch_workspace *work)
{
    return construct_validated_packet_policy(packet, binding, work,
                                             SGX535_PAIR_BACKGROUND_SOURCE_DEFAULT, NULL);
}
static enum sgx535_task_status construct(const struct sgx535_task_request *request,
                                         const struct sgx535_task_packet *packet,
                                         const struct sgx535_task_launch_binding *binding,
                                         struct sgx535_task_launch_workspace *work)
{
    if (sgx535_task_packet_validate(request, packet, &work->regenerated) != SGX535_TASK_BLOCKED)
        return SGX535_TASK_INVALID;
    return construct_validated_packet(packet, binding, work);
}
static int arguments(const struct sgx535_task_request *r, const struct sgx535_task_packet *p,
                     const struct sgx535_task_launch_binding *b, const void *out,
                     struct sgx535_task_launch_workspace *w)
{
    return r && p && b && out && w && !overlap(w, sizeof(*w), r, sizeof(*r)) &&
           !overlap(w, sizeof(*w), p, sizeof(*p)) && !overlap(w, sizeof(*w), b, sizeof(*b)) &&
           !overlap(w, sizeof(*w), out, sizeof(struct sgx535_task_launch)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r, sizeof(*r)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), p, sizeof(*p)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), b, sizeof(*b));
}
enum sgx535_task_status sgx535_task_launch_build(const struct sgx535_task_request *r,
                                                 const struct sgx535_task_packet *p,
                                                 const struct sgx535_task_launch_binding *b,
                                                 struct sgx535_task_launch *out,
                                                 struct sgx535_task_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        memcpy(out, &w->result, sizeof(*out));
    return status;
}
enum sgx535_task_status sgx535_task_launch_validate(const struct sgx535_task_request *r,
                                                    const struct sgx535_task_packet *p,
                                                    const struct sgx535_task_launch_binding *b,
                                                    const struct sgx535_task_launch *out,
                                                    struct sgx535_task_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : SGX535_TASK_BLOCKED;
}

static int pair_launch_arguments(const struct sgx535_task_pair_request *r,
                                 const struct sgx535_task_packet *p,
                                 const struct sgx535_task_launch_binding *b, const void *out,
                                 struct sgx535_task_launch_workspace *w)
{
    return r && p && b && out && w && !overlap(w, sizeof(*w), r, sizeof(*r)) &&
           !overlap(w, sizeof(*w), p, sizeof(*p)) && !overlap(w, sizeof(*w), b, sizeof(*b)) &&
           !overlap(w, sizeof(*w), out, sizeof(struct sgx535_task_launch)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r, sizeof(*r)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), p, sizeof(*p)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), b, sizeof(*b));
}
static enum sgx535_task_status pair_launch_construct(const struct sgx535_task_pair_request *r,
                                                     const struct sgx535_task_packet *p,
                                                     const struct sgx535_task_launch_binding *b,
                                                     struct sgx535_task_launch_workspace *w)
{
    enum sgx535_task_status status = sgx535_task_pair_packet_validate(r, p, &w->regenerated);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return construct_validated_packet(p, b, w);
}
enum sgx535_task_status sgx535_task_pair_launch_build(const struct sgx535_task_pair_request *r,
                                                      const struct sgx535_task_packet *p,
                                                      const struct sgx535_task_launch_binding *b,
                                                      struct sgx535_task_launch *out,
                                                      struct sgx535_task_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!pair_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = pair_launch_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_task_pair_launch_validate(const struct sgx535_task_pair_request *r,
                                                         const struct sgx535_task_packet *p,
                                                         const struct sgx535_task_launch_binding *b,
                                                         const struct sgx535_task_launch *out,
                                                         struct sgx535_task_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!pair_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = pair_launch_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}
static int compiled_arguments(const struct sgx535_compiled_color0_request *r,
                              const struct sgx535_task_packet *p,
                              const struct sgx535_task_launch_binding *b, const void *out,
                              struct sgx535_compiled_color0_launch_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !b || !out || !w || !r->fragment || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    if (bytes > ~(unsigned long)0 - (unsigned long)r->fragment)
        return 0;
    return !overlap(w, sizeof(*w), r, sizeof(*r)) && !overlap(w, sizeof(*w), p, sizeof(*p)) &&
           !overlap(w, sizeof(*w), b, sizeof(*b)) && !overlap(w, sizeof(*w), r->fragment, bytes) &&
           !overlap(w, sizeof(*w), out, sizeof(struct sgx535_task_launch)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r, sizeof(*r)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), p, sizeof(*p)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), b, sizeof(*b)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r->fragment, bytes);
}
static enum sgx535_task_status compiled_construct(const struct sgx535_compiled_color0_request *r,
                                                  const struct sgx535_task_packet *p,
                                                  const struct sgx535_task_launch_binding *b,
                                                  struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status = sgx535_compiled_color0_packet_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return construct_validated_packet(p, b, &w->launch);
}
enum sgx535_task_status sgx535_compiled_color0_launch_build(
    const struct sgx535_compiled_color0_request *r, const struct sgx535_task_packet *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_task_launch *out,
    struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!compiled_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = compiled_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->launch.result;
    return status;
}
enum sgx535_task_status sgx535_compiled_color0_launch_validate(
    const struct sgx535_compiled_color0_request *r, const struct sgx535_task_packet *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_task_launch *out,
    struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!compiled_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = compiled_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->launch.result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}

/* Packet7 requires its own typed whole-packet regeneration before projection. */
static int window_arguments(const struct sgx535_window_color0_request *r,
                            const struct sgx535_task_packet *p,
                            const struct sgx535_task_launch_binding *b, const void *out,
                            struct sgx535_compiled_color0_launch_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !b || !out || !w || !r->fragment || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    if (bytes > ~(unsigned long)0 - (unsigned long)r->fragment)
        return 0;
    return !overlap(w, sizeof(*w), r, sizeof(*r)) && !overlap(w, sizeof(*w), p, sizeof(*p)) &&
           !overlap(w, sizeof(*w), b, sizeof(*b)) && !overlap(w, sizeof(*w), r->fragment, bytes) &&
           !overlap(w, sizeof(*w), out, sizeof(struct sgx535_task_launch)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r, sizeof(*r)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), p, sizeof(*p)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), b, sizeof(*b)) &&
           !overlap(out, sizeof(struct sgx535_task_launch), r->fragment, bytes);
}
static enum sgx535_task_status window_construct(const struct sgx535_window_color0_request *r,
                                                const struct sgx535_task_packet *p,
                                                const struct sgx535_task_launch_binding *b,
                                                struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status = sgx535_window_color0_packet_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return construct_validated_packet(p, b, &w->launch);
}
enum sgx535_task_status sgx535_window_color0_launch_build(
    const struct sgx535_window_color0_request *r, const struct sgx535_task_packet *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_task_launch *out,
    struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!window_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = window_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->launch.result;
    return status;
}
enum sgx535_task_status sgx535_window_color0_launch_validate(
    const struct sgx535_window_color0_request *r, const struct sgx535_task_packet *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_task_launch *out,
    struct sgx535_compiled_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!window_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = window_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->launch.result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}

#include "sgx535_normalized_color0_launch.inc"
#include "sgx535_normalized_color0_background.inc"
#include "sgx535_normalized_color0_mask_list_launch.inc"
#include "sgx535_normalized_color0_window_list_launch.inc"

