// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_window_mesh.h"
#include "sgx535_window_mesh_internal.h"
#include "sgx535_vertex_pds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static int fail_alloc;
static unsigned alloc_calls;
static uint32_t *drift_at;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
    alloc_calls++;
    if (drift_at) {
        *drift_at ^= 1u;
        drift_at = NULL;
    }
    return fail_alloc ? NULL : __real_calloc(n, size);
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line%d: %s\n", __LINE__, #x);                                         \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void uv2_layout(void)
{
    uint32_t clip[18] = {0, 0, 0, 0x3f800000u, 0xbf800000u, 0x40000000u,
                         0, 0, 0, 0x3f800000u, 0x80000000u, 0x3f800000u,
                         0, 0, 0, 0x3f800000u, 0x7f7fffffu, 0xff7fffffu};
    uint32_t window_words[18] = {0x40000000u, 0x40000000u, 0, 0x3f800000u,
                                 0x41600000u, 0x40000000u, 0, 0x3f000000u,
                                 0x40000000u, 0x41600000u, 0, 0x40000000u};
    const uint32_t saved_clip[18] = {0, 0, 0, 0x3f800000u, 0xbf800000u, 0x40000000u,
                                     0, 0, 0, 0x3f800000u, 0x80000000u, 0x3f800000u,
                                     0, 0, 0, 0x3f800000u, 0x7f7fffffu, 0xff7fffffu};
    uint8_t geometry[256], saved_geometry[256];
    struct sgx535_window_mesh_uv2_source source = {64, 64, 3, 7, clip, window_words};
    struct sgx535_window_mesh_uv2_layout p, old;
    memset(geometry, 0xa5, sizeof(geometry));
    CHECK(sgx535_window_mesh_uv2_build(&source, &p, geometry, sizeof(geometry)) ==
          SGX535_TASK_BLOCKED);
    CHECK(p.logical_slot == 7 && !p.physical_set && p.vertex_count == 3 && p.geometry_bytes == 72 &&
          p.stride_bytes == 24 && p.output_dwords == 6 && p.outselects == 0x06001000u &&
          p.texsize == 1 && !p.texfloat);
    CHECK(p.dma.input_words == 6 && p.dma.transfer_count == 1 &&
          p.dma.transfers[0].word_count == 6 && !p.dma.transfers[0].source_byte_offset &&
          !p.dma.transfers[0].preceding_words &&
          p.dma.transfers[0].descriptor_word == 0x80a00005u && p.use.input_words == 6 &&
          p.use.instruction_count == 2 && p.use.words[0] == 0xa0000000u &&
          p.use.words[1] == 0x28a15001u && p.use.words[2] == 0xa0200000u &&
          p.use.words[3] == 0xfb275000u);
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned j = 0; j < 4; j++)
            CHECK(get(geometry + i * 24u + j * 4u) == window_words[i * 4u + j]);
        for (unsigned j = 0; j < 2; j++)
            CHECK(get(geometry + i * 24u + 16u + j * 4u) == clip[i * 6u + 4u + j]);
    }
    for (unsigned i = 72; i < sizeof(geometry); i++)
        CHECK(geometry[i] == 0xa5);
    CHECK(!memcmp(clip, saved_clip, sizeof(clip)));
    old = p;
    for (unsigned slot = 0; slot < SGX535_FRAGMENT_INPUT_SLOTS; slot++) {
        source.logical_slot = slot;
        CHECK(sgx535_window_mesh_uv2_build(&source, &p, geometry, sizeof(geometry)) ==
              SGX535_TASK_BLOCKED);
        old.logical_slot = slot;
        CHECK(!memcmp(&p, &old, sizeof(p)) && !p.physical_set);
        CHECK(sgx535_window_mesh_uv2_validate(&source, &p, geometry, 72) == SGX535_TASK_BLOCKED);
    }
    memcpy(saved_geometry, geometry, sizeof(geometry));
    for (unsigned i = 0; i < sizeof(p); i++) {
        ((uint8_t *)&p)[i] ^= 1u;
        CHECK(sgx535_window_mesh_uv2_validate(&source, &p, geometry, 72) == SGX535_TASK_INVALID);
        ((uint8_t *)&p)[i] ^= 1u;
    }
    for (unsigned i = 0; i < 72; i++) {
        geometry[i] ^= 1u;
        CHECK(sgx535_window_mesh_uv2_validate(&source, &p, geometry, 72) == SGX535_TASK_INVALID);
        geometry[i] ^= 1u;
    }
    CHECK(!memcmp(geometry, saved_geometry, sizeof(geometry)));
    for (unsigned fault = 0; fault < 13; fault++) {
        struct sgx535_window_mesh_uv2_source bad = source;
        enum sgx535_task_status expected = SGX535_TASK_UNSUPPORTED;
        switch (fault) {
        case 0:
            bad.width = 0;
            break;
        case 1:
            bad.height = 4097;
            break;
        case 2:
            bad.vertex_count = 0;
            break;
        case 3:
            bad.vertex_count = 4;
            break;
        case 4:
            bad.vertex_count = 4097;
            break;
        case 5:
            bad.logical_slot = SGX535_FRAGMENT_INPUT_SLOTS;
            break;
        case 6:
            clip[4] = 0x7fc00000u;
            break;
        case 7:
            window_words[3] = 0;
            break;
        case 8:
            window_words[2] = 0x40000000u;
            break;
        case 9:
            clip[3] = 0xbf800000u;
            break;
        case 10:
            bad.clip_varyings = NULL;
            expected = SGX535_TASK_INVALID;
            break;
        case 11:
            bad.window_position = NULL;
            expected = SGX535_TASK_INVALID;
            break;
        default:
            bad.clip_varyings = (const uint32_t *)(uintptr_t)(UINTPTR_MAX - 4u);
            expected = SGX535_TASK_INVALID;
            break;
        }
        CHECK(sgx535_window_mesh_uv2_build(&bad, &p, geometry, sizeof(geometry)) == expected);
        CHECK(!memcmp(&p, &old, sizeof(p)) && !memcmp(geometry, saved_geometry, sizeof(geometry)));
        memcpy(clip, saved_clip, sizeof(clip));
        window_words[2] = 0;
        window_words[3] = 0x3f800000u;
    }
    CHECK(sgx535_window_mesh_uv2_build(&source, &p, geometry, 71) == SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(NULL, &p, geometry, sizeof(geometry)) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(&source, NULL, geometry, sizeof(geometry)) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(&source, &p, NULL, sizeof(geometry)) == SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(&source, (void *)geometry, geometry, sizeof(geometry)) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(&source, &p, (void *)clip, sizeof(clip)) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_build(&source, &p, (void *)window_words, sizeof(window_words)) ==
          SGX535_TASK_INVALID);
    struct sgx535_window_mesh_uv2_source bad = source;
    bad.clip_varyings = window_words;
    CHECK(sgx535_window_mesh_uv2_build(&bad, &p, geometry, sizeof(geometry)) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_uv2_validate(&source, &p, geometry, 73) == SGX535_TASK_INVALID);
    CHECK(!memcmp(&p, &old, sizeof(p)) && !memcmp(geometry, saved_geometry, sizeof(geometry)) &&
          !memcmp(clip, saved_clip, sizeof(clip)));
}
static void fragment_layout(void)
{
    struct sgx535_window_mesh_fragment_layout p, old;
    CHECK(SGX535_WINDOW_MESH_FRAGMENT_USE_BYTES == 1536u);
    CHECK(SGX535_NIR_MAX_INSTRUCTIONS == 512u && SGX535_NIR_MAX_WORDS == 1024u);
    for (uint32_t words = 2; words <= 1024; words += 2) {
        CHECK(sgx535_window_mesh_fragment_layout(words, &p) == SGX535_TASK_BLOCKED);
        uint32_t bytes = words * 4u, event = ((bytes + 31u) / 32u) * 32u;
        uint32_t background = event + 32u < 1536u ? 1536u : event + 32u;
        CHECK(p.code_bytes == bytes && p.event_offset_bytes == event &&
              p.foreground_extent_bytes == event + 32u && p.background_offset_bytes == background &&
              p.clear_offset_bytes == background + 32u &&
              p.foreground_extent_bytes <= SGX535_WINDOW_MESH_FRAGMENT_STORAGE_BYTES);
    }
    old = p;
    const uint32_t bad[] = {0, 1, 3, 1025, UINT32_MAX - 1u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(sgx535_window_mesh_fragment_layout(bad[i], &p) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(&p, &old, sizeof(p)));
    }
    CHECK(sgx535_window_mesh_fragment_layout(2, NULL) == SGX535_TASK_INVALID);
}
static uint32_t large_fragment(struct sgx535_fragment_instruction *ir, unsigned components,
                               unsigned steps)
{
    CHECK(components && components <= 2 && 9u + components * steps <= SGX535_NIR_MAX_INSTRUCTIONS);
    ir[4] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                 .immediate = 0x3f000000};
    uint32_t at = 5, last[2] = {0, 1};
    for (unsigned c = 0; c < components; c++)
        for (unsigned i = 0; i < steps; i++) {
            ir[at] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAD,
                                                          .source = {last[c], 4, c ^ 1u}};
            last[c] = at++;
        }
    for (unsigned c = 0; c < 4; c++)
        ir[at++] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {last[c % components], 0, 0}};
    return at;
}
static void put(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8u * i));
}
#include "normalized_mask_list_reference.h"
static uint32_t f32(unsigned value)
{
    float f = (float)value;
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
static const uint32_t window[3][4] = {{0x40000000, 0x40000000, 0x3f000000, 0x3f800000},
                                      {0x41600000, 0x40000000, 0x3f000000, 0x3f000000},
                                      {0x41500000, 0x41400000, 0x3f000000, 0x40000000}};
static struct sgx535_window_mesh_request request(struct sgx535_window_mesh_vertex *v,
                                                 uint32_t *indices,
                                                 struct sgx535_fragment_instruction *ir,
                                                 uint32_t vertices, uint32_t count, uint32_t width,
                                                 uint32_t height)
{
    struct sgx535_window_mesh_request r = {.version = 14,
                                           .profile = 13,
                                           .core_id = 0x01130000,
                                           .revision = 0x00010201,
                                           .width = width,
                                           .height = height,
                                           .order = SGX535_WINDOW_MESH_INDEXED,
                                           .interpolation = 1,
                                           .vertices = v,
                                           .indices = indices,
                                           .vertex_count = vertices,
                                           .index_count = count,
                                           .fragment = ir,
                                           .fragment_count = 8};
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    for (unsigned i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4].index = i;
        ir[i + 4].source[0] = i;
    }
    for (unsigned i = 0; i < vertices; i++) {
        memcpy(v[i].window_xyz_reciprocal_w, window[i % 3u], 16);
        if (width < 16)
            v[i].window_xyz_reciprocal_w[0] = 0;
        if (height < 16)
            v[i].window_xyz_reciprocal_w[1] = 0;
        v[i].rgba[0] = v[i].rgba[3] = 0x3f800000u;
        v[i].rgba[1] = v[i].rgba[2] = 0;
    }
    for (unsigned i = 0; i < count; i++)
        indices[i] = i % vertices;
    r.window.origin = 1;
    r.window.sample_mask = 0xffffffffu;
    r.window.viewport_scale_f32[0] = f32(width);
    r.window.viewport_scale_f32[1] = f32(height) | 0x80000000u;
    r.window.viewport_scale_f32[2] = 0x3f000000u;
    r.window.viewport_translate_f32[0] = f32(width);
    r.window.viewport_translate_f32[1] = f32(height);
    r.window.viewport_translate_f32[2] = 0x3f000000u;
    return r;
}
static void binding(const struct sgx535_window_mesh_task *p, struct sgx535_window_mesh_binding *b,
                    struct sgx535_background_binding *bg)
{
    static const uint32_t va[8] = {0x20000000, 0x40000000, 0x40001000, 0x41000000,
                                   0x41040000, 0x42000000, 0x44000000, 0x30000000};
    memset(b, 0, sizeof(*b));
    memset(bg, 0, sizeof(*bg));
    b->epoch = 37;
    b->bank_index[0] = 3;
    b->bank_index[1] = 4;
    for (unsigned i = 0; i < 8; i++)
        b->roles[i] = (struct sgx535_task_launch_role){
            101u + i, va[i], p->requirements[i].minimum_bytes, p->requirements[i].access, 0};
    bg->epoch = 37;
    bg->parameter_storage_id = 108;
    bg->fragment_bank_index = 4;
    bg->pds_exec_base = 0x20000000;
    bg->bif_3d_req_base = 0x30000000;
    bg->parameter_va = 0x30000000;
    bg->parameter_bytes = 0x2000000;
    bg->resources[0] = (struct sgx535_background_resource){103, 0x40001000, 4096, 1, 1, {0, 0}};
    bg->resources[1] = (struct sgx535_background_resource){101, 0x20000000, 4096, 2, 1, {0, 0}};
    bg->resources[2] = (struct sgx535_background_resource){109, 0x32000000, 4096, 3, 1, {0, 0}};
}
static void literal_geometry(const struct sgx535_window_mesh_request *r, uint8_t *bytes,
                             uint32_t size)
{
    uint32_t vbytes = r->vertex_count * 32u + 64u;
    static const uint16_t mask[6] = {0, 1, 2, 3, 2, 1};
    memset(bytes, 0, size);
    for (unsigned i = 0; i < r->vertex_count; i++)
        for (unsigned j = 0; j < 4; j++) {
            put(bytes + i * 32u + j * 4u, r->vertices[i].window_xyz_reciprocal_w[j]);
            put(bytes + i * 32u + 16u + j * 4u, r->vertices[i].rgba[j]);
        }
    for (unsigned i = 0; i < 4; i++) {
        uint32_t words[4] = {i % 2 ? f32(r->width) : 0, i / 2 ? f32(r->height) : 0, 0x3f800000,
                             0x3f800000};
        for (unsigned j = 0; j < 4; j++)
            put(bytes + r->vertex_count * 32u + i * 16u + j * 4u, words[j]);
    }
    for (unsigned i = 0; i < 6u + r->index_count; i++) {
        uint32_t v = i < 6 ? mask[i] : r->indices[i - 6];
        bytes[vbytes + i * 2u] = (uint8_t)v;
        bytes[vbytes + i * 2u + 1] = (uint8_t)(v >> 8);
    }
}
static uint8_t *image_bytes(struct sgx535_window_mesh_task *p, unsigned role)
{
    return role == 0 ? p->pds : role == 1 ? p->vertex_use : p->fragment_use;
}
static void literal_images(struct sgx535_window_mesh_task *expected,
                           const struct sgx535_window_mesh_request *r,
                           const struct sgx535_window_mesh_binding *b)
{
    uint8_t seed[2048];
    struct sgx535_normalized_color0_tile16_request first = {0};
    uint32_t vbytes = r->vertex_count * 32u + 64u, stride = (r->width * 4u + 127u) & ~127u;
    for (unsigned i = 0; i < 3; i++)
        memcpy(first.rgba[i], r->vertices[i].rgba, 16);
    mask_literal(seed, &first, 0);
    memset(expected->pds, 0, sizeof(expected->pds));
    memset(expected->vertex_use, 0, sizeof(expected->vertex_use));
    memset(expected->fragment_use, 0, sizeof(expected->fragment_use));
    for (unsigned i = 2; i < 16; i++)
        memcpy(image_bytes(expected, mask_sections[i].role) + mask_sections[i].resource_byte_offset,
               seed + mask_sections[i].byte_offset, mask_sections[i].byte_count);
    put(expected->pds + 212, ((r->width + 15u) / 16u) << 16 | ((r->height + 15u) / 16u));
    put(expected->pds + 672, 0x8000u | (((stride / 8u) - 1u) << 16));
    put(expected->pds + 684, (r->height - 1u) << 12 | (r->width - 1u));
    put(expected->pds + 724, 0x81400000u | r->index_count);
    struct sgx535_task_packet_reloc relocs[40];
    memcpy(relocs, mask_relocs, sizeof(relocs));
    relocs[0].span_bytes = r->vertex_count * 32u;
    relocs[4].resource_byte_offset = r->vertex_count * 32u;
    relocs[32].span_bytes = stride * r->height;
    relocs[34].resource_byte_offset = vbytes;
    relocs[37].resource_byte_offset = vbytes + 12u;
    relocs[37].span_bytes = r->index_count * 2u;
    if (b)
        for (unsigned i = 0; i < 40; i++) {
            const struct sgx535_task_packet_reloc *a = &relocs[i];
            uint64_t address = b->roles[a->role].gpu_va_bytes + a->resource_byte_offset;
            uint32_t value;
            if (a->kind == 1)
                value = (uint32_t)((address >> a->right_shift) << a->left_shift);
            else {
                unsigned dm = a->role - 1u;
                uint32_t units = (uint32_t)(address & 0x7ffffu) / 16u;
                value = b->bank_index[dm] | (units / 2048u) * 16u | (units % 2048u) * 256u;
            }
            uint8_t *dst = image_bytes(expected, mask_sections[a->section].role) +
                           mask_sections[a->section].resource_byte_offset + a->section_byte_offset;
            put(dst, (get(dst) & ~a->mask) | (value & a->mask));
        }
}
/* Event programs have independent structural/behavioral checks; the remaining
 * bytes retain the complete independent image/relocation oracle. */
static int mesh_program_equal(const uint8_t *pds, const uint8_t *fragment_use,
                              const struct sgx535_window_mesh_task *expected)
{
    const uint32_t pds_offset = 416, use_offset = 160;
    return sgx535_pixel_event_reference_check(pds + pds_offset, 148, fragment_use + use_offset, 32,
                                              1, get(expected->pds + pds_offset + 8),
                                              get(expected->pds + pds_offset),
                                              get(expected->pds + pds_offset + 16) | 0x00200000u) &&
           !memcmp(pds, expected->pds, pds_offset) &&
           !memcmp(pds + pds_offset + 148, expected->pds + pds_offset + 148,
                   sizeof(expected->pds) - pds_offset - 148) &&
           !memcmp(fragment_use, expected->fragment_use, use_offset) &&
           !memcmp(fragment_use + use_offset + 32, expected->fragment_use + use_offset + 32,
                   sizeof(expected->fragment_use) - use_offset - 32);
}
static void oracle(const struct sgx535_window_mesh_request *r,
                   const struct sgx535_window_mesh_task *p, const uint8_t *geometry,
                   const struct sgx535_window_mesh_bound *bound, uint8_t *expected,
                   struct sgx535_window_mesh_task *literal)
{
    uint32_t vertices = r->vertex_count, indices = r->index_count;
    uint32_t vbytes = vertices * 32u + 64u, ibytes = (12u + indices * 2u + 3u) & ~3u,
             size = vbytes + ibytes;
    CHECK(p->version == 14 && p->profile == 13 && p->width == r->width && p->height == r->height);
    CHECK(p->vertex_count == vertices && p->index_count == indices &&
          p->window.sample_mask == 0xffffffffu);
    CHECK(p->geometry.foreground_vertex_bytes == vertices * 32u &&
          p->geometry.mask_vertex_offset_bytes == vertices * 32u);
    CHECK(p->geometry.vertex_bytes == vbytes && p->geometry.index_offset_bytes == vbytes);
    CHECK(p->geometry.foreground_index_offset_bytes == vbytes + 12 &&
          p->geometry.foreground_index_bytes == indices * 2u);
    CHECK(p->geometry.index_bytes == ibytes && p->geometry.bytes == size &&
          p->geometry.allocation_bytes == ((size + 4095u) / 4096u) * 4096u);
    literal_geometry(r, expected, size);
    CHECK(!memcmp(geometry, expected, size));
    CHECK(p->section_count == 16 && p->relocation_count == 40 && p->ta_stream_section == 15 &&
          p->event_pds_section == 11);
    CHECK(p->fragment_code_bytes == 144 && p->fragment_temp_words == 7 &&
          p->dms_temp_allocation_bytes == 32);
    CHECK(p->event_use_offset_bytes == 160 && p->fragment_use_extent_bytes == 192);
    CHECK(p->missing_mesh_contracts == 31 && p->missing_packet_contracts == 0x1ff &&
          p->fragment.missing_contracts == 0x3ff);
    CHECK(!p->fragment.arithmetic_qualified && !p->fragment.numerical_qualified &&
          !p->fragment.linkage_qualified);
    CHECK(!p->fragment.output_qualified && !p->fragment.hardware_qualified &&
          !p->fragment.nonfinite_behavior_qualified);
    CHECK(sgx535_fragment_normalized_native_admission(&p->fragment) == SGX535_NIR_ERR_UNSUPPORTED);
    literal_images(literal, r, NULL);
    CHECK(mesh_program_equal(p->pds, p->fragment_use, literal));
    CHECK(!memcmp(p->vertex_use, literal->vertex_use, sizeof(p->vertex_use)));
    struct sgx535_task_packet_reloc relocs[40];
    memcpy(relocs, mask_relocs, sizeof(relocs));
    relocs[0].span_bytes = vertices * 32u;
    relocs[4].resource_byte_offset = vertices * 32u;
    relocs[32].span_bytes = p->stride_bytes * r->height;
    relocs[34].resource_byte_offset = vbytes;
    relocs[37].resource_byte_offset = vbytes + 12u;
    relocs[37].span_bytes = indices * 2u;
    CHECK(!memcmp(p->relocations, relocs, sizeof(relocs)));
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_window_mesh_section *s = &p->sections[i];
        CHECK(s->kind == mask_sections[i].kind && s->role == mask_sections[i].role &&
              s->alignment == mask_sections[i].alignment);
        CHECK(s->byte_offset == s->resource_byte_offset);
        CHECK(s->resource_byte_offset + s->byte_count <= p->requirements[s->role].minimum_bytes);
        if (i < 2)
            CHECK(s->source == SGX535_WINDOW_MESH_GEOMETRY);
        else
            CHECK(s->source == (s->role == 0   ? SGX535_WINDOW_MESH_PDS
                                : s->role == 1 ? SGX535_WINDOW_MESH_VERTEX_USE
                                               : SGX535_WINDOW_MESH_FRAGMENT_USE));
    }
    if (bound) {
        CHECK(bound->resolved_relocations == 40 && bound->missing_launch_contracts == 0x75);
        CHECK(bound->stages[0].count == 3 && bound->stages[1].count == 7 &&
              bound->stages[2].count == 26);
        CHECK(bound->ta_stream_va_bytes == 0x200002b0 && bound->event_pds_va_bytes == 0x200001a0);
        literal_images(literal, r, &bound->binding);
        CHECK(mesh_program_equal(bound->image.pds, bound->image.fragment_use, literal));
        CHECK(!memcmp(bound->image.vertex_use, literal->vertex_use, sizeof(p->vertex_use)));
    }
}
static uint32_t secondary_dms_reference(uint32_t s, uint32_t temp, uint32_t budget)
{
    uint32_t block = ((temp + 3u) / 4u) * 16u;
    uint32_t groups = 192u / block;
    uint32_t pixels = budget / 16u;
    if (groups > 4u)
        groups = 4u;
    if (groups > budget / 16u)
        groups = budget / 16u;
    if (!groups)
        groups = 1u;
    if (pixels > 32u)
        pixels = 32u;
    if (!pixels)
        pixels = 1u;
    if ((pixels < 29u ? pixels + 3u : 32u) > groups * 8u)
        pixels = groups * 8u - 3u;
    return 4u + (groups - 1u) * 65536u + (pixels % 32u) * 0x02000000u +
           ((s + 31u) / 32u) * 0x40000u + 0x8000u;
}
static void ordinary_secondary(struct sgx535_window_mesh_vertex *v, uint32_t *indices,
                               struct sgx535_fragment_instruction *ir,
                               struct sgx535_window_mesh_workspace *w,
                               struct sgx535_window_mesh_background_image *image,
                               struct sgx535_window_mesh_background_bound *bound, uint8_t *geometry,
                               uint8_t *scratch, uint32_t capacity)
{
    static const struct {
        uint32_t words, dma_count, first, second, budget;
    } cases[] = {{2, 1, 0x80000001u, 0, 394},
                 {5, 1, 0x80000004u, 0, 394},
                 {16, 1, 0x8000000fu, 0, 394},
                 {17, 2, 0x01e0000fu, 0x80001000u, 394},
                 {31, 2, 0x01e0000fu, 0x81c0100eu, 394},
                 {32, 1, 0x80e00037u, 0, 394},
                 {33, 1, 0x8140002au, 0, 384},
                 {128, 1, 0x80e000f7u, 0, 341}};
    struct sgx535_window_mesh_gl_window_background_request r = {
        .version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION,
        .packed_background_argb = 0xff123456u,
        .foreground = request(v, indices, ir, 3, 3, 64, 64)};
    struct sgx535_single_sample_source samples = {
        .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 6};
    uint32_t words[128];
    struct sgx535_window_mesh_uniform_source uniforms = {.words = words, .word_count = 128};
    struct sgx535_window_mesh_binding b;
    struct sgx535_background_binding bg;
    struct sgx535_window_mesh_background_image *saved = malloc(sizeof(*saved));
    struct sgx535_window_mesh_background_bound *saved_bound = malloc(sizeof(*saved_bound));
    uint8_t saved_geometry[512];
    CHECK(saved && saved_bound);
    r.foreground.window.half_pixel_center = r.foreground.window.bottom_edge_rule = 1;
    r.foreground.window.viewport_scale_f32[0] = r.foreground.window.viewport_translate_f32[0] =
        r.foreground.window.viewport_translate_f32[1] = 0x42000000u;
    r.foreground.window.viewport_scale_f32[1] = 0xc2000000u;
    r.foreground.fragment_count = 6;
    ir[0] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_INPUT};
    ir[1] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_UNIFORM};
    for (uint32_t i = 0; i < 4; i++)
        ir[i + 2u] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i % 2u, 0, 0}};
    for (uint32_t i = 0; i < 128; i++)
        words[i] = 0x7fc00000u + i;
    words[0] = 0x80000000u;
    words[1] = 0x7f800000u;
    words[2] = 0xff800000u;
    for (uint32_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint32_t s = cases[c].words;
        ir[1].index = s - 2u;
        memset(geometry, 0xa5, capacity);
        CHECK(sgx535_window_mesh_single_sample_background_secondary_build(
                  &r, &samples, &uniforms, image, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_BLOCKED);
        const struct sgx535_window_mesh_task *p = &image->foreground;
        const struct sgx535_window_mesh_secondary_layout *u = &p->secondary;
        CHECK(u->physical_sa_words == s && u->logical_uniform_extent == s - 1u &&
              u->uniform_sa_words == s - 1u && u->reserved_sa_words == 1u && !u->literal_sa_words &&
              !u->sampler_state_sa_words && p->fragment.primary_register_count == 4 &&
              p->fragment.secondary_register_count == s &&
              p->fragment.encoding.words[0] == 0xa0000000u &&
              p->fragment.encoding.words[2] == (0xe0200000u | ((s - 1u) << 7)));
        for (uint32_t i = 0; i < 4; i++)
            CHECK(u->logical_uniform_words_used[i] ==
                  (i == (s - 2u) / 32u ? 1u << ((s - 2u) % 32u) : 0));
        CHECK(u->secondary_resource_offset_bytes == 864u &&
              u->secondary_program_bytes == (cases[c].dma_count == 2 ? 80u : 76u) &&
              u->upload_resource_offset_bytes == 960u && u->upload_bytes == s * 4u &&
              u->dms_sa_budget_words == cases[c].budget &&
              get(p->secondary_upload + 4u) == cases[c].first &&
              get(p->secondary_upload + 36u) == cases[c].second &&
              get(p->secondary_upload + 64u) == 0x07030223u &&
              get(p->secondary_upload + u->secondary_program_bytes - 8u) == 0x07042345u &&
              get(p->secondary_upload + u->secondary_program_bytes - 4u) == 0xaf000000u);
        if (cases[c].dma_count == 2u)
            CHECK(get(p->secondary_upload + 68u) == 0x07601763u);
        CHECK(!get(p->secondary_upload) && !get(p->secondary_upload + 8u) &&
              !get(p->secondary_upload + 12u) && !get(p->secondary_upload + 40u) &&
              !get(p->secondary_upload + 96u));
        for (uint32_t i = 1; i < s; i++)
            CHECK(get(p->secondary_upload + 96u + i * 4u) == words[i - 1u]);
        for (uint32_t i = u->secondary_program_bytes; i < 96u; i++)
            CHECK(!p->secondary_upload[i]);
        for (uint32_t i = 96u + s * 4u; i < sizeof(p->secondary_upload); i++)
            CHECK(!p->secondary_upload[i]);
        uint32_t secondary = 16u, mte = 16u, event = 16u;
        for (uint32_t i = 0; i < p->section_count; i++) {
            if (p->sections[i].kind == SGX535_PACKET_SECONDARY_PDS)
                secondary = i;
            else if (p->sections[i].kind == SGX535_PACKET_MTE_STATE)
                mte = i;
            else if (p->sections[i].kind == SGX535_PACKET_EVENT_USE)
                event = i;
        }
        CHECK(secondary < 16u && mte < 16u && event < 16u);
        CHECK(p->sections[secondary].source == SGX535_WINDOW_MESH_SECONDARY_UPLOAD &&
              !p->sections[secondary].byte_offset &&
              p->sections[secondary].resource_byte_offset == 864u &&
              p->sections[secondary].byte_count == 96u + s * 4u &&
              p->sections[event].byte_count == 40u &&
              u->empty_use_offset_bytes == p->event_use_offset_bytes + 32u &&
              get(p->fragment_use + u->empty_use_offset_bytes) == 0 &&
              get(p->fragment_use + u->empty_use_offset_bytes + 4u) == 0xf8040140u);
        uint32_t dms = secondary_dms_reference(s, p->fragment_temp_words, cases[c].budget);
        CHECK(get(p->pds + p->sections[mte].byte_offset + 8u) == 0x10000000u &&
              get(p->pds + p->sections[mte].byte_offset + 12u) == dms &&
              p->relocation_count == 43u + cases[c].dma_count &&
              geometry[p->geometry.bytes] == 0xa5);
        binding(p, &b, &bg);
        CHECK(sgx535_window_mesh_single_sample_background_secondary_bind(
                  &r, &samples, &uniforms, image, geometry, p->geometry.bytes, &b, &bg, bound, w,
                  scratch, capacity) == SGX535_TASK_BLOCKED);
        const struct sgx535_window_mesh_task *resolved = &bound->foreground.image;
        CHECK(get(resolved->secondary_upload) == 0x200003c0u &&
              get(resolved->pds + p->sections[mte].byte_offset + 8u) == 0x10000036u &&
              get(resolved->secondary_upload + 8u) ==
                  (4u | (((0x1000u + u->empty_use_offset_bytes) & 0x78000u) >> 11u) |
                   (((0x1000u + u->empty_use_offset_bytes) & 0x7ff0u) << 4u)) &&
              !resolved->fragment.hardware_qualified && resolved->missing_mesh_contracts == 31u);
        if (cases[c].dma_count == 2u)
            CHECK(get(resolved->secondary_upload + 32u) == 0x20000400u);
        uint32_t dma_refs = 0, empty_refs = 0;
        for (uint32_t i = 0; i < p->relocation_count; i++) {
            const struct sgx535_task_packet_reloc *rel = &p->relocations[i];
            if (rel->section == secondary) {
                if (rel->role == SGX535_TASK_PDS) {
                    CHECK(rel->resource_byte_offset >= 960u &&
                          rel->resource_byte_offset + rel->span_bytes <= 960u + s * 4u);
                    dma_refs++;
                } else {
                    CHECK(rel->role == SGX535_TASK_USE_FRAGMENT && rel->span_bytes == 8u &&
                          rel->resource_byte_offset == u->empty_use_offset_bytes);
                    empty_refs++;
                }
            }
        }
        CHECK(dma_refs == cases[c].dma_count && empty_refs == 3u);
    }
    *saved = *image;
    *saved_bound = *bound;
    uint32_t bytes = image->foreground.geometry.bytes;
    CHECK(bytes <= sizeof(saved_geometry));
    memcpy(saved_geometry, geometry, bytes);
    CHECK(sgx535_window_mesh_single_sample_background_f32_build(&r, &samples, image, geometry,
                                                                capacity, w, scratch, capacity) ==
          SGX535_TASK_UNSUPPORTED);
    for (uint32_t fault = 0; fault < 8; fault++) {
        struct sgx535_window_mesh_uniform_source bad = uniforms;
        enum sgx535_task_status expected = SGX535_TASK_UNSUPPORTED;
        if (fault == 0)
            bad.word_count = 126;
        else if (fault == 1)
            bad.constant_flags = 0x80u;
        else if (fault == 2)
            bad.reserved[1] = 1;
        else if (fault == 3) {
            bad.word_count = 129;
            expected = SGX535_TASK_INVALID;
        } else if (fault == 4) {
            bad.words = w->uniforms;
            expected = SGX535_TASK_INVALID;
        } else if (fault == 5) {
            ir[1].index = 127;
        } else if (fault == 6) {
            fail_alloc = 1;
            expected = SGX535_TASK_NOMEM;
        } else {
            drift_at = words + 126;
        }
        CHECK(sgx535_window_mesh_single_sample_background_secondary_build(
                  &r, &samples, &bad, image, geometry, capacity, w, scratch, capacity) == expected);
        fail_alloc = 0;
        if (fault == 5)
            ir[1].index = 126;
        if (fault == 7) {
            CHECK(!drift_at);
            words[126] ^= 1u;
        }
        CHECK(!memcmp(image, saved, sizeof(*image)) && !memcmp(geometry, saved_geometry, bytes));
    }
    for (uint32_t i = 0; i < sizeof(image->foreground.secondary_upload); i += 31) {
        image->foreground.secondary_upload[i] ^= 1u;
        CHECK(sgx535_window_mesh_single_sample_background_secondary_bind(
                  &r, &samples, &uniforms, image, geometry, bytes, &b, &bg, bound, w, scratch,
                  capacity) == SGX535_TASK_INVALID);
        image->foreground.secondary_upload[i] ^= 1u;
        CHECK(!memcmp(bound, saved_bound, sizeof(*bound)));
    }
    ir[1] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MOV, .source = {0}};
    struct sgx535_window_mesh_uniform_source empty = {0};
    CHECK(sgx535_window_mesh_single_sample_background_secondary_build(
              &r, &samples, &empty, image, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(sgx535_window_mesh_single_sample_background_f32_build(&r, &samples, saved, geometry,
                                                                capacity, w, scratch,
                                                                capacity) == SGX535_TASK_BLOCKED &&
          !memcmp(image, saved, sizeof(*image)) && !image->foreground.secondary.physical_sa_words &&
          image->foreground.relocation_count == 40u);
    for (uint32_t i = 0; i < 4u; i++)
        ir[i] =
            (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_INPUT, .component = i};
    r.foreground.fragment_count = large_fragment(ir, 1, 192);
    ir[4] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_UNIFORM};
    samples.fragment_instructions = r.foreground.fragment_count;
    CHECK(sgx535_window_mesh_single_sample_background_secondary_build(
              &r, &samples, &uniforms, image, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(image->foreground.fragment_code_bytes > 1536u &&
          image->background.sections[0].resource_offset >=
              image->foreground.secondary.empty_use_offset_bytes + 8u &&
          !(image->background.sections[0].resource_offset & 31u));
    binding(&image->foreground, &b, &bg);
    bg.resources[0].allocation_bytes = b.roles[SGX535_TASK_USE_FRAGMENT].allocation_bytes;
    CHECK(sgx535_window_mesh_single_sample_background_secondary_bind(
              &r, &samples, &uniforms, image, geometry, image->foreground.geometry.bytes, &b, &bg,
              bound, w, scratch, capacity) == SGX535_TASK_BLOCKED);
    free(saved_bound);
    free(saved);
    puts(
        "PASS ordinary generic F32 uniform upload: source-owned raw SA0 prefix, sixteen-DWORD PDS, full relocations and combined PA4/SA budget; CPU only");
}
static void no_input_mesh(struct sgx535_window_mesh_vertex *v, uint32_t *indices,
                          struct sgx535_fragment_instruction *ir,
                          struct sgx535_window_mesh_workspace *w, struct sgx535_window_mesh_task *p,
                          struct sgx535_window_mesh_task *old,
                          struct sgx535_window_mesh_bound *bound, uint8_t *geometry,
                          uint8_t *scratch, uint8_t *old_geometry, uint32_t capacity)
{
    static const uint32_t color[4] = {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u};
    struct sgx535_window_mesh_request r = request(v, indices, ir, 3, 3, 16, 16);
    struct sgx535_window_mesh_binding b;
    struct sgx535_background_binding bg;
    r.interpolation = SGX535_WINDOW_MESH_NO_INPUT;
    r.order = SGX535_WINDOW_MESH_DEINDEXED;
    for (unsigned i = 0; i < 3; i++) {
        v[i].window_xyz_reciprocal_w[3] = 0x3f800000u;
        memset(v[i].rgba, 0, sizeof(v[i].rgba));
    }
    for (unsigned i = 0; i < 4; i++) {
        ir[i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                     .immediate = color[i]};
        ir[i + 4] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i, 0, 0}};
    }
    memset(geometry, 0xa5, capacity);
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(!p->fragment.primary_register_count && !p->fragment.secondary_register_count &&
          p->geometry.foreground_vertex_bytes == 48 && p->geometry.mask_vertex_offset_bytes == 48 &&
          p->geometry.index_offset_bytes == 112 && p->geometry.bytes == 132);
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 4; j++)
            CHECK(get(geometry + i * 16u + j * 4u) == v[i].window_xyz_reciprocal_w[j]);
    CHECK(geometry[p->geometry.bytes] == 0xa5);
    for (unsigned i = 0; i < p->section_count; i++) {
        const struct sgx535_window_mesh_section *section = &p->sections[i];
        const uint8_t *bytes = p->pds + section->byte_offset;
        if (section->kind == SGX535_PACKET_FRAGMENT_PDS)
            CHECK(section->byte_count == 56 && get(bytes + 48) == 0x07000345u &&
                  get(bytes + 52) == 0xaf000000u);
        if (section->kind == SGX535_PACKET_MTE_STATE)
            CHECK(get(bytes + 28) == 0x04001000u && get(bytes + 36) == 0 &&
                  get(bytes + 12) == w->pa0_task.dms_word);
        if (section->kind == SGX535_PACKET_VERTEX_PDS) {
            struct sgx535_vertex_pds_program expected;
            CHECK(sgx535_vertex_pds_project(4, 4, &expected) == SGX535_VERTEX_DMA_OK);
            CHECK(get(bytes + 4) == expected.words[1]);
        }
    }
    CHECK(sgx535_window_mesh_validate(&r, p, geometry, p->geometry.bytes, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    binding(p, &b, &bg);
    CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &b, bound, w, scratch,
                                  capacity) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_window_mesh_bound_validate(&r, p, geometry, p->geometry.bytes, &b, bound, w,
                                            scratch, capacity) == SGX535_TASK_BLOCKED);
    *old = *p;
    memcpy(old_geometry, geometry, capacity);
    for (unsigned fault = 0; fault < 4; fault++) {
        if (fault == 0)
            ir[0].opcode = SGX535_FRAGMENT_INPUT;
        if (fault == 1)
            v[0].rgba[0] = 0x3f000000u;
        if (fault == 2)
            v[0].window_xyz_reciprocal_w[3] = 0x3f000000u;
        if (fault == 3)
            ir[0].immediate = 0x7fc12345u;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
        ir[0] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                     .immediate = color[0]};
        v[0].rgba[0] = 0;
        v[0].window_xyz_reciprocal_w[3] = 0x3f800000u;
    }
}
static void save(const char *dir, const char *name, const void *data, size_t bytes)
{
    char p[4096];
    int n = snprintf(p, sizeof(p), "%s/%s", dir, name);
    CHECK(n > 0 && (size_t)n < sizeof(p));
    FILE *f = fopen(p, "wbx");
    CHECK(f);
    CHECK(fwrite(data, 1, bytes, f) == bytes);
    CHECK(!fclose(f));
}
int main(int argc, char **argv)
{
    uv2_layout();
    const unsigned capacity = 160000;
    (void)mask_binding;
    struct sgx535_window_mesh_vertex *v = calloc(4096, sizeof(*v));
    uint32_t *indices = calloc(12288, sizeof(*indices));
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_window_mesh_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_window_mesh_task *p = calloc(1, sizeof(*p)), *old = calloc(1, sizeof(*p)),
                                   *literal = calloc(1, sizeof(*p));
    struct sgx535_window_mesh_bound *bound = calloc(1, sizeof(*bound)),
                                    *old_bound = calloc(1, sizeof(*bound));
    struct sgx535_window_mesh_background_image *bp = calloc(1, sizeof(*bp));
    struct sgx535_window_mesh_background_bound *bo = calloc(1, sizeof(*bo));
    uint8_t *geometry = calloc(1, capacity), *scratch = calloc(1, capacity),
            *expected = calloc(1, capacity), *old_geometry = calloc(1, capacity);
    struct sgx535_window_mesh_request r, bad;
    struct sgx535_window_mesh_binding b, bad_b;
    struct sgx535_background_binding bg, bad_bg;
    CHECK(argc == 1 || argc == 2);
    CHECK(v && indices && ir && w && p && old && literal && bound && old_bound && bp && bo &&
          geometry && scratch && expected && old_geometry);
    fragment_layout();
    no_input_mesh(v, indices, ir, w, p, old, bound, geometry, scratch, old_geometry, capacity);
    ordinary_secondary(v, indices, ir, w, bp, bo, geometry, scratch, capacity);
    static const struct {
        unsigned vertices, indices, width, height;
    } cases[] = {{3, 3, 16, 16},         {6, 6, 64, 64},          {4096, 12288, 1280, 720},
                 {3, 12288, 1280, 1280}, {4096, 3, 2047, 2047},   {4095, 4095, 63, 17},
                 {3, 3, 1, 1},           {4096, 12288, 1280, 800}};
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        r = request(v, indices, ir, cases[c].vertices, cases[c].indices, cases[c].width,
                    cases[c].height);
        if (c == 5)
            r.order = SGX535_WINDOW_MESH_DEINDEXED;
        memset(geometry, 0xa5, capacity);
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_BLOCKED);
        CHECK(sgx535_window_mesh_validate(&r, p, geometry, p->geometry.bytes, w, scratch,
                                          capacity) == SGX535_TASK_BLOCKED);
        CHECK(geometry[p->geometry.bytes] == 0xa5 && geometry[capacity - 1] == 0xa5);
        binding(p, &b, &bg);
        CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &b, bound, w, scratch,
                                      capacity) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_window_mesh_bound_validate(&r, p, geometry, p->geometry.bytes, &b, bound, w,
                                                scratch, capacity) == SGX535_TASK_BLOCKED);
        oracle(&r, p, geometry, bound, expected, literal);
        struct sgx535_window_mesh_background_request br = {
            .version = 5, .packed_background_argb = 0xff1d2743, .foreground = r};
        CHECK(sgx535_window_mesh_background_build(&br, bp, geometry, capacity, w, scratch,
                                                  capacity) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_window_mesh_background_validate(&br, bp, geometry, p->geometry.bytes, w,
                                                     scratch, capacity) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_window_mesh_background_bind(&br, bp, geometry, p->geometry.bytes, &b, &bg, bo,
                                                 w, scratch, capacity) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_window_mesh_background_bound_validate(&br, bp, geometry, p->geometry.bytes, &b,
                                                           &bg, bo, w, scratch,
                                                           capacity) == SGX535_TASK_BLOCKED);
        CHECK(bp->version == 5 && bp->background.version == 3 &&
              bo->background.resolved_relocations == 5);
        uint8_t bg_literal[192];
        mask_background_literal(bg_literal, 0xff1d2743, 1);
        put(bg_literal + 164, ((r.width + 0x400u) << 20) | 0x4000u);
        put(bg_literal + 172, 0x40000000u | ((r.height + 0x400u) << 4));
        CHECK(!memcmp(bg_literal, bo->background.image.bytes, 192));
        CHECK(get(bo->background.image.bytes + 32) == 0x16004 &&
              get(bo->background.image.bytes + 64) == 0x20);
        CHECK(get(bo->background.image.bytes + 164) == ((r.width + 0x400u) << 20 | 0x4000u));
        CHECK(get(bo->background.image.bytes + 172) == (0x40000000u | ((r.height + 0x400u) << 4)));
        CHECK(bo->missing_background_contracts == 31 &&
              bo->foreground.image.missing_mesh_contracts == 31);
        if (c == 2) {
            CHECK(p->geometry.bytes == 155724 && p->geometry.allocation_bytes == 159744);
            CHECK(p->scene.clear_page_count == 16 && p->scene.allocation_bytes == 111904);
        }
        if (c == 3)
            CHECK(p->scene.clear_page_count == 16 && p->scene.allocation_bytes == 142624);
        if (c == 7) {
            static const uint32_t scene[16] = {
                0, 1120,   0x0a028028, 0x0701c01c, 1120, 0x3104f, 0x31f4ff, 65536,
                0, 119296, 119376,     119520,     0,    0,       0,        0};
            CHECK(!memcmp(p->scene.words, scene, sizeof(scene)));
            CHECK(p->scene.clear_page_start == 0 && p->scene.clear_page_count == 16 &&
                  p->scene.allocation_bytes == 119584);
            CHECK(p->requirements[4].minimum_bytes == 122880 &&
                  p->requirements[4].minimum_bytes / 4096u == 30);
            CHECK(p->stride_bytes == 5120 && p->requirements[5].minimum_bytes == 4096000);
            CHECK(get(p->pds + 672) == 0x027f8000 && get(p->pds + 684) == 0x31f4ff);
            /* The PDS word keeps the 80x50 tile extent; RENDBOX2 is 0, as EMGD. */
            CHECK(get(p->pds + 212) == 0x00500032 && bound->stages[2].actions[3].offset == 0x410 &&
                  bound->stages[2].actions[3].value == 0);
        }
        if (argc == 2 && (c == 0 || c == 2 || c == 7)) {
            char name[80];
            int n = snprintf(name, sizeof(name), "case%u.geometry.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, geometry, p->geometry.bytes);
            n = snprintf(name, sizeof(name), "case%u.pds.bound.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, bo->foreground.image.pds, sizeof(p->pds));
            n = snprintf(name, sizeof(name), "case%u.vertex-use.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, p->vertex_use, sizeof(p->vertex_use));
            n = snprintf(name, sizeof(name), "case%u.fragment-use.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, p->fragment_use, sizeof(p->fragment_use));
            n = snprintf(name, sizeof(name), "case%u.bg.bound.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, bo->background.image.bytes, 192);
            n = snprintf(name, sizeof(name), "case%u.task.bin", c);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, p, sizeof(*p));
        }
    }
    /* Ordered U32->U16 projection must retain reversed/repeated/degenerate
     * index records exactly; no sorting, deindexing or winding inference. */
    r = request(v, indices, ir, 3, 6, 16, 16);
    indices[0] = 2;
    indices[1] = 1;
    indices[2] = 0;
    indices[3] = indices[4] = indices[5] = 0;
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(get(p->pds + 724) == 0x81400006);
    for (unsigned i = 0; i < 6; i++)
        CHECK(((unsigned)geometry[p->geometry.foreground_index_offset_bytes + 2u * i] |
               (unsigned)geometry[p->geometry.foreground_index_offset_bytes + 2u * i + 1u] << 8) ==
              indices[i]);
    /* Program capacity is independent of mesh size and the old serialized2KiB
     * task. A shader beyond the old976B composition still gets a whole mesh. */
    r = request(v, indices, ir, 4096, 12288, 1280, 800);
    ir[4] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                 .immediate = 0x3f000000};
    for (unsigned i = 0; i < 120; i++)
        ir[5u + i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAD,
                                                          .source = {i ? 4u + i : 0u, 4, 1}};
    for (unsigned i = 0; i < 4; i++)
        ir[125u + i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i ? i : 124u, 0, 0}};
    r.fragment_count = 129;
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(p->fragment_code_bytes > 976 && p->fragment_use_extent_bytes <= 1536 &&
          p->geometry.bytes == 155724);
    CHECK(sgx535_window_mesh_validate(&r, p, geometry, p->geometry.bytes, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    binding(p, &b, &bg);
    CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &b, bound, w, scratch,
                                  capacity) == SGX535_TASK_BLOCKED);
    struct sgx535_window_mesh_background_request pressure = {
        .version = 5, .packed_background_argb = 0xff1d2743, .foreground = r};
    CHECK(sgx535_window_mesh_background_build(&pressure, bp, geometry, capacity, w, scratch,
                                              capacity) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_window_mesh_background_bind(&pressure, bp, geometry, p->geometry.bytes, &b, &bg,
                                             bo, w, scratch, capacity) == SGX535_TASK_BLOCKED);
    printf("external_pressure_code_bytes=%u TEMP=%u geometry=%u\n", p->fragment_code_bytes,
           p->fragment_temp_words, p->geometry.bytes);
    *old = *p;
    memcpy(old_geometry, geometry, capacity);
    for (unsigned i = 0; i < 200; i++)
        ir[5u + i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAD,
                                                          .source = {i ? 4u + i : 0u, 4, 1}};
    for (unsigned i = 0; i < 4; i++)
        ir[205u + i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i ? i : 204u, 0, 0}};
    r.fragment_count = 209;
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(p->fragment_code_bytes > 1536 && p->fragment_use_extent_bytes > 1536);
    for (unsigned components = 1; components <= 2; components++) {
        r = request(v, indices, ir, 3, 3, 16, 16);
        r.fragment_count = large_fragment(ir, components, components == 1 ? 240u : 244u);
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_BLOCKED);
        CHECK(p->fragment_code_bytes > 1536 &&
              p->event_use_offset_bytes == ((p->fragment_code_bytes + 31u) / 32u) * 32u &&
              p->fragment_use_extent_bytes == p->event_use_offset_bytes + 32u);
        for (unsigned i = 0; i < p->fragment.encoding.word_count; i++)
            CHECK(get(p->fragment_use + i * 4u) == p->fragment.encoding.words[i]);
        for (unsigned i = p->fragment_code_bytes; i < p->event_use_offset_bytes; i++)
            CHECK(!p->fragment_use[i]);
        CHECK(sgx535_pixel_event_reference_check(
            p->pds + 416, 148, p->fragment_use + p->event_use_offset_bytes, 32, 1,
            get(p->pds + 424), get(p->pds + 416), get(p->pds + 432) | 0x00200000u));
        CHECK(sgx535_window_mesh_validate(&r, p, geometry, p->geometry.bytes, w, scratch,
                                          capacity) == SGX535_TASK_BLOCKED);
        pressure.foreground = r;
        CHECK(sgx535_window_mesh_background_build(&pressure, bp, geometry, capacity, w, scratch,
                                                  capacity) == SGX535_TASK_BLOCKED);
        uint32_t background = p->fragment_use_extent_bytes;
        CHECK(bp->background.sections[0].resource_offset == background);
        for (unsigned i = 0; i < 3; i++)
            CHECK(bp->background.relocations[i].resource_offset == background);
        CHECK(bp->foreground.requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes ==
              ((background + 32u + 8u + 4095u) / 4096u) * 4096u);
        printf(
            "fragment_storage components=%u words=%u code=%u event=%u background=%u clear=%u owner=%u\n",
            components, p->fragment.encoding.word_count, p->fragment_code_bytes,
            p->event_use_offset_bytes, background, background + 32u,
            bp->foreground.requirements[SGX535_TASK_USE_FRAGMENT].minimum_bytes);
    }
    r.fragment_count = large_fragment(ir, 1, 493);
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    CHECK(p->fragment.encoding.word_count == SGX535_NIR_MAX_WORDS &&
          p->fragment_code_bytes == 4096 && p->event_use_offset_bytes == 4096 &&
          p->fragment_use_extent_bytes == SGX535_WINDOW_MESH_FRAGMENT_STORAGE_BYTES);
    *old = *p;
    memcpy(old_geometry, geometry, capacity);
    r.fragment_count = large_fragment(ir, 1, 494);
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    r = request(v, indices, ir, 3, 3, 16, 16);
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    binding(p, &b, &bg);
    CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &b, bound, w, scratch,
                                  capacity) == SGX535_TASK_BLOCKED);
    *old = *p;
    *old_bound = *bound;
    memcpy(old_geometry, geometry, capacity);
    for (unsigned c = 0; c < 23; c++) {
        bad = r;
        if (c == 0)
            bad.version = 13;
        if (c == 1)
            bad.profile = 12;
        if (c == 2)
            bad.width = 0;
        if (c == 3)
            bad.height = 2048;
        if (c == 4)
            bad.vertex_count = 4097;
        if (c == 5)
            bad.index_count = 12291;
        if (c == 6)
            bad.index_count = 4;
        if (c == 7)
            bad.order = 2, bad.vertex_count = 4;
        if (c == 8)
            bad.window.origin = 2;
        if (c == 9)
            bad.window.half_pixel_center = 1;
        if (c == 10)
            bad.window.bottom_edge_rule = 1;
        if (c == 11)
            bad.window.sample_mask = 0xfffffffeu;
        if (c == 12)
            bad.window.min_samples = 2;
        if (c == 13)
            bad.window.viewport_scale_f32[2] = 0x7f800000;
        if (c == 14)
            bad.window.reserved = 1;
        if (c == 15)
            bad.reserved[2] = 1;
        if (c == 16)
            bad.fragment_count = 4;
        if (c == 17)
            bad.indices = NULL;
        if (c == 18)
            bad.vertices = NULL;
        if (c == 19)
            bad.vertex_count = 0xffffffffu;
        if (c == 20)
            bad.index_count = 0xffffffffu;
        if (c == 21)
            bad.width = 0xffffffffu;
        if (c == 22)
            bad.height = 0xffffffffu;
        CHECK(sgx535_window_mesh_build(&bad, p, geometry, capacity, w, scratch, capacity) < 0);
        CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    }
    for (unsigned c = 0; c < 9; c++) {
        uint32_t *field = c < 4   ? &v[1].window_xyz_reciprocal_w[c]
                          : c < 8 ? &v[1].rgba[c - 4]
                                  : &indices[2],
                 saved = *field;
        *field = c == 0   ? 0x41800001u
                 : c == 1 ? 0xbf800000u
                 : c == 2 ? 0x3f800001u
                 : c == 3 ? 0
                 : c == 8 ? 3
                          : 0x80000000u;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
        *field = saved;
    }
    for (unsigned c = 0; c < 4; c++) {
        uint32_t *field = c == 0   ? &v[0].window_xyz_reciprocal_w[0]
                          : c == 1 ? &indices[0]
                          : c == 2 ? &ir[0].component
                                   : &r.window.sample_mask;
        uint32_t saved = *field;
        drift_at = field;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
        *field = saved;
    }
    {
        static const uint32_t good[] = {1u,          0x007fffffu, 0x00800000u, 0x3eaaaaabu,
                                        0x3f000000u, 0x40000000u, 0x7f7fffffu};
        static const uint32_t badw[] = {0,           0x80000000u, 0xbf800000u,
                                        0x7f800000u, 0x7fc12345u, 0x7f800001u};
        uint32_t saved = v[1].window_xyz_reciprocal_w[3];
        for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            v[1].window_xyz_reciprocal_w[3] = good[i];
            CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
                  SGX535_TASK_BLOCKED);
            CHECK(get(geometry + 44) == good[i]);
        }
        for (unsigned i = 0; i < sizeof(badw) / sizeof(badw[0]); i++) {
            v[1].window_xyz_reciprocal_w[3] = badw[i];
            CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
                  SGX535_TASK_UNSUPPORTED);
        }
        v[1].window_xyz_reciprocal_w[3] = saved;
        for (unsigned i = 0; i < 3; i++) {
            uint32_t coordinate = v[0].window_xyz_reciprocal_w[i];
            v[0].window_xyz_reciprocal_w[i] = 0x80000000u;
            CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
                  SGX535_TASK_BLOCKED);
            CHECK(get(geometry + i * 4u) == 0x80000000u);
            v[0].window_xyz_reciprocal_w[i] = coordinate;
        }
        r.interpolation = SGX535_VARYING_FLAT_REPLICATED;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_BLOCKED);
        v[1].rgba[0] = 0;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_UNSUPPORTED);
        v[1].rgba[0] = 0x3f800000u;
        r.interpolation = SGX535_VARYING_SMOOTH;
        CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
              SGX535_TASK_BLOCKED);
        *old = *p;
        memcpy(old_geometry, geometry, capacity);
    }
    fail_alloc = 1;
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_NOMEM);
    fail_alloc = 0;
    CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    CHECK(sgx535_window_mesh_build(&r, p, geometry, p->geometry.bytes - 1, w, scratch, capacity) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, geometry, capacity) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_window_mesh_build(&r, p, (uint8_t *)v, capacity, w, scratch, capacity) ==
          SGX535_TASK_INVALID);
    for (unsigned i = 0; i < 16; i++) {
        p->sections[i].byte_count++;
        CHECK(sgx535_window_mesh_validate(&r, p, geometry, old->geometry.bytes, w, scratch,
                                          capacity) == SGX535_TASK_INVALID);
        CHECK(sgx535_window_mesh_bind(&r, p, geometry, old->geometry.bytes, &b, bound, w, scratch,
                                      capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
        *p = *old;
    }
    for (unsigned i = 0; i < 40; i++) {
        p->relocations[i].resource_byte_offset ^= 16;
        CHECK(sgx535_window_mesh_bind(&r, p, geometry, old->geometry.bytes, &b, bound, w, scratch,
                                      capacity) == SGX535_TASK_INVALID);
        *p = *old;
    }
    for (unsigned c = 0; c < 17; c++) {
        bad_b = b;
        if (c < 8)
            bad_b.roles[c].storage_id = b.roles[(c + 1u) % 8u].storage_id;
        if (c == 8)
            bad_b.roles[3].allocation_bytes = 0;
        if (c == 9)
            bad_b.roles[4].gpu_va_bytes = b.roles[3].gpu_va_bytes;
        if (c == 10)
            bad_b.bank_index[1] = 3;
        if (c == 11)
            bad_b.epoch = 0;
        if (c == 12)
            bad_b.roles[3].gpu_va_bytes = UINT64_C(0xfffffffffffff000);
        if (c == 13)
            bad_b.roles[3].allocation_bytes = UINT64_C(0xfffffffffffff000);
        if (c == 14) /* Past 4 GiB. */
            bad_b.roles[3].gpu_va_bytes = 0xfffff000, bad_b.roles[3].allocation_bytes = 8192;
        if (c == 15)
            bad_b.roles[3].access = 3;
        if (c == 16)
            bad_b.roles[3].reserved = 1;
        CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &bad_b, bound, w, scratch,
                                      capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
    }
    drift_at = &b.bank_index[0];
    CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &b, bound, w, scratch,
                                  capacity) == SGX535_TASK_INVALID);
    CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
    b = old_bound->binding;
    for (unsigned a = 0; a < 8; a++)
        for (unsigned other = 0; other < a; other++) {
            bad_b = b;
            bad_b.roles[a].gpu_va_bytes = b.roles[other].gpu_va_bytes;
            CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &bad_b, bound, w,
                                          scratch, capacity) == SGX535_TASK_INVALID);
            CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
        }
    const void *aliases[] = {&r, v, indices, ir, geometry, scratch, w};
    for (unsigned a = 0; a < sizeof(aliases) / sizeof(aliases[0]); a++) {
        CHECK(sgx535_window_mesh_build(&r, (struct sgx535_window_mesh_task *)aliases[a], geometry,
                                       capacity, w, scratch, capacity) == SGX535_TASK_INVALID);
        if (a != 5)
            CHECK(sgx535_window_mesh_build(&r, p, geometry, capacity, w, (uint8_t *)aliases[a],
                                           capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    }
    /* Literal bank/address oracle also exercises both address subfields at
     * non-default PDS/USE locations without relocating canonical owners in place. */
    bad_b = b;
    bad_b.roles[0].gpu_va_bytes = 0x20180000;
    bad_b.roles[1].gpu_va_bytes = 0x40010000;
    bad_b.roles[2].gpu_va_bytes = 0x40018000;
    bad_b.bank_index[0] = 7;
    bad_b.bank_index[1] = 8;
    CHECK(sgx535_window_mesh_bind(&r, p, geometry, p->geometry.bytes, &bad_b, bound, w, scratch,
                                  capacity) == SGX535_TASK_BLOCKED);
    literal_images(literal, &r, &bad_b);
    CHECK(mesh_program_equal(bound->image.pds, bound->image.fragment_use, literal));
    CHECK(get(bound->image.pds + 128) == 0x00080038u);
    *bound = *old_bound;
    for (size_t i = 0; i < sizeof(*bound); i += 1237) {
        ((uint8_t *)bound)[i] ^= 1;
        CHECK(sgx535_window_mesh_bound_validate(&r, p, geometry, p->geometry.bytes, &b, bound, w,
                                                scratch, capacity) == SGX535_TASK_INVALID);
        *bound = *old_bound;
    }
    geometry[100] ^= 1;
    CHECK(sgx535_window_mesh_validate(&r, p, geometry, p->geometry.bytes, w, scratch, capacity) ==
          SGX535_TASK_INVALID);
    geometry[100] ^= 1;
    struct sgx535_window_mesh_background_request br = {
        .version = 5, .packed_background_argb = 0xff1d2743, .foreground = r};
    CHECK(sgx535_window_mesh_background_build(&br, bp, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_BLOCKED);
    for (unsigned c = 0; c < 4; c++) {
        bad_bg = bg;
        if (c == 0)
            bad_bg.epoch++;
        if (c == 1)
            bad_bg.resources[2].storage_id = b.roles[4].storage_id;
        if (c == 2)
            bad_bg.resources[2].gpu_va_bytes = 0x30000000;
        if (c == 3)
            bad_bg.resources[0].gpu_va_bytes += 4096;
        CHECK(sgx535_window_mesh_background_bind(&br, bp, geometry, p->geometry.bytes, &b, &bad_bg,
                                                 bo, w, scratch, capacity) == SGX535_TASK_INVALID);
    }
    /* A complete source span can start before the outer request and end in
     * its4..16B prefix without touching foreground. Refuse before compilation,
     * not merely later when the malformed image/binding fails comparison. */
    {
        size_t prefix = (sizeof(*bp) + 63u) & ~(size_t)63u;
        uint8_t *arena = calloc(1, prefix + sizeof(br) + 64u);
        struct sgx535_window_mesh_background_request *parent;
        unsigned calls[3];
        struct sgx535_window_mesh_background_bound *saved = calloc(1, sizeof(*saved));
        CHECK(arena && saved);
        parent = (struct sgx535_window_mesh_background_request *)(arena + prefix);
        *parent = br;
        *saved = *bo;
        for (unsigned which = 0; which < 3; which++) {
            const struct sgx535_window_mesh_background_image *alias_image =
                which == 0
                    ? (const struct sgx535_window_mesh_background_image *)((uint8_t *)parent + 4u -
                                                                           sizeof(*bp))
                    : bp;
            const struct sgx535_window_mesh_binding *alias_binding =
                which == 1 ? (const struct sgx535_window_mesh_binding *)((uint8_t *)parent + 8u -
                                                                         sizeof(b))
                           : &b;
            const struct sgx535_background_binding *alias_background =
                which == 2 ? (const struct sgx535_background_binding *)((uint8_t *)parent + 16u -
                                                                        sizeof(bg))
                           : &bg;
            alloc_calls = 0;
            CHECK(sgx535_window_mesh_background_bind(
                      parent, alias_image, geometry, p->geometry.bytes, alias_binding,
                      alias_background, bo, w, scratch, capacity) == SGX535_TASK_INVALID);
            calls[which] = alloc_calls;
            CHECK(!memcmp(bo, saved, sizeof(*bo)));
        }
        printf("outer_prefix_allocations=%u,%u,%u\n", calls[0], calls[1], calls[2]);
        fflush(stdout);
        CHECK(!calls[0] && !calls[1] && !calls[2]);
        free(arena);
        free(saved);
    }
    /* A changed source during nested foreground compilation must not replace
     * the outer background snapshot or publish partial image/geometry. */
    struct sgx535_window_mesh_background_image *saved_background = malloc(sizeof(*bp));
    CHECK(saved_background);
    *saved_background = *bp;
    memcpy(old_geometry, geometry, capacity);
    uint32_t background_color = br.packed_background_argb;
    drift_at = &br.packed_background_argb;
    CHECK(sgx535_window_mesh_background_build(&br, bp, geometry, capacity, w, scratch, capacity) ==
          SGX535_TASK_UNSUPPORTED);
    CHECK(!drift_at && !memcmp(bp, saved_background, sizeof(*bp)) &&
          !memcmp(geometry, old_geometry, capacity));
    br.packed_background_argb = background_color;
    free(saved_background);
    /* Construction snapshots are scratch, not admissible caller inputs. */
    memset(&w->bind_binding, 0xa5, sizeof(w->bind_binding));
    memset(&w->bind_background, 0xa5, sizeof(w->bind_background));
    memset(&w->bind_request, 0xa5, sizeof(w->bind_request));
    memset(&w->background_request, 0xa5, sizeof(w->background_request));
    memset(&w->foreground_request, 0xa5, sizeof(w->foreground_request));
    CHECK(sgx535_window_mesh_background_bind(&br, bp, geometry, p->geometry.bytes, &b, &bg, bo, w,
                                             scratch, capacity) == SGX535_TASK_BLOCKED);
    w->foreground_request = r;
    CHECK(sgx535_window_mesh_build(&w->foreground_request, p, geometry, capacity, w, scratch,
                                   capacity) == SGX535_TASK_INVALID);
    w->background_request = br;
    CHECK(sgx535_window_mesh_background_bind(&w->background_request, bp, geometry,
                                             p->geometry.bytes, &b, &bg, bo, w, scratch,
                                             capacity) == SGX535_TASK_INVALID);
    w->bind_binding = b;
    CHECK(sgx535_window_mesh_background_bind(&br, bp, geometry, p->geometry.bytes, &w->bind_binding,
                                             &bg, bo, w, scratch, capacity) == SGX535_TASK_INVALID);
    w->bind_background = bg;
    CHECK(sgx535_window_mesh_background_bind(&br, bp, geometry, p->geometry.bytes, &b,
                                             &w->bind_background, bo, w, scratch,
                                             capacity) == SGX535_TASK_INVALID);
    /* The once-per-scene mask is not a fourth attribute tuple or a per-draw
     * reset. Indices select4 mask vertices at a distinct DMA base, then one
     * ordinary U16 list whose full count is literal without count bias. */
    CHECK(get(p->pds + 696) == 0x81400006 && get(p->pds + 724) == 0x81400003);
    CHECK(get(p->pds + 712) == 0x02000103 && get(p->pds + 740) == 0x04000203);
    printf(
        "window_mesh_checks=%u external4096_12288=PASS old_packet_not_reinterpreted=TRUE native_qualified=FALSE\n",
        checks);
    free(v);
    free(indices);
    free(ir);
    free(w);
    free(p);
    free(old);
    free(literal);
    free(bound);
    free(old_bound);
    free(bp);
    free(bo);
    free(geometry);
    free(scratch);
    free(expected);
    free(old_geometry);
    return 0;
}
