// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
/* Independent checked-in word oracle, pinned to the original unit source. */
static const uint32_t fragment[36] = {
    0xa0000100, 0x28a10000, 0xa0200080, 0x28a10000, 0xa0400000, 0x28a10000, 0xa0600180, 0x28a10000,
    0x00800000, 0xfca00000, 0x00a00000, 0xfca0f1c0, 0x00c00004, 0x18a00200, 0x00000305, 0x18a00000,
    0x00c00084, 0x18a00200, 0x00200305, 0x18a00000, 0x00c00104, 0x18a00200, 0x00400305, 0x18a00000,
    0x00c00184, 0x18a00200, 0x00600305, 0x18a00000, 0x20040000, 0x40a10c11, 0x20040080, 0x40a10c09,
    0x20040100, 0x40a10c05, 0x20040180, 0x40a50c21};
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
static inline int mask_reference_equal(const uint8_t *actual, const uint8_t *expected,
                                       const struct sgx535_task_packet_section sections[16],
                                       int bound)
{
    return sgx535_pixel_event_reference_packet(
        actual, expected, 2048, sections[11].byte_offset, sections[5].byte_offset, 1,
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
