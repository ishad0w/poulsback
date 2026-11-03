// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_background_object.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static int dump_artifacts;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t w)
{
    p[0] = (uint8_t)w;
    p[1] = (uint8_t)(w >> 8);
    p[2] = (uint8_t)(w >> 16);
    p[3] = (uint8_t)(w >> 24);
}
static void save(const char *name, const void *bytes, size_t size)
{
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, size, file) == size);
    CHECK(fclose(file) == 0);
}
static struct sgx535_background_request request(unsigned side, uint32_t color)
{
    struct sgx535_background_request r = {SGX535_BACKGROUND_VERSION,
                                          SGX535_ENGINE121_CORE_ID,
                                          SGX535_ENGINE121_REVISION,
                                          side,
                                          side,
                                          color,
                                          {0, 0}};
    return r;
}
static struct sgx535_background_binding binding(void)
{
    struct sgx535_background_binding b = {0};
    unsigned i;
    b.epoch = UINT64_C(0x100000003);
    b.parameter_storage_id = UINT64_C(0x200000004);
    b.fragment_bank_index = 4;
    b.pds_exec_base = 0x20000000;
    b.bif_3d_req_base = 0x30000000;
    b.parameter_va = 0x30000000;
    b.parameter_bytes = 0x02000000;
    for (i = 0; i < 3; i++) {
        b.resources[i].storage_id = UINT64_C(0x300000005) + i;
        b.resources[i].allocation_bytes = 4096;
        b.resources[i].role = i + 1;
        b.resources[i].access = SGX535_TASK_ACCESS_READ;
    }
    b.resources[0].gpu_va_bytes = 0x40001000;
    b.resources[1].gpu_va_bytes = 0x20000000;
    b.resources[2].gpu_va_bytes = 0x32000000;
    return b;
}
/* Literal selected DRI17-word record; PDS addresses are unbound here. No
 * expected byte is obtained by calling the candidate encoder/linker. */
static void literal_image(const struct sgx535_background_request *r, uint32_t low, uint32_t high,
                          uint8_t bytes[SGX535_BACKGROUND_BYTES])
{
    static const uint32_t object[17] = {
        0, 0x00030000, 0x0c000000, 0,          0,          0,          0,          0,         0,
        2, 0,          0x40004000, 0x3f800000, 0x41004000, 0x3f800000, 0x40004100, 0x3f800000};
    unsigned i;
    memset(bytes, 0, SGX535_BACKGROUND_BYTES);
    put(bytes, low);
    put(bytes + 4, high);
    put(bytes + 64, 0x20);
    put(bytes + 80, 0x07000345);
    put(bytes + 84, 0xaf000000);
    put(bytes + 96, 0xaf000000);
    for (i = 0; i < 17; i++)
        put(bytes + 112 + i * 4, object[i]);
    if (r->width == 64) {
        put(bytes + 164, 0x44004000);
        put(bytes + 172, 0x40004400);
    }
}
static void image_metadata(const struct sgx535_background_image *x)
{
    static const struct sgx535_background_section sections[4] = {{1, 0, 8, 1, 64, 32},
                                                                 {2, 32, 56, 2, 768, 32},
                                                                 {3, 96, 4, 2, 832, 32},
                                                                 {4, 112, 68, 3, 0, 16}};
    static const struct sgx535_background_reloc relocs[5] = {
        {1, 1, 0, 1, 64, 8, 32, 0xf, 0},
        {2, 1, 0, 1, 64, 8, 32, 0xf0, 0},
        {2, 1, 0, 1, 64, 8, 32, 0x7ff00, 0},
        {3, 3, 0, 2, 832, 4, 32, 0xffffff, 4},
        {3, 3, 8, 2, 768, 56, 32, 0xffffff, 4}};
    CHECK(x->byte_count == 180 && x->section_count == 4 && x->relocation_count == 5);
    CHECK(x->missing_contracts == 15);
    CHECK(!memcmp(x->sections, sections, sizeof(sections)));
    CHECK(!memcmp(x->relocations, relocs, sizeof(relocs)));
}
static void expect_bind_refusal(const struct sgx535_background_request *r,
                                const struct sgx535_background_image *image,
                                const struct sgx535_background_binding *b)
{
    struct sgx535_background_bound out, saved, work;
    memset(&out, 0x5a, sizeof(out));
    saved = out;
    CHECK(sgx535_background_bind(r, image, b, &out, &work) < 0);
    CHECK(!memcmp(&out, &saved, sizeof(out)));
}
static void leaf(void)
{
    static const struct {
        uint32_t color, low, high;
    } colors[] = {{0, 0, 0xfca40001},
                  {0xffffffff, 0x001fffff, 0xfca7f1f1},
                  {0xff37a9d5, 0x0017a9d5, 0xfca7f191},
                  {0x01020304, 0x00020304, 0xfca40081},
                  {0x80000000, 0, 0xfca60001},
                  {1, 1, 0xfca40001}};
    struct sgx535_background_request r, bad;
    struct sgx535_background_image image, saved, work, tamper;
    struct sgx535_background_binding b, bad_binding;
    struct sgx535_background_bound bound, bound_work, bound_bad;
    uint8_t expected[SGX535_BACKGROUND_BYTES];
    unsigned side, c, i, j;
    for (side = 16; side <= 64; side *= 4)
        for (c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
            r = request(side, colors[c].color);
            CHECK(sgx535_background_build(&r, &image, &work) == SGX535_TASK_BLOCKED);
            image_metadata(&image);
            literal_image(&r, colors[c].low, colors[c].high, expected);
            CHECK(!memcmp(image.bytes, expected, sizeof(expected)));
            CHECK(sgx535_background_validate(&r, &image, &work) == SGX535_TASK_BLOCKED);
            b = binding();
            CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) ==
                  SGX535_TASK_BLOCKED);
            put(expected + 32, 0x00010404);
            put(expected + 36, 0);
            put(expected + 40, 0);
            put(expected + 112, 0x34);
            put(expected + 120, 0x0c000030);
            CHECK(!memcmp(bound.image.bytes, expected, sizeof(expected)));
            CHECK(bound.bgobjtag_word == 0x02200000 && bound.bgobj_word == 0x200 &&
                  bound.bgdepth_word == 0x3f800000);
            CHECK(bound.bank.bank_base_va_bytes == 0x40000000 &&
                  bound.bank.entry_offset_bytes == 0x1040);
            CHECK(bound.bank.register_offset_bytes == 0xa1c &&
                  bound.bank.register_word == 0x02800000);
            CHECK(bound.resolved_relocations == 5 && bound.image.missing_contracts == 15);
            CHECK(!memcmp(&bound.binding, &b, sizeof(b)));
            CHECK(sgx535_background_bound_validate(&r, &image, &b, &bound, &bound_work) ==
                  SGX535_TASK_BLOCKED);
            /* Destination5 consumes data0/1/8. Decode the literal first task word
         * in16-byte units, independently of the bank/link encoders. Moving
         * the low entry field into unused data2 dispatches the wrong entry. */
            CHECK(get(bound.image.bytes + 32) == 0x00010404);
            CHECK(0x40000000u + (((get(bound.image.bytes + 32) >> 4) & 15u) << 15) +
                      (((get(bound.image.bytes + 32) >> 8) & 2047u) << 4) ==
                  0x40001040u);
            CHECK(get(bound.image.bytes + 36) == 0 && get(bound.image.bytes + 40) == 0);
            bound_bad = bound;
            put(bound_bad.image.bytes + 32, 4);
            put(bound_bad.image.bytes + 40, 0x10400);
            CHECK(sgx535_background_bound_validate(&r, &image, &b, &bound_bad, &bound_work) ==
                  SGX535_TASK_INVALID);
            if (dump_artifacts && side == 16 && c == 0) {
                FILE *file;
                save("background-request.original.bin", &r, sizeof(r));
                save("background-image.original.bin", &image, sizeof(image));
                save("background-binding.original.bin", &b, sizeof(b));
                save("background-bound.original.bin", &bound, sizeof(bound));
                save("background-use.upload.original.bin", bound.image.bytes, 8);
                save("background-primary-pds.upload.original.bin", bound.image.bytes + 32, 56);
                save("background-secondary-pds.upload.original.bin", bound.image.bytes + 96, 4);
                save("background-rastgeom.upload.original.bin", bound.image.bytes + 112, 68);
                file = fopen("background-layout.original.json", "w");
                CHECK(file != NULL);
                CHECK(
                    fprintf(
                        file,
                        "{\"scope\":\"CPU construction only; no real owner or publication\",\"request_bytes\":%zu,\"image_bytes\":%zu,\"binding_bytes\":%zu,\"bound_bytes\":%zu,\"live_payload_bytes\":136,\"missing_contracts\":%u,\"bgobjtag\":%u,\"bgobj\":%u,\"bgdepth\":%u}\n",
                        sizeof(r), sizeof(image), sizeof(b), sizeof(bound),
                        bound.image.missing_contracts, bound.bgobjtag_word, bound.bgobj_word,
                        bound.bgdepth_word) > 0);
                CHECK(fclose(file) == 0);
            }
        }
    r = request(16, 0);
    CHECK(sgx535_background_build(&r, &image, &work) == SGX535_TASK_BLOCKED);
    /* Full canonical regeneration includes section metadata and all unused
     * bytes, rather than validating only convenient word subsets. */
    for (i = 0; i < sizeof(image); i++) {
        tamper = image;
        ((uint8_t *)&tamper)[i] ^= 1;
        CHECK(sgx535_background_validate(&r, &tamper, &work) == SGX535_TASK_INVALID);
        expect_bind_refusal(&r, &tamper, &(struct sgx535_background_binding){0});
    }
    b = binding();
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) == SGX535_TASK_BLOCKED);
    for (i = 0; i < sizeof(bound); i++) {
        bound_bad = bound;
        ((uint8_t *)&bound_bad)[i] ^= 1;
        CHECK(sgx535_background_bound_validate(&r, &image, &b, &bound_bad, &bound_work) ==
              SGX535_TASK_INVALID);
    }
    /* Nonzero PDS page offset, a different USE32KiB block, and a separate
     * RASTGEOM page: literal addresses expose ADD/subtraction mistakes. */
    b.resources[0].gpu_va_bytes = 0x40009000;
    b.resources[1].gpu_va_bytes = 0x20003000;
    b.resources[2].gpu_va_bytes = 0x32005000;
    b.fragment_bank_index = 7;
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) == SGX535_TASK_BLOCKED);
    CHECK(get(bound.image.bytes + 32) == 0x00010417 && get(bound.image.bytes + 36) == 0 &&
          get(bound.image.bytes + 40) == 0);
    CHECK(0x40000000u + (((get(bound.image.bytes + 32) >> 4) & 15u) << 15) +
              (((get(bound.image.bytes + 32) >> 8) & 2047u) << 4) ==
          0x40009040u);
    CHECK(get(bound.image.bytes + 112) == 0x334 && get(bound.image.bytes + 120) == 0x0c000330);
    CHECK(bound.bgobjtag_word == 0x02200500 && bound.bank.register_offset_bytes == 0xa28);
    b = binding();
    b.resources[0].gpu_va_bytes = 0x40018000;
    b.fragment_bank_index = 7;
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) == SGX535_TASK_BLOCKED);
    CHECK(get(bound.image.bytes + 32) == 0x437 && get(bound.image.bytes + 36) == 0 &&
          get(bound.image.bytes + 40) == 0);
    CHECK(0x40000000u + (((get(bound.image.bytes + 32) >> 4) & 15u) << 15) +
              (((get(bound.image.bytes + 32) >> 8) & 2047u) << 4) ==
          0x40018040u);
    b = binding();
    b.resources[2].gpu_va_bytes = 0x3ffff000;
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) == SGX535_TASK_BLOCKED);
    CHECK(bound.bgobjtag_word == 0x02ffff00);
    b = binding();
    b.resources[0].gpu_va_bytes = 0x40080000;
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound_work) == SGX535_TASK_BLOCKED);
    CHECK(bound.bank.register_word == 0x02801000 && get(bound.image.bytes + 32) == 0x404 &&
          get(bound.image.bytes + 36) == 0 && get(bound.image.bytes + 40) == 0);
    b = binding();
    for (i = 0; i < 3; i++)
        for (j = 0; j < 7; j++) {
            bad_binding = b;
            if (j == 0)
                bad_binding.resources[i].storage_id = 0;
            if (j == 1)
                bad_binding.resources[i].storage_id = b.parameter_storage_id;
            if (j == 2)
                bad_binding.resources[i].allocation_bytes = 0;
            if (j == 3)
                bad_binding.resources[i].allocation_bytes = 8192;
            if (j == 4)
                bad_binding.resources[i].gpu_va_bytes++;
            if (j == 5)
                bad_binding.resources[i].access |= SGX535_TASK_ACCESS_WRITE;
            if (j == 6)
                bad_binding.resources[i].role = 9;
            expect_bind_refusal(&r, &image, &bad_binding);
        }
    for (i = 0; i < 12; i++) {
        bad_binding = b;
        switch (i) {
        case 0:
            bad_binding.epoch = 0;
            break;
        case 1:
            bad_binding.resources[2].storage_id = b.resources[1].storage_id;
            break;
        case 2:
            bad_binding.resources[2].gpu_va_bytes = 0x31fff000;
            break;
        case 3:
            bad_binding.resources[2].gpu_va_bytes = 0x40000000;
            break;
        case 4: /* Past the bank end. */
            bad_binding.resources[0].gpu_va_bytes = 0x4ffff000;
            bad_binding.resources[0].allocation_bytes = 8192;
            break;
        case 5:
            bad_binding.fragment_bank_index = 2;
            break;
        case 6:
            bad_binding.fragment_bank_index = 16;
            break;
        case 7:
            bad_binding.bif_3d_req_base = 0x40000000;
            break;
        case 8:
            bad_binding.pds_exec_base = 0x20003000;
            break;
        case 9:
            bad_binding.parameter_bytes += 4096;
            break;
        case 10:
            bad_binding.parameter_va = 0x30001000;
            break;
        default:
            bad_binding.reserved = 1;
            break;
        }
        expect_bind_refusal(&r, &image, &bad_binding);
    }
    for (i = 0; i < 6; i++) {
        bad = r;
        if (i == 0)
            bad.version++;
        if (i == 1)
            bad.core_id++;
        if (i == 2)
            bad.revision++;
        if (i == 3)
            bad.width = 32;
        if (i == 4)
            bad.height = 64;
        if (i == 5)
            bad.reserved[1] = 1;
        memset(&saved, 0x3c, sizeof(saved));
        tamper = saved;
        CHECK(sgx535_background_build(&bad, &tamper, &work) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(&saved, &tamper, sizeof(saved)));
    }
    saved = image;
    CHECK(sgx535_background_build(&r, &image, &image) == SGX535_TASK_INVALID);
    CHECK(!memcmp(&saved, &image, sizeof(image)));
    CHECK(sgx535_background_bind(&r, &image, &b, &bound, &bound) == SGX535_TASK_INVALID);
    CHECK(sgx535_background_bind(&r, &image, &b, (struct sgx535_background_bound *)(void *)&image,
                                 &bound_work) == SGX535_TASK_INVALID);
}
static uint32_t float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static struct sgx535_task_pair_request pair_request(unsigned side, unsigned different)
{
    static const int xy[2][6][2] = {{{2, 2}, {14, 2}, {13, 12}, {2, 2}, {13, 12}, {2, 14}},
                                    {{1, 3}, {12, 3}, {3, 13}, {6, 1}, {15, 8}, {6, 15}}};
    struct sgx535_task_pair_request p = {0};
    unsigned i;
    p.version = 8;
    p.profile = 5;
    p.core_id = SGX535_ENGINE121_CORE_ID;
    p.revision = SGX535_ENGINE121_REVISION;
    p.width = p.height = side;
    p.packed_argb = different ? 0x193c7ae2 : 0xff37a9d5;
    for (i = 0; i < 6; i++) {
        unsigned x = (unsigned)xy[different][i][0], y = (unsigned)xy[different][i][1];
        p.clip_position[i / 3][i % 3][0] = float_bits((float)x / 8.0f - 1.0f);
        p.clip_position[i / 3][i % 3][1] = float_bits(1.0f - (float)y / 8.0f);
        p.clip_position[i / 3][i % 3][2] = 0;
        p.clip_position[i / 3][i % 3][3] = 0x3f800000;
    }
    return p;
}
static void pair_scene(void)
{
    struct sgx535_background_pair_workspace *work = malloc(sizeof(*work));
    struct sgx535_background_pair_image *out = malloc(sizeof(*out)),
                                        *saved = malloc(sizeof(*saved));
    struct sgx535_task_packet *base = malloc(sizeof(*base)), *scratch = malloc(sizeof(*scratch));
    struct sgx535_background_request bg;
    struct sgx535_task_pair_request pair;
    unsigned side, different, i;
    CHECK(work && out && saved && base && scratch);
    for (side = 16; side <= 64; side *= 4)
        for (different = 0; different < 2; different++) {
            pair = pair_request(side, different);
            bg = request(side, different ? 0x7f214365 : 0);
            CHECK(sgx535_background_pair_build(&pair, &bg, out, work) == SGX535_TASK_BLOCKED);
            CHECK(sgx535_background_pair_validate(&pair, &bg, out, work) == SGX535_TASK_BLOCKED);
            CHECK(sgx535_task_pair_packet_build(&pair, base, scratch) == SGX535_TASK_BLOCKED);
            CHECK(!memcmp(base, &out->triangles, sizeof(*base)));
            CHECK(out->triangles.version == 8 && out->triangles.profile == 5 &&
                  out->triangles.section_count == 16);
            CHECK(out->triangles.missing_contracts == SGX535_PACKET_MISSING_ALL);
            CHECK(out->background.missing_contracts == 15);
            if (dump_artifacts && side == 16 && !different) {
                save("pair16-request.original.bin", &pair, sizeof(pair));
                save("pair16-base-packet.original.bin", base, sizeof(*base));
                save("pair16-composed-packet.original.bin", out, sizeof(*out));
            }
            *saved = *out;
            for (i = 0; i < 16; i++) {
                unsigned index = out->triangles.sections[i].byte_offset;
                out->triangles.bytes[index] ^= 1;
                CHECK(sgx535_background_pair_validate(&pair, &bg, out, work) ==
                      SGX535_TASK_INVALID);
                *out = *saved;
            }
            pair.clip_position[0][1][3] = 0;
            *saved = *out;
            CHECK(sgx535_background_pair_build(&pair, &bg, out, work) < 0);
            CHECK(!memcmp(saved, out, sizeof(*out)));
        }
    pair = pair_request(16, 0);
    bg = request(64, 0);
    *saved = *out;
    CHECK(sgx535_background_pair_build(&pair, &bg, out, work) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(saved, out, sizeof(*out)));
    CHECK(sgx535_background_pair_build(&pair, &bg,
                                       (struct sgx535_background_pair_image *)(void *)work,
                                       work) == SGX535_TASK_INVALID);
    free(scratch);
    free(base);
    free(saved);
    free(out);
    free(work);
}
int main(void)
{
    const char *dump = getenv("POULSBACK_BACKGROUND_DUMP");
    dump_artifacts = dump && !strcmp(dump, "1");
    leaf();
    pair_scene();
    printf("background object/program + actual packet8 scene: %u checks PASS (CPU only)\n", checks);
    return 0;
}
