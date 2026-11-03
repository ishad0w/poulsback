// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_depth_window_mesh.h"
#define main legacy_window_mesh_main
#define __wrap_calloc legacy_wrap_calloc
#include "test_sgx535_window_mesh.c"
#undef __wrap_calloc
#undef main

static unsigned late_alloc_serial;
static uint32_t *late_word;
void *__wrap_calloc(size_t n, size_t bytes);
void *__wrap_calloc(size_t n, size_t bytes)
{
    if (late_word && alloc_calls + 1u == late_alloc_serial) {
        *late_word ^= 1u;
        late_word = NULL;
    }
    return legacy_wrap_calloc(n, bytes);
}

static struct sgx535_depth_window_mesh_request
depth_request(struct sgx535_window_mesh_vertex *v, uint32_t *ix,
              struct sgx535_fragment_instruction *ir, uint32_t vertices, uint32_t count,
              uint32_t width, uint32_t height, uint32_t format)
{
    struct sgx535_depth_window_mesh_request r = {0};
    r.version = 1;
    r.format = format;
    r.scene.version = 5;
    r.scene.packed_background_argb = 0xff1d2743u;
    r.scene.foreground = request(v, ix, ir, vertices, count, width, height);
    return r;
}
static void depth_binding(const struct sgx535_depth_window_mesh_task *p,
                          struct sgx535_depth_window_mesh_binding *b)
{
    memset(b, 0, sizeof(*b));
    binding(&p->seed.foreground, &b->scene, &b->background);
    b->depth = (struct sgx535_depth_window_mesh_owner){
        110, 0x50000000, (p->depth.allocation_bytes + 4095u) & ~UINT64_C(4095), 3, 0};
}
static void literal_sections(const struct sgx535_depth_window_mesh_request *r,
                             struct sgx535_window_mesh_section s[20])
{
    uint32_t vb = r->scene.foreground.vertex_count * 32u + 64u;
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_task_packet_section *a = &mask_sections[i];
        s[i] = (struct sgx535_window_mesh_section){a->kind,
                                                   a->role == 0   ? 1u
                                                   : a->role == 1 ? 2u
                                                   : a->role == 2 ? 3u
                                                                  : 0u,
                                                   a->resource_byte_offset,
                                                   a->byte_count,
                                                   a->role,
                                                   a->resource_byte_offset,
                                                   a->alignment};
    }
    s[0].byte_count = vb;
    s[1].byte_offset = s[1].resource_byte_offset = vb;
    s[1].byte_count = (12u + r->scene.foreground.index_count * 2u + 3u) & ~3u;
    s[15] = (struct sgx535_window_mesh_section){13, 1, 1088, 96, 0, 1088, 16};
    s[16] = (struct sgx535_window_mesh_section){4, 4, 0, 8, 2, 1568, 32};
    s[17] = (struct sgx535_window_mesh_section){8, 1, 896, 56, 0, 896, 32};
    s[18] = (struct sgx535_window_mesh_section){9, 1, 960, 60, 0, 960, 32};
    s[19] = (struct sgx535_window_mesh_section){11, 1, 1024, 44, 0, 1024, 16};
}
static void literal_relocs(const struct sgx535_depth_window_mesh_request *r,
                           struct sgx535_task_packet_reloc a[52])
{
    uint32_t v = r->scene.foreground.vertex_count * 32u, vb = v + 64u;
    uint32_t stride = (r->scene.foreground.width * 4u + 127u) & ~127u;
    memcpy(a, mask_relocs, sizeof(mask_relocs));
    a[0].span_bytes = v;
    a[4].resource_byte_offset = v;
    a[32].span_bytes = stride * r->scene.foreground.height;
    a[34].resource_byte_offset = vb;
    a[37].resource_byte_offset = vb + 12u;
    a[37].span_bytes = r->scene.foreground.index_count * 2u;
    for (unsigned i = 36; i < 40; i++)
        a[i].section_byte_offset += 28;
#define A(s, o, k, t, n, l, m, x) {1, s, o, k, t, n, l, m, x, 0}
#define U(s, o, k, t, n)                                                                           \
    {2, s, o, k, t, n, 16, 15, 0, 0}, {3, s, o, k, t, n, 16, 240, 15, 4},                          \
        {3, s, o, k, t, n, 16, 0x7ff00, 4, 8}
    const struct sgx535_task_packet_reloc extra[12] = {U(17, 0, 2, 1568, 8),
                                                       A(18, 0, 0, 1024, 44, 16, 0xffffffffu, 0),
                                                       U(18, 8, 1, 32, 16),
                                                       A(19, 12, 0, 192, 4, 32, 0xffffff, 4),
                                                       A(19, 20, 0, 896, 56, 32, 0xffffff, 4),
                                                       A(15, 28, 0, 960, 60, 32, 0xfffffff, 4),
                                                       A(15, 40, 3, vb, 12, 2, 0xffffffffu, 0),
                                                       A(15, 48, 0, 64, 64, 32, 0xfffffff, 4)};
#undef U
#undef A
    memcpy(a + 40, extra, sizeof(extra));
}
static void literal_pds(const struct sgx535_depth_window_mesh_request *r,
                        struct sgx535_window_mesh_task *base, uint8_t pds[1184],
                        const struct sgx535_depth_window_mesh_binding *b)
{
    struct sgx535_window_mesh_section sections[20];
    struct sgx535_task_packet_reloc relocs[52];
    literal_images(base, &r->scene.foreground, NULL);
    memset(pds, 0, 1184);
    for (unsigned i = 0; i < 15; i++)
        if (mask_sections[i].role == 0)
            memcpy(pds + mask_sections[i].resource_byte_offset,
                   base->pds + mask_sections[i].resource_byte_offset, mask_sections[i].byte_count);
    put(pds + 580, 0x00400000);
    const uint32_t fp[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x07000345, 0xaf000000};
    const uint32_t state[15] = {0, 0x8140000a, 0x00200000, 0, 0,          0,          0,         0,
                                0, 0,          0,          0, 0x07030223, 0x07042345, 0xaf000000};
    const uint32_t mte[11] = {0x54c5, 0x01e00100, 0x0e000000, 0,       0x30000, 0x0c000000,
                              0,      0,          0x04001000, 0x10000, 0};
    const uint32_t stream[24] = {0x40000000,
                                 0x0c02a203,
                                 0x81400006,
                                 0,
                                 0,
                                 0,
                                 0x02000103,
                                 0x40000000,
                                 0x0c02a203,
                                 0x81400006,
                                 0,
                                 0,
                                 0,
                                 0x02000103,
                                 0x40000000,
                                 0x0c02a203,
                                 0x81400000u | r->scene.foreground.index_count,
                                 0,
                                 0,
                                 0,
                                 0x04000203,
                                 0x60000000,
                                 0x0c02a201,
                                 0xc0000000};
    for (unsigned i = 0; i < 14; i++)
        put(pds + 896 + i * 4u, fp[i]);
    for (unsigned i = 0; i < 15; i++)
        put(pds + 960 + i * 4u, state[i]);
    for (unsigned i = 0; i < 11; i++)
        put(pds + 1024 + i * 4u, mte[i]);
    for (unsigned i = 0; i < 24; i++)
        put(pds + 1088 + i * 4u, stream[i]);
    literal_sections(r, sections);
    literal_relocs(r, relocs);
    if (b)
        for (unsigned i = 0; i < 52; i++) {
            const struct sgx535_task_packet_reloc *a = &relocs[i];
            uint64_t address = b->scene.roles[a->role].gpu_va_bytes + a->resource_byte_offset;
            uint32_t value;
            if (a->kind == 1)
                value = (uint32_t)((address >> a->right_shift) << a->left_shift);
            else {
                uint32_t units = (uint32_t)(address & 0x7ffffu) / 16u;
                value = b->scene.bank_index[a->role - 1u] | (units / 2048u) * 16u |
                        (units % 2048u) * 256u;
            }
            CHECK(sections[a->section].source == 1);
            uint8_t *dst = pds + sections[a->section].byte_offset + a->section_byte_offset;
            put(dst, (get(dst) & ~a->mask) | (value & a->mask));
        }
}
static int depth_pds_equal(const uint8_t actual[1184], const uint8_t expected[1184],
                           const uint8_t *fragment_use)
{
    const uint32_t offset = 416;
    return sgx535_pixel_event_reference_check(actual + offset, 148, fragment_use + 160, 32, 1,
                                              get(expected + offset + 8), get(expected + offset),
                                              get(expected + offset + 16) | 0x00200000u) &&
           !memcmp(actual, expected, offset) &&
           !memcmp(actual + offset + 148, expected + offset + 148, 1184u - offset - 148u);
}
static void complete_oracle(const struct sgx535_depth_window_mesh_request *r,
                            const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry,
                            struct sgx535_window_mesh_task *literal, uint8_t *expected,
                            const struct sgx535_depth_window_mesh_bound *b)
{
    uint8_t pds[1184];
    struct sgx535_window_mesh_section sections[20];
    struct sgx535_task_packet_reloc relocs[52];
    uint32_t width = r->scene.foreground.width, height = r->scene.foreground.height;
    uint32_t pitch = (width + 31u) & ~31u, ah = (height + 31u) & ~31u,
             cpp = r->format == 1 ? 2u : 4u;
    uint32_t bytes = p->seed.foreground.geometry.bytes;
    literal_geometry(&r->scene.foreground, expected, bytes);
    CHECK(!memcmp(expected, geometry, bytes));
    CHECK(p->version == 1 && p->section_count == 20 && p->relocation_count == 52 &&
          p->ta_stream_section == 15 && p->event_pds_section == 11);
    CHECK(p->depth.width == width && p->depth.height == height && p->depth.pitch_pixels == pitch);
    CHECK(p->depth.allocation_height == ah && p->depth.stride_bytes == pitch * cpp &&
          p->depth.allocation_bytes == (uint64_t)pitch * ah * cpp);
    CHECK(p->depth.zls_control ==
          (((pitch / 16u - 1u) << 8) | (r->format == 1 ? 0xa0000u : 0x50000u) | 0x4000d0u));
    CHECK(p->depth.missing_contracts == 0x7f && p->missing_depth_contracts == 0x7f &&
          p->depth.isp_set_bits == 0x400000);
    CHECK(p->seed.foreground.missing_mesh_contracts == 31 &&
          p->seed.foreground.missing_packet_contracts == 0x1ff);
    CHECK(p->seed.foreground.fragment.missing_contracts == 0x3ff);
    literal_sections(r, sections);
    CHECK(!memcmp(sections, p->sections, sizeof(sections)));
    literal_relocs(r, relocs);
    CHECK(!memcmp(relocs, p->relocations, sizeof(relocs)));
    literal_pds(r, literal, pds, NULL);
    CHECK(depth_pds_equal(p->pds, pds, p->seed.foreground.fragment_use));
    CHECK(mesh_program_equal(p->seed.foreground.pds, p->seed.foreground.fragment_use, literal));
    CHECK(!memcmp(literal->vertex_use, p->seed.foreground.vertex_use, sizeof(literal->vertex_use)));
    CHECK(get(p->clear_use) == 0xff00 && get(p->clear_use + 4) == 0xfca7f181);
    /* Mask, clear, ordinary and termination select their own complete state
     * records. Ordinary CPRES0 consumes its requested state after clear. */
    CHECK(get(p->pds + 576) == 0x54c1 && get(p->pds + 580) == 0x400000);
    CHECK(get(p->pds + 624) == 0x54c5 && get(p->pds + 632) == 0x02000000);
    CHECK(get(p->pds + 1024) == 0x54c5 && get(p->pds + 1032) == 0x0e000000);
    CHECK(get(p->pds + 1096) == 0x81400006 && get(p->pds + 1124) == 0x81400006);
    CHECK(get(p->pds + 1152) == (0x81400000u | r->scene.foreground.index_count) &&
          get(p->pds + 1180) == 0xc0000000);
    CHECK(sgx535_depth_window_mesh_native_admission(b) == SGX535_TASK_UNSUPPORTED);
    if (b) {
        literal_pds(r, literal, pds, &b->binding);
        CHECK(depth_pds_equal(b->image.pds, pds, b->image.seed.foreground.fragment_use));
        CHECK(b->resolved_relocations == 52 && b->missing_launch_contracts == 0x75 &&
              b->missing_depth_contracts == 0x7f);
        CHECK(!b->hardware_qualified && !b->layout_qualified && !b->visibility_qualified &&
              !b->retirement_qualified);
        CHECK(b->ta_stream_va_bytes == 0x20000440 && b->event_pds_va_bytes == 0x200001a0);
        CHECK(b->reset_actions == 2 && b->required_isp_reset[0].offset == 0x80 &&
              b->required_isp_reset[0].value == 0x20);
        CHECK(b->required_isp_reset[1].offset == 0x80 && !b->required_isp_reset[1].value);
        CHECK(b->depth_base_words[0] == 0x50000000);
        for (unsigned i = 1; i < 5; i++)
            CHECK(!b->depth_base_words[i]);
        unsigned zls = 0, bases = 0, ta = 0, background = 0, background_depth = 0;
        for (unsigned i = 0; i < 3; i++)
            for (unsigned j = 0; j < b->stages[i].count; j++) {
                const struct sgx535_engine121_action *a = &b->stages[i].actions[j];
                CHECK(a->offset != 0x80);
                if (i == 1 && a->offset == 0x238) {
                    CHECK(a->value == 0x20000440);
                    ta++;
                }
                if (i != 2)
                    continue;
                if (a->offset == 0x480) {
                    CHECK(a->value == p->depth.zls_control);
                    zls++;
                }
                /* EMGD's background: stencil byte zero, the draw mask set and
                 * the background tag (0x300). Check the final
                 * stores, not only the bound background descriptor (0x200). */
                if (a->offset == 0x4bc) {
                    CHECK(a->kind == SGX535_ENGINE121_WRITE && a->value == 0x300 && !a->mask);
                    background++;
                }
                if (a->offset == 0x4b8) {
                    CHECK(a->kind == SGX535_ENGINE121_WRITE && a->value == 0x3f800000 && !a->mask);
                    background_depth++;
                }
                for (unsigned k = 0; k < 5; k++)
                    if (a->offset == p->depth.address_pairs[k].register_offset) {
                        CHECK(a->value == b->depth_base_words[k]);
                        bases++;
                    }
            }
        CHECK(zls == 1 && bases == 5 && ta == 1 && background == 1 && background_depth == 1);
        uint8_t bg[192];
        mask_background_literal(bg, r->scene.packed_background_argb, 1);
        put(bg + 164, ((width + 0x400u) << 20) | 0x4000u);
        put(bg + 172, 0x40000000u | ((height + 0x400u) << 4));
        CHECK(!memcmp(bg, b->background.image.bytes, sizeof(bg)));
    }
}

#ifndef POULSBACK_DEPTH_WINDOW_MESH_MAIN
#define POULSBACK_DEPTH_WINDOW_MESH_MAIN main
#endif
int POULSBACK_DEPTH_WINDOW_MESH_MAIN(int argc, char **argv)
{
    const unsigned capacity = 160000, scratch_capacity = 320000;
    CHECK(argc == 1 || argc == 2);
    CHECK(legacy_window_mesh_main(1, argv) == 0);
    const unsigned legacy_checks = checks;
    checks = 0;
    struct sgx535_window_mesh_vertex *v = calloc(4096, sizeof(*v));
    uint32_t *ix = calloc(12288, sizeof(*ix));
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_depth_window_mesh_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_depth_window_mesh_task *p = calloc(1, sizeof(*p)), *old = calloc(1, sizeof(*p));
    struct sgx535_depth_window_mesh_bound *b = calloc(1, sizeof(*b)),
                                          *saved = calloc(1, sizeof(*b));
    struct sgx535_window_mesh_task *literal = calloc(1, sizeof(*literal));
    uint8_t *geometry = calloc(1, capacity), *scratch = calloc(1, scratch_capacity),
            *expected = calloc(1, capacity), *old_geometry = calloc(1, capacity);
    CHECK(v && ix && ir && w && p && old && b && saved && literal && geometry && scratch &&
          expected && old_geometry);
    struct sgx535_depth_window_mesh_request r, bad;
    struct sgx535_depth_window_mesh_binding bind, bb;
    static const struct {
        unsigned v, i, w, h;
    } cases[] = {{3, 3, 16, 16},       {6, 6, 16, 16},           {4096, 12288, 16, 16},
                 {4095, 4095, 64, 64}, {4096, 12288, 1280, 800}, {3, 3, 2047, 2047}};
    for (unsigned format = 1; format <= 2; format++)
        for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            r = depth_request(v, ix, ir, cases[c].v, cases[c].i, cases[c].w, cases[c].h, format);
            if (c == 3)
                r.scene.foreground.order = SGX535_WINDOW_MESH_DEINDEXED;
            if (c == 2 || c == 4)
                for (unsigned i = 0; i < r.scene.foreground.index_count; i++)
                    ix[i] = (i & 1u) ? 4095u : ((12287u - i) % 4096u);
            memset(geometry, 0xa5, capacity);
            CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch,
                                                 scratch_capacity) == SGX535_TASK_BLOCKED);
            uint32_t bytes = p->seed.foreground.geometry.bytes;
            for (unsigned i = bytes; i < capacity; i++)
                CHECK(geometry[i] == 0xa5);
            complete_oracle(&r, p, geometry, literal, expected, NULL);
            CHECK(sgx535_depth_window_mesh_validate(&r, p, geometry, bytes, w, scratch,
                                                    scratch_capacity) == SGX535_TASK_BLOCKED);
            depth_binding(p, &bind);
            memset(b, 0x5a, sizeof(*b));
            *saved = *b;
            enum sgx535_task_status status = sgx535_depth_window_mesh_bind(
                &r, p, geometry, bytes, &bind, b, w, scratch, scratch_capacity);
            CHECK(status == SGX535_TASK_BLOCKED);
            complete_oracle(&r, p, geometry, literal, expected, b);
            CHECK(sgx535_depth_window_mesh_bound_validate(&r, p, geometry, bytes, &bind, b, w,
                                                          scratch,
                                                          scratch_capacity) == SGX535_TASK_BLOCKED);
        }
    /* Export genuine constructor inputs and complete packets for a separate
     * exact-rational prospective front/back oracle. No depth bytes or CPU
     * shader execution are synthesized by the C fixture. */
    r = depth_request(v, ix, ir, 6, 6, 16, 16, 1);
    for (unsigned i = 0; i < 6; i++) {
        v[i].window_xyz_reciprocal_w[2] = i < 3 ? 0x3e800000u : 0x3f400000u;
        v[i].window_xyz_reciprocal_w[3] = 0x3f800000u;
        v[i].rgba[0] = i < 3 ? 0x3f800000u : 0;
        v[i].rgba[1] = i < 3 ? 0 : 0x3f800000u;
        v[i].rgba[2] = 0;
        v[i].rgba[3] = 0x3f800000u;
    }
    for (unsigned order = 0; order < 2; order++) {
        for (unsigned i = 0; i < 6; i++)
            ix[i] = order ? (i + 3u) % 6u : i;
        CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch,
                                             scratch_capacity) == SGX535_TASK_BLOCKED);
        depth_binding(p, &bind);
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, p->seed.foreground.geometry.bytes,
                                            &bind, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_BLOCKED);
        complete_oracle(&r, p, geometry, literal, expected, b);
        if (argc == 2) {
            char name[80];
            int n;
            n = snprintf(name, sizeof(name), "order%u.vertices.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, v, 6 * sizeof(*v));
            n = snprintf(name, sizeof(name), "order%u.indices.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, ix, 6 * sizeof(*ix));
            n = snprintf(name, sizeof(name), "order%u.geometry.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, geometry, p->seed.foreground.geometry.bytes);
            n = snprintf(name, sizeof(name), "order%u.task.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, p, sizeof(*p));
            n = snprintf(name, sizeof(name), "order%u.bound.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, b, sizeof(*b));
            n = snprintf(name, sizeof(name), "order%u.pds.original.bin", order);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, b->image.pds, sizeof(b->image.pds));
        }
    }
    r = depth_request(v, ix, ir, 3, 3, 16, 16, 1);
    CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch, scratch_capacity) ==
          SGX535_TASK_BLOCKED);
    depth_binding(p, &bind);
    uint32_t bytes = p->seed.foreground.geometry.bytes;
    unsigned before_bind_allocations = alloc_calls;
    CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bind, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_BLOCKED);
    unsigned bind_allocations = alloc_calls - before_bind_allocations;
    CHECK(bind_allocations >= 2);
    *old = *p;
    *saved = *b;
    memcpy(old_geometry, geometry, capacity);
    CHECK(sgx535_depth_window_mesh_bind(&r, NULL, geometry, bytes, &bind, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, NULL, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_bound_validate(&r, NULL, geometry, bytes, &bind, b, w, scratch,
                                                  scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_bound_validate(&r, p, geometry, bytes, NULL, b, w, scratch,
                                                  scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(!memcmp(b, saved, sizeof(*b)));
    for (unsigned i = 0; i < 52; i++) {
        *p = *old;
        p->relocations[i].span_bytes ^= 1u;
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bind, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(b, saved, sizeof(*b)));
    }
    *p = *old;
    for (unsigned i = 0; i < 1184; i++) {
        if (i % 17u)
            continue;
        p->pds[i] ^= 1u;
        CHECK(sgx535_depth_window_mesh_validate(&r, p, geometry, bytes, w, scratch,
                                                scratch_capacity) == SGX535_TASK_INVALID);
        p->pds[i] ^= 1u;
    }
    for (unsigned i = 0; i < 9; i++) {
        bb = bind;
        bb.depth.storage_id =
            i < 8 ? bb.scene.roles[i].storage_id : bb.background.resources[2].storage_id;
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bb, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(b, saved, sizeof(*b)));
    }
    /* Unaligned, across a 256 MiB window, over USE code, in the parameter
     * window, past 4 GiB. */
    const uint64_t bad_va[] = {0x50000800, 0x60000000 - 4096, 0x40001000, 0x32000000, 0x100000000};
    for (unsigned i = 0; i < sizeof(bad_va) / sizeof(bad_va[0]); i++) {
        bb = bind;
        bb.depth.gpu_va_bytes = bad_va[i];
        bb.depth.allocation_bytes += 4096;
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bb, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(b, saved, sizeof(*b)));
    }
    for (unsigned i = 0; i < 5; i++) {
        bb = bind;
        if (i == 0)
            bb.depth.access = 1;
        if (i == 1)
            bb.depth.allocation_bytes = 2048;
        if (i == 2)
            bb.depth.allocation_bytes = 0; /* Smaller than required; larger is fine. */
        if (i == 3)
            bb.depth.reserved = 1;
        if (i == 4)
            bb.scene.epoch++;
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bb, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(b, saved, sizeof(*b)));
    }
    bad = r;
    bad.format = 3;
    CHECK(sgx535_depth_window_mesh_build(&bad, p, geometry, capacity, w, scratch,
                                         scratch_capacity) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, bytes - 1, w, scratch,
                                         scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch, bytes * 2u - 1u) ==
          SGX535_TASK_INVALID);
    CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    fail_alloc = 1;
    CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch, scratch_capacity) ==
          SGX535_TASK_NOMEM);
    fail_alloc = 0;
    CHECK(!memcmp(p, old, sizeof(*p)) && !memcmp(geometry, old_geometry, capacity));
    uint32_t *drifts[] = {&r.format, &v[0].rgba[0], &ix[0], &ir[0].component};
    for (unsigned i = 0; i < 4; i++) {
        uint32_t value = *drifts[i];
        drift_at = drifts[i];
        CHECK(sgx535_depth_window_mesh_build(&r, p, geometry, capacity, w, scratch,
                                             scratch_capacity) == SGX535_TASK_UNSUPPORTED);
        CHECK(!drift_at && !memcmp(p, old, sizeof(*p)) &&
              !memcmp(geometry, old_geometry, capacity));
        *drifts[i] = value;
    }
    drift_at = &bind.depth.access;
    CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bind, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_UNSUPPORTED);
    CHECK(!drift_at && !memcmp(b, saved, sizeof(*b)));
    bind.depth.access = 3;
    /* A caller-owned image or binding can drift during the second full source
     * regeneration. Checking only before that sleeping call is insufficient. */
    uint32_t *late_inputs[] = {&p->depth.zls_control, &bind.depth.access, &r.format,
                               &r.scene.packed_background_argb};
    for (unsigned i = 0; i < sizeof(late_inputs) / sizeof(late_inputs[0]); i++) {
        uint32_t value = *late_inputs[i];
        late_word = late_inputs[i];
        late_alloc_serial = alloc_calls + bind_allocations;
        CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bind, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_UNSUPPORTED);
        CHECK(!late_word && !memcmp(b, saved, sizeof(*b)));
        *late_inputs[i] = value;
    }
    /* The complete parent prefix participates, including fields before the
     * nested mesh request. No dereference of the false input span occurs. */
    size_t arena_bytes = sizeof(*p) + sizeof(bind) + sizeof(r) + 64u;
    uint8_t *arena = malloc(arena_bytes);
    CHECK(arena);
    struct sgx535_depth_window_mesh_request *ar = (void *)(arena + sizeof(*p) + sizeof(bind));
    memcpy(ar, &r, sizeof(r));
    for (unsigned n = 4; n <= 16; n *= 2) {
        const struct sgx535_depth_window_mesh_task *ap =
            (const void *)((uint8_t *)ar - sizeof(*p) + n);
        const struct sgx535_depth_window_mesh_binding *ab =
            (const void *)((uint8_t *)ar - sizeof(bind) + n);
        CHECK(sgx535_depth_window_mesh_bind(ar, ap, geometry, bytes, &bind, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(sgx535_depth_window_mesh_bind(ar, p, geometry, bytes, ab, b, w, scratch,
                                            scratch_capacity) == SGX535_TASK_INVALID);
        CHECK(!memcmp(b, saved, sizeof(*b)));
    }
    free(arena);
    /* Workspace snapshots must be refreshed, and remain forbidden inputs. */
    memset(&w->bind_request, 0xa5, sizeof(w->bind_request));
    memset(&w->regenerate_request, 0xa5, sizeof(w->regenerate_request));
    memset(&w->bind_binding, 0xa5, sizeof(w->bind_binding));
    CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &bind, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(b, saved, sizeof(*b)));
    w->regenerate_request = r;
    CHECK(sgx535_depth_window_mesh_bind(&w->regenerate_request, p, geometry, bytes, &bind, b, w,
                                        scratch, scratch_capacity) == SGX535_TASK_INVALID);
    w->bind_request = r;
    CHECK(sgx535_depth_window_mesh_bind(&w->bind_request, p, geometry, bytes, &bind, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_INVALID);
    w->bind_binding = bind;
    CHECK(sgx535_depth_window_mesh_bind(&r, p, geometry, bytes, &w->bind_binding, b, w, scratch,
                                        scratch_capacity) == SGX535_TASK_INVALID);
    CHECK(!memcmp(b, saved, sizeof(*b)));
    free(v);
    free(ix);
    free(ir);
    free(w);
    free(p);
    free(old);
    free(b);
    free(saved);
    free(literal);
    free(geometry);
    free(scratch);
    free(expected);
    free(old_geometry);
    printf("depth WINDOW mesh: %u checks; legacy mesh: %u checks; CPU only/HOLD gates unchanged\n",
           checks, legacy_checks);
    return 0;
}
