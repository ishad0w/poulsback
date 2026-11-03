// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_window_mesh_state.h"
#include "sgx535_depth_window_mesh_single_sample.h"
#include "sgx535_background_internal.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_usse.h"
#include <errno.h>
#include <stdbool.h>
int legacy_window_mesh_main(int, char **);
int unused_fixed_depth_main(int, char **);
#define POULSBACK_DEPTH_WINDOW_MESH_MAIN unused_fixed_depth_main
#include "test_sgx535_depth_window_mesh.c"
#undef POULSBACK_DEPTH_WINDOW_MESH_MAIN

static const void *ordered_header_source;
static void *ordered_header_scratch;
static size_t ordered_header_bytes;
static unsigned ordered_header_control;
static const void *ordered_payload_probe;
static unsigned ordered_payload_follows;
static const struct sgx535_depth_window_mesh_binding *ordered_binding_probe;
static struct sgx535_depth_window_mesh_single_sample_request *ordered_binding_restore;

/* Exact header-copy edge before the first allocator. Volatile byte accesses
 * retain ordinary memcpy behavior without recursively calling the builtin. */
void *memcpy(void *destination, const void *source, size_t bytes)
{
    volatile unsigned char *dst = destination;
    const volatile unsigned char *src = source;
    if (source == ordered_binding_probe && bytes == sizeof(*ordered_binding_probe)) {
        ordered_binding_restore[0].mesh.scene.foreground.vertex_count = 3;
        ordered_binding_probe = NULL;
    }
    if (source == ordered_payload_probe)
        ordered_payload_follows++;
    if (ordered_header_source && source == ordered_header_source && bytes == ordered_header_bytes) {
        struct sgx535_depth_window_mesh_single_sample_request *r = (void *)(uintptr_t)source;
        ordered_header_source = NULL;
        if (!ordered_header_control)
            r[0].mesh.scene.foreground.vertex_count = 6;
        else
            r[1].mesh.scene.foreground.fragment = ordered_header_scratch;
    }
    for (size_t i = 0; i < bytes; i++)
        dst[i] = src[i];
    return destination;
}

static void state_image(struct sgx535_depth_window_mesh_task *p, uint32_t compare, uint32_t write)
{
    static const uint32_t compare_bits[8] = {0,         0x400000,  0x800000,  0xc00000,
                                             0x1000000, 0x1400000, 0x1800000, 0x1c00000};
    p->version = 2;
    p->depth.isp_set_bits = compare_bits[compare] | (write ? 0u : 0x100000u);
    put(p->pds + 580, p->depth.isp_set_bits);
}
static const uint8_t *ordered_reference_source(const struct sgx535_depth_window_mesh_task *p,
                                               const struct sgx535_window_mesh_section *s,
                                               const uint8_t *geometry)
{
    const uint8_t *sources[] = {geometry, p->pds, p->seed.foreground.vertex_use,
                                p->seed.foreground.fragment_use, p->clear_use};
    CHECK(s->source < 5);
    return sources[s->source] + s->byte_offset;
}
static void ordered_reference(const struct sgx535_depth_window_mesh_single_sample_request r[2],
                              const struct sgx535_depth_window_mesh_ordered_view *v)
{
    static const unsigned later[] = {0, 1, 3, 6, 7, 9, 12};
    unsigned map[2][20], section = 0, relocation = 0;
    struct sgx535_depth_window_mesh_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_depth_window_mesh_task *p = calloc(1, sizeof(*p));
    uint8_t geometry[512], scratch[1024];
    CHECK(w && p);
    for (unsigned j = 0; j < 20; j++)
        map[0][j] = j == 15 ? v->ta_stream_section : section++;
    memcpy(map[1], map[0], sizeof(map[0]));
    for (unsigned j = 0; j < sizeof(later) / sizeof(later[0]); j++)
        map[1][later[j]] = section++;
    CHECK(section == v->ta_stream_section);
    for (unsigned group = 0; group < 2; group++) {
        struct sgx535_single_sample_lowering lowering;
        CHECK(sgx535_depth_window_mesh_single_sample_build(&r[group], p, &lowering, geometry,
                                                           sizeof(geometry), w, scratch,
                                                           sizeof(scratch)) == SGX535_TASK_BLOCKED);
        for (unsigned j = 0; j < 20; j++) {
            if (j == 15)
                continue;
            const struct sgx535_window_mesh_section *a = &p->sections[j];
            const struct sgx535_window_mesh_section *b = &v->sections[map[group][j]];
            /* Shared sections must independently regenerate byte-identically;
             * every selected program/PDS/MTE/geometry image is also exact. */
            CHECK(a->kind == b->kind && a->role == b->role && a->byte_count == b->byte_count &&
                  !memcmp(ordered_reference_source(p, a, geometry), v->bytes + b->byte_offset,
                          a->byte_count));
        }
        const struct sgx535_window_mesh_section *ta = &v->sections[v->ta_stream_section];
        const uint8_t *old_ta = p->pds + p->sections[15].byte_offset;
        CHECK(!memcmp(v->bytes + ta->byte_offset + 56u + 28u * group, old_ta + 56u, 28));
        if (!group)
            CHECK(!memcmp(v->bytes + ta->byte_offset, old_ta, 56) &&
                  !memcmp(v->bytes + ta->byte_offset + 112, old_ta + 84, 12));
        for (unsigned j = 0; j < p->relocation_count; j++) {
            struct sgx535_task_packet_reloc expected = p->relocations[j];
            bool selected = !group;
            for (unsigned k = 0; k < sizeof(later) / sizeof(later[0]); k++)
                selected |= expected.section == later[k];
            if (expected.section == 15) {
                if (group &&
                    (expected.section_byte_offset < 56 || expected.section_byte_offset >= 84))
                    continue;
                if (expected.section_byte_offset >= 84)
                    expected.section_byte_offset += 28;
                else if (expected.section_byte_offset >= 56)
                    expected.section_byte_offset += group * 28u;
            } else if (!selected) {
                continue;
            }
            expected.section = map[group][expected.section];
            bool mapped = expected.role == SGX535_TASK_TARGET;
            for (unsigned k = 0; !mapped && k < 20; k++) {
                const struct sgx535_window_mesh_section *s = &p->sections[k];
                if (s->role == expected.role &&
                    expected.resource_byte_offset >= s->resource_byte_offset &&
                    expected.resource_byte_offset - s->resource_byte_offset <= s->byte_count &&
                    expected.span_bytes <=
                        s->byte_count - (expected.resource_byte_offset - s->resource_byte_offset)) {
                    expected.resource_byte_offset =
                        v->sections[map[group][k]].resource_byte_offset +
                        expected.resource_byte_offset - s->resource_byte_offset;
                    mapped = true;
                }
            }
            CHECK(mapped && relocation < v->relocation_count &&
                  !memcmp(&expected, &v->relocations[relocation++], sizeof(expected)));
        }
    }
    CHECK(relocation == v->relocation_count);
    free(p);
    free(w);
}
#ifdef SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE
#ifdef SGX535_DEPTH_WINDOW_MESH_F32_BIND_CPU_V1
static void f32_bound_controls(struct sgx535_depth_window_mesh_single_sample_request r[2],
                               struct sgx535_depth_window_mesh_f32_ordered *source,
                               const struct sgx535_depth_window_mesh_ordered *legacy)
{
    const struct sgx535_depth_window_mesh_ordered_view *v =
        sgx535_depth_window_mesh_f32_ordered_view(source);
    struct sgx535_depth_window_mesh_task *first = calloc(1, sizeof(*first));
    struct sgx535_depth_window_mesh_workspace *work = calloc(1, sizeof(*work));
    struct sgx535_single_sample_lowering lowering;
    struct sgx535_depth_window_mesh_binding b;
    struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
    struct sgx535_depth_window_mesh_f32_ordered_bound *bound = NULL, *failed;
    struct sgx535_depth_window_mesh_ordered_bound *old = NULL;
    uint8_t geometry[256], temporary[512];
    {
        /* No depth attachment: every fragment passes and nothing is written,
         * the window mesh's own ISP word; only 0 and 1 are meanings. */
        struct sgx535_depth_window_mesh_single_sample_request none = r[0];
        none.mesh.no_depth = 1;
        CHECK(v && first && work &&
              sgx535_depth_window_mesh_single_sample_build(
                  &none, first, &lowering, geometry, sizeof(geometry), work, temporary,
                  sizeof(temporary)) == SGX535_TASK_BLOCKED);
        CHECK(first->pds[580] == 0x00 && first->pds[581] == 0x00 && first->pds[582] == 0xd0 &&
              first->pds[583] == 0x01);
        none.mesh.no_depth = 2;
        CHECK(sgx535_depth_window_mesh_single_sample_build(
                  &none, first, &lowering, geometry, sizeof(geometry), work, temporary,
                  sizeof(temporary)) != SGX535_TASK_BLOCKED);
    }
    CHECK(v && first && work &&
          sgx535_depth_window_mesh_single_sample_build(&r[0], first, &lowering, geometry,
                                                       sizeof(geometry), work, temporary,
                                                       sizeof(temporary)) == SGX535_TASK_BLOCKED);
    /* LESS with writes: compare 1 at bit 22, write-disable bit 20 clear. */
    CHECK(first->pds[580] == 0x00 && first->pds[581] == 0x00 && first->pds[582] == 0x40 &&
          first->pds[583] == 0x00);
    depth_binding(first, &b);
    for (unsigned i = 0; i < SGX535_TASK_IMAGE_ROLES; i++)
        b.scene.roles[i].allocation_bytes = v->requirements[i].minimum_bytes;
    b.background.resources[0].allocation_bytes =
        b.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
    b.background.resources[1].allocation_bytes = b.scene.roles[SGX535_TASK_PDS].allocation_bytes;
    CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(r, 2, &sizes));
    void *scratch = calloc(1, (size_t)sizes.workspace_bytes + 32u);
    CHECK(scratch);
    memset((uint8_t *)scratch + sizes.workspace_bytes, 0xa5, 32);
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &b, &bound, scratch,
                                                     sizes.workspace_bytes));
    const struct sgx535_depth_window_mesh_ordered_bound_view *bv =
        sgx535_depth_window_mesh_f32_ordered_bound_view(bound);
    CHECK(bv && bv->source != v && bv->source->group_count == 2 &&
          bv->resolved_relocations == v->relocation_count &&
          bv->metadata_bytes == sizes.bound_metadata_bytes && !bv->hardware_qualified &&
          !bv->layout_qualified && !bv->visibility_qualified && !bv->retirement_qualified &&
          (bv->source->missing_mesh_contracts & SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE));
    CHECK(!sgx535_depth_window_mesh_ordered_bound_view((const void *)bound));
    CHECK(sgx535_depth_window_mesh_ordered_bound_validate(
              r, 2, legacy, &b, (const void *)bound, scratch, sizes.workspace_bytes) == -ESTALE);
    sgx535_depth_window_mesh_ordered_bound_destroy((void *)bound);
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, bound, scratch,
                                                               sizes.workspace_bytes));
    CHECK(!sgx535_depth_window_mesh_ordered_bind(r, 2, legacy, &b, &old, scratch,
                                                 sizes.workspace_bytes));
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bound_view((const void *)old));
    sgx535_depth_window_mesh_f32_ordered_bound_destroy((void *)old);
    CHECK(!sgx535_depth_window_mesh_ordered_bound_validate(r, 2, legacy, &b, old, scratch,
                                                           sizes.workspace_bytes));
    sgx535_depth_window_mesh_ordered_bound_destroy(old);
    failed = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_f32_ordered_bind(r, 2, (const void *)legacy, &b, &failed,
                                                    scratch, sizes.workspace_bytes) == -EINVAL &&
          failed == (void *)(uintptr_t)0x1234);
    old = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_ordered_bind(r, 2, (const void *)source, &b, &old, scratch,
                                                sizes.workspace_bytes) == -EINVAL &&
          old == (void *)(uintptr_t)0x1234);
    uint8_t *source_byte = (void *)(uintptr_t)(v->bytes + v->sections[0].byte_offset);
    *source_byte ^= 1;
    /* Binding trusts the source's bytes (it copies them); validation rebuilds
     * the source from the requests and refuses both bounds. */
    failed = NULL;
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &b, &failed, scratch,
                                                     sizes.workspace_bytes) &&
          failed);
    CHECK(sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, failed, scratch,
                                                              sizes.workspace_bytes) == -ESTALE);
    sgx535_depth_window_mesh_f32_ordered_bound_destroy(failed);
    CHECK(sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, bound, scratch,
                                                              sizes.workspace_bytes) == -ESTALE);
    *source_byte ^= 1;
    uint32_t *source_marker = (void *)(uintptr_t)&v->missing_mesh_contracts;
    *source_marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    failed = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &b, &failed, scratch,
                                                    sizes.workspace_bytes) == -ESTALE &&
          failed == (void *)(uintptr_t)0x1234);
    *source_marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    for (unsigned i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *a = &v->relocations[i];
        const struct sgx535_window_mesh_section *s = &v->sections[a->section];
        uint64_t address = b.scene.roles[a->role].gpu_va_bytes + a->resource_byte_offset;
        uint32_t value;
        if (a->kind == SGX535_PACKET_DRI_OFFSET)
            value = (uint32_t)((address >> a->right_shift) << a->left_shift);
        else {
            unsigned dm = a->role - SGX535_TASK_USE_VERTEX;
            uint32_t units = (uint32_t)(address & 0x7ffffu) / 16u;
            CHECK(dm < 2);
            value = b.scene.bank_index[dm] | (units / 2048u) * 16u | (units % 2048u) * 256u;
        }
        uint32_t offset = s->byte_offset + a->section_byte_offset;
        CHECK((get(bv->bytes + offset) & a->mask) == (value & a->mask));
        uint8_t *byte = (void *)(uintptr_t)(bv->bytes + offset);
        *byte ^= 1;
        CHECK(sgx535_depth_window_mesh_f32_ordered_bound_validate(
                  r, 2, source, &b, bound, scratch, sizes.workspace_bytes) == -ESTALE);
        *byte ^= 1;
    }
    uint32_t *marker = (void *)(uintptr_t)&bv->source->missing_mesh_contracts;
    *marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    CHECK(!sgx535_depth_window_mesh_ordered_bound_view((const void *)bound) &&
          !sgx535_depth_window_mesh_f32_ordered_bound_view(bound));
    CHECK(sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, bound, scratch,
                                                              sizes.workspace_bytes) == -ESTALE);
    *marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    for (unsigned kind = 0; kind < 6; kind++) {
        struct sgx535_depth_window_mesh_binding changed = b;
        if (kind == 0)
            changed.scene.roles[SGX535_TASK_PDS].allocation_bytes -= 4096;
        if (kind == 1)
            changed.scene.bank_index[1] = changed.scene.bank_index[0];
        if (kind == 2)
            changed.depth.allocation_bytes -= 4096;
        if (kind == 3)
            changed.background.resources[0].storage_id++;
        if (kind == 4)
            changed.scene.roles[SGX535_TASK_TARGET].gpu_va_bytes = UINT64_MAX;
        if (kind == 5) { /* Across a 256 MiB window: ZLS has one high-4 base. */
            changed.depth.gpu_va_bytes = 0x60000000u - 4096u;
            changed.depth.allocation_bytes += 4096;
        }
        failed = (void *)(uintptr_t)0x1234;
        CHECK(sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &changed, &failed, scratch,
                                                        sizes.workspace_bytes) &&
              failed == (void *)(uintptr_t)0x1234);
    }
    {
        /* Any depth placement inside one window, with room to spare; vertex
         * data and target anywhere above the PDS and parameter windows. */
        struct sgx535_depth_window_mesh_binding moved = b;
        struct sgx535_depth_window_mesh_f32_ordered_bound *placed = NULL;
        const struct sgx535_depth_window_mesh_ordered_bound_view *pv;
        moved.depth.gpu_va_bytes = 0x50400000u;
        moved.depth.allocation_bytes += 4096;
        moved.scene.roles[SGX535_TASK_VERTEX_DATA].gpu_va_bytes = 0x60000000u;
        moved.scene.roles[SGX535_TASK_TARGET].gpu_va_bytes = 0xc0000000u;
        CHECK(!sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &moved, &placed, scratch,
                                                         sizes.workspace_bytes));
        pv = sgx535_depth_window_mesh_f32_ordered_bound_view(placed);
        CHECK(pv && pv->depth_base_words[0] == 0x50000000u &&
              pv->depth_base_words[1] == 0x00400000u);
        sgx535_depth_window_mesh_f32_ordered_bound_destroy(placed);
    }
    for (unsigned serial = 1; serial <= 2; serial++) {
        unsigned saved = b.scene.bank_index[1];
        late_word = &b.scene.bank_index[1];
        late_alloc_serial = alloc_calls + serial;
        failed = (void *)(uintptr_t)0x1234;
        CHECK(sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &b, &failed, scratch,
                                                        sizes.workspace_bytes) == -ESTALE &&
              !late_word && failed == (void *)(uintptr_t)0x1234);
        b.scene.bank_index[1] = saved;
    }
    unsigned before = alloc_calls;
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, bound, scratch,
                                                               sizes.workspace_bytes));
    unsigned allocations = alloc_calls - before;
    for (unsigned serial = 1; serial <= allocations; serial++) {
        late_word = (uint32_t *)&fail_alloc;
        late_alloc_serial = alloc_calls + serial;
        CHECK(sgx535_depth_window_mesh_f32_ordered_bound_validate(
                  r, 2, source, &b, bound, scratch, sizes.workspace_bytes) == -ENOMEM &&
              !late_word);
        fail_alloc = 0;
    }
    CHECK(!sgx535_depth_window_mesh_f32_ordered_bound_validate(r, 2, source, &b, bound, scratch,
                                                               sizes.workspace_bytes));
    for (unsigned i = 0; i < 32; i++)
        CHECK(((uint8_t *)scratch)[sizes.workspace_bytes + i] == 0xa5);
    sgx535_depth_window_mesh_f32_ordered_bound_destroy(bound);
    free(scratch);
    free(work);
    free(first);
}
#endif
static void f32_ordered_controls(struct sgx535_depth_window_mesh_single_sample_request r[2],
                                 void *scratch, uint32_t bytes,
                                 const struct sgx535_depth_window_mesh_ordered *legacy)
{
    struct sgx535_depth_window_mesh_f32_ordered *o = NULL, *refused = NULL;
    CHECK(!sgx535_depth_window_mesh_f32_ordered_view((const void *)legacy));
    CHECK(!sgx535_depth_window_mesh_f32_ordered_create(r, 2, &o, scratch, bytes));
    const struct sgx535_depth_window_mesh_ordered_view *v =
        sgx535_depth_window_mesh_f32_ordered_view(o);
    CHECK(v && v->group_count == 2 && v->vertex_count == 6 &&
          v->missing_mesh_contracts ==
              (SGX535_WINDOW_MESH_MISSING_ALL | SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE));
    CHECK(!sgx535_depth_window_mesh_ordered_view((const void *)o));
    CHECK(sgx535_depth_window_mesh_ordered_validate(r, 2, (const void *)o, scratch, bytes) ==
          -ESTALE);
    CHECK(!sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes));
    uint32_t *marker = (void *)(uintptr_t)&v->missing_mesh_contracts;
    *marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    CHECK(!sgx535_depth_window_mesh_ordered_view((const void *)o) &&
          !sgx535_depth_window_mesh_f32_ordered_view(o) &&
          sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes) == -ESTALE &&
          sgx535_depth_window_mesh_ordered_validate(r, 2, (const void *)o, scratch, bytes) ==
              -ESTALE);
    *marker ^= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    const struct sgx535_depth_window_mesh_ordered_view *old =
        sgx535_depth_window_mesh_ordered_view(legacy);
    CHECK(old);
    marker = (void *)(uintptr_t)&old->missing_mesh_contracts;
    *marker |= SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    CHECK(!sgx535_depth_window_mesh_ordered_view(legacy) &&
          !sgx535_depth_window_mesh_f32_ordered_view((const void *)legacy) &&
          sgx535_depth_window_mesh_f32_ordered_validate(r, 2, (const void *)legacy, scratch,
                                                        bytes) == -ESTALE);
    *marker &= ~SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE;
    for (unsigned i = 0; i < 2; i++) {
        const struct sgx535_depth_window_mesh_ordered_group *g = &v->groups[i];
        const struct sgx535_window_mesh_section *m = &v->sections[g->mte_section];
        const struct sgx535_window_mesh_section *state = &v->sections[g->state_section];
        const struct sgx535_window_mesh_section *code = &v->sections[g->fragment_section];
        const uint8_t *data = v->bytes + m->byte_offset;
        CHECK(m->byte_count == 92 && get(data) == 0xd4c1u && get(data + 28) == 0x08001000u &&
              get(data + 36) == 7 && !get(data + 40) && !get(data + 44) &&
              get(data + 48) == 0x54c5u);
        /* Independent eleven-word DMA control: stride11, burst11, one line. */
        CHECK(state->byte_count == 124 && get(v->bytes + state->byte_offset + 68) == 0x8140000au);
        bool use_entry = false;
        for (unsigned k = 0; k < v->relocation_count; k++) {
            const struct sgx535_task_packet_reloc *a = &v->relocations[k];
            if (a->section == g->state_section && a->section_byte_offset == 72) {
                CHECK(a->role == SGX535_TASK_USE_VERTEX && a->resource_byte_offset == 32 &&
                      a->span_bytes == 16);
                use_entry = true;
            }
        }
        CHECK(use_entry);
        uint32_t pair[2] = {get(v->bytes + code->byte_offset),
                            get(v->bytes + code->byte_offset + 4)};
        struct sgx535_mov mov;
        CHECK(sgx535_mov_decode(pair, &mov) == SGX535_MOV_OK &&
              mov.source.bank == SGX535_MOV_PRIMARY && mov.source.number == (i ? 1u : 0u));
        uint8_t *changed = (void *)(uintptr_t)(data + 40);
        *changed ^= 1;
        CHECK(sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes) == -ESTALE);
        *changed ^= 1;
    }
    CHECK(!sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes));
    unsigned iterators = 0, state_programs = 0;
    for (unsigned k = 0; k < v->section_count; k++) {
        const struct sgx535_window_mesh_section *s = &v->sections[k];
        const uint8_t *data = v->bytes + s->byte_offset;
        if (s->kind == SGX535_PACKET_FRAGMENT_PDS && s->byte_count == 60) {
            CHECK(get(data + 36) == 0x0fd0000fu);
            iterators++;
        }
        if (s->kind == SGX535_PACKET_STATE_USE) {
            CHECK(s->resource_byte_offset == 32 && s->byte_count == 48 &&
                  get(data) == 0xa0000000u && get(data + 4) == 0x28a1a001u &&
                  get(data + 8) == 0xa0200580u && get(data + 12) == 0xfb274000u);
            state_programs++;
        }
    }
    CHECK(iterators == 2 && state_programs == 1);
    const uint8_t *ta = v->bytes + v->sections[v->ta_stream_section].byte_offset;
    for (unsigned k = 0; k < 2; k++)
        CHECK((get(ta + v->groups[k].ta_byte_offset + 4u) & 255u) == 3u);
#ifdef SGX535_DEPTH_WINDOW_MESH_F32_BIND_CPU_V1
    f32_bound_controls(r, o, legacy);
#endif
    /* The source is copied, not kept alive through borrowed caller arrays. */
    struct sgx535_window_mesh_vertex *vertices =
        (void *)(uintptr_t)r[0].mesh.scene.foreground.vertices;
    vertices[0].window_xyz_reciprocal_w[3] = 0x3f000000u;
    CHECK(sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes) == -ESTALE);
    CHECK(!sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes));
    sgx535_depth_window_mesh_f32_ordered_destroy(refused);
    refused = NULL;
    vertices[0].window_xyz_reciprocal_w[3] = 0;
    CHECK(sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes) && !refused);
    vertices[0].window_xyz_reciprocal_w[3] = 0x3f800000u;
    CHECK(sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes - 1u) &&
          !refused);
    CHECK(!sgx535_depth_window_mesh_f32_ordered_validate(r, 2, o, scratch, bytes));
    for (unsigned serial = 1; serial <= 2; serial++) {
        late_alloc_serial = alloc_calls + serial;
        late_word = &r[1].mesh.scene.foreground.height;
        uint32_t height = *late_word;
        refused = (void *)(uintptr_t)0x1234;
        CHECK(sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes) ==
                  -ESTALE &&
              !late_word && refused == (void *)(uintptr_t)0x1234);
        r[1].mesh.scene.foreground.height = height;
    }
    fail_alloc = 1;
    refused = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes) == -ENOMEM &&
          refused == (void *)(uintptr_t)0x1234);
    fail_alloc = 0;
    /* One frame has a depth attachment or none: its groups agree. */
    r[1].mesh.no_depth = 1;
    refused = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes) &&
          refused == (void *)(uintptr_t)0x1234);
    r[0].mesh.no_depth = 1;
    CHECK(!sgx535_depth_window_mesh_f32_ordered_create(r, 2, &refused, scratch, bytes) &&
          refused != (void *)(uintptr_t)0x1234);
    sgx535_depth_window_mesh_f32_ordered_destroy(refused);
    r[0].mesh.no_depth = r[1].mesh.no_depth = 0;
    sgx535_depth_window_mesh_f32_ordered_destroy(o);
}
#endif
static void ordered_window_extents(void)
{
    static const struct {
        uint32_t width, height, half_x, half_y, color_stride, depth_pitch, depth_rows;
    } cases[] = {
        {1, 1, 0x3f000000u, 0x3f000000u, 128, 32, 32},
        {63, 17, 0x41fc0000u, 0x41080000u, 256, 64, 32},
        {300, 300, 0x43160000u, 0x43160000u, 1280, 320, 320},
        {640, 640, 0x43a00000u, 0x43a00000u, 2560, 640, 640},
        {1280, 800, 0x44200000u, 0x43c80000u, 5120, 1280, 800},
        {2047, 2047, 0x447fe000u, 0x447fe000u, 8192, 2048, 2048},
    };
    for (unsigned at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        struct sgx535_depth_window_mesh_single_sample_request r[2] = {0};
        struct sgx535_window_mesh_vertex vertices[2][3] = {0};
        struct sgx535_fragment_instruction ir[2][SGX535_NIR_MAX_INSTRUCTIONS];
        uint32_t indices[2][3], bytes;
        struct sgx535_depth_window_mesh_ordered *o = NULL, *refused = NULL;
        for (unsigned i = 0; i < 2; i++) {
            struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
            *m = request(vertices[i], indices[i], ir[i], 3, 3, cases[at].width, cases[at].height);
            r[i].mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
            r[i].mesh.format = SGX535_DEPTH_D24S8;
            r[i].mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
            r[i].mesh.scene.packed_background_argb = 0xff000000u;
            m->order = SGX535_WINDOW_MESH_DEINDEXED;
            m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
            m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] =
                cases[at].half_x;
            m->window.viewport_translate_f32[1] = cases[at].half_y;
            m->window.viewport_scale_f32[1] = cases[at].half_y | 0x80000000u;
            for (unsigned j = 0; j < 3; j++)
                vertices[i][j].window_xyz_reciprocal_w[3] = 0x3f800000u;
            r[i].samples = (struct sgx535_single_sample_source){
                .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
        }
        ir[1][0].component = 1;
        CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, 2, &bytes));
        uint8_t *scratch = calloc(1, (size_t)bytes + 32u);
        CHECK(scratch);
        memset(scratch + bytes, 0x5a, 32);
        CHECK(!sgx535_depth_window_mesh_ordered_create(r, 2, &o, scratch, bytes));
        const struct sgx535_depth_window_mesh_ordered_view *v =
            sgx535_depth_window_mesh_ordered_view(o);
        CHECK(v && v->group_count == 2 && v->vertex_count == 6 &&
              v->depth.width == cases[at].width && v->depth.height == cases[at].height &&
              v->depth.pitch_pixels == cases[at].depth_pitch &&
              v->depth.allocation_height == cases[at].depth_rows &&
              v->depth.allocation_bytes ==
                  (uint64_t)cases[at].depth_pitch * cases[at].depth_rows * 4u &&
              v->requirements[SGX535_TASK_TARGET].minimum_bytes ==
                  ((cases[at].color_stride * cases[at].height + 4095u) & ~4095u));
        CHECK(!sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes));
        ordered_reference(r, v);
#ifdef SGX535_DEPTH_WINDOW_MESH_F32_MISSING_LINKAGE
        f32_ordered_controls(r, scratch, bytes, o);
#endif
        /* A rounded or stale viewport must not become a resized packet. */
        r[1].mesh.scene.foreground.window.viewport_scale_f32[0] ^= 1u;
        CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, &refused, scratch, bytes) && !refused);
        r[1].mesh.scene.foreground.window.viewport_scale_f32[0] ^= 1u;
        CHECK(!sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes));
        for (unsigned i = 0; i < 32; i++)
            CHECK(scratch[bytes + i] == 0x5a);
        sgx535_depth_window_mesh_ordered_destroy(o);
        free(scratch);
    }
}
/* The VERTEX_USE section that group G's vertex PDS links to. */
static uint32_t ordered_vertex_use_of(const struct sgx535_depth_window_mesh_ordered_view *v,
                                      uint32_t g)
{
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *rel = &v->relocations[i];
        if (rel->section != v->groups[g].vertex_pds_section || rel->role != SGX535_TASK_USE_VERTEX)
            continue;
        for (uint32_t k = 0; k < v->section_count; k++) {
            const struct sgx535_window_mesh_section *s = &v->sections[k];
            if (s->role == rel->role && rel->resource_byte_offset >= s->resource_byte_offset &&
                rel->resource_byte_offset < s->resource_byte_offset + s->byte_count) {
                CHECK(s->kind == SGX535_PACKET_VERTEX_USE && (found == UINT32_MAX || found == k));
                found = k;
            }
        }
    }
    CHECK(found != UINT32_MAX);
    return found;
}
/* One frame of draws with vertex colours and a draw with a uniform colour and
 * no fragment input (glColor text over the scene), in both orders and both
 * families. The uniform draw's vertex USE program moves the position only, so
 * it is its own copy; in the F32 family that draw is built as primary colour. */
static void ordered_mixed_inputs(void)
{
    static const uint32_t colour[4] = {0x3f800000u, 0x3f000000u, 0, 0x3f800000u};
    for (unsigned f32 = 0; f32 < 2; f32++)
        for (unsigned uniform_first = 0; uniform_first < 2; uniform_first++) {
            struct sgx535_depth_window_mesh_single_sample_request r[2] = {0};
            struct sgx535_window_mesh_vertex vertices[2][3] = {0};
            struct sgx535_fragment_instruction ir[2][SGX535_NIR_MAX_INSTRUCTIONS];
            uint32_t indices[2][3], uniforms[4], bytes;
            struct sgx535_depth_window_mesh_ordered *legacy = NULL;
            struct sgx535_depth_window_mesh_f32_ordered *source = NULL;
            struct sgx535_depth_window_mesh_ordered_bound *legacy_bound = NULL;
            struct sgx535_depth_window_mesh_f32_ordered_bound *bound = NULL;
            struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
            struct sgx535_depth_window_mesh_binding b;
            struct sgx535_window_mesh_task *owners = calloc(1, sizeof(*owners));
            const struct sgx535_depth_window_mesh_ordered_view *v;
            const struct sgx535_depth_window_mesh_ordered_bound_view *bv;
            const unsigned uniform = uniform_first ? 0u : 1u;
            CHECK(owners);
            memcpy(uniforms, colour, sizeof(uniforms));
            for (unsigned i = 0; i < 2; i++) {
                struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
                *m = request(vertices[i], indices[i], ir[i], 3, 3, 64, 64);
                r[i].mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
                r[i].mesh.format = SGX535_DEPTH_D24S8;
                r[i].mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
                r[i].mesh.scene.packed_background_argb = 0xff000000u;
                m->order = SGX535_WINDOW_MESH_DEINDEXED;
                m->interpolation = SGX535_VARYING_FLAT_REPLICATED;
                m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
                m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
                m->window.viewport_scale_f32[1] = 0xc2000000u;
                m->window.viewport_translate_f32[1] = 0x42000000u;
                /* Unequal W only in the F32 family, and never without inputs. */
                for (unsigned j = 0; j < 3; j++)
                    if (!f32 || i == uniform)
                        vertices[i][j].window_xyz_reciprocal_w[3] = 0x3f800000u;
                r[i].samples = (struct sgx535_single_sample_source){
                    .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
            }
            {
                struct sgx535_window_mesh_request *m = &r[uniform].mesh.scene.foreground;
                m->interpolation = SGX535_WINDOW_MESH_NO_INPUT;
                m->uniforms = uniforms;
                m->uniform_word_count = 4;
                for (unsigned c = 0; c < 4; c++)
                    ir[uniform][c] = (struct sgx535_fragment_instruction){
                        .opcode = SGX535_FRAGMENT_UNIFORM, .index = c};
                for (unsigned j = 0; j < 3; j++)
                    memset(vertices[uniform][j].rgba, 0, sizeof(vertices[uniform][j].rgba));
            }
            CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, 2, &bytes));
            CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(r, 2, &sizes));
            CHECK(sizes.workspace_bytes >= bytes);
            void *scratch = calloc(1, sizes.workspace_bytes);
            CHECK(scratch);
            if (f32) {
                CHECK(!sgx535_depth_window_mesh_f32_ordered_create(r, 2, &source, scratch, bytes));
                v = sgx535_depth_window_mesh_f32_ordered_view(source);
                CHECK(v &&
                      !sgx535_depth_window_mesh_f32_ordered_validate(r, 2, source, scratch, bytes));
            } else {
                CHECK(!sgx535_depth_window_mesh_ordered_create(r, 2, &legacy, scratch, bytes));
                v = sgx535_depth_window_mesh_ordered_view(legacy);
                CHECK(v &&
                      !sgx535_depth_window_mesh_ordered_validate(r, 2, legacy, scratch, bytes));
            }
            /* The later group adds seven sections and its own vertex USE; the
             * uniform group also its secondary PDS and event USE. */
            CHECK(v->group_count == 2 && v->section_count == (uniform_first ? 29u : 30u) &&
                  v->ta_stream_section == v->section_count - 1u &&
                  v->sections[v->ta_stream_section].kind == SGX535_PACKET_TA_STREAM &&
                  v->requirements[SGX535_TASK_USE_VERTEX].minimum_bytes == 8192u);
            uint32_t first_use = ordered_vertex_use_of(v, 0),
                     later_use = ordered_vertex_use_of(v, 1);
            CHECK(first_use != later_use && v->sections[first_use].resource_byte_offset == 0 &&
                  v->sections[later_use].resource_byte_offset == 4096u &&
                  v->sections[later_use].byte_count == v->sections[first_use].byte_count &&
                  memcmp(v->bytes + v->sections[first_use].byte_offset,
                         v->bytes + v->sections[later_use].byte_offset,
                         v->sections[first_use].byte_count));
            for (uint32_t i = 0; i < v->relocation_count; i++)
                CHECK(v->relocations[i].section < v->section_count);
            /* Owners as tests/unit/test_sgx535_window_mesh.c binds them; the
             * fragment code moves past the two-page vertex code. */
            memcpy(owners->requirements, v->requirements, sizeof(owners->requirements));
            memset(&b, 0, sizeof(b));
            binding(owners, &b.scene, &b.background);
            b.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes = 0x40004000u;
            b.background.resources[0].gpu_va_bytes = 0x40004000u;
            b.background.resources[0].allocation_bytes =
                b.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
            b.background.resources[1].allocation_bytes =
                b.scene.roles[SGX535_TASK_PDS].allocation_bytes;
            b.depth = (struct sgx535_depth_window_mesh_owner){
                110, 0x50000000, (v->depth.allocation_bytes + 4095u) & ~UINT64_C(4095), 3, 0};
            if (f32) {
                CHECK(!sgx535_depth_window_mesh_f32_ordered_bind(r, 2, source, &b, &bound, scratch,
                                                                 sizes.workspace_bytes));
                bv = sgx535_depth_window_mesh_f32_ordered_bound_view(bound);
                CHECK(bv && !sgx535_depth_window_mesh_f32_ordered_bound_validate(
                                r, 2, source, &b, bound, scratch, sizes.workspace_bytes));
            } else {
                CHECK(!sgx535_depth_window_mesh_ordered_bind(r, 2, legacy, &b, &legacy_bound,
                                                             scratch, sizes.workspace_bytes));
                bv = sgx535_depth_window_mesh_ordered_bound_view(legacy_bound);
                CHECK(bv && !sgx535_depth_window_mesh_ordered_bound_validate(
                                r, 2, legacy, &b, legacy_bound, scratch, sizes.workspace_bytes));
            }
            CHECK(bv->resolved_relocations == v->relocation_count);
            printf("ORDERED_MIXED_INPUTS f32=%u uniform_first=%u sections=%u relocations=%u "
                   "vertex_use=%u,%u\n",
                   f32, uniform_first, v->section_count, v->relocation_count,
                   v->sections[first_use].resource_byte_offset,
                   v->sections[later_use].resource_byte_offset);
            sgx535_depth_window_mesh_f32_ordered_bound_destroy(bound);
            sgx535_depth_window_mesh_ordered_bound_destroy(legacy_bound);
            sgx535_depth_window_mesh_f32_ordered_destroy(source);
            sgx535_depth_window_mesh_ordered_destroy(legacy);
            free(scratch);
            free(owners);
        }
}
static void ordered_fragment_storage(void)
{
    struct sgx535_depth_window_mesh_single_sample_request r = {0};
    struct sgx535_window_mesh_vertex vertices[3];
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t indices[3];
    struct sgx535_depth_window_mesh_task *first = calloc(1, sizeof(*first));
    struct sgx535_depth_window_mesh_workspace *packet = calloc(1, sizeof(*packet));
    CHECK(first && packet);
    for (unsigned components = 1; components <= 2; components++) {
        struct sgx535_window_mesh_request *m = &r.mesh.scene.foreground;
        struct sgx535_depth_window_mesh_ordered *source = NULL;
        struct sgx535_depth_window_mesh_ordered_bound *bound = NULL;
        struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
        struct sgx535_depth_window_mesh_binding binding;
        struct sgx535_single_sample_lowering lowering;
        uint8_t geometry[256], temporary[512];
        *m = request(vertices, indices, ir, 3, 3, 64, 64);
        r.mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
        r.mesh.format = SGX535_DEPTH_D24S8;
        r.mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
        r.mesh.scene.packed_background_argb = 0xff000000u;
        m->order = SGX535_WINDOW_MESH_DEINDEXED;
        m->interpolation = SGX535_VARYING_FLAT_REPLICATED;
        m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
        m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
        m->window.viewport_scale_f32[1] = 0xc2000000u;
        m->window.viewport_translate_f32[1] = 0x42000000u;
        for (unsigned i = 0; i < 3; i++)
            vertices[i].window_xyz_reciprocal_w[3] = 0x3f800000u;
        m->fragment_count = large_fragment(ir, components, components == 1 ? 240u : 244u);
        r.samples =
            (struct sgx535_single_sample_source){.version = 1,
                                                 .effective_framebuffer_samples = 1,
                                                 .fragment_instructions = m->fragment_count};
        CHECK(sgx535_depth_window_mesh_single_sample_build(
                  &r, first, &lowering, geometry, sizeof(geometry), packet, temporary,
                  sizeof(temporary)) == SGX535_TASK_BLOCKED);
        CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(&r, 1, &sizes));
        void *scratch = calloc(1, (size_t)sizes.workspace_bytes + 32u);
        CHECK(scratch);
        memset((uint8_t *)scratch + sizes.workspace_bytes, 0xa5, 32);
        CHECK(!sgx535_depth_window_mesh_ordered_create(&r, 1, &source, scratch,
                                                       sizes.workspace_bytes));
        const struct sgx535_depth_window_mesh_ordered_view *v =
            sgx535_depth_window_mesh_ordered_view(source);
        CHECK(v && v->group_count == 1 && v->section_count == 20);
        const struct sgx535_window_mesh_section *fragment_section =
            &v->sections[v->groups[0].fragment_section];
        const struct sgx535_window_mesh_section *event = NULL, *clear = NULL;
        for (unsigned i = 0; i < v->section_count; i++) {
            if (v->sections[i].kind == SGX535_PACKET_EVENT_USE)
                event = &v->sections[i];
            if (v->sections[i].kind == SGX535_PACKET_FRAGMENT_USE && v->sections[i].byte_count == 8)
                clear = &v->sections[i];
        }
        uint32_t event_offset = ((fragment_section->byte_count + 31u) / 32u) * 32u;
        uint32_t background = event_offset + 32u;
        uint32_t required = ((background + 32u + 8u + 4095u) / 4096u) * 4096u;
        CHECK(fragment_section->byte_count > 1536 && event && clear &&
              fragment_section->resource_byte_offset == 0 &&
              event->resource_byte_offset == event_offset && event->byte_count == 32 &&
              clear->resource_byte_offset == background + 32u &&
              v->background.sections[0].resource_offset == background &&
              v->requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes == required);
        for (unsigned i = 0; i < first->seed.foreground.fragment.encoding.word_count; i++)
            CHECK(get(v->bytes + fragment_section->byte_offset + i * 4u) ==
                  first->seed.foreground.fragment.encoding.words[i]);
        CHECK(!memcmp(v->bytes + clear->byte_offset, first->clear_use, 8));
        for (unsigned i = 0; i < 3; i++)
            CHECK(v->background.relocations[i].resource_offset == background);
        CHECK(!sgx535_depth_window_mesh_ordered_validate(&r, 1, source, scratch,
                                                         sizes.workspace_bytes));
        depth_binding(first, &binding);
        for (unsigned i = 0; i < 8; i++)
            binding.scene.roles[i].allocation_bytes = v->requirements[i].minimum_bytes;
        binding.background.resources[0].allocation_bytes = required;
        binding.background.resources[1].allocation_bytes =
            binding.scene.roles[SGX535_TASK_PDS].allocation_bytes;
        CHECK(!sgx535_depth_window_mesh_ordered_bind(&r, 1, source, &binding, &bound, scratch,
                                                     sizes.workspace_bytes));
        const struct sgx535_depth_window_mesh_ordered_bound_view *bv =
            sgx535_depth_window_mesh_ordered_bound_view(bound);
        CHECK(bv && bv->resolved_relocations == v->relocation_count && !bv->hardware_qualified &&
              !bv->layout_qualified && !bv->visibility_qualified && !bv->retirement_qualified);
        const uint8_t *event_pds = bv->bytes + v->sections[v->event_pds_section].byte_offset;
        CHECK(sgx535_pixel_event_reference_check(event_pds, 148, bv->bytes + event->byte_offset, 32,
                                                 1, get(event_pds + 8), get(event_pds),
                                                 get(event_pds + 16) | 0x00200000u));
        for (unsigned i = 0; i < v->relocation_count; i++) {
            const struct sgx535_task_packet_reloc *rel = &v->relocations[i];
            const struct sgx535_window_mesh_section *section = &v->sections[rel->section];
            uint64_t address =
                binding.scene.roles[rel->role].gpu_va_bytes + rel->resource_byte_offset;
            uint32_t value;
            if (rel->kind == SGX535_PACKET_DRI_OFFSET)
                value = (uint32_t)((address >> rel->right_shift) << rel->left_shift);
            else {
                unsigned dm = rel->role - SGX535_TASK_USE_VERTEX;
                uint32_t units = (uint32_t)(address & 0x7ffffu) / 16u;
                CHECK(dm < 2);
                value =
                    binding.scene.bank_index[dm] | (units / 2048u) * 16u | (units % 2048u) * 256u;
            }
            CHECK((get(bv->bytes + section->byte_offset + rel->section_byte_offset) & rel->mask) ==
                  (value & rel->mask));
        }
        CHECK(!sgx535_depth_window_mesh_ordered_bound_validate(&r, 1, source, &binding, bound,
                                                               scratch, sizes.workspace_bytes));
        if (components == 2) {
            CHECK(required == 8192);
            for (unsigned control = 0; control < 3; control++) {
                struct sgx535_depth_window_mesh_binding bad = binding;
                struct sgx535_depth_window_mesh_ordered_bound *failed = (void *)(uintptr_t)0x1234;
                /* Complete owner fits the512KiB bank but BG/clear cross the
                 * existing32KiB entry block; then past the bank end, and extent. */
                if (!control)
                    bad.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes = 0x40007000u;
                if (control == 1)
                    bad.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes = 0x4007f000u;
                if (control == 2)
                    bad.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes = 4096;
                bad.background.resources[0].gpu_va_bytes =
                    bad.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes;
                bad.background.resources[0].allocation_bytes =
                    bad.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
                CHECK(sgx535_depth_window_mesh_ordered_bind(&r, 1, source, &bad, &failed, scratch,
                                                            sizes.workspace_bytes) < 0 &&
                      failed == (void *)(uintptr_t)0x1234);
                printf(
                    "FRAGMENT_STORAGE_REFUSAL control=%u owner_va=%llx owner_bytes=%llu\n", control,
                    (unsigned long long)bad.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes,
                    (unsigned long long)bad.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes);
            }
        }
        printf(
            "ORDERED_FRAGMENT_STORAGE components=%u code=%u event=%u background=%u clear=%u owner=%u payload=%u\n",
            components, fragment_section->byte_count, event_offset, background, background + 32u,
            required, v->byte_count);
        for (unsigned i = 0; i < 32; i++)
            CHECK(((uint8_t *)scratch)[sizes.workspace_bytes + i] == 0xa5);
        sgx535_depth_window_mesh_ordered_bound_destroy(bound);
        sgx535_depth_window_mesh_ordered_destroy(source);
        free(scratch);
    }
    free(packet);
    free(first);
}
static void ordered_image(void)
{
    struct sgx535_depth_window_mesh_single_sample_request r[2] = {0}, bad[2];
    struct sgx535_window_mesh_vertex vertices[2][6] = {0};
    struct sgx535_fragment_instruction ir[2][SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t indices[2][3], bytes;
    struct sgx535_depth_window_mesh_ordered *o = NULL, *failed;
    for (unsigned i = 0; i < 2; i++) {
        struct sgx535_window_mesh_request *m = &r[i].mesh.scene.foreground;
        *m = request(vertices[i], indices[i], ir[i], 3, 3, 64, 64);
        r[i].mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
        r[i].mesh.format = SGX535_DEPTH_D24S8;
        r[i].mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
        r[i].mesh.scene.packed_background_argb = 0xff000000u;
        m->order = SGX535_WINDOW_MESH_DEINDEXED;
        m->interpolation = i ? SGX535_VARYING_FLAT_REPLICATED : SGX535_VARYING_SMOOTH;
        m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
        m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
        m->window.viewport_scale_f32[1] = 0xc2000000u;
        m->window.viewport_translate_f32[1] = 0x42000000u;
        for (unsigned j = 0; j < 3; j++)
            vertices[i][j].window_xyz_reciprocal_w[3] = 0x3f800000u;
        r[i].samples = (struct sgx535_single_sample_source){
            .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
    }
    ir[1][0].component = 1;
    CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, 2, &bytes));
    void *scratch = calloc(1, (size_t)bytes + 32u);
    CHECK(scratch);
    memset((uint8_t *)scratch + bytes, 0x5a, 32);
    unsigned start = alloc_calls;
    CHECK(!sgx535_depth_window_mesh_ordered_create(r, 2, &o, scratch, bytes));
    unsigned allocations = alloc_calls - start;
    CHECK(allocations >= 2);
    const struct sgx535_depth_window_mesh_ordered_view *v =
        sgx535_depth_window_mesh_ordered_view(o);
    CHECK(v && v->group_count == 2 && v->vertex_count == 6 && v->section_count == 27 &&
          v->missing_depth_contracts == SGX535_DEPTH_MISSING_ALL);
    const struct sgx535_window_mesh_section *ta = &v->sections[v->ta_stream_section];
    const uint8_t *stream = v->bytes + ta->byte_offset;
    CHECK(ta->byte_count == 124 && get(stream + 8) == 0x81400006u &&
          get(stream + 36) == 0x81400006u && get(stream + 64) == 0x81400003u &&
          get(stream + 92) == 0x81400003u && get(stream + 120) == 0xc0000000u);
    unsigned events = 0, clears = 0;
    for (unsigned i = 0; i < v->section_count; i++) {
        events += v->sections[i].kind == SGX535_PACKET_EVENT_PDS;
        clears +=
            v->sections[i].kind == SGX535_PACKET_FRAGMENT_USE && v->sections[i].byte_count == 8;
    }
    CHECK(events == 1 && clears == 1 &&
          !sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes));
    ordered_reference(r, v);
    {
        /* Complete first-frame/shared-background extents, independently fixed
         * before compaction. The old TA envelope remains conservatively kept. */
        static const uint32_t offsets[7] = {192, 352, 1600, 1184, 1312, 1376, 1504};
        static const uint32_t roles[7] = {3, 3, 2, 0, 0, 0, 0};
        static const uint32_t counts[7] = {160, 20, 144, 128, 60, 124, 92};
        static const uint32_t minimum[8] = {4096, 4096,  4096,     4096,
                                            8192, 16384, 33554432, 33554432};
        for (unsigned i = 0; i < 7; i++) {
            const struct sgx535_window_mesh_section *section = &v->sections[19 + i];
            CHECK(section->role == roles[i] && section->resource_byte_offset == offsets[i] &&
                  section->byte_count == counts[i]);
        }
        for (unsigned i = 0; i < 8; i++)
            CHECK(v->requirements[i].minimum_bytes == minimum[i]);
        CHECK(v->sections[v->ta_stream_section].resource_byte_offset == 1600);
    }
    {
        struct sgx535_depth_window_mesh_single_sample_request repeated[32];
        for (unsigned i = 0; i < 32; i++)
            repeated[i] = r[0];
        for (unsigned groups = 1; groups <= 32; groups += 31) {
            struct sgx535_depth_window_mesh_ordered *control = NULL;
            uint32_t capacity;
            CHECK(!sgx535_depth_window_mesh_ordered_workspace(repeated, groups, &capacity));
            void *work = calloc(1, capacity);
            CHECK(work && !sgx535_depth_window_mesh_ordered_create(repeated, groups, &control, work,
                                                                   capacity));
            const struct sgx535_depth_window_mesh_ordered_view *view =
                sgx535_depth_window_mesh_ordered_view(control);
            CHECK(view && view->requirements[SGX535_TASK_SCENE].minimum_bytes == 8192 &&
                  view->requirements[SGX535_TASK_TARGET].minimum_bytes == 16384 &&
                  view->requirements[SGX535_TASK_DPM_TABLES].minimum_bytes == 33554432 &&
                  view->requirements[SGX535_TASK_DPM_PARAMETERS].minimum_bytes == 33554432);
            for (unsigned i = 0; i < 3; i++) {
                static const unsigned packed_roles[3] = {0, 2, 3};
                uint32_t bytes_needed = view->requirements[packed_roles[i]].minimum_bytes;
                CHECK(groups == 1 ? bytes_needed == 4096 : bytes_needed > 4096);
            }
            CHECK(!sgx535_depth_window_mesh_ordered_validate(repeated, groups, control, work,
                                                             capacity));
            sgx535_depth_window_mesh_ordered_destroy(control);
            free(work);
        }
    }
    {
        struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
        struct sgx535_depth_window_mesh_ordered_bound *bound = NULL, *refused;
        struct sgx535_depth_window_mesh_task *first_task = calloc(1, sizeof(*first_task));
        struct sgx535_depth_window_mesh_workspace *first_work = calloc(1, sizeof(*first_work));
        struct sgx535_depth_window_mesh_binding bindings;
        struct sgx535_single_sample_lowering lowering;
        uint8_t geometry[512], temporary[1024];
        CHECK(first_task && first_work);
        CHECK(sgx535_depth_window_mesh_single_sample_build(
                  &r[0], first_task, &lowering, geometry, sizeof(geometry), first_work, temporary,
                  sizeof(temporary)) == SGX535_TASK_BLOCKED);
        depth_binding(first_task, &bindings);
        {
            struct sgx535_background_request bg = {.version = SGX535_BACKGROUND_WINDOW_MESH_VERSION,
                                                   .core_id = r[0].mesh.scene.foreground.core_id,
                                                   .revision = r[0].mesh.scene.foreground.revision,
                                                   .width = 64,
                                                   .height = 64,
                                                   .packed_argb =
                                                       r[0].mesh.scene.packed_background_argb};
            struct sgx535_background_bound old, full, reference;
            CHECK(sgx535_background_window_mesh_bind_seed(&bg, &first_task->seed.background,
                                                          &bindings.background,
                                                          &old) == SGX535_TASK_BLOCKED);
            CHECK(sgx535_background_window_mesh_bind_full_owner_seed(
                      &bg, &first_task->seed.background, &bindings.background, &full) ==
                      SGX535_TASK_BLOCKED &&
                  !memcmp(&old, &full, sizeof(old)));
            reference = full;
            bindings.background.resources[0].allocation_bytes = 8192;
            bindings.background.resources[1].allocation_bytes = 8192;
            CHECK(sgx535_background_window_mesh_bind_seed(&bg, &first_task->seed.background,
                                                          &bindings.background,
                                                          &old) == SGX535_TASK_INVALID);
            CHECK(sgx535_background_window_mesh_bind_full_owner_seed(
                      &bg, &first_task->seed.background, &bindings.background, &full) ==
                      SGX535_TASK_BLOCKED &&
                  !memcmp(reference.image.bytes, full.image.bytes, sizeof(reference.image.bytes)));
            for (unsigned kind = 0; kind < 5; kind++) {
                struct sgx535_background_binding bad_binding = bindings.background;
                if (kind == 0)
                    bad_binding.resources[0].allocation_bytes = UINT64_MAX;
                if (kind == 1)
                    bad_binding.resources[1].allocation_bytes = 8193;
                if (kind == 2)
                    bad_binding.resources[0].gpu_va_bytes = 0x4ffff000u;
                if (kind == 3)
                    bad_binding.fragment_bank_index = 16;
                if (kind == 4)
                    bad_binding.resources[2].storage_id = bad_binding.resources[0].storage_id;
                CHECK(sgx535_background_window_mesh_bind_full_owner_seed(
                          &bg, &first_task->seed.background, &bad_binding, &full) !=
                      SGX535_TASK_BLOCKED);
            }
        }
        for (unsigned i = 0; i < 8; i++)
            bindings.scene.roles[i].allocation_bytes = v->requirements[i].minimum_bytes;
        bindings.background.resources[0].allocation_bytes =
            bindings.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
        bindings.background.resources[1].allocation_bytes =
            bindings.scene.roles[SGX535_TASK_PDS].allocation_bytes;
        bindings.background.parameter_bytes =
            (uint32_t)bindings.scene.roles[SGX535_TASK_DPM_PARAMETERS].allocation_bytes;
        CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(r, 2, &sizes) &&
              sizes.workspace_bytes > bytes && sizes.source_metadata_bytes &&
              sizes.bound_metadata_bytes > sizes.source_metadata_bytes);
        void *bind_scratch = calloc(1, (size_t)sizes.workspace_bytes + 32u);
        CHECK(bind_scratch);
        memset((uint8_t *)bind_scratch + sizes.workspace_bytes, 0xa5, 32);
        int binding_result = sgx535_depth_window_mesh_ordered_bind(
            r, 2, o, &bindings, &bound, bind_scratch, sizes.workspace_bytes);
        printf("ORDERED_BIND result=%d groups=%u sections=%u\n", binding_result, v->group_count,
               v->section_count);
        fflush(stdout);
        CHECK(!binding_result && bound);
        printf(
            "ORDERED_STORAGE workspace=%u bind_workspace=%u source_metadata=%u bound_metadata=%u "
            "create_allocations=%u\n",
            bytes, sizes.workspace_bytes, sizes.source_metadata_bytes, sizes.bound_metadata_bytes,
            allocations);
        {
            struct sgx535_depth_window_mesh_ordered_bind_sizes transient;
            r[0].mesh.scene.foreground.vertex_count = 6;
            CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(r, 2, &transient));
            r[0].mesh.scene.foreground.vertex_count = 3;
            void *transient_scratch = calloc(1, transient.workspace_bytes);
            CHECK(transient_scratch);
            r[0].mesh.scene.foreground.vertex_count = 6;
            ordered_binding_probe = &bindings;
            ordered_binding_restore = r;
            refused = (void *)(uintptr_t)0x1234;
            int result = sgx535_depth_window_mesh_ordered_bind(
                r, 2, o, &bindings, &refused, transient_scratch, transient.workspace_bytes);
            printf("ORDERED_BIND_PLAN result=%d restored=%u out_unchanged=%u\n", result,
                   !ordered_binding_probe, refused == (void *)(uintptr_t)0x1234);
            fflush(stdout);
            CHECK(result == -ESTALE && !ordered_binding_probe &&
                  refused == (void *)(uintptr_t)0x1234);
            free(transient_scratch);
        }
        const struct sgx535_depth_window_mesh_ordered_bound_view *bv =
            sgx535_depth_window_mesh_ordered_bound_view(bound);
        CHECK(
            bv && bv->source != v && bv->source->group_count == 2 &&
            bv->resolved_relocations == v->relocation_count &&
            bv->metadata_bytes == sizes.bound_metadata_bytes && !bv->hardware_qualified &&
            !bv->layout_qualified && !bv->visibility_qualified && !bv->retirement_qualified &&
            bv->ta_stream_va_bytes == bindings.scene.roles[SGX535_TASK_PDS].gpu_va_bytes +
                                          v->sections[v->ta_stream_section].resource_byte_offset &&
            bv->event_pds_va_bytes == bindings.scene.roles[SGX535_TASK_PDS].gpu_va_bytes +
                                          v->sections[v->event_pds_section].resource_byte_offset &&
            !sgx535_depth_window_mesh_ordered_bound_validate(r, 2, o, &bindings, bound,
                                                             bind_scratch, sizes.workspace_bytes));
        /* Repeated validation must rebuild packet state after comparison uses
         * its workspace. Poisoning also rejects reliance on a previous call. */
        for (unsigned pass = 0; pass < 3; pass++) {
            memset(scratch, (int)(0x31u + pass), bytes);
            memset(bind_scratch, (int)(0x61u + pass), sizes.workspace_bytes);
            CHECK(!sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes));
            CHECK(!sgx535_depth_window_mesh_ordered_bound_validate(
                r, 2, o, &bindings, bound, bind_scratch, sizes.workspace_bytes));
        }
        {
            unsigned before = alloc_calls;
            CHECK(!sgx535_depth_window_mesh_ordered_bound_validate(
                r, 2, o, &bindings, bound, bind_scratch, sizes.workspace_bytes));
            unsigned validation_allocations = alloc_calls - before;
            CHECK(validation_allocations);
            printf("ORDERED_STORAGE bound_validation_allocations=%u\n", validation_allocations);
            for (unsigned i = 1; i <= validation_allocations; i++) {
                late_word = (uint32_t *)&fail_alloc;
                late_alloc_serial = alloc_calls + i;
                CHECK(sgx535_depth_window_mesh_ordered_bound_validate(
                          r, 2, o, &bindings, bound, bind_scratch, sizes.workspace_bytes) ==
                          -ENOMEM &&
                      !late_word);
                fail_alloc = 0;
            }
            CHECK(!sgx535_depth_window_mesh_ordered_bound_validate(
                r, 2, o, &bindings, bound, bind_scratch, sizes.workspace_bytes));
        }
        for (unsigned i = 0; i < 32; i++) {
            CHECK(((uint8_t *)scratch)[bytes + i] == 0x5a);
            CHECK(((uint8_t *)bind_scratch)[sizes.workspace_bytes + i] == 0xa5);
        }
        for (unsigned i = 0; i < v->relocation_count; i++) {
            const struct sgx535_task_packet_reloc *rel = &v->relocations[i];
            const struct sgx535_window_mesh_section *section = &v->sections[rel->section];
            const struct sgx535_task_launch_role *owner = &bindings.scene.roles[rel->role];
            uint64_t address = owner->gpu_va_bytes + rel->resource_byte_offset, value;
            if (rel->kind == SGX535_PACKET_DRI_OFFSET)
                value = (address >> rel->right_shift) << rel->left_shift;
            else {
                unsigned bank = rel->role - SGX535_TASK_USE_VERTEX;
                struct sgx535_use_code_bank_input input = {
                    (uint32_t)owner->gpu_va_bytes, (uint32_t)owner->allocation_bytes,
                    (uint32_t)address, bindings.scene.bank_index[bank], bank};
                struct sgx535_pds_use_link_projection link;
                CHECK(bank < 2 &&
                      sgx535_pds_use_link_project(&input, &link) == SGX535_PDS_USE_LINK_OK);
                value = rel->kind == SGX535_PACKET_USE_BANK ? link.bank_selector_fields
                                                            : link.address_fields;
            }
            uint32_t offset = section->byte_offset + rel->section_byte_offset;
            CHECK((get(bv->bytes + offset) & rel->mask) == ((uint32_t)value & rel->mask));
        }
        {
            struct sgx535_depth_window_mesh_single_sample_request many[32];
            struct sgx535_depth_window_mesh_ordered_bind_sizes large_sizes;
            struct sgx535_depth_window_mesh_ordered *large = NULL;
            for (unsigned i = 0; i < 32; i++)
                many[i] = r[0];
            CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(many, 32, &large_sizes));
            void *work = calloc(1, large_sizes.workspace_bytes);
            CHECK(work && !sgx535_depth_window_mesh_ordered_create(many, 32, &large, work,
                                                                   large_sizes.workspace_bytes));
            refused = (void *)(uintptr_t)0x1234;
            CHECK(sgx535_depth_window_mesh_ordered_bind(many, 32, large, &bindings, &refused, work,
                                                        large_sizes.workspace_bytes) == -EINVAL &&
                  refused == (void *)(uintptr_t)0x1234);
            sgx535_depth_window_mesh_ordered_destroy(large);
            free(work);
        }
        uint8_t *bound_bytes = (void *)(uintptr_t)bv->bytes;
        bound_bytes[v->sections[v->groups[1].fragment_section].byte_offset] ^= 1;
        CHECK(sgx535_depth_window_mesh_ordered_bound_validate(
                  r, 2, o, &bindings, bound, bind_scratch, sizes.workspace_bytes) == -ESTALE);
        bound_bytes[v->sections[v->groups[1].fragment_section].byte_offset] ^= 1;
        refused = (void *)(uintptr_t)0x1234;
        bindings.scene.roles[SGX535_TASK_PDS].allocation_bytes = 0;
        CHECK(sgx535_depth_window_mesh_ordered_bind(r, 2, o, &bindings, &refused, bind_scratch,
                                                    sizes.workspace_bytes) < 0 &&
              refused == (void *)(uintptr_t)0x1234);
        sgx535_depth_window_mesh_ordered_bound_destroy(bound);
        free(bind_scratch);
        free(first_work);
        free(first_task);
    }
    for (unsigned i = 0; i < 2; i++) {
        const struct sgx535_depth_window_mesh_ordered_group *g = &v->groups[i];
        CHECK(g->first_vertex == i * 3u && g->vertex_count == 3 &&
              !memcmp(&g->samples, &r[i].samples, sizeof(g->samples)) &&
              g->lowering.fragment_invocations == 1 &&
              g->interpolation == r[i].mesh.scene.foreground.interpolation);
    }
    for (unsigned i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *a = &v->relocations[i];
        CHECK(a->section < v->section_count && a->role < 8 &&
              a->resource_byte_offset + a->span_bytes <= v->requirements[a->role].minimum_bytes);
    }
    for (unsigned kind = 0; kind < 9; kind++) {
        memcpy(bad, r, sizeof(bad));
        if (kind == 0)
            bad[1].mesh.scene.packed_background_argb ^= 1;
        if (kind == 1)
            bad[1].samples.depth_storage_samples = 1;
        if (kind == 2)
            bad[1].samples.fragment_instructions--;
        if (kind == 3)
            bad[1].mesh.scene.foreground.window.half_pixel_center = 0;
        if (kind == 4)
            bad[1].mesh.format = SGX535_DEPTH_D16;
        if (kind == 5)
            bad[1].mesh.scene.foreground.order = SGX535_WINDOW_MESH_INDEXED;
        if (kind == 6)
            bad[1].mesh.scene.foreground.reserved[0] = 1;
        if (kind == 7)
            bad[1].mesh.scene.foreground.fragment_count = 0;
        if (kind == 8)
            bad[1].samples.reserved[2] = 1;
        failed = (void *)(uintptr_t)0x1234;
        CHECK(sgx535_depth_window_mesh_ordered_create(bad, 2, &failed, scratch, bytes) < 0 &&
              failed == (void *)(uintptr_t)0x1234);
    }
    vertices[1][2].window_xyz_reciprocal_w[3] = 0x3f000000u;
    failed = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, &failed, scratch, bytes) == -EOPNOTSUPP &&
          failed == (void *)(uintptr_t)0x1234);
    vertices[1][2].window_xyz_reciprocal_w[3] = 0x3f800000u;
    for (unsigned i = 1; i <= allocations; i++) {
        late_word = (uint32_t *)&fail_alloc;
        late_alloc_serial = alloc_calls + i;
        failed = (void *)(uintptr_t)0x1234;
        CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, &failed, scratch, bytes) == -ENOMEM &&
              failed == (void *)(uintptr_t)0x1234 && !late_word);
        fail_alloc = 0;
    }
    uint32_t *mutations[] = {&r[1].samples.fragment_instructions, &ir[1][0].component,
                             &vertices[1][2].rgba[3], &indices[1][2]};
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        for (unsigned serial = 1; serial <= allocations; serial += allocations - 1u) {
            uint32_t saved = *mutations[i];
            late_word = mutations[i];
            late_alloc_serial = alloc_calls + serial;
            failed = (void *)(uintptr_t)0x1234;
            CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, &failed, scratch, bytes) ==
                      -ESTALE &&
                  failed == (void *)(uintptr_t)0x1234 && !late_word);
            *mutations[i] = saved;
        }
    }
    uint8_t *payload_bytes = (void *)(uintptr_t)v->bytes;
    payload_bytes[v->sections[v->groups[1].fragment_section].byte_offset] ^= 1;
    CHECK(sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes) == -ESTALE);
    payload_bytes[v->sections[v->groups[1].fragment_section].byte_offset] ^= 1;
    CHECK(!sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes));
    failed = (void *)(uintptr_t)0x1234;
    CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, &failed, scratch, bytes - 1u) == -EINVAL &&
          failed == (void *)(uintptr_t)0x1234);
    CHECK(sgx535_depth_window_mesh_ordered_create(r, 2, (void *)&r, scratch, bytes) == -EINVAL);
    for (unsigned control = 0; control < 2; control++) {
        memcpy(bad, r, sizeof(bad));
        ordered_header_source = bad;
        ordered_header_scratch = scratch;
        ordered_header_bytes = sizeof(bad);
        ordered_header_control = control;
        ordered_payload_probe = bad[0].mesh.scene.foreground.vertices;
        ordered_payload_follows = 0;
        failed = (void *)(uintptr_t)0x1234;
        int status = sgx535_depth_window_mesh_ordered_create(bad, 2, &failed, scratch, bytes);
        ordered_payload_probe = NULL;
        printf(
            "ORDERED_EARLY_HEADER control=%u result=%d payload_follows=%u fired=%u out_unchanged=%u\n",
            control, status, ordered_payload_follows, !ordered_header_source,
            failed == (void *)(uintptr_t)0x1234);
        fflush(stdout);
        CHECK(status == -ESTALE && !ordered_payload_follows && !ordered_header_source &&
              failed == (void *)(uintptr_t)0x1234);
    }
    memcpy(bad, r, sizeof(bad));
    bad[1].mesh.scene.foreground.vertex_count = bad[1].mesh.scene.foreground.index_count = 4095;
    CHECK(sgx535_depth_window_mesh_ordered_create(bad, 2, &failed, scratch, bytes) == -E2BIG &&
          failed == (void *)(uintptr_t)0x1234);
    struct sgx535_task_packet_reloc *changed = (void *)(uintptr_t)v->relocations;
    changed[0].resource_byte_offset ^= 4;
    CHECK(sgx535_depth_window_mesh_ordered_validate(r, 2, o, scratch, bytes) == -ESTALE);
    changed[0].resource_byte_offset ^= 4;
    sgx535_depth_window_mesh_ordered_destroy(o);
    for (unsigned i = 0; i < 32; i++)
        CHECK(((uint8_t *)scratch)[bytes + i] == 0x5a);
    free(scratch);
}
/* Group G of an ordered clip-space test frame, or its window-space twin: the
 * same draw with window positions (W 1). Inputs are smooth or flat; without
 * inputs the colour is the uniform COLOUR. */
static void clip_group(struct sgx535_depth_window_mesh_single_sample_request *r,
                       struct sgx535_window_mesh_vertex v[3], uint32_t ix[3],
                       struct sgx535_fragment_instruction *ir, const uint32_t colour[4], bool clip,
                       uint32_t interpolation, uint32_t cull)
{
    /* Any finite clip position, W zero and negative included: the MTE clips. */
    static const uint32_t positions[3][4] = {{0xbf000000u, 0xbf000000u, 0x3e800000u, 0x3f800000u},
                                             {0x40000000u, 0xbf400000u, 0x3f000000u, 0xbf800000u},
                                             {0x3e800000u, 0x3f400000u, 0xc0400000u, 0}};
    struct sgx535_window_mesh_request *m = &r->mesh.scene.foreground;
    memset(r, 0, sizeof(*r));
    *m = request(v, ix, ir, 3, 3, 64, 64);
    r->mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
    r->mesh.format = SGX535_DEPTH_D24S8;
    r->mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
    r->mesh.scene.packed_background_argb = 0xff000000u;
    m->order = SGX535_WINDOW_MESH_DEINDEXED;
    m->interpolation = interpolation;
    m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
    m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
    m->window.viewport_scale_f32[1] = 0xc2000000u;
    m->window.viewport_translate_f32[1] = 0x42000000u;
    r->samples = (struct sgx535_single_sample_source){
        .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
    if (interpolation == SGX535_WINDOW_MESH_NO_INPUT) {
        m->uniforms = colour;
        m->uniform_word_count = 4;
        for (unsigned c = 0; c < 4; c++)
            ir[c] =
                (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_UNIFORM, .index = c};
        for (unsigned j = 0; j < 3; j++)
            memset(v[j].rgba, 0, sizeof(v[j].rgba));
    }
    for (unsigned j = 0; j < 3; j++)
        v[j].window_xyz_reciprocal_w[3] = 0x3f800000u;
    if (clip) {
        m->version = SGX535_WINDOW_MESH_CLIP_VERSION;
        m->cull = cull;
        for (unsigned j = 0; j < 3; j++)
            memcpy(v[j].window_xyz_reciprocal_w, positions[j], 16);
    }
}
static int clip_create(const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n,
                       bool f32, void **o, const struct sgx535_depth_window_mesh_ordered_view **v,
                       void *scratch, uint32_t bytes)
{
    int ret = f32 ? sgx535_depth_window_mesh_f32_ordered_create(r, n, (void *)o, scratch, bytes)
                  : sgx535_depth_window_mesh_ordered_create(r, n, (void *)o, scratch, bytes);
    if (ret)
        return ret;
    *v = f32 ? sgx535_depth_window_mesh_f32_ordered_view(*o)
             : sgx535_depth_window_mesh_ordered_view(*o);
    CHECK(*v);
    return f32 ? sgx535_depth_window_mesh_f32_ordered_validate(r, n, *o, scratch, bytes)
               : sgx535_depth_window_mesh_ordered_validate(r, n, *o, scratch, bytes);
}
static void clip_destroy(void *o, bool f32)
{
    if (f32)
        sgx535_depth_window_mesh_f32_ordered_destroy(o);
    else
        sgx535_depth_window_mesh_ordered_destroy(o);
}
/* The ordered frame's bound view, its relocated words checked independently. */
static const struct sgx535_depth_window_mesh_ordered_bound_view *
clip_bind(const struct sgx535_depth_window_mesh_single_sample_request *r, uint32_t n, bool f32,
          void *o, const struct sgx535_depth_window_mesh_ordered_view *v, void **bound,
          void *scratch, uint32_t bytes)
{
    struct sgx535_window_mesh_task *owners = calloc(1, sizeof(*owners));
    struct sgx535_depth_window_mesh_binding b;
    const struct sgx535_depth_window_mesh_ordered_bound_view *bv;
    CHECK(owners);
    memcpy(owners->requirements, v->requirements, sizeof(owners->requirements));
    memset(&b, 0, sizeof(b));
    binding(owners, &b.scene, &b.background);
    b.scene.roles[SGX535_TASK_USE_FRAGMENT].gpu_va_bytes = 0x40004000u;
    b.background.resources[0].gpu_va_bytes = 0x40004000u;
    b.background.resources[0].allocation_bytes =
        b.scene.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
    b.background.resources[1].allocation_bytes = b.scene.roles[SGX535_TASK_PDS].allocation_bytes;
    b.depth = (struct sgx535_depth_window_mesh_owner){
        110, 0x50000000, (v->depth.allocation_bytes + 4095u) & ~UINT64_C(4095), 3, 0};
    if (f32) {
        CHECK(
            !sgx535_depth_window_mesh_f32_ordered_bind(r, n, o, &b, (void *)bound, scratch, bytes));
        bv = sgx535_depth_window_mesh_f32_ordered_bound_view(*bound);
        CHECK(bv && !sgx535_depth_window_mesh_f32_ordered_bound_validate(r, n, o, &b, *bound,
                                                                         scratch, bytes));
    } else {
        CHECK(!sgx535_depth_window_mesh_ordered_bind(r, n, o, &b, (void *)bound, scratch, bytes));
        bv = sgx535_depth_window_mesh_ordered_bound_view(*bound);
        CHECK(bv && !sgx535_depth_window_mesh_ordered_bound_validate(r, n, o, &b, *bound, scratch,
                                                                     bytes));
    }
    CHECK(bv->resolved_relocations == v->relocation_count);
    for (uint32_t i = 0; i < v->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *rel = &v->relocations[i];
        const struct sgx535_task_launch_role *owner = &b.scene.roles[rel->role];
        uint64_t address = owner->gpu_va_bytes + rel->resource_byte_offset, value;
        if (rel->kind == SGX535_PACKET_DRI_OFFSET)
            value = (address >> rel->right_shift) << rel->left_shift;
        else {
            unsigned bank = rel->role - SGX535_TASK_USE_VERTEX;
            uint32_t units = (uint32_t)(address & 0x7ffffu) / 16u;
            CHECK(bank < 2);
            value = b.scene.bank_index[bank] | (units / 2048u) * 16u | (units % 2048u) * 256u;
        }
        CHECK((get(bv->bytes + v->sections[rel->section].byte_offset + rel->section_byte_offset) &
               rel->mask) == ((uint32_t)value & rel->mask));
    }
    free(owners);
    return bv;
}
static void clip_bound_destroy(void *bound, bool f32)
{
    if (f32)
        sgx535_depth_window_mesh_f32_ordered_bound_destroy(bound);
    else
        sgx535_depth_window_mesh_ordered_bound_destroy(bound);
}
static uint32_t clip_register(const struct sgx535_depth_window_mesh_ordered_bound_view *bv,
                              unsigned stage, uint32_t offset)
{
    uint32_t found = 0, value = 0;
    for (uint32_t i = 0; i < bv->stages[stage].count; i++)
        if (bv->stages[stage].actions[i].offset == offset) {
            value = bv->stages[stage].actions[i].value;
            found++;
        }
    CHECK(found == 1);
    return value;
}
/* The section of V of ROLE holding [OFFSET, OFFSET + BYTES), or UINT32_MAX. */
static uint32_t clip_target(const struct sgx535_depth_window_mesh_ordered_view *v, uint32_t role,
                            uint32_t offset, uint32_t bytes)
{
    for (uint32_t i = 0; i < v->section_count; i++) {
        const struct sgx535_window_mesh_section *s = &v->sections[i];
        if (s->role == role && offset >= s->resource_byte_offset &&
            offset - s->resource_byte_offset <= s->byte_count &&
            bytes <= s->byte_count - (offset - s->resource_byte_offset))
            return i;
    }
    return UINT32_MAX;
}
/* Clip-space ordered frames against their window-space twins:
 * each foreground draw's own MTE record, sixteen-word state DMA and state USE
 * program, and TA attribute allocation; mask, clear and every other section
 * and relocation as in the twin. Both families, with and without uniforms. */
static void ordered_clip_space(void)
{
    static const uint32_t colour[4] = {0x3f800000u, 0x3f000000u, 0, 0x3f800000u};
    static const uint32_t viewport[6] = {0x42000000u, 0x42000000u, 0x42000000u,
                                         0xc2000000u, 0x3f000000u, 0x3f000000u};
    static const uint32_t program16[4] = {0xa0000000u, 0x28a1f001u, 0xa0200800u, 0xfb274000u};
    for (unsigned f32 = 0; f32 < 2; f32++)
        for (unsigned uniform = 0; uniform < 2; uniform++)
            for (uint32_t n = 1; n <= 2; n++) {
                struct sgx535_depth_window_mesh_single_sample_request c[2], w[2];
                struct sgx535_window_mesh_vertex cv[2][3], wv[2][3];
                struct sgx535_fragment_instruction cir[2][SGX535_NIR_MAX_INSTRUCTIONS],
                    wir[2][SGX535_NIR_MAX_INSTRUCTIONS];
                uint32_t cix[2][3], wix[2][3];
                struct sgx535_depth_window_mesh_ordered_bind_sizes sizes;
                const struct sgx535_depth_window_mesh_ordered_view *v, *t;
                const struct sgx535_depth_window_mesh_ordered_bound_view *bv, *tv;
                void *o = NULL, *to = NULL, *bound = NULL, *tbound = NULL;
                for (unsigned g = 0; g < n; g++) {
                    /* One draw is the uniform one, or of two the second. */
                    uint32_t interpolation = uniform && g + 1u == n ? SGX535_WINDOW_MESH_NO_INPUT
                                             : g                    ? SGX535_VARYING_FLAT_REPLICATED
                                                                    : SGX535_VARYING_SMOOTH;
                    uint32_t cull = g ? SGX535_WINDOW_MESH_CULL_CCW : SGX535_WINDOW_MESH_CULL_CW;
                    clip_group(&c[g], cv[g], cix[g], cir[g], colour, true, interpolation, cull);
                    clip_group(&w[g], wv[g], wix[g], wir[g], colour, false, interpolation, 0);
                }
                CHECK(!sgx535_depth_window_mesh_ordered_bind_sizes(c, n, &sizes));
                void *scratch = calloc(1, sizes.workspace_bytes);
                CHECK(scratch);
                CHECK(!clip_create(c, n, f32, &o, &v, scratch, sizes.workspace_bytes));
                CHECK(!clip_create(w, n, f32, &to, &t, scratch, sizes.workspace_bytes));
                CHECK(v->group_count == n && v->section_count == t->section_count &&
                      v->relocation_count == t->relocation_count &&
                      v->ta_stream_section == t->ta_stream_section &&
                      !memcmp(v->requirements, t->requirements, sizeof(v->requirements)));
                for (uint32_t i = 0; i < v->section_count; i++) {
                    const struct sgx535_window_mesh_section *a = &v->sections[i],
                                                            *b = &t->sections[i];
                    const uint8_t *x = v->bytes + a->byte_offset, *y = t->bytes + b->byte_offset;
                    CHECK(a->kind == b->kind && a->role == b->role && !a->source);
                    if (a->kind == SGX535_PACKET_MTE_STATE && b->byte_count == 92) {
                        CHECK(a->byte_count == 108 && !memcmp(x + 64, y + 48, 44) &&
                              !memcmp(x + 4, y + 4, 16) && !memcmp(x + 20, viewport, 24) &&
                              get(x + 44) == get(y + 28) && get(x + 48) == 0x33d6bf95u &&
                              get(x + 56) == get(y + 36) && get(x + 60) == get(y + 40) &&
                              get(y + 32) == 0x10000u);
                    } else if (a->kind == SGX535_PACKET_STATE_USE) {
                        CHECK(a->byte_count == 48 && !memcmp(x, y, 16) &&
                              !memcmp(x + 32, y + 32, 16) && !memcmp(x + 16, program16, 16));
                    } else if (a->kind == SGX535_PACKET_STATE_PDS && a->byte_count == 124) {
                        uint32_t dma = get(x + 68);
                        CHECK(!memcmp(x, y, 68) && !memcmp(x + 72, y + 72, 52) &&
                              ((dma & 15u) + 1u) * (((dma >> 4) & 15u) + 1u) == 16u);
                    } else if (a->kind == SGX535_PACKET_TA_STREAM) {
                        CHECK(a->byte_count == b->byte_count && !memcmp(x, y, 56) &&
                              !memcmp(x + 56 + 28 * n, y + 56 + 28 * n, 12));
                        for (uint32_t g = 0; g < n; g++) {
                            const uint8_t *e = x + 56 + 28 * g, *f = y + 56 + 28 * g;
                            CHECK(!memcmp(e, f, 4) && !memcmp(e + 8, f + 8, 20) &&
                                  get(f + 4) == 0x0c02a203u && get(e + 4) == 0x0c02a204u);
                        }
                    } else if (a->kind == SGX535_PACKET_VERTICES) {
                        CHECK(a->byte_count == b->byte_count); /* positions below */
                    } else
                        CHECK(a->byte_count == b->byte_count && !memcmp(x, y, a->byte_count));
                    /* The first draw's and the frame's placements stay, but the
                     * PBE state's, past the grown record. Later draws compact. */
                    if (a->kind == SGX535_PACKET_PBE_STATE)
                        CHECK(b->resource_byte_offset == 672 && a->resource_byte_offset == 688);
                    else if (i < 19u + (uniform && n == 1))
                        CHECK(a->resource_byte_offset == b->resource_byte_offset);
                }
                for (uint32_t g = 0; g < n; g++) {
                    const struct sgx535_window_mesh_request *m = &c[g].mesh.scene.foreground;
                    const struct sgx535_window_mesh_section *mte =
                        &v->sections[v->groups[g].mte_section];
                    const uint8_t *x = v->bytes + mte->byte_offset;
                    const bool generic = f32 && m->interpolation != SGX535_WINDOW_MESH_NO_INPUT;
                    uint32_t control =
                        0x00088000u | m->cull |
                        (m->interpolation == SGX535_VARYING_FLAT_REPLICATED ? 0x60000u : 0u);
                    CHECK(get(x) == (generic ? 0xdd41u : 0x5d41u) && get(x + 52) == control &&
                          get(x + 44) == (generic            ? 0x08001000u
                                          : m->interpolation ? 0x08001800u
                                                             : 0x04001000u) &&
                          get(x + 56) == (generic ? 7u : 0u) && !get(x + 60) &&
                          get(x + 64) == 0x54c5u);
                    /* The draw's positions are copied, never transformed. */
                    const struct sgx535_window_mesh_section *vs = NULL;
                    for (uint32_t i = 0; i < v->relocation_count; i++) {
                        const struct sgx535_task_packet_reloc *rel = &v->relocations[i];
                        if (rel->section == v->groups[g].vertex_pds_section &&
                            rel->role == SGX535_TASK_VERTEX_DATA && !rel->section_byte_offset)
                            vs = &v->sections[clip_target(v, rel->role, rel->resource_byte_offset,
                                                          rel->span_bytes)];
                    }
                    CHECK(vs && vs->kind == SGX535_PACKET_VERTICES);
                    for (uint32_t j = 0; j < 3; j++)
                        CHECK(
                            !memcmp(v->bytes + vs->byte_offset + j * (m->interpolation ? 32u : 16u),
                                    cv[g][j].window_xyz_reciprocal_w, 16));
                }
                /* Relocations: same sites as the twin; the state programs DMA
                 * the record (16 words) and the mask's (+64, unchanged), link the
                 * sixteen-output program, and the event the moved PBE state. */
                for (uint32_t i = 0; i < v->relocation_count; i++) {
                    const struct sgx535_task_packet_reloc *a = &v->relocations[i],
                                                          *b = &t->relocations[i];
                    const struct sgx535_window_mesh_section *s = &v->sections[a->section];
                    uint32_t target =
                        clip_target(v, a->role, a->resource_byte_offset, a->span_bytes);
                    CHECK(a->kind == b->kind && a->section == b->section &&
                          a->section_byte_offset == b->section_byte_offset && a->role == b->role &&
                          a->mask == b->mask && a->section_byte_offset + 4u <= s->byte_count &&
                          (target != UINT32_MAX || a->role == SGX535_TASK_TARGET));
                    if (target == UINT32_MAX)
                        continue;
                    const struct sgx535_window_mesh_section *d = &v->sections[target];
                    uint32_t at = a->resource_byte_offset - d->resource_byte_offset;
                    if (d->kind == SGX535_PACKET_MTE_STATE && d->byte_count == 108)
                        CHECK(s->kind == SGX535_PACKET_STATE_PDS &&
                              (a->section_byte_offset ? at == 0 && a->span_bytes == 64
                                                      : at == 64 && a->span_bytes == 44));
                    if (s->kind == SGX535_PACKET_STATE_PDS && s->byte_count == 124 &&
                        a->role == SGX535_TASK_USE_VERTEX)
                        CHECK(d->kind == SGX535_PACKET_STATE_USE &&
                              at == (a->section_byte_offset == 72 ? 16u : 0u));
                    if (d->kind == SGX535_PACKET_PBE_STATE)
                        CHECK(s->kind == SGX535_PACKET_EVENT_PDS && !at);
                }
                tv = clip_bind(w, n, f32, to, t, &tbound, scratch, sizes.workspace_bytes);
                bv = clip_bind(c, n, f32, o, v, &bound, scratch, sizes.workspace_bytes);
                /* MTE_CTRL enables the W clamp for clip-space frames only. */
                CHECK(clip_register(bv, SGX535_TASK_LAUNCH_TA, 0x23c) == 0xa2u &&
                      clip_register(tv, SGX535_TASK_LAUNCH_TA, 0x23c) == 0x82u &&
                      clip_register(bv, SGX535_TASK_LAUNCH_RASTER, 0x800) == 1u);
                for (unsigned stage = 0; stage < SGX535_TASK_LAUNCH_STAGE_COUNT; stage++) {
                    CHECK(bv->stages[stage].count == tv->stages[stage].count);
                    for (uint32_t i = 0; i < bv->stages[stage].count; i++)
                        CHECK(bv->stages[stage].actions[i].offset == 0x23c ||
                              (bv->stages[stage].actions[i].offset ==
                                   tv->stages[stage].actions[i].offset &&
                               (bv->stages[stage].actions[i].offset == 0x238 ||
                                bv->stages[stage].actions[i].value ==
                                    tv->stages[stage].actions[i].value)));
                }
                printf("ORDERED_CLIP_SPACE f32=%u uniform=%u groups=%u sections=%u "
                       "relocations=%u\n",
                       f32, uniform, n, v->section_count, v->relocation_count);
                clip_bound_destroy(bound, f32);
                clip_bound_destroy(tbound, f32);
                clip_destroy(o, f32);
                clip_destroy(to, f32);
                free(scratch);
            }
}
/* MTE control for every cull mode and both shading modes, and the requests
 * a clip-space frame refuses. */
static void ordered_clip_controls(void)
{
    static const uint32_t colour[4] = {0x3f800000u, 0x3f000000u, 0, 0x3f800000u};
    struct sgx535_depth_window_mesh_single_sample_request r[2];
    struct sgx535_window_mesh_vertex vertices[2][3];
    struct sgx535_fragment_instruction ir[2][SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t indices[2][3], bytes;
    const struct sgx535_depth_window_mesh_ordered_view *v;
    void *o = NULL, *refused;
    clip_group(&r[0], vertices[0], indices[0], ir[0], colour, true, SGX535_VARYING_SMOOTH, 0);
    CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, 1, &bytes));
    void *scratch = calloc(1, bytes);
    CHECK(scratch);
    for (unsigned f32 = 0; f32 < 2; f32++)
        for (uint32_t cull = 0; cull < 3; cull++)
            for (unsigned flat = 0; flat < 2; flat++) {
                clip_group(&r[0], vertices[0], indices[0], ir[0], colour, true,
                           flat ? SGX535_VARYING_FLAT_REPLICATED : SGX535_VARYING_SMOOTH, cull);
                CHECK(!clip_create(r, 1, f32, &o, &v, scratch, bytes));
                const uint8_t *x = v->bytes + v->sections[v->groups[0].mte_section].byte_offset;
                CHECK(get(x) == (f32 ? 0xdd41u : 0x5d41u) &&
                      get(x + 52) == (0x00088000u | (flat ? 0x60000u : 0u) | cull));
                clip_destroy(o, f32);
            }
    for (unsigned kind = 0; kind < 7; kind++) {
        uint32_t n = 1;
        clip_group(&r[0], vertices[0], indices[0], ir[0], colour, true, SGX535_VARYING_SMOOTH,
                   SGX535_WINDOW_MESH_CULL_CW);
        if (kind == 0)
            r[0].mesh.scene.foreground.cull = 3;
        if (kind == 1) { /* Window-space requests do not cull. */
            clip_group(&r[0], vertices[0], indices[0], ir[0], colour, false, SGX535_VARYING_SMOOTH,
                       0);
            r[0].mesh.scene.foreground.cull = SGX535_WINDOW_MESH_CULL_CW;
        }
        if (kind == 2)
            vertices[0][1].window_xyz_reciprocal_w[3] = 0x7f800000u;
        if (kind == 3)
            vertices[0][2].window_xyz_reciprocal_w[0] = 0xffc00000u;
        if (kind == 4)
            r[0].mesh.scene.foreground.version = SGX535_WINDOW_MESH_CLIP_VERSION + 1u;
        if (kind >= 5) { /* A frame does not mix clip- and window-space draws. */
            clip_group(&r[1], vertices[1], indices[1], ir[1], colour, false, SGX535_VARYING_SMOOTH,
                       0);
            if (kind == 6) {
                struct sgx535_depth_window_mesh_single_sample_request swap = r[0];
                r[0] = r[1];
                r[1] = swap;
            }
            n = 2;
        }
        uint32_t need;
        CHECK(!sgx535_depth_window_mesh_ordered_workspace(r, n, &need));
        void *work = calloc(1, need);
        CHECK(work);
        for (unsigned f32 = 0; f32 < 2; f32++) {
            refused = (void *)(uintptr_t)0x1234;
            CHECK(clip_create(r, n, f32, &refused, &v, work, need) == -EOPNOTSUPP &&
                  refused == (void *)(uintptr_t)0x1234);
        }
        free(work);
    }
    /* Only the single-sample GL window constructors take clip-space input:
     * the legacy and the fixed GL window ones build the window-space request
     * and refuse it as clip-space (its window positions are finite clip ones). */
    {
        struct sgx535_window_mesh_workspace *mesh = calloc(1, sizeof(*mesh));
        struct sgx535_window_mesh_task *task = calloc(1, sizeof(*task));
        struct sgx535_window_mesh_request *m = &r[0].mesh.scene.foreground;
        uint8_t geometry[512], temporary[512];
        CHECK(mesh && task);
        for (unsigned gl = 0; gl < 2; gl++) {
            clip_group(&r[0], vertices[0], indices[0], ir[0], colour, false, SGX535_VARYING_SMOOTH,
                       0);
            if (gl)
                m->window.min_samples = 1;
            else
                m->window.half_pixel_center = m->window.bottom_edge_rule = 0;
            for (unsigned clip = 0; clip < 2; clip++) {
                m->version = clip ? SGX535_WINDOW_MESH_CLIP_VERSION : SGX535_WINDOW_MESH_VERSION;
                CHECK((gl ? sgx535_window_mesh_gl_window_build(m, task, geometry, sizeof(geometry),
                                                               mesh, temporary, sizeof(temporary))
                          : sgx535_window_mesh_build(m, task, geometry, sizeof(geometry), mesh,
                                                     temporary, sizeof(temporary))) ==
                      (clip ? SGX535_TASK_UNSUPPORTED : SGX535_TASK_BLOCKED));
            }
        }
        free(task);
        free(mesh);
    }
    free(scratch);
}
int main(int argc, char **argv)
{
    const unsigned capacity = 4096, cap = 8192;
    struct sgx535_window_mesh_vertex v[6];
    uint32_t ix[6];
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_depth_window_mesh_state_request r, bad;
    struct sgx535_depth_window_mesh_binding bind;
    struct sgx535_depth_window_mesh_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_depth_window_mesh_task *out = malloc(sizeof(*out)),
                                         *saved = malloc(sizeof(*saved));
    struct sgx535_depth_window_mesh_task *fixed = malloc(sizeof(*fixed)),
                                         *expected = malloc(sizeof(*expected));
    struct sgx535_depth_window_mesh_bound *bound = malloc(sizeof(*bound)),
                                          *old_bound = malloc(sizeof(*old_bound));
    struct sgx535_depth_window_mesh_bound *fixed_bound = malloc(sizeof(*fixed_bound)),
                                          *expected_bound = malloc(sizeof(*expected_bound));
    uint8_t geometry[4096], before_geometry[4096], fixed_geometry[4096], scratch[8192];
    CHECK(argc == 1 || argc == 2 || argc == 3);
    CHECK(w && out && saved && fixed && expected && bound && old_bound && fixed_bound &&
          expected_bound);
    for (unsigned format = 1; format <= 2; format++)
        for (unsigned compare = 0; compare < 8; compare++)
            for (unsigned write = 0; write < 2; write++) {
                memset(&r, 0, sizeof(r));
                r.version = 2;
                r.compare = compare;
                r.write_enable = write;
                r.mesh = depth_request(v, ix, ir, 6, 6, 64, 64, format);
                CHECK(sgx535_depth_window_mesh_build(&r.mesh, fixed, fixed_geometry, capacity, w,
                                                     scratch, cap) == SGX535_TASK_BLOCKED);
                memset(geometry, 0xa5, sizeof(geometry));
                CHECK(sgx535_depth_window_mesh_state_build(&r, out, geometry, capacity, w, scratch,
                                                           cap) == SGX535_TASK_BLOCKED);
                uint32_t bytes = out->seed.foreground.geometry.bytes;
                *expected = *fixed;
                state_image(expected, compare, write);
                CHECK(!memcmp(out, expected, sizeof(*out)) &&
                      !memcmp(geometry, fixed_geometry, bytes));
                for (unsigned i = bytes; i < capacity; i++)
                    CHECK(geometry[i] == 0xa5);
                CHECK(sgx535_depth_window_mesh_state_validate(&r, out, geometry, bytes, w, scratch,
                                                              cap) == SGX535_TASK_BLOCKED);
                CHECK(sgx535_depth_window_mesh_validate(&r.mesh, out, geometry, bytes, w, scratch,
                                                        cap) == SGX535_TASK_INVALID);
                depth_binding(fixed, &bind);
                CHECK(sgx535_depth_window_mesh_bind(&r.mesh, fixed, fixed_geometry, bytes, &bind,
                                                    fixed_bound, w, scratch,
                                                    cap) == SGX535_TASK_BLOCKED);
                CHECK(sgx535_depth_window_mesh_state_bind(&r, out, geometry, bytes, &bind, bound, w,
                                                          scratch, cap) == SGX535_TASK_BLOCKED);
                *expected_bound = *fixed_bound;
                expected_bound->version = 2;
                state_image(&expected_bound->image, compare, write);
                CHECK(!memcmp(bound, expected_bound, sizeof(*bound)));
                CHECK(sgx535_depth_window_mesh_state_bound_validate(&r, out, geometry, bytes, &bind,
                                                                    bound, w, scratch,
                                                                    cap) == SGX535_TASK_BLOCKED);
                CHECK(bound->missing_launch_contracts == 0x75 &&
                      bound->missing_depth_contracts == 0x7f && !bound->hardware_qualified &&
                      !bound->layout_qualified && !bound->visibility_qualified &&
                      !bound->retirement_qualified);
                CHECK(sgx535_depth_window_mesh_native_admission(bound) == SGX535_TASK_UNSUPPORTED);
                if (argc >= 2) {
                    char name[80];
                    int n = snprintf(name, sizeof(name), "state-f%u-c%u-w%u.pds", format, compare,
                                     write);
                    CHECK(n > 0 && (size_t)n < sizeof(name));
                    save(argv[1], name, bound->image.pds, sizeof(bound->image.pds));
                    if (argc == 3) {
                        n = snprintf(name, sizeof(name), "state-f%u-c%u-w%u.object", format,
                                     compare, write);
                        CHECK(n > 0 && (size_t)n < sizeof(name));
                        save(argv[2], name, &bound->image, sizeof(bound->image));
                        n = snprintf(name, sizeof(name), "state-f%u-c%u-w%u.binding", format,
                                     compare, write);
                        CHECK(n > 0 && (size_t)n < sizeof(name));
                        save(argv[2], name, bound, sizeof(*bound));
                    }
                }
            }
    uint32_t bytes = out->seed.foreground.geometry.bytes;
    *saved = *out;
    *old_bound = *bound;
    memcpy(before_geometry, geometry, sizeof(geometry));
    for (unsigned kind = 0; kind < 5; kind++) {
        bad = r;
        if (kind == 0)
            bad.version = 1;
        if (kind == 1)
            bad.compare = 8;
        if (kind == 2)
            bad.write_enable = 2;
        if (kind == 3)
            bad.reserved = 1;
        if (kind == 4)
            bad.mesh.version = 2;
        CHECK(sgx535_depth_window_mesh_state_build(&bad, out, geometry, capacity, w, scratch,
                                                   cap) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(out, saved, sizeof(*out)) &&
              !memcmp(geometry, before_geometry, sizeof(geometry)));
    }
    CHECK(sgx535_depth_window_mesh_state_build(NULL, out, geometry, capacity, w, scratch, cap) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_state_build(&r, out, geometry, bytes - 1u, w, scratch, cap) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_state_build(&r, out, geometry, capacity, w, scratch,
                                               bytes * 2u - 1u) == SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_state_build(&r, (void *)&r, geometry, capacity, w, scratch,
                                               cap) == SGX535_TASK_INVALID);
    CHECK(!memcmp(out, saved, sizeof(*out)) &&
          !memcmp(geometry, before_geometry, sizeof(geometry)));
    fail_alloc = 1;
    CHECK(sgx535_depth_window_mesh_state_build(&r, out, geometry, capacity, w, scratch, cap) ==
          SGX535_TASK_NOMEM);
    CHECK(sgx535_depth_window_mesh_state_bind(&r, out, geometry, bytes, &bind, bound, w, scratch,
                                              cap) == SGX535_TASK_NOMEM);
    fail_alloc = 0;
    CHECK(!memcmp(out, saved, sizeof(*out)) && !memcmp(bound, old_bound, sizeof(*bound)) &&
          !memcmp(geometry, before_geometry, sizeof(geometry)));
    uint32_t *mutations[] = {&r.compare,    &r.write_enable, &r.mesh.format,
                             &v[5].rgba[3], &ix[5],          &ir[0].component};
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        uint32_t original = *mutations[i];
        drift_at = mutations[i];
        CHECK(sgx535_depth_window_mesh_state_build(&r, out, geometry, capacity, w, scratch, cap) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!drift_at && !memcmp(out, saved, sizeof(*out)) &&
              !memcmp(geometry, before_geometry, sizeof(geometry)));
        *mutations[i] = original;
    }
    unsigned start = alloc_calls;
    CHECK(sgx535_depth_window_mesh_state_bind(&r, out, geometry, bytes, &bind, bound, w, scratch,
                                              cap) == SGX535_TASK_BLOCKED);
    unsigned count = alloc_calls - start;
    CHECK(count >= 2);
    uint32_t *late[] = {&r.compare, &out->depth.isp_set_bits, &bind.depth.access};
    for (unsigned i = 0; i < 3; i++) {
        uint32_t original = *late[i];
        late_word = late[i];
        late_alloc_serial = alloc_calls + count;
        CHECK(sgx535_depth_window_mesh_state_bind(&r, out, geometry, bytes, &bind, bound, w,
                                                  scratch, cap) == SGX535_TASK_UNSUPPORTED);
        CHECK(!late_word && !memcmp(bound, old_bound, sizeof(*bound)));
        *late[i] = original;
    }
    /* The new prefix is a real source span, including compare/write words
     * preceding the old request. Rejection does not inspect the false image. */
    size_t n = sizeof(*out) + sizeof(bind) + sizeof(r) + 64u;
    uint8_t *arena = malloc(n);
    CHECK(arena);
    struct sgx535_depth_window_mesh_state_request *ar =
        (void *)(arena + sizeof(*out) + sizeof(bind));
    *ar = r;
    const struct sgx535_depth_window_mesh_task *aliased =
        (void *)((uint8_t *)ar - sizeof(*out) + 8u);
    CHECK(sgx535_depth_window_mesh_state_bind(ar, aliased, geometry, bytes, &bind, bound, w,
                                              scratch, cap) == SGX535_TASK_INVALID);
    CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
    free(arena);
    free(expected_bound);
    free(fixed_bound);
    free(old_bound);
    free(bound);
    free(expected);
    free(fixed);
    free(saved);
    free(out);
    free(w);
    ordered_image();
    ordered_window_extents();
    ordered_fragment_storage();
    ordered_mixed_inputs();
    ordered_clip_space();
    ordered_clip_controls();
    printf(
        "depth state packet: %u checks;32 complete D16/D24 compare/write packets; all native gates closed\n",
        checks);
    return 0;
}
