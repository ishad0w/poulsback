// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
/* The old fixed16 fixture remains an independent byte-regression oracle. */
#define main normalized_tile16_old_main
#include "test_sgx535_normalized_color0_tile16.c"
#undef main
#include "sgx535_normalized_color0_mask.h"

static const struct sgx535_task_packet_section mask_sections[16] = {
    {1, 0, 160, 3, 0, 16},      {2, 160, 20, 3, 160, 2},   {3, 192, 32, 1, 0, 32},
    {4, 224, 144, 2, 0, 32},    {5, 368, 48, 1, 32, 32},   {6, 416, 32, 2, 160, 32},
    {7, 448, 128, 0, 0, 32},    {8, 576, 60, 0, 128, 32},  {14, 640, 4, 0, 192, 32},
    {9, 656, 124, 0, 224, 32},  {15, 784, 60, 0, 352, 32}, {10, 848, 148, 0, 416, 32},
    {11, 1008, 92, 0, 576, 16}, {16, 1104, 8, 0, 208, 16}, {12, 1120, 16, 0, 672, 16},
    {13, 1136, 68, 0, 688, 16}};
#define A(s, o, r, t, n, a, m, x, y) {1, s, o, r, t, n, a, m, x, y}
#define U(s, o, r, t, n)                                                                           \
    {2, s, o, r, t, n, 16, 15, 0, 0}, {3, s, o, r, t, n, 16, 240, 15, 4},                          \
        {3, s, o, r, t, n, 16, 0x7ff00, 4, 8}
static const struct sgx535_task_packet_reloc mask_relocs[40] = {
    A(6, 0, 3, 0, 96, 16, 0xffffffffu, 0, 0),
    U(6, 16, 1, 0, 16),
    A(6, 64, 3, 96, 64, 16, 0xffffffffu, 0, 0),
    U(6, 80, 1, 16, 16),
    U(7, 0, 2, 0, 144),
    A(9, 0, 0, 624, 44, 16, 0xffffffffu, 0, 0),
    U(9, 8, 1, 32, 16),
    A(9, 64, 0, 576, 40, 16, 0xffffffffu, 0, 0),
    U(9, 72, 1, 48, 16),
    A(10, 0, 0, 208, 8, 16, 0xffffffffu, 0, 0),
    U(10, 8, 1, 64, 16),
    A(12, 8, 0, 192, 4, 32, 0xffffff, 4, 0),
    A(12, 16, 0, 128, 60, 32, 0xffffff, 4, 0),
    A(11, 8, 0, 672, 16, 16, 0xffffffffu, 0, 0),
    U(11, 0, 2, 160, 8),
    U(11, 16, 2, 176, 16),
    A(14, 4, 5, 0, 2048, 4, 0xfffffffcu, 2, 2),
    A(15, 0, 0, 224, 60, 32, 0xfffffff, 4, 0),
    A(15, 12, 3, 160, 12, 2, 0xffffffffu, 0, 0),
    A(15, 20, 0, 64, 64, 32, 0xfffffff, 4, 0),
    A(15, 28, 0, 288, 60, 32, 0xfffffff, 4, 0),
    A(15, 40, 3, 172, 6, 2, 0xffffffffu, 0, 0),
    A(15, 48, 0, 0, 64, 32, 0xfffffff, 4, 0),
    A(15, 56, 0, 352, 60, 32, 0xfffffff, 4, 0)};
#undef U
#undef A
static void mask_words(uint8_t bytes[2048], unsigned section, const uint32_t *w, size_t n)
{
    CHECK(n * 4 == mask_sections[section].byte_count);
    for (size_t i = 0; i < n; i++)
        put(bytes + mask_sections[section].byte_offset + i * 4, w[i]);
}
static void mask_literal(uint8_t bytes[2048],
                         const struct sgx535_normalized_color0_tile16_request *r, int bound)
{
    uint32_t v[40] = {0x40000000, 0x40000000, 0x3f000000, 0x3f800000, 0,          0,
                      0,          0,          0x41600000, 0x40000000, 0x3f000000, 0x3f800000,
                      0,          0,          0,          0,          0x41500000, 0x41400000,
                      0x3f000000, 0x3f800000, 0,          0,          0,          0,
                      0,          0,          0x3f800000, 0x3f800000, 0x41800000, 0,
                      0x3f800000, 0x3f800000, 0,          0x41800000, 0x3f800000, 0x3f800000,
                      0x41800000, 0x41800000, 0x3f800000, 0x3f800000};
    const uint32_t ix[5] = {0x00010000, 0x00030002, 0x00010002, 0x00010000, 2};
    const uint32_t vu[8] = {0xa0000000, 0x28a17001, 0xa0200000, 0xfb275000,
                            0xa0000000, 0x28a13001, 0xa0200000, 0xfb275000};
    const uint32_t su[12] = {0xa0000000, 0x28a1a001, 0xa0200580, 0xfb274000,
                             0xa0000000, 0x28a19001, 0xa0200500, 0xfb274000,
                             0xa0000000, 0x28a11001, 0xa0200100, 0xfb274000};
    uint32_t vp[32] = {0,  0x80e00007, 0, 0, 0x00200000, 0,          0,          0,
                       32, 0,          0, 0, 0x67800070, 0x2f030343, 0x030803e5, 0xaf000000,
                       0,  0x80600003, 0, 0, 0x00200000, 0,          0,          0,
                       16, 0,          0, 0, 0x67800070, 0x2f030343, 0x030803e5, 0xaf000000};
    uint32_t fp[15] = {0x80000, 0x38000000, 0, 0, 0,          0,          0,         0,
                       0x20,    0x0fd0a00f, 0, 0, 0x07000345, 0x070418a2, 0xaf000000};
    const uint32_t second[1] = {0xaf000000};
    uint32_t sp[31] = {
        0, 0x8140000a, 0x00200000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07030223, 0x07042345, 0xaf000000, 0,
        0, 0x81200009, 0x00200000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t tp[15] = {0, 0x80200001, 0x00200000, 0, 0,          0,          0,         0,
                       0, 0,          0,          0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t mte[23] = {0x54c1, 0x01d00000, 0,          0x30030004, 0x0c000000, 0,
                        0,      0x08001800, 0x10000,    0,          0,          0,
                        0x54c5, 0x07f00100, 0x02000000, 0,          0,          0,
                        0,      0,          0x04001000, 0x10000,    0};
    const uint32_t term[2] = {0x2000, 0x10001};
    uint32_t pbe[4] = {0xf8000, 0, 0, 0xf00f};
    uint32_t ta[17] = {0x40000000, 0x0c02a203, 0x81400006, 0,          0,         0,
                       0x02000103, 0x40000000, 0x0c02a203, 0x81400003, 0,         0,
                       0,          0x04000203, 0x60000000, 0x0c02a201, 0xc0000000};
    for (unsigned i = 0; i < 3; i++)
        memcpy(v + i * 8 + 4, r->rgba[i], 16);
    if (bound) {
        vp[0] = 0x40002000;
        vp[4] = 0x00200003;
        vp[16] = 0x40002060;
        vp[20] = 0x00200103;
        fp[0] = 0x00090004;
        sp[0] = 0x20000270;
        sp[2] = 0x00200203;
        sp[16] = 0x20000240;
        sp[18] = 0x00200303;
        tp[0] = 0x200000d0;
        tp[2] = 0x00200403;
        mte[2] = 0xc;
        mte[4] = 0x0c000008;
        pbe[1] = 0x42005000;
        ta[0] = 0x4200000e;
        ta[3] = 0x400020a0;
        ta[5] = 0x02000004;
        ta[7] = 0x42000012;
        ta[10] = 0x400020ac;
        ta[12] = 0x02000000;
        ta[14] = 0x62000016;
    }
    memset(bytes, 0, 2048);
    mask_words(bytes, 0, v, 40);
    mask_words(bytes, 1, ix, 5);
    mask_words(bytes, 2, vu, 8);
    mask_words(bytes, 3, fragment, 36);
    mask_words(bytes, 4, su, 12);
    mask_words(bytes, 6, vp, 32);
    mask_words(bytes, 7, fp, 15);
    mask_words(bytes, 8, second, 1);
    mask_words(bytes, 9, sp, 31);
    mask_words(bytes, 10, tp, 15);
    mask_words(bytes, 12, mte, 23);
    mask_words(bytes, 13, term, 2);
    mask_words(bytes, 14, pbe, 4);
    mask_words(bytes, 15, ta, 17);
}
static int mask_packet_equal(const uint8_t *actual, const uint8_t *expected, int bound)
{
    return sgx535_pixel_event_reference_packet(
        actual, expected, 2048, mask_sections[11].byte_offset, mask_sections[5].byte_offset, 1,
        bound ? 0x200002a0u : 0, bound ? 0x00010a04u : 0, bound ? 0x00210b04u : 0x00200000u);
}

static void mask_binding(const struct sgx535_normalized_color0_background_image *p,
                         struct sgx535_task_launch_binding *b, struct sgx535_background_binding *bb)
{
    const uint32_t va[8] = {0x20000000, 0x40000000, 0x40001000, 0x40002000,
                            0x40003000, 0x42005000, 0x40005000, 0x30000000};
    memset(b, 0, sizeof(*b));
    memset(bb, 0, sizeof(*bb));
    b->epoch = 37;
    b->bank_index[0] = 3;
    b->bank_index[1] = 4;
    for (unsigned i = 0; i < 8; i++)
        b->roles[i] = (struct sgx535_task_launch_role){
            101 + i, va[i], p->foreground.packet.requirements[i].minimum_bytes,
            p->foreground.packet.requirements[i].access, 0};
    bb->epoch = 37;
    bb->fragment_bank_index = 4;
    bb->pds_exec_base = 0x20000000;
    bb->bif_3d_req_base = 0x30000000;
    bb->parameter_storage_id = 108;
    bb->parameter_va = 0x30000000;
    bb->parameter_bytes = 0x2000000;
    bb->resources[0] = (struct sgx535_background_resource){103, 0x40001000, 4096, 1, 1, {0, 0}};
    bb->resources[1] = (struct sgx535_background_resource){101, 0x20000000, 4096, 2, 1, {0, 0}};
    bb->resources[2] = (struct sgx535_background_resource){109, 0x32000000, 4096, 3, 1, {0, 0}};
}
static void mask_background_literal(uint8_t bytes[192], uint32_t color, int bound)
{
    uint32_t w[48] = {0};
    w[0] = color & 0x1fffff;
    w[1] = 0xfca40001u | ((color >> 21) & 31u) << 4 | ((color >> 26) & 63u) << 12;
    w[16] = 0x20;
    w[20] = 0x07000345;
    w[21] = 0xaf000000;
    w[24] = 0xaf000000;
    w[29] = 0x30000;
    w[30] = 0x0c000000;
    w[37] = 2;
    w[39] = 0x40004000;
    w[40] = w[42] = w[44] = 0x3f800000;
    w[41] = 0x41004000;
    w[43] = 0x40004100;
    if (bound) {
        w[8] = 0x16004;
        w[28] = 0x34;
        w[30] = 0x0c000030;
    }
    for (unsigned i = 0; i < 48; i++)
        put(bytes + i * 4, w[i]);
}
static void mask_extent(const struct sgx535_normalized_color0_task *p)
{
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_task_packet_section *s = &p->packet.sections[i];
        CHECK(s->byte_offset + s->byte_count <= p->packet.byte_count);
        CHECK(s->resource_byte_offset + s->byte_count <=
              p->packet.requirements[s->role].minimum_bytes);
        CHECK(!(s->resource_byte_offset & (s->alignment - 1)));
        if (s->role == 0)
            CHECK(s->resource_byte_offset + s->byte_count <= 768);
        if (s->role == 2)
            CHECK(s->resource_byte_offset + s->byte_count <= 1536);
        for (unsigned j = 0; j < i; j++)
            if (s->role == p->packet.sections[j].role) {
                const struct sgx535_task_packet_section *t = &p->packet.sections[j];
                CHECK(s->resource_byte_offset >= t->resource_byte_offset + t->byte_count ||
                      t->resource_byte_offset >= s->resource_byte_offset + s->byte_count);
            }
    }
    for (unsigned i = 0; i < 40; i++) {
        const struct sgx535_task_packet_reloc *r = &p->packet.relocations[i];
        const struct sgx535_task_packet_section *s = &p->packet.sections[r->section];
        CHECK(r->section_byte_offset + 4 <= s->byte_count);
        CHECK(r->resource_byte_offset + r->span_bytes <=
              p->packet.requirements[r->role].minimum_bytes);
    }
    CHECK(p->packet.missing_contracts == 0x1ff && p->fragment.missing_contracts == 0x3ff);
    CHECK(!p->fragment.arithmetic_qualified && !p->fragment.numerical_qualified &&
          !p->fragment.linkage_qualified);
    CHECK(!p->fragment.output_qualified && !p->fragment.hardware_qualified &&
          !p->fragment.nonfinite_behavior_qualified);
    CHECK(sgx535_fragment_normalized_native_admission(&p->fragment) == SGX535_NIR_ERR_UNSUPPORTED);
}
int main(int argc, char **argv)
{
    char *old_argv[1] = {(char *)"old"};
    CHECK(normalized_tile16_old_main(1, old_argv) == 0);
    struct sgx535_fragment_instruction *ir = calloc(512, sizeof(*ir));
    struct sgx535_normalized_color0_background_request r = {.version = 2,
                                                            .packed_background_argb = 0xff1d2743},
                                                       bad;
    struct sgx535_normalized_color0_background_image *p = calloc(1, sizeof(*p)),
                                                     *saved = calloc(1, sizeof(*p)),
                                                     *mut = calloc(1, sizeof(*p));
    struct sgx535_normalized_color0_background_bound *out = calloc(1, sizeof(*out)),
                                                     *saved_out = calloc(1, sizeof(*out));
    struct sgx535_normalized_color0_background_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_normalized_color0_launch_workspace *lw = calloc(1, sizeof(*lw));
    struct sgx535_task_launch_binding b, bad_b;
    struct sgx535_background_binding bb, bad_bb;
    uint8_t expected[2048], background[192];
    unsigned found = 0;
    CHECK(ir && p && saved && mut && out && saved_out && w && lw);
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--dump")));
    r.foreground = request(ir);
    r.foreground.version = 11;
    r.foreground.profile = 10;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_mask_background_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->version == 2 && p->foreground.packet.version == 11 &&
          p->foreground.packet.profile == 10);
    CHECK(p->foreground.packet.section_count == 16 && p->foreground.packet.relocation_count == 40);
    CHECK(p->foreground.packet.byte_count == 1204 && p->foreground.fragment_code_bytes == 144);
    CHECK(p->foreground.fragment_temp_words == 7 && p->foreground.dms_temp_allocation_bytes == 32);
    CHECK(p->foreground.event_use_offset_bytes == 160 &&
          p->foreground.fragment_use_extent_bytes == 192);
    CHECK(!memcmp(p->foreground.packet.sections, mask_sections, sizeof(mask_sections)));
    CHECK(!memcmp(p->foreground.packet.relocations, mask_relocs, sizeof(mask_relocs)));
    mask_literal(expected, &r.foreground, 0);
    CHECK(mask_packet_equal(p->foreground.packet.bytes, expected, 0));
    mask_background_literal(background, r.packed_background_argb, 0);
    CHECK(!memcmp(p->background.bytes, background, 192));
    mask_extent(&p->foreground);
    mask_binding(p, &b, &bb);
    CHECK(sgx535_normalized_color0_mask_background_bind(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_mask_background_bound_validate(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_BLOCKED);
    CHECK(out->version == 2 && out->missing_background_contracts == 15);
    mask_literal(expected, &r.foreground, 1);
    CHECK(mask_packet_equal(out->launch.launch.bytes, expected, 1));
    mask_background_literal(background, r.packed_background_argb, 1);
    CHECK(!memcmp(out->background.image.bytes, background, 192));
    CHECK(out->background.resolved_relocations == 5 &&
          out->background.bank.entry_offset_bytes == 0x1600);
    CHECK(out->launch.launch.resolved_relocations == 40 && out->launch.launch.packet_version == 11);
    CHECK(out->launch.launch.ta_stream_va_bytes == 0x200002b0 &&
          out->launch.launch.event_pds_va_bytes == 0x200001a0);
    CHECK(out->launch.launch.missing_packet_contracts == 0x1ff &&
          out->launch.launch.missing_launch_contracts == 0x75);
    CHECK(out->launch.fragment_missing_contracts == 0x3ff && !out->launch.hardware_qualified);
    CHECK(out->launch.launch.stages[0].count == 3 && out->launch.launch.stages[1].count == 7 &&
          out->launch.launch.stages[2].count == 26);
    for (unsigned i = 0; i < 26; i++)
        if (out->launch.launch.stages[2].actions[i].offset == 0x4c4) {
            CHECK(out->launch.launch.stages[2].actions[i].value == 0x02200000);
            found++;
        }
    CHECK(found == 1);
    CHECK(sgx535_normalized_color0_mask_tile16_launch_build(
              &r.foreground, &p->foreground, &b, &w->result.launch, lw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_mask_tile16_launch_validate(
              &r.foreground, &p->foreground, &b, &w->result.launch, lw) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(w->result.launch.launch.bytes, out->launch.launch.bytes, 2048));
    *saved = *p;
    *saved_out = *out;
    if (argc == 2) {
        save("mask-color0-image.bin", p, sizeof(*p));
        save("mask-color0-bound.bin", out, sizeof(*out));
    }
    /* Explicit front doors never reinterpret one another's versions. */
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(sgx535_normalized_color0_tile16_task_build(&r.foreground, &p->foreground,
                                                     &w->foreground) == SGX535_TASK_UNSUPPORTED);
    bad = r;
    bad.version = 1;
    bad.foreground.version = 10;
    bad.foreground.profile = 8;
    CHECK(sgx535_normalized_color0_mask_background_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    for (size_t i = 0; i < sizeof(*p); i += 31) {
        *mut = *saved;
        ((uint8_t *)mut)[i] ^= 1;
        CHECK(sgx535_normalized_color0_mask_background_validate(&r, mut, w) == SGX535_TASK_INVALID);
        CHECK(sgx535_normalized_color0_mask_background_bind(&r, mut, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    for (size_t i = 0; i < sizeof(*out); i += 37) {
        ((uint8_t *)out)[i] ^= 1;
        CHECK(sgx535_normalized_color0_mask_background_bound_validate(&r, p, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
        ((uint8_t *)out)[i] ^= 1;
    }
    for (unsigned i = 0; i < 40; i++) {
        *mut = *saved;
        mut->foreground.packet.relocations[i].span_bytes++;
        CHECK(sgx535_normalized_color0_mask_background_bind(&r, mut, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    for (unsigned i = 0; i < 16; i++) {
        *mut = *saved;
        mut->foreground.packet.sections[i].resource_byte_offset += 16;
        CHECK(sgx535_normalized_color0_mask_background_bind(&r, mut, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
    }
    for (unsigned mode = 0; mode < 18; mode++) {
        bad = r;
        if (mode == 0)
            bad.version = 3;
        if (mode == 1)
            bad.foreground.version = 10;
        if (mode == 2)
            bad.foreground.profile = 8;
        if (mode == 3)
            bad.reserved[0] = 1;
        if (mode == 4)
            bad.foreground.reserved[1] = 1;
        if (mode == 5)
            bad.foreground.core_id++;
        if (mode == 6)
            bad.foreground.revision++;
        if (mode == 7)
            bad.foreground.clip_position[0][3] = 0x40000000;
        if (mode == 8)
            bad.foreground.clip_position[1][0] = 0x7fc12345;
        if (mode == 9)
            bad.foreground.rgba[2][1] = 0x80000000;
        if (mode == 10)
            bad.foreground.rgba[0][0] = 0x7f800000;
        if (mode == 11)
            bad.foreground.interpolation = 0;
        if (mode == 12)
            bad.foreground.interpolation = 2;
        if (mode == 13)
            bad.foreground.fragment_count = 4;
        if (mode == 14)
            bad.foreground.fragment_count = 513;
        if (mode == 15)
            bad.foreground.fragment = NULL;
        if (mode == 16)
            bad.foreground.fragment = (void *)(~(unsigned long)0 - 3u);
        if (mode == 17)
            bad.foreground.clip_position[0][2] = 0x3f800001;
        CHECK(sgx535_normalized_color0_mask_background_build(&bad, p, w) < 0);
        CHECK(!memcmp(p, saved, sizeof(*p)));
    }
    for (unsigned mode = 0; mode < 17; mode++) {
        bad_b = b;
        bad_bb = bb;
        if (mode == 0)
            bad_bb.epoch++;
        if (mode == 1)
            bad_bb.fragment_bank_index = 5;
        if (mode == 2)
            bad_bb.parameter_storage_id++;
        if (mode == 3)
            bad_bb.parameter_va += 4096;
        if (mode == 4)
            bad_bb.resources[0].storage_id++;
        if (mode == 5)
            bad_bb.resources[0].gpu_va_bytes += 4096;
        if (mode == 6)
            bad_bb.resources[1].storage_id++;
        if (mode == 7)
            bad_bb.resources[1].gpu_va_bytes += 4096;
        if (mode == 8)
            bad_bb.resources[2].storage_id = b.roles[1].storage_id;
        if (mode == 9)
            bad_bb.resources[2].gpu_va_bytes += 4096;
        if (mode == 10)
            bad_bb.resources[2].access = 3;
        if (mode == 11)
            bad_bb.resources[2].allocation_bytes = 8192;
        if (mode == 12)
            bad_b.roles[1].gpu_va_bytes = 0x32000000;
        if (mode == 13)
            bad_bb.pds_exec_base += 32;
        if (mode == 14)
            bad_bb.bif_3d_req_base = 0x32000000;
        if (mode == 15)
            bad_b.roles[0].allocation_bytes = 8192;
        if (mode == 16)
            bad_b.epoch = 0;
        CHECK(sgx535_normalized_color0_mask_background_bind(&r, p, &bad_b, &bad_bb, out, w) < 0);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, (void *)p) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_mask_background_bind(&r, p, &b, &bb, (void *)&bb, w) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_mask_tile16_task_build(&r.foreground, (void *)ir,
                                                          &w->foreground) == SGX535_TASK_INVALID);
    fail_alloc = 1;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_NOMEM);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    fail_alloc = 0;
    /* Arbitrary colors and flat replication use the same eight-word path. */
    for (unsigned i = 0; i < 12; i++) {
        r.foreground = request(ir);
        r.foreground.version = 11;
        r.foreground.profile = 10;
        r.foreground.rgba[i % 3][i % 4] = i & 1 ? 0x3f123456 : 1;
        CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
        mask_literal(expected, &r.foreground, 0);
        CHECK(mask_packet_equal(p->foreground.packet.bytes, expected, 0));
    }
    r.foreground.interpolation = 2;
    memcpy(r.foreground.rgba[1], r.foreground.rgba[0], 16);
    memcpy(r.foreground.rgba[2], r.foreground.rgba[0], 16);
    r.foreground.clip_position[0][0] = 0xbf000000;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(get(p->foreground.packet.bytes) == 0x40800000);
    mask_extent(&p->foreground);
    /* One new complete raw-sign -> normalized PACK program, never a recognizer. */
    r.foreground = request(ir);
    r.foreground.version = 11;
    r.foreground.profile = 10;
    r.foreground.fragment_count = 14;
    ir[4] =
        (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x80000000, 0, 0, {0, 0, 0}};
    ir[5] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RAW_XOR, 0, 0, 0, {0, 4, 0}};
    ir[6] =
        (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x7fffffff, 0, 0, {0, 0, 0}};
    ir[7] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RAW_AND, 0, 0, 0, {1, 6, 0}};
    ir[8] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RAW_AND, 0, 0, 0, {2, 6, 0}};
    ir[9] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {3, 0, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[10 + i] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT,
                                                          0,
                                                          i,
                                                          0,
                                                          {i == 0   ? 5
                                                           : i == 1 ? 7
                                                           : i == 2 ? 8
                                                                    : 9,
                                                           0, 0}};
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    mask_extent(&p->foreground);
    {
        /* Independently retained raw-sign register schedule, including the complete
   * clamping/PACK suffix. No production encoder builds these words. */
        const uint32_t raw[48] = {
            0xa0000100, 0x28a10000, 0xa0200080, 0x28a10000, 0xa0400000, 0x28a10000, 0xa0600180,
            0x28a10000, 0x00800000, 0xfca20000, 0x00a00004, 0x58a00000, 0x001fffff, 0xfca1f1f0,
            0x00800080, 0x50a00000, 0x00200100, 0x50a00000, 0x20000180, 0x28a10000, 0x00400000,
            0xfca00000, 0x00600000, 0xfca0f1c0, 0x00c00282, 0x18a00200, 0x00a00303, 0x18a00000,
            0x00c00202, 0x18a00200, 0x00800303, 0x18a00000, 0x00c00082, 0x18a00200, 0x00200303,
            0x18a00000, 0x00c00002, 0x18a00200, 0x00000303, 0x18a00000, 0x20040280, 0x40a10c11,
            0x20040200, 0x40a10c09, 0x20040080, 0x40a10c05, 0x20040000, 0x40a50c21};
        CHECK(p->foreground.fragment_code_bytes == 192 &&
              p->foreground.event_use_offset_bytes == 192);
        for (unsigned i = 0; i < 48; i++)
            CHECK(get(p->foreground.packet.bytes + p->foreground.packet.sections[3].byte_offset +
                      i * 4) == raw[i]);
    }
    CHECK(sgx535_normalized_color0_mask_background_bind(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_BLOCKED);
    bad = r;
    bad.version = 1;
    bad.foreground.version = 10;
    bad.foreground.profile = 8;
    CHECK(sgx535_normalized_color0_background_build(&bad, mut, w) == SGX535_TASK_UNSUPPORTED);
    ir[6].immediate = 0x7f800000;
    *saved = *p;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    /* Exact serialized capacity boundary:108 input/MOV definitions +4outputs
  *lower to122pairs=976B and byte_count2036. The109th definition needs984B,
  *whose aligned following sections would make byte_count2052. */
    memset(ir, 0, 512 * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (unsigned i = 1; i < 108; i++)
        ir[i] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {i - 1, 0, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[108 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {107, 0, 0}};
    r.foreground.fragment_count = 112;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->foreground.fragment_code_bytes == 976 && p->foreground.event_use_offset_bytes == 992 &&
          p->foreground.packet.byte_count == 2036);
    mask_extent(&p->foreground);
    CHECK(sgx535_normalized_color0_mask_background_bind(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_BLOCKED);
    *saved = *p;
    ir[108] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {107, 0, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[109 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {108, 0, 0}};
    r.foreground.fragment_count = 113;
    CHECK(sgx535_normalized_color0_mask_background_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    printf("normalized COLOR0 mask+BG: %u checks; CPU construction only, native admission closed\n",
           checks);
    free(lw);
    free(w);
    free(saved_out);
    free(out);
    free(mut);
    free(saved);
    free(p);
    free(ir);
    return 0;
}
