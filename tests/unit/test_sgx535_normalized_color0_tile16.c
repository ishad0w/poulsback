// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
#include "sgx535_normalized_color0_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static int fail_alloc;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
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
static void put(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> (i * 8));
}
static const struct sgx535_task_packet_section sections[16] = {
    {1, 0, 96, 3, 0, 16},      {2, 96, 8, 3, 128, 2},     {3, 112, 16, 1, 0, 32},
    {4, 128, 144, 2, 0, 32},   {5, 272, 48, 1, 32, 32},   {6, 320, 32, 2, 160, 32},
    {7, 352, 64, 0, 0, 32},    {8, 416, 60, 0, 64, 32},   {14, 480, 4, 0, 128, 32},
    {9, 496, 60, 0, 160, 32},  {15, 560, 60, 0, 224, 32}, {10, 624, 148, 0, 288, 32},
    {11, 784, 40, 0, 448, 16}, {16, 832, 8, 0, 496, 16},  {12, 848, 16, 0, 512, 16},
    {13, 864, 40, 0, 544, 16}};
#define A(s, o, r, t, n, a, m, x, y) {1, s, o, r, t, n, a, m, x, y}
#define U(s, o, r, t, n)                                                                           \
    {2, s, o, r, t, n, 16, 15, 0, 0}, {3, s, o, r, t, n, 16, 240, 15, 4},                          \
        {3, s, o, r, t, n, 16, 0x7ff00, 4, 8}
static const struct sgx535_task_packet_reloc relocs[29] = {
    A(6, 0, 3, 0, 96, 16, 0xffffffffu, 0, 0),
    U(6, 16, 1, 0, 16),
    U(7, 0, 2, 0, 144),
    A(9, 0, 0, 448, 40, 16, 0xffffffffu, 0, 0),
    U(9, 8, 1, 32, 16),
    A(10, 0, 0, 496, 8, 16, 0xffffffffu, 0, 0),
    U(10, 8, 1, 64, 16),
    A(12, 8, 0, 128, 4, 32, 0xffffff, 4, 0),
    A(12, 16, 0, 64, 60, 32, 0xffffff, 4, 0),
    A(11, 8, 0, 512, 16, 16, 0xffffffffu, 0, 0),
    U(11, 0, 2, 160, 8),
    U(11, 16, 2, 176, 16),
    A(14, 4, 5, 0, 2048, 4, 0xfffffffcu, 2, 2),
    A(15, 0, 0, 160, 60, 32, 0xfffffff, 4, 0),
    A(15, 12, 3, 128, 6, 2, 0xffffffffu, 0, 0),
    A(15, 20, 0, 0, 64, 32, 0xfffffff, 4, 0),
    A(15, 28, 0, 224, 60, 32, 0xfffffff, 4, 0)};
#undef A
#undef U
static const uint32_t fragment[36] = {
    0xa0000100, 0x28a10000, 0xa0200080, 0x28a10000, 0xa0400000, 0x28a10000, 0xa0600180, 0x28a10000,
    0x00800000, 0xfca00000, 0x00a00000, 0xfca0f1c0, 0x00c00004, 0x18a00200, 0x00000305, 0x18a00000,
    0x00c00084, 0x18a00200, 0x00200305, 0x18a00000, 0x00c00104, 0x18a00200, 0x00400305, 0x18a00000,
    0x00c00184, 0x18a00200, 0x00600305, 0x18a00000, 0x20040000, 0x40a10c11, 0x20040080, 0x40a10c09,
    0x20040100, 0x40a10c05, 0x20040180, 0x40a50c21};
static struct sgx535_normalized_color0_tile16_request
request(struct sgx535_fragment_instruction *ir)
{
    struct sgx535_normalized_color0_tile16_request r = {
        .version = 10,
        .profile = 8,
        .core_id = 0x01130000,
        .revision = 0x00010201,
        .interpolation = 1,
        .fragment = ir,
        .fragment_count = 8,
        .clip_position = {{0xbf400000, 0x3f400000, 0, 0x3f800000},
                          {0x3f400000, 0x3f400000, 0, 0x3f800000},
                          {0x3f200000, 0xbf000000, 0, 0x3f800000}},
        .rgba = {{0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000},
                 {0x3f000000, 0x3f400000, 0x3e800000, 0x3f800000},
                 {0x3f400000, 0x3e800000, 0x3f000000, 0x3f800000}}};
    memset(ir, 0, 512 * sizeof(*ir));
    for (unsigned i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4].index = i;
        ir[i + 4].source[0] = i;
    }
    return r;
}
static void words(uint8_t *bytes, unsigned section, const uint32_t *w, size_t n)
{
    CHECK(n * 4 == sections[section].byte_count);
    for (size_t i = 0; i < n; i++)
        put(bytes + sections[section].byte_offset + i * 4, w[i]);
}
static void literal(uint8_t bytes[2048], const struct sgx535_normalized_color0_tile16_request *r,
                    int bound)
{
    uint32_t v[24] = {0x40000000, 0x40000000, 0x3f000000, 0x3f800000, 0, 0, 0, 0,
                      0x41600000, 0x40000000, 0x3f000000, 0x3f800000, 0, 0, 0, 0,
                      0x41500000, 0x41400000, 0x3f000000, 0x3f800000, 0, 0, 0, 0};
    const uint32_t index[2] = {0x00010000, 2};
    const uint32_t vu[4] = {0xa0000000, 0x28a17001, 0xa0200000, 0xfb275000};
    const uint32_t su[12] = {0xa0000000, 0x28a19001, 0xa0200500, 0xfb274000, 0,         0, 0,
                             0,          0xa0000000, 0x28a11001, 0xa0200100, 0xfb274000};
    uint32_t vp[16] = {0,  0x80e00007, 0, 0, 0x00200000, 0,          0,          0,
                       32, 0,          0, 0, 0x67800070, 0x2f030343, 0x030803e5, 0xaf000000};
    uint32_t fp[15] = {0x80000, 0x38000000, 0, 0, 0,          0,          0,         0,
                       0x20,    0x0fd0a00f, 0, 0, 0x07000345, 0x070418a2, 0xaf000000};
    const uint32_t second[1] = {0xaf000000};
    uint32_t sp[15] = {0, 0x81200009, 0x00200000, 0, 0,          0,          0,         0,
                       0, 0,          0,          0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t tp[15] = {0, 0x80200001, 0x00200000, 0, 0,          0,          0,         0,
                       0, 0,          0,          0, 0x07030223, 0x07042345, 0xaf000000};
    uint32_t mte[10] = {0x54c1, 0x01d00000, 0,          0x30030004, 0x0c000000,
                        0,      0,          0x08001800, 0x10000,    0};
    const uint32_t term[2] = {0x2000, 0x10001};
    uint32_t pbe[4] = {0xf8000, 0, 0, 0xf00f};
    uint32_t ta[10] = {0x40000000, 0x0c02a203, 0x81400003, 0,          0,
                       0,          0x04000203, 0x60000000, 0x0c02a201, 0xc0000000};
    for (unsigned i = 0; i < 3; i++)
        memcpy(v + i * 8 + 4, r->rgba[i], 16);
    /* USE41010000 has bank base41000000: entry units1000 split to high20
  * and low0. Offsets+32/+64 add low200/400, not a byte-address field. */
    if (bound) {
        vp[0] = 0x43003000;
        vp[4] = 0x00200023;
        fp[0] = 0x000a002f;
        sp[0] = 0x200031c0;
        sp[2] = 0x00200223;
        tp[0] = 0x200031f0;
        tp[2] = 0x00200423;
        mte[2] = 0x00000308;
        mte[4] = 0x0c000304;
        pbe[1] = 0x45000000;
        ta[0] = 0x4200030a;
        ta[3] = 0x43003080;
        ta[5] = 0x02000300;
        ta[7] = 0x6200030e;
    }
    memset(bytes, 0, 2048);
    words(bytes, 0, v, 24);
    words(bytes, 1, index, 2);
    words(bytes, 2, vu, 4);
    words(bytes, 3, fragment, 36);
    words(bytes, 4, su, 12);
    words(bytes, 6, vp, 16);
    words(bytes, 7, fp, 15);
    words(bytes, 8, second, 1);
    words(bytes, 9, sp, 15);
    words(bytes, 10, tp, 15);
    words(bytes, 12, mte, 10);
    words(bytes, 13, term, 2);
    words(bytes, 14, pbe, 4);
    words(bytes, 15, ta, 10);
}
static int tile16_packet_equal(const uint8_t *actual, const uint8_t *expected, int bound)
{
    return sgx535_pixel_event_reference_packet(
        actual, expected, 2048, sections[11].byte_offset, sections[5].byte_offset, 1,
        bound ? 0x20003200u : 0, bound ? 0x00020a2fu : 0, bound ? 0x00220b2fu : 0x00200000u);
}

static void masks(const struct sgx535_normalized_color0_task *p,
                  const struct sgx535_normalized_color0_launch *l)
{
    CHECK(p->packet.version == 10 && p->packet.profile == 8 &&
          p->packet.missing_contracts == 0x1ff);
    CHECK(p->fragment.missing_contracts == 0x3ff && p->fragment.reference_policy == 1);
    CHECK(!p->fragment.arithmetic_qualified && !p->fragment.numerical_qualified &&
          !p->fragment.linkage_qualified);
    CHECK(!p->fragment.output_qualified && !p->fragment.hardware_qualified &&
          !p->fragment.nonfinite_behavior_qualified);
    CHECK(sgx535_fragment_normalized_native_admission(&p->fragment) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(l->launch.packet_version == 10 && l->launch.missing_packet_contracts == 0x1ff &&
          l->launch.missing_launch_contracts == 0x75);
    CHECK(l->fragment_missing_contracts == 0x3ff && l->reference_policy == 1 &&
          !l->arithmetic_qualified && !l->numerical_qualified);
    CHECK(!l->linkage_qualified && !l->output_qualified && !l->hardware_qualified &&
          !l->nonfinite_behavior_qualified);
}
static void save(const char *name, const void *p, size_t bytes)
{
    FILE *f = fopen(name, "wb");
    CHECK(f);
    CHECK(fwrite(p, 1, bytes, f) == bytes);
    CHECK(!fclose(f));
}
int main(int argc, char **argv)
{
    struct sgx535_fragment_instruction *ir = calloc(512, sizeof(*ir));
    struct sgx535_normalized_color0_tile16_request r, bad;
    struct sgx535_normalized_color0_task *p = calloc(1, sizeof(*p)), *saved = calloc(1, sizeof(*p)),
                                         *mut = calloc(1, sizeof(*p));
    struct sgx535_normalized_color0_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_normalized_color0_launch *l = calloc(1, sizeof(*l)),
                                           *saved_l = calloc(1, sizeof(*l));
    struct sgx535_normalized_color0_launch_workspace *lw = calloc(1, sizeof(*lw));
    struct sgx535_task_launch_binding b = {0}, bad_b;
    uint8_t expected[2048];
    const uint32_t bases[8] = {0x20003000, 0x41010000, 0x42012000, 0x43003000,
                               0x44000000, 0x45000000, 0x46000000, 0x30000000};
    CHECK(ir && p && saved && mut && w && l && saved_l && lw);
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--dump")));
    r = request(ir);
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_tile16_task_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->packet.width == 16 && p->packet.height == 16 && p->packet.stride_bytes == 128 &&
          p->packet.byte_count == 904);
    CHECK(p->packet.requirements[5].minimum_bytes == 4096 &&
          p->packet.requirements[4].minimum_bytes == 8192);
    CHECK(p->packet.section_count == 16 && p->packet.relocation_count == 29);
    CHECK(!memcmp(p->packet.sections, sections, sizeof(sections)));
    CHECK(!memcmp(p->packet.relocations, relocs, sizeof(relocs)));
    literal(expected, &r, 0);
    CHECK(tile16_packet_equal(p->packet.bytes, expected, 0));
    CHECK(p->fragment_code_bytes == 144 && p->fragment_temp_words == 7 &&
          p->dms_temp_allocation_bytes == 32);
    CHECK(p->event_use_offset_bytes == 160 && p->event_use_bytes == 32 &&
          p->fragment_use_extent_bytes == 192);
    b.epoch = 31;
    b.bank_index[0] = 3;
    b.bank_index[1] = 15;
    for (unsigned i = 0; i < 8; i++)
        b.roles[i] = (struct sgx535_task_launch_role){101 + i, bases[i],
                                                      p->packet.requirements[i].minimum_bytes,
                                                      p->packet.requirements[i].access, 0};
    CHECK(sgx535_normalized_color0_tile16_launch_build(&r, p, &b, l, lw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_tile16_launch_validate(&r, p, &b, l, lw) == SGX535_TASK_BLOCKED);
    literal(expected, &r, 1);
    CHECK(tile16_packet_equal(l->launch.bytes, expected, 1));
    masks(p, l);
    CHECK(l->launch.resolved_relocations == 29 && l->launch.ta_stream_va_bytes == 0x20003220 &&
          l->launch.event_pds_va_bytes == 0x20003120);
    CHECK(l->launch.stages[0].count == 3 && l->launch.stages[1].count == 7 &&
          l->launch.stages[2].count == 26);
    unsigned found = 0;
    for (unsigned i = 0; i < l->launch.stages[2].count; i++)
        if (l->launch.stages[2].actions[i].offset == 0x410) { /* RENDBOX2 0, as EMGD */
            CHECK(l->launch.stages[2].actions[i].value == 0);
            found++;
        }
    CHECK(found == 1);
    *saved = *p;
    *saved_l = *l;
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_task_packet_section *s = &p->packet.sections[i];
        CHECK(s->byte_offset + s->byte_count <= 904 &&
              s->resource_byte_offset + s->byte_count <=
                  p->packet.requirements[s->role].minimum_bytes);
        for (unsigned j = 0; j < i; j++)
            if (s->role == p->packet.sections[j].role) {
                const struct sgx535_task_packet_section *t = &p->packet.sections[j];
                CHECK(s->resource_byte_offset >= t->resource_byte_offset + t->byte_count ||
                      t->resource_byte_offset >= s->resource_byte_offset + s->byte_count);
            }
    }
    if (argc == 2) {
        save("tile16-request.bin", &r, sizeof(r));
        save("tile16-task.bin", p, sizeof(*p));
        save("tile16-launch.bin", l, sizeof(*l));
    }
    /* Arbitrary endpoint/fraction tuples are consumed through the ordinary IR,
  * never recognized as an application or replaced by a constant fragment. */
    for (unsigned i = 0; i < 12; i++) {
        r = request(ir);
        r.rgba[i % 3][i % 4] = i & 1 ? 0x3f123456 : 0x00000001;
        CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
        literal(expected, &r, 0);
        CHECK(tile16_packet_equal(p->packet.bytes, expected, 0));
    }
    r = request(ir);
    r.interpolation = 2;
    memcpy(r.rgba[1], r.rgba[0], 16);
    memcpy(r.rgba[2], r.rgba[0], 16);
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
    literal(expected, &r, 0);
    CHECK(tile16_packet_equal(p->packet.bytes, expected, 0));
    r.clip_position[0][0] = 0xbf000000;
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(get(p->packet.bytes) == 0x40800000 &&
          !memcmp(p->packet.bytes + 4, saved->packet.bytes + 4, 12));
    /* Generic arithmetic: four INPUTs, CONSTANT, MAD, MIN, four OUTPUTs. */
    r = request(ir);
    r.fragment_count = 11;
    ir[4] =
        (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x3f000000, 0, 0, {0, 0, 0}};
    ir[5] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MAD, 0, 0, 0, {0, 4, 1}};
    ir[6] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MIN, 0, 0, 0, {5, 2, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[7 + i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_OUTPUT, 0, i, 0, {i == 0 ? 6 : i, 0, 0}};
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->fragment_code_bytes > 144 && p->event_use_offset_bytes >= p->fragment_code_bytes);
    CHECK(sgx535_normalized_color0_tile16_launch_build(&r, p, &b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    for (unsigned i = 0; i < p->packet.relocation_count; i++)
        if (p->packet.relocations[i].role == 2) {
            const struct sgx535_task_packet_reloc *q = &p->packet.relocations[i];
            unsigned k = p->packet.sections[q->section].kind;
            CHECK(k == 8 ? q->span_bytes == p->fragment_code_bytes
                         : q->resource_byte_offset ==
                               p->event_use_offset_bytes + (q->section_byte_offset ? 16 : 0));
        }
    r = request(ir);
    *p = *saved;
    *l = *saved_l;
    for (unsigned mode = 0; mode < 23; mode++) {
        bad = r;
        if (mode == 0)
            bad.version = 9;
        if (mode == 1)
            bad.profile = 6;
        if (mode == 2)
            bad.core_id++;
        if (mode == 3)
            bad.revision++;
        if (mode == 4)
            bad.reserved[0] = 1;
        if (mode == 5)
            bad.reserved[1] = 1;
        if (mode == 6)
            bad.reserved[2] = 1;
        if (mode == 7)
            bad.interpolation = 0;
        if (mode == 8)
            bad.interpolation = 3;
        if (mode == 9)
            bad.interpolation = 2;
        if (mode == 10)
            bad.clip_position[0][3] = 0x40000000;
        if (mode == 11)
            bad.clip_position[1][0] = 0x3f800001;
        if (mode == 12)
            bad.clip_position[2][1] = 0xff800000;
        if (mode == 13)
            bad.clip_position[0][2] = 0x7fc01234;
        if (mode == 14)
            bad.rgba[0][0] = 0x80000000;
        if (mode == 15)
            bad.rgba[1][1] = 0x7f800000;
        if (mode == 16)
            bad.rgba[2][2] = 0x7fc01234;
        if (mode == 17)
            bad.rgba[0][3] = 0x3f800001;
        if (mode == 18)
            bad.rgba[1][0] = 0xbf000000;
        if (mode == 19)
            bad.fragment_count = 4;
        if (mode == 20)
            bad.fragment_count = 513;
        if (mode == 21)
            bad.fragment = NULL;
        if (mode == 22)
            bad.fragment = (void *)(~(unsigned long)0 - 3u);
        enum sgx535_task_status expected_status =
            mode >= 19 ? SGX535_TASK_INVALID : SGX535_TASK_UNSUPPORTED;
        CHECK(sgx535_normalized_color0_tile16_task_build(&bad, p, w) == expected_status);
        CHECK(!memcmp(p, saved, sizeof(*p)));
        CHECK(sgx535_normalized_color0_tile16_launch_build(&bad, p, &b, l, lw) == expected_status);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
    }
    for (size_t i = 0; i < sizeof(*p); i += 53) {
        *mut = *p;
        ((uint8_t *)mut)[i] ^= 1;
        CHECK(sgx535_normalized_color0_tile16_task_validate(&r, mut, w) == SGX535_TASK_UNSUPPORTED);
        CHECK(sgx535_normalized_color0_tile16_launch_build(&r, mut, &b, l, lw) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
    }
    for (unsigned i = 0; i < 8; i++) {
        bad_b = b;
        bad_b.roles[i].storage_id = 0;
        CHECK(sgx535_normalized_color0_tile16_launch_build(&r, p, &bad_b, l, lw) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
    }
    for (size_t i = 0; i < sizeof(*l); i += 43) {
        ((uint8_t *)l)[i] ^= 1;
        CHECK(sgx535_normalized_color0_tile16_launch_validate(&r, p, &b, l, lw) ==
              SGX535_TASK_INVALID);
        ((uint8_t *)l)[i] ^= 1;
    }
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, (void *)p) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, (void *)&r, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_tile16_launch_build(&r, p, &b, l, (void *)l) ==
          SGX535_TASK_INVALID);
    fail_alloc = 1;
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_NOMEM);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    fail_alloc = 0;
    ir[3].source[2] = 1;
    CHECK(sgx535_normalized_color0_tile16_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    printf("normalized COLOR0 fixed16: %u checks; CPU construction only, native admission closed\n",
           checks);
    free(lw);
    free(saved_l);
    free(l);
    free(w);
    free(mut);
    free(saved);
    free(p);
    free(ir);
    return 0;
}
