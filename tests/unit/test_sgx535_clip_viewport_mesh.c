// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Runs actual pure constructors. No kernel/source/MMIO/device is modeled. */
int legacy_clip_control_main(int, char **);
#define main legacy_clip_control_main
#include "test_sgx535_window_mesh.c"
#undef main
#include "sgx535_clip_viewport_mesh.h"
#include "sgx535_background_internal.h"
#include "sgx535_window_mesh_gl_window.h"
#include "poulsback_window_mesh_actor_input.generated.h"

static void filled(const void *p, size_t n, unsigned char byte)
{
    const unsigned char *v = p;
    for (size_t i = 0; i < n; i++)
        CHECK(v[i] == byte);
}
static uint8_t *cv_region(struct sgx535_clip_viewport_mesh_task *p, unsigned role)
{
    return role == 0 ? p->pds : role == 1 ? p->vertex_use : p->fragment_use;
}
static void cv_binding_make(const struct sgx535_clip_viewport_mesh_task *p,
                            struct sgx535_window_mesh_binding *b,
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
    bg->bif_3d_req_base = bg->parameter_va = 0x30000000;
    bg->parameter_bytes = 0x2000000;
    bg->resources[0] = (struct sgx535_background_resource){103, 0x40001000, 4096, 1, 1, {0, 0}};
    bg->resources[1] = (struct sgx535_background_resource){101, 0x20000000, 4096, 2, 1, {0, 0}};
    bg->resources[2] = (struct sgx535_background_resource){109, 0x32000000, 4096, 3, 1, {0, 0}};
}
static void word_oracle(const struct sgx535_clip_viewport_mesh_task *p)
{
    static const uint32_t vp[6] = {0x42000000, 0x42000000, 0x42000000,
                                   0xc2000000, 0x3f000000, 0x3f000000};
    static const uint32_t fg[16] = {
        0x55c1,     0x01d00000, 0,          0x3060004,  0x0c000000, 0,          0, 0x42000000,
        0x42000000, 0x42000000, 0xc2000000, 0x3f000000, 0x3f000000, 0x08001800, 0, 0};
    /* DMS scalar is independently derived from actual compiler resource cost. */
    uint32_t words[16];
    memcpy(words, fg, sizeof(words));
    uint32_t alloc = (p->fragment.encoding.temp_register_count * 4u + 15u) & ~15u;
    uint32_t instances = 0xc0u / alloc;
    if (instances > 4)
        instances = 4;
    if (!instances)
        instances = 1;
    uint32_t pixels = 24;
    if (pixels + 3u > instances * 8u)
        pixels = instances * 8u - 3u;
    words[3] = 4u | ((instances - 1u) << 16) | (pixels << 25);
    CHECK(p->version == 15 && p->profile == 14 && sizeof(p->pds) == 1024);
    CHECK(!memcmp(p->viewport_words, vp, sizeof(vp)));
    for (unsigned i = 0; i < 16; i++)
        CHECK(get(p->pds + 576 + i * 4u) == words[i]);
    static const uint32_t mask_state[11] = {0x54c5, 0x07f00100, 0x02000000, 0,          0, 0,
                                            0,      0,          0x04001000, 0x00010000, 0};
    for (unsigned i = 0; i < 11; i++)
        CHECK(get(p->pds + 640 + i * 4u) == mask_state[i]);
    CHECK(get(p->vertex_use + 32) == 0xa0000000 && get(p->vertex_use + 36) == 0x28a1a001);
    CHECK(get(p->vertex_use + 40) == 0xa0200580 && get(p->vertex_use + 44) == 0xfb274000);
    CHECK(get(p->vertex_use + 48) == 0xa0000000 && get(p->vertex_use + 52) == 0x28a1f001);
    CHECK(get(p->vertex_use + 56) == 0xa0200800 && get(p->vertex_use + 60) == 0xfb274000);
    CHECK(get(p->vertex_use + 64) == 0xa0000000 && get(p->vertex_use + 68) == 0x28a11001);
    CHECK(get(p->vertex_use + 72) == 0xa0200100 && get(p->vertex_use + 76) == 0xfb274000);
    /* State DMA units are DWORDs, counts11 and16, exact descriptors from the
     * independent vertex-DMA field reference used by the old packet tests. */
    CHECK(get(p->pds + 228) == 0x8140000au);
    CHECK(get(p->pds + 292) == 0x81e0000fu);
    /* Decode the actual DMA footprint, emitted USE count and primary bytes
     * independently.134db0 uses low8 in16-byte units; PDS data is separate. */
    static const uint32_t allocation_bytes[3] = {48, 64, 16};
    static const uint32_t state_words[3] = {11, 16, 2};
    for (unsigned i = 0; i < 3; i++) {
        uint32_t control = get(p->pds + 708 + i * 28);
        uint32_t dma = get(p->pds + 228 + i * 64);
        uint32_t dma_words = ((dma & 15u) + 1u) * (((dma >> 4) & 15u) + 1u);
        CHECK(dma_words == state_words[i] && ((control & 255u) << 4) == allocation_bytes[i] &&
              allocation_bytes[i] >= dma_words * 4u);
        CHECK(((control >> 26) & 63u) * 16u == 48 &&
              ((get(p->vertex_use + 40 + i * 16) >> 7) & 127u) == dma_words);
    }
    CHECK(get(p->pds + 708) == 0x0c02a203 && get(p->pds + 736) == 0x0c02a204 &&
          get(p->pds + 764) == 0x0c02a201);
    CHECK(p->section_count == 16 && p->relocation_count == 40);
    static const uint32_t offsets[16] = {0,   256, 0,   0,   32,  0,   0,   128,
                                         192, 224, 352, 416, 576, 208, 688, 704};
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_window_mesh_section *s = &p->sections[i];
        CHECK(s->resource_byte_offset == offsets[i] || i == 5);
        if (s->role == 0)
            CHECK((uint64_t)s->resource_byte_offset + s->byte_count <= 1024);
        CHECK(s->role < 8 && (uint64_t)s->resource_byte_offset + s->byte_count <=
                                 p->requirements[s->role].minimum_bytes);
        for (unsigned j = 0; j < i; j++)
            if (s->role == p->sections[j].role)
                CHECK(s->resource_byte_offset >=
                          p->sections[j].resource_byte_offset + p->sections[j].byte_count ||
                      p->sections[j].resource_byte_offset >=
                          s->resource_byte_offset + s->byte_count);
    }
    CHECK(p->sections[12].byte_count == 108 && p->sections[15].byte_count == 68);
    CHECK(p->requirements[0].minimum_bytes == 4096 && p->requirements[3].minimum_bytes == 4096 &&
          p->requirements[4].minimum_bytes == 8192 && p->requirements[5].minimum_bytes == 16384);
    struct sgx535_task_packet_reloc expected[40];
    memcpy(expected, mask_relocs, sizeof(expected));
    expected[0].span_bytes = 192;
    expected[4].resource_byte_offset = 192;
    expected[11].resource_byte_offset = 640;
    expected[15].span_bytes = 64;
    expected[25].resource_byte_offset = 688;
    expected[32].span_bytes = 16384;
    expected[34].resource_byte_offset = 256;
    expected[37].resource_byte_offset = 268;
    expected[37].span_bytes = 12;
    for (unsigned i = 0; i < 40; i++) {
        struct sgx535_task_packet_reloc a = p->relocations[i], e = expected[i];
        /* Compiled fragment and event code extents remain cost-derived. */
        if (e.role == 2) {
            if (p->sections[e.section].kind == SGX535_PACKET_FRAGMENT_PDS)
                e.span_bytes = p->fragment_code_bytes;
            else
                e.resource_byte_offset = p->event_use_offset_bytes + e.resource_byte_offset - 160u;
        }
        CHECK(!memcmp(&a, &e, sizeof(a)));
    }
}
static void relocation_oracle(const struct sgx535_clip_viewport_mesh_task *p,
                              const struct sgx535_clip_viewport_mesh_bound *b)
{
    struct sgx535_clip_viewport_mesh_task *q = malloc(sizeof(*q));
    CHECK(q);
    *q = *p;
    for (unsigned i = 0; i < 40; i++) {
        const struct sgx535_task_packet_reloc *a = &p->relocations[i];
        const struct sgx535_window_mesh_section *s = &p->sections[a->section];
        uint64_t va = b->binding.roles[a->role].gpu_va_bytes + a->resource_byte_offset;
        uint32_t value;
        CHECK(va % a->alignment == 0 && (uint64_t)a->resource_byte_offset + a->span_bytes <=
                                            b->binding.roles[a->role].allocation_bytes);
        if (a->kind == 1)
            value = (uint32_t)((va >> a->right_shift) << a->left_shift);
        else {
            unsigned dm = a->role - 1u;
            uint32_t units = (uint32_t)(va & 0x7ffffu) / 16u;
            value = b->binding.bank_index[dm] | units / 2048u * 16u | units % 2048u * 256u;
        }
        uint8_t *dst = cv_region(q, s->role) + s->resource_byte_offset + a->section_byte_offset;
        put(dst, (get(dst) & ~a->mask) | (value & a->mask));
    }
    CHECK(!memcmp(q, &b->image, sizeof(*q)));
    CHECK(b->resolved_relocations == 40);
    CHECK(b->ta_stream_va_bytes == b->binding.roles[0].gpu_va_bytes + 704 &&
          b->event_pds_va_bytes == b->binding.roles[0].gpu_va_bytes + 416);
    free(q);
}
static void export_projection(const char *dir,
                              const struct sgx535_clip_viewport_mesh_background_image *p,
                              const struct sgx535_clip_viewport_mesh_background_bound *b)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/projection.original.json", dir);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wx");
    CHECK(f);
    CHECK(
        fprintf(
            f,
            "{\"schema\":\"poulsback-clip-viewport-task15-cpu-projection-v1\",\"task\":15,\"profile\":14,\"outer\":7,\"depth_enabled\":false,\"physical_z_viewport_qualified\":false,\"foreground_relocations\":40,\"background_relocations\":5,\"resource\":[") >
        0);
    for (unsigned i = 0; i < 8; i++) {
        const struct sgx535_task_launch_role *r = &b->foreground.binding.roles[i];
        CHECK(fprintf(f, "%s{\"role\":%u,\"va\":%llu,\"bytes\":%llu,\"access\":%u}", i ? "," : "",
                      i, (unsigned long long)r->gpu_va_bytes,
                      (unsigned long long)r->allocation_bytes, r->access) > 0);
    }
    CHECK(fprintf(f, "],\"sections\":[") > 0);
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_window_mesh_section *s = &p->foreground.sections[i];
        CHECK(fprintf(f, "%s[%u,%u,%u,%u,%u]", i ? "," : "", s->kind, s->role,
                      s->resource_byte_offset, s->byte_count, s->alignment) > 0);
    }
    CHECK(fprintf(f, "],\"relocations\":[") > 0);
    for (unsigned i = 0; i < 40; i++) {
        const struct sgx535_task_packet_reloc *r = &p->foreground.relocations[i];
        CHECK(fprintf(f, "%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", i ? "," : "", r->kind, r->section,
                      r->section_byte_offset, r->role, r->resource_byte_offset, r->span_bytes,
                      r->alignment, r->mask, r->right_shift, r->left_shift) > 0);
    }
    CHECK(fprintf(f, "],\"groups\":[") > 0);
    for (unsigned i = 0; i < 3; i++) {
        const struct sgx535_task_launch_stage_plan *s = &b->foreground.stages[i];
        CHECK(fprintf(f, "%s{\"stage\":%u,\"actions\":[", i ? "," : "", i) > 0);
        for (unsigned j = 0; j < s->count; j++) {
            const struct sgx535_engine121_action *a = &s->actions[j];
            CHECK(fprintf(f, "%s[%u,%u,%u,%u]", j ? "," : "", a->kind, a->offset, a->value,
                          a->mask) > 0);
        }
        CHECK(fprintf(f, "]}") > 0);
    }
    CHECK(fprintf(f, "]}\n") > 0);
    CHECK(!fclose(f));
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    struct sgx535_clip_viewport_mesh_vertex v[6];
    struct sgx535_window_mesh_vertex side[6];
    uint32_t indices[6], clip[6][4];
    struct sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    memcpy(side, window_mesh_input_vertices, sizeof(side));
    memcpy(clip, window_mesh_input_clip, sizeof(clip));
    memcpy(indices, window_mesh_input_indices, sizeof(indices));
    memset(ir, 0, sizeof(ir));
    memcpy(ir, window_mesh_input_program, 224);
    for (unsigned i = 0; i < 6; i++) {
        memcpy(v[i].clip_xyzw, clip[i], 16);
        memcpy(v[i].rgba, side[i].rgba, 16);
    }
    struct sgx535_clip_viewport_mesh_request r = {.version = 15,
                                                  .profile = 14,
                                                  .core_id = 0x01130000,
                                                  .revision = 0x00010201,
                                                  .width = 64,
                                                  .height = 64,
                                                  .interpolation = 1,
                                                  .vertices = v,
                                                  .window_sidecar = side,
                                                  .indices = indices,
                                                  .vertex_count = 6,
                                                  .index_count = 6,
                                                  .fragment = ir,
                                                  .fragment_count = 8};
    memcpy(&r.conventions, window_mesh_input_window_conventions, 48);
    struct sgx535_clip_viewport_mesh_background_request bgreq = {
        .version = 7, .packed_background_argb = 0xff000000, .foreground = r};
    struct sgx535_clip_viewport_mesh_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_clip_viewport_mesh_task *p = calloc(1, sizeof(*p)),
                                          *badp = calloc(1, sizeof(*badp));
    struct sgx535_clip_viewport_mesh_bound *bound = calloc(1, sizeof(*bound));
    struct sgx535_clip_viewport_mesh_background_image *image = calloc(1, sizeof(*image)),
                                                      *badimage = calloc(1, sizeof(*badimage));
    struct sgx535_clip_viewport_mesh_background_bound *full = calloc(1, sizeof(*full)),
                                                      *badfull = calloc(1, sizeof(*badfull));
    struct sgx535_window_mesh_binding b;
    struct sgx535_background_binding bg;
    uint8_t g[4096], scratch[4096], other[4096];
    CHECK(w && p && badp && bound && image && badimage && full && badfull);
    memset(g, 0xa5, sizeof(g));
    CHECK(sgx535_clip_viewport_mesh_build(&r, p, g, sizeof(g), w, scratch, sizeof(scratch)) ==
          SGX535_TASK_BLOCKED);
    CHECK(sgx535_clip_viewport_mesh_validate(&r, p, g, 280, w, scratch, sizeof(scratch)) ==
          SGX535_TASK_BLOCKED);
    word_oracle(p);
    CHECK(g[280] == 0xa5 && !memcmp(g, v, 192));
    for (unsigned i = 0; i < 6; i++) {
        CHECK(get(g + i * 32) == clip[i][0] && get(g + i * 32 + 4) == clip[i][1]);
    }
    CHECK(memcmp(g, side, 192));
    cv_binding_make(p, &b, &bg);
    CHECK(sgx535_clip_viewport_mesh_bind(&r, p, g, 280, &b, bound, w, scratch, sizeof(scratch)) ==
          SGX535_TASK_BLOCKED);
    CHECK(sgx535_clip_viewport_mesh_bound_validate(&r, p, g, 280, &b, bound, w, scratch,
                                                   sizeof(scratch)) == SGX535_TASK_BLOCKED);
    relocation_oracle(p, bound);
    CHECK(sgx535_clip_viewport_mesh_background_build(&bgreq, image, g, sizeof(g), w, scratch,
                                                     sizeof(scratch)) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_clip_viewport_mesh_background_validate(&bgreq, image, g, 280, w, scratch,
                                                        sizeof(scratch)) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_clip_viewport_mesh_background_bind(&bgreq, image, g, 280, &b, &bg, full, w,
                                                    scratch,
                                                    sizeof(scratch)) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_clip_viewport_mesh_background_bound_validate(&bgreq, image, g, 280, &b, &bg, full,
                                                              w, scratch, sizeof(scratch)) ==
          SGX535_TASK_BLOCKED);
    CHECK(image->version == 7 && image->background.version == 4 &&
          full->background.resolved_relocations == 5);
    CHECK(image->background.sections[1].resource_offset == 1024 &&
          image->background.sections[2].resource_offset == 1088);
    CHECK(image->background.relocations[3].resource_offset == 1088 &&
          image->background.relocations[4].resource_offset == 1024);
    CHECK(get(full->background.image.bytes + 112) == 0x44u);
    CHECK(get(full->background.image.bytes + 120) == 0x0c000040u);
    CHECK(get(full->background.image.bytes + 32) == 0x16004u);
    CHECK(get(image->foreground.pds + 580) == 0x01d00000u);
    for (unsigned i = 0; i < full->foreground.stages[2].count; i++) {
        const struct sgx535_engine121_action *a = &full->foreground.stages[2].actions[i];
        if (a->offset == 0x480)
            CHECK(a->value == 0xf0000);
        if (a->offset == 0xcb0 || a->offset == 0x484 || a->offset == 0x488 || a->offset == 0x48c ||
            a->offset == 0x490)
            CHECK(!a->value);
    }
    CHECK(full->missing_background_contracts == 31 && p->missing_mesh_contracts == 31 &&
          p->missing_packet_contracts);
    CHECK(sgx535_fragment_normalized_native_admission(&p->fragment) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!p->fragment.hardware_qualified && !p->fragment.numerical_qualified);
    /* Full field, state-byte, stage-word and bound-resource authentication. */
    *badp = *p;
    put(badp->pds + 736, 0x0c02a203);
    CHECK(sgx535_clip_viewport_mesh_validate(&r, badp, g, 280, w, scratch, sizeof(scratch)) ==
          SGX535_TASK_INVALID);
    *badfull = *full;
    put(badfull->foreground.image.pds + 736, 0x0c02a203);
    CHECK(sgx535_clip_viewport_mesh_background_bound_validate(
              &bgreq, image, g, 280, &b, &bg, badfull, w, scratch, sizeof(scratch)) ==
          SGX535_TASK_INVALID);
    for (unsigned i = 0; i < 6; i++) {
        *badp = *p;
        badp->pds[604 + i * 4] ^= 1;
        CHECK(sgx535_clip_viewport_mesh_validate(&r, badp, g, 280, w, scratch, sizeof(scratch)) ==
              SGX535_TASK_INVALID);
    }
    for (unsigned i = 576; i < 684; i++) {
        *badp = *p;
        badp->pds[i] ^= 1;
        CHECK(sgx535_clip_viewport_mesh_validate(&r, badp, g, 280, w, scratch, sizeof(scratch)) ==
              SGX535_TASK_INVALID);
    }
    for (unsigned stage = 0; stage < 3; stage++)
        for (unsigned i = 0; i < full->foreground.stages[stage].count; i++) {
            *badfull = *full;
            badfull->foreground.stages[stage].actions[i].value ^= 1;
            CHECK(sgx535_clip_viewport_mesh_background_bound_validate(
                      &bgreq, image, g, 280, &b, &bg, badfull, w, scratch, sizeof(scratch)) ==
                  SGX535_TASK_INVALID);
        }
    for (unsigned i = 0; i < 24; i++) {
        struct sgx535_clip_viewport_mesh_request q = r;
        uint32_t saved = 0;
        switch (i) {
        case 0:
            q.version = 14;
            break;
        case 1:
            q.profile = 13;
            break;
        case 2:
            q.width = 63;
            break;
        case 3:
            q.height = 63;
            break;
        case 4:
            q.conventions.half_pixel_center = 0;
            break;
        case 5:
            q.conventions.bottom_edge_rule = 0;
            break;
        case 6:
            q.conventions.origin = 0;
            break;
        case 7:
            q.conventions.min_samples = 0;
            break;
        case 8:
            q.conventions.sample_mask = 1;
            break;
        case 9:
            q.conventions.viewport_scale_f32[1] ^= 1;
            break;
        case 10:
            q.conventions.viewport_translate_f32[0] ^= 1;
            break;
        case 11:
            q.reserved[0] = 1;
            break;
        case 12:
            q.vertex_count = ~0u;
            break;
        case 13:
            q.index_count = 0;
            break;
        case 14:
            q.vertices = NULL;
            break;
        case 15:
            q.window_sidecar = NULL;
            break;
        case 16:
            q.fragment_count = ~0u;
            break;
        case 17:
            saved = v[0].clip_xyzw[3];
            v[0].clip_xyzw[3] = 0;
            break;
        case 18:
            saved = v[0].clip_xyzw[0];
            v[0].clip_xyzw[0] = 0x7fc00000;
            break;
        case 19:
            saved = v[0].clip_xyzw[0];
            v[0].clip_xyzw[0] ^= 1;
            break;
        case 20:
            saved = side[0].window_xyz_reciprocal_w[0];
            side[0].window_xyz_reciprocal_w[0] ^= 1;
            break;
        case 21:
            saved = side[0].rgba[0];
            side[0].rgba[0] ^= 1;
            break;
        case 22:
            saved = indices[0];
            indices[0] = 5;
            break;
        default:
            q.vertices = (const struct sgx535_clip_viewport_mesh_vertex *)(const void *)side;
            break;
        }
        memset(badp, 0x5a, sizeof(*badp));
        memset(other, 0xa5, sizeof(other));
        CHECK(sgx535_clip_viewport_mesh_build(&q, badp, other, sizeof(other), w, scratch,
                                              sizeof(scratch)) != SGX535_TASK_BLOCKED);
        filled(badp, sizeof(*badp), 0x5a);
        filled(other, sizeof(other), 0xa5);
        if (i == 17)
            v[0].clip_xyzw[3] = saved;
        else if (i == 18 || i == 19)
            v[0].clip_xyzw[0] = saved;
        else if (i == 20)
            side[0].window_xyz_reciprocal_w[0] = saved;
        else if (i == 21)
            side[0].rgba[0] = saved;
        else if (i == 22)
            indices[0] = saved;
    }
    uint32_t *mutations[] = {&r.version, &v[0].clip_xyzw[0], &side[0].window_xyz_reciprocal_w[0],
                             &ir[0].opcode};
    for (unsigned i = 0; i < 4; i++) {
        uint32_t saved = *mutations[i];
        drift_at = mutations[i];
        memset(badp, 0x5a, sizeof(*badp));
        CHECK(sgx535_clip_viewport_mesh_build(&r, badp, other, sizeof(other), w, scratch,
                                              sizeof(scratch)) == SGX535_TASK_UNSUPPORTED);
        CHECK(!drift_at);
        filled(badp, sizeof(*badp), 0x5a);
        *mutations[i] = saved;
    }
    drift_at = &b.bank_index[0];
    memset(badfull, 0x5a, sizeof(*badfull));
    CHECK(sgx535_clip_viewport_mesh_background_bind(&bgreq, image, g, 280, &b, &bg, badfull, w,
                                                    scratch,
                                                    sizeof(scratch)) == SGX535_TASK_INVALID);
    CHECK(!drift_at);
    b.bank_index[0] = 3;
    filled(badfull, sizeof(*badfull), 0x5a);
    fail_alloc = 1;
    memset(badimage, 0x5a, sizeof(*badimage));
    CHECK(sgx535_clip_viewport_mesh_background_build(&bgreq, badimage, other, sizeof(other), w,
                                                     scratch,
                                                     sizeof(scratch)) == SGX535_TASK_NOMEM);
    fail_alloc = 0;
    filled(badimage, sizeof(*badimage), 0x5a);
    CHECK(sgx535_clip_viewport_mesh_background_build(NULL, badimage, other, sizeof(other), w,
                                                     scratch,
                                                     sizeof(scratch)) == SGX535_TASK_INVALID);
    CHECK(sgx535_clip_viewport_mesh_build(&r, (void *)&r, other, sizeof(other), w, scratch,
                                          sizeof(scratch)) == SGX535_TASK_INVALID);
    CHECK(sgx535_clip_viewport_mesh_build(&r, badp, scratch, sizeof(scratch), w, scratch,
                                          sizeof(scratch)) == SGX535_TASK_INVALID);
    CHECK(sgx535_clip_viewport_mesh_background_build(&bgreq, image, g, sizeof(g), w, scratch,
                                                     sizeof(scratch)) == SGX535_TASK_BLOCKED);
    /* Regenerate old typed6 plus default0/0 paths. These stay768/BG768/832. */
    struct sgx535_window_mesh_workspace *oldw = calloc(1, sizeof(*oldw));
    struct sgx535_window_mesh_background_image *old = calloc(1, sizeof(*old));
    struct sgx535_window_mesh_request oldr = {.version = 14,
                                              .profile = 13,
                                              .core_id = r.core_id,
                                              .revision = r.revision,
                                              .width = 64,
                                              .height = 64,
                                              .order = 2,
                                              .interpolation = 1,
                                              .window = r.conventions,
                                              .vertices = side,
                                              .indices = indices,
                                              .vertex_count = 6,
                                              .index_count = 6,
                                              .fragment = ir,
                                              .fragment_count = 8};
    struct sgx535_window_mesh_gl_window_background_request oldq = {
        .version = 6, .packed_background_argb = 0xff000000, .foreground = oldr};
    CHECK(oldw && old &&
          sgx535_window_mesh_gl_window_background_build(&oldq, old, other, sizeof(other), oldw,
                                                        scratch,
                                                        sizeof(scratch)) == SGX535_TASK_BLOCKED);
    CHECK(sizeof(old->foreground.pds) == 768 && old->background.version == 3 &&
          old->background.sections[1].resource_offset == 768 &&
          old->background.sections[2].resource_offset == 832);
    save(argv[1], "old-window6-pds.original.bin", old->foreground.pds, 768);
    save(argv[1], "old-bg3-bytes.original.bin", old->background.bytes, old->background.byte_count);
    save(argv[1], "old-window6-vuse.original.bin", old->foreground.vertex_use,
         sizeof(old->foreground.vertex_use));
    save(argv[1], "old-window6-fuse.original.bin", old->foreground.fragment_use,
         sizeof(old->foreground.fragment_use));
    save(argv[1], "task15-pds.original.bin", p->pds, sizeof(p->pds));
    save(argv[1], "task15-vuse.original.bin", p->vertex_use, sizeof(p->vertex_use));
    save(argv[1], "task15-fuse.original.bin", p->fragment_use, sizeof(p->fragment_use));
    save(argv[1], "task15-bound-pds.original.bin", full->foreground.image.pds, sizeof(p->pds));
    save(argv[1], "task15-bound-vuse.original.bin", full->foreground.image.vertex_use,
         sizeof(p->vertex_use));
    save(argv[1], "task15-bound-fuse.original.bin", full->foreground.image.fragment_use,
         sizeof(p->fragment_use));
    save(argv[1], "task15-geometry.original.bin", g, 280);
    save(argv[1], "bg7-bytes.original.bin", image->background.bytes, image->background.byte_count);
    save(argv[1], "task15-relocations.original.bin", p->relocations, sizeof(p->relocations));
    export_projection(argv[1], image, full);
    printf(
        "PASS typed CLIP15/profile14/viewport6/BG7 full45relocs; %u checks; edge/retirement UNKNOWN\n",
        checks);
    free(oldw);
    free(old);
    free(w);
    free(p);
    free(badp);
    free(bound);
    free(image);
    free(badimage);
    free(full);
    free(badfull);
    CHECK(!legacy_clip_control_main(argc, argv));
    return 0;
}
