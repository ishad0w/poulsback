// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
#include "sgx535_background_launch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
static void put(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}
static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static const struct sgx535_task_packet_section sections[16] = {
    {1, 0, 160, 3, 0, 16},     {2, 160, 24, 3, 160, 2},   {3, 192, 16, 1, 0, 32},
    {4, 208, 8, 2, 0, 32},     {5, 224, 48, 1, 32, 32},   {6, 272, 32, 2, 32, 32},
    {7, 304, 64, 0, 0, 32},    {8, 368, 56, 0, 64, 32},   {14, 432, 4, 0, 128, 32},
    {9, 448, 124, 0, 160, 32}, {15, 576, 60, 0, 288, 32}, {10, 640, 148, 0, 352, 32},
    {11, 800, 92, 0, 512, 16}, {16, 896, 8, 0, 608, 16},  {12, 912, 16, 0, 624, 16},
    {13, 928, 68, 0, 672, 16}};
/* Literal producer relocation rows; none is obtained from the builder. */
#define A(s, o, r, t, n, a, m, x, y) {1, s, o, r, t, n, a, m, x, y}
#define U(s, o, r, t, n)                                                                           \
    {2, s, o, r, t, n, 16, 15, 0, 0}, {3, s, o, r, t, n, 16, 240, 15, 4},                          \
        {3, s, o, r, t, n, 16, 0x7ff00, 4, 8}
static const struct sgx535_task_packet_reloc relocs[36] = {
    A(6, 0, 3, 0, 160, 16, 0xffffffffu, 0, 0),
    U(6, 16, 1, 0, 16),
    U(7, 0, 2, 0, 8),
    A(9, 0, 0, 560, 44, 16, 0xffffffffu, 0, 0),
    U(9, 8, 1, 32, 16),
    A(9, 64, 0, 512, 40, 16, 0xffffffffu, 0, 0),
    U(9, 72, 1, 48, 16),
    A(10, 0, 0, 608, 8, 16, 0xffffffffu, 0, 0),
    U(10, 8, 1, 64, 16),
    A(12, 8, 0, 128, 4, 32, 0xffffff, 4, 0),
    A(12, 16, 0, 64, 56, 32, 0xffffff, 4, 0),
    A(11, 8, 0, 624, 16, 16, 0xffffffffu, 0, 0),
    U(11, 0, 2, 32, 8),
    U(11, 16, 2, 48, 16),
    A(14, 4, 5, 0, 2048, 4, 0xfffffffcu, 2, 2),
    A(15, 0, 0, 160, 60, 32, 0xfffffff, 4, 0),
    A(15, 12, 3, 160, 12, 2, 0xffffffffu, 0, 0),
    A(15, 20, 0, 0, 64, 32, 0xfffffff, 4, 0),
    A(15, 28, 0, 224, 60, 32, 0xfffffff, 4, 0),
    A(15, 40, 3, 172, 12, 2, 0xffffffffu, 0, 0),
    A(15, 48, 0, 0, 64, 32, 0xfffffff, 4, 0),
    A(15, 56, 0, 288, 60, 32, 0xfffffff, 4, 0)};
#undef A
#undef U
static struct sgx535_task_pair_request request(void)
{
    struct sgx535_task_pair_request r = {
        .version = 8,
        .profile = 7,
        .core_id = 0x01130000,
        .revision = 0x00010201,
        .width = 16,
        .height = 16,
        .packed_argb = 0xff37a9d5,
        .clip_position = {{{0xbf400000, 0x3f400000, 0, 0x3f800000},
                           {0x3f400000, 0x3f400000, 0, 0x3f800000},
                           {0x3f200000, 0xbf000000, 0, 0x3f800000}},
                          {{0xbf400000, 0x3f400000, 0, 0x3f800000},
                           {0x3f200000, 0xbf000000, 0, 0x3f800000},
                           {0xbf400000, 0xbf400000, 0, 0x3f800000}}}};
    return r;
}
static void words(uint8_t *bytes, unsigned section, const uint32_t *w, size_t n)
{
    CHECK(n * 4 == sections[section].byte_count);
    for (size_t i = 0; i < n; i++)
        put(bytes + sections[section].byte_offset + i * 4, w[i]);
}
static void literal(uint8_t bytes[2048], uint32_t color, int alternate, int bound)
{
    uint32_t v[40] = {0x40000000, 0x40000000, 0x3f000000, 0x3f800000, 0x41600000, 0x40000000,
                      0x3f000000, 0x3f800000, 0x41500000, 0x41400000, 0x3f000000, 0x3f800000,
                      0x40000000, 0x40000000, 0x3f000000, 0x3f800000, 0x41500000, 0x41400000,
                      0x3f000000, 0x3f800000, 0x40000000, 0x41600000, 0x3f000000, 0x3f800000,
                      0,          0,          0x3f800000, 0x3f800000, 0x41800000, 0,
                      0x3f800000, 0x3f800000, 0,          0x41800000, 0x3f800000, 0x3f800000,
                      0x41800000, 0x41800000, 0x3f800000, 0x3f800000};
    static const uint32_t index[6] = {0x00070006, 0x00090008, 0x00070008,
                                      0x00010000, 0x00030002, 0x00050004};
    static const uint32_t vu[4] = {0xa0000000, 0x28a13001, 0xa0200000, 0xfb275000};
    uint32_t fs[2] = {color & 0x1fffff,
                      0xfca40001u | ((color >> 21) & 31u) << 4 | ((color >> 26) & 63u) << 12};
    static const uint32_t su[12] = {0xa0000000, 0x28a1a001, 0xa0200580, 0xfb274000,
                                    0xa0000000, 0x28a19001, 0xa0200500, 0xfb274000,
                                    0xa0000000, 0x28a11001, 0xa0200100, 0xfb274000};
    uint32_t vp[16] = {0,  0x80600003, 0, 0, 0x00200000, 0,          0,          0,
                       16, 0,          0, 0, 0x67800070, 0x2f030343, 0x030803e5, 0xaf000000};
    uint32_t fp[14] = {0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x07000345, 0xaf000000};
    const uint32_t secondary[1] = {0xaf000000};
    uint32_t sp[31] = {
        0, 0x8140000a, 0x00200000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07030223, 0x07042345, 0xaf000000, 0,
        0, 0x81200009, 0x00200000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t tp[15] = {0, 0x80200001, 0x00200000, 0, 0,          0,          0,         0,
                       0, 0,          0,          0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t mte[23] = {
        0x54c1,     0x01d00000, 0, 0x30000, 0xc000000, 0, 0, 0x04001000, 0x10000, 0, 0, 0, 0x54c5,
        0x07f00100, 0x02000000, 0, 0,       0,         0, 0, 0x04001000, 0x10000, 0};
    const uint32_t term[2] = {0x2000, 0x10001};
    uint32_t pbe[4] = {0xf8000, 0, 0, 0xf00f};
    uint32_t ta[17] = {0x40000000, 0x0c02a203, 0x81400006, 0,          0,         0,
                       0x02000103, 0x40000000, 0x0c02a203, 0x81400006, 0,         0,
                       0,          0x02000103, 0x60000000, 0x0c02a201, 0xc0000000};
    if (alternate)
        v[0] = 0x40800000;
    if (bound) {
        vp[0] = 0x40002000;
        vp[4] = 0x00200003;
        fp[0] = 0x10004;
        sp[0] = 0x20000230;
        sp[2] = 0x00200203;
        sp[16] = 0x20000200;
        sp[18] = 0x00200303;
        tp[0] = 0x20000260;
        tp[2] = 0x00200403;
        mte[2] = 8;
        mte[4] = 0x0c000004;
        pbe[1] = 0x42005000;
        ta[0] = 0x4200000a;
        ta[3] = 0x400020a0;
        ta[5] = 0x02000000;
        ta[7] = 0x4200000e;
        ta[10] = 0x400020ac;
        ta[12] = 0x02000000;
        ta[14] = 0x62000012;
    }
    memset(bytes, 0, 2048);
    words(bytes, 0, v, 40);
    words(bytes, 1, index, 6);
    words(bytes, 2, vu, 4);
    words(bytes, 3, fs, 2);
    words(bytes, 4, su, 12);
    words(bytes, 6, vp, 16);
    words(bytes, 7, fp, 14);
    words(bytes, 8, secondary, 1);
    words(bytes, 9, sp, 31);
    words(bytes, 10, tp, 15);
    words(bytes, 12, mte, 23);
    words(bytes, 13, term, 2);
    words(bytes, 14, pbe, 4);
    words(bytes, 15, ta, 17);
}
static int pair_mask_packet_equal(const uint8_t *actual, const uint8_t *expected, int bound)
{
    return sgx535_pixel_event_reference_packet(
        actual, expected, 2048, sections[11].byte_offset, sections[5].byte_offset, 1,
        bound ? 0x20000270u : 0, bound ? 0x00010204u : 0, bound ? 0x00210304u : 0x00200000u);
}

static void save(const char *name, const void *p, size_t n)
{
    FILE *f = fopen(name, "wb");
    CHECK(f);
    CHECK(fwrite(p, 1, n, f) == n);
    CHECK(!fclose(f));
}
int main(int argc, char **argv)
{
    struct sgx535_task_pair_request r = request(), bad;
    struct sgx535_task_packet *p = calloc(1, sizeof(*p)), *scratch = calloc(1, sizeof(*p)),
                              *tamper = calloc(1, sizeof(*p));
    struct sgx535_task_launch *out = calloc(1, sizeof(*out)), *saved = calloc(1, sizeof(*out));
    struct sgx535_task_launch_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_task_launch_binding b = {0}, bad_b;
    uint8_t expected[2048];
    static const uint32_t colors[4] = {0xff37a9d5, 0x10234567, 0, 0xffffffff};
    static const uint32_t va[8] = {0x20000000, 0x40000000, 0x40001000, 0x40002000,
                                   0x40003000, 0x42005000, 0x40005000, 0x30000000};
    CHECK(p && scratch && tamper && out && saved && w);
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--dump")));
    for (unsigned alternate = 0; alternate < 2; alternate++)
        for (unsigned c = 0; c < 4; c++) {
            r = request();
            r.packed_argb = colors[c];
            if (alternate)
                r.clip_position[0][0][0] = 0xbf000000;
            CHECK(sgx535_task_pair_mask_set_packet_build(&r, p, scratch) == SGX535_TASK_BLOCKED);
            CHECK(p->profile == 7 && p->version == 8 && p->section_count == 16 &&
                  p->relocation_count == 36 && p->byte_count == 996);
            CHECK(p->missing_contracts == 0x7f &&
                  !memcmp(p->sections, sections, sizeof(sections)) &&
                  !memcmp(p->relocations, relocs, sizeof(relocs)));
            literal(expected, r.packed_argb, (int)alternate, 0);
            CHECK(pair_mask_packet_equal(p->bytes, expected, 0));
            CHECK(sgx535_task_pair_packet_validate(&r, p, scratch) == SGX535_TASK_BLOCKED);
            CHECK(sgx535_task_pair_packet_build(&r, tamper, scratch) == SGX535_TASK_BLOCKED &&
                  !memcmp(p, tamper, sizeof(*p)));
            unsigned population = 0;
            for (unsigned i = 0; i < 16; i++) {
                population += p->sections[i].byte_count;
                CHECK(p->sections[i].resource_byte_offset + p->sections[i].byte_count <= 4096);
                for (unsigned j = 0; j < i; j++)
                    if (p->sections[i].role == p->sections[j].role)
                        CHECK(p->sections[i].resource_byte_offset >=
                                  p->sections[j].resource_byte_offset + p->sections[j].byte_count ||
                              p->sections[j].resource_byte_offset >=
                                  p->sections[i].resource_byte_offset + p->sections[i].byte_count);
            }
            CHECK(population == 928);
            b.epoch = 3;
            b.bank_index[0] = 3;
            b.bank_index[1] = 4;
            for (unsigned i = 0; i < 8; i++)
                b.roles[i] = (struct sgx535_task_launch_role){
                    101 + i, va[i], p->requirements[i].minimum_bytes, p->requirements[i].access, 0};
            CHECK(sgx535_task_pair_launch_build(&r, p, &b, out, w) == SGX535_TASK_BLOCKED);
            CHECK(sgx535_task_pair_launch_validate(&r, p, &b, out, w) == SGX535_TASK_BLOCKED);
            literal(expected, r.packed_argb, (int)alternate, 1);
            CHECK(pair_mask_packet_equal(out->bytes, expected, 1));
            CHECK(out->resolved_relocations == 36 && out->ta_stream_va_bytes == 0x200002a0 &&
                  out->event_pds_va_bytes == 0x20000160);
            CHECK(out->missing_packet_contracts == 0x7f && out->missing_launch_contracts == 0x75);
            if (argc == 2 && !alternate && !c) {
                FILE *f;
                save("mask-request.bin", &r, sizeof(r));
                save("mask-packet.bin", p, sizeof(*p));
                save("mask-launch.bin", out, sizeof(*out));
                f = fopen("mask-layout.json", "w");
                CHECK(f);
                CHECK(
                    fprintf(
                        f,
                        "{\"scope\":\"CPU construction only\",\"packet_bytes\":%zu,\"request_bytes\":%zu,\"launch_bytes\":%zu,\"populated_bytes\":928,\"packet_profile\":7,\"sections\":16,\"relocations\":36}\n",
                        sizeof(*p), sizeof(r), sizeof(*out)) > 0);
                CHECK(!fclose(f));
            }
        }
    r = request();
    CHECK(sgx535_task_pair_packet_build(&r, p, scratch) == SGX535_TASK_BLOCKED);
    *saved = *out;
    /* Every packet byte and metadata byte is regenerated, including padding. */
    for (size_t i = 0; i < sizeof(*p); i++) {
        *tamper = *p;
        ((uint8_t *)tamper)[i] ^= 1;
        CHECK(sgx535_task_pair_packet_validate(&r, tamper, scratch) == SGX535_TASK_INVALID);
    }
    for (unsigned mode = 0; mode < 12; mode++) {
        bad = r;
        if (mode == 0)
            bad.width = bad.height = 64;
        if (mode == 1)
            bad.width = bad.height = 0;
        if (mode == 2)
            bad.profile = 6;
        if (mode == 3)
            bad.version = 3;
        if (mode == 4)
            bad.core_id++;
        if (mode == 5)
            bad.revision++;
        if (mode == 6)
            bad.reserved[1] = 1;
        if (mode == 7)
            bad.clip_position[0][0][3] = 0;
        if (mode == 8)
            bad.clip_position[1][1][0] = 0x7fc00001;
        if (mode == 9)
            bad.clip_position[0][1][1] = 0x7f800000;
        if (mode == 10)
            bad.clip_position[0][0][0] = 0x40000000;
        if (mode == 11)
            bad.height = 64;
        *tamper = *p;
        CHECK(sgx535_task_pair_mask_set_packet_build(&bad, tamper, scratch) < 0 &&
              !memcmp(tamper, p, sizeof(*p)));
    }
    CHECK(sgx535_task_pair_packet_build(&r, p, p) == SGX535_TASK_INVALID);
    CHECK(sgx535_task_pair_packet_build(&r, (struct sgx535_task_packet *)&r, scratch) ==
          SGX535_TASK_INVALID);
    for (unsigned mode = 0; mode < 5; mode++) {
        bad_b = b;
        if (mode == 0)
            bad_b.roles[0].gpu_va_bytes += 16;
        if (mode == 1)
            bad_b.roles[3].allocation_bytes = 159;
        if (mode == 2)
            bad_b.roles[3].storage_id = bad_b.roles[0].storage_id;
        if (mode == 3)
            bad_b.roles[3].gpu_va_bytes = bad_b.roles[0].gpu_va_bytes;
        if (mode == 4)
            bad_b.bank_index[1] = 16;
        *out = *saved;
        CHECK(sgx535_task_pair_launch_build(&r, p, &bad_b, out, w) < 0 &&
              !memcmp(out, saved, sizeof(*out)));
    }
    /* Correctly serialized profile7 cannot be relabeled as the old profile. */
    bad = r;
    bad.profile = 5;
    CHECK(sgx535_task_pair_packet_validate(&bad, p, scratch) == SGX535_TASK_INVALID);
    CHECK(sgx535_task_pair_mask_set_packet_build(&bad, tamper, scratch) == SGX535_TASK_UNSUPPORTED);
    CHECK(get(p->bytes + 800) == 0x54c1 && get(p->bytes + 848) == 0x54c5);
    printf("mask-set packet: %u checks; source/CPU only, all physical contracts retained\n",
           checks);
    free(w);
    free(saved);
    free(out);
    free(tamper);
    free(scratch);
    free(p);
    return 0;
}
