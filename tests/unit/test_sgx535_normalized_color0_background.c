// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Reuse the independent complete foreground byte/relocation oracle. */
#define main normalized_tile16_base_fixture_main
#include "test_sgx535_normalized_color0_tile16.c"
#undef main
#include "sgx535_normalized_color0_background.h"

static void scene_binding(const struct sgx535_normalized_color0_background_image *p,
                          struct sgx535_task_launch_binding *b,
                          struct sgx535_background_binding *bb)
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
    bb->epoch = b->epoch;
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
static void background_literal(uint32_t words[48], uint32_t color, int bound)
{
    memset(words, 0, 192);
    words[0] = color & 0x1fffff;
    words[1] = 0xfca40001u | ((color >> 21) & 31u) << 4 | ((color >> 26) & 63u) << 12;
    words[16] = 0x20;
    words[20] = 0x07000345;
    words[21] = 0xaf000000;
    words[24] = 0xaf000000;
    words[29] = 0x30000;
    words[30] = 0x0c000000;
    words[37] = 2;
    words[39] = 0x40004000;
    words[40] = words[42] = words[44] = 0x3f800000;
    words[41] = 0x41004000;
    words[43] = 0x40004100;
    if (bound) {
        words[8] = 0x00016004;
        words[28] = 0x34;
        words[30] = 0x0c000030;
    }
}
static void check_background(const struct sgx535_background_image *p, uint32_t color, int bound)
{
    const struct sgx535_background_section s[4] = {{1, 0, 8, 1, 1536, 32},
                                                   {2, 32, 56, 2, 768, 32},
                                                   {3, 96, 4, 2, 832, 32},
                                                   {4, 112, 68, 3, 0, 16}};
    const struct sgx535_background_reloc r[5] = {{1, 1, 0, 1, 1536, 8, 32, 15, 0},
                                                 {2, 1, 0, 1, 1536, 8, 32, 240, 0},
                                                 {2, 1, 0, 1, 1536, 8, 32, 0x7ff00, 0},
                                                 {3, 3, 0, 2, 832, 4, 32, 0xffffff, 4},
                                                 {3, 3, 8, 2, 768, 56, 32, 0xffffff, 4}};
    uint32_t expected[48];
    background_literal(expected, color, bound);
    CHECK(p->version == 2 && p->width == 16 && p->height == 16 && p->packed_argb == color &&
          p->missing_contracts == 15);
    CHECK(p->byte_count == 180 && p->section_count == 4 && p->relocation_count == 5);
    CHECK(!memcmp(p->sections, s, sizeof(s)) && !memcmp(p->relocations, r, sizeof(r)));
    for (unsigned i = 0; i < 48; i++)
        CHECK(get(p->bytes + i * 4) == expected[i]);
}
static void disjoint_sections(const struct sgx535_normalized_color0_background_image *p)
{
    for (unsigned i = 0; i < 4; i++) {
        const struct sgx535_background_section *a = &p->background.sections[i];
        CHECK(a->resource_offset + a->byte_count <= 4096);
        if (a->role == 3)
            continue;
        unsigned role = a->role == 1 ? 2 : 0;
        for (unsigned j = 0; j < 16; j++) {
            const struct sgx535_task_packet_section *b = &p->foreground.packet.sections[j];
            if (role == b->role)
                CHECK(a->resource_offset >= b->resource_byte_offset + b->byte_count ||
                      b->resource_byte_offset >= a->resource_offset + a->byte_count);
        }
    }
    CHECK(p->foreground.fragment_use_extent_bytes <= 1536);
}
int main(int argc, char **argv)
{
    char *base_argv[1] = {(char *)"base"};
    CHECK(normalized_tile16_base_fixture_main(1, base_argv) == 0);
    struct sgx535_fragment_instruction *ir = calloc(512, sizeof(*ir));
    struct sgx535_normalized_color0_background_request r = {.version = 1,
                                                            .packed_background_argb = 0xff1d2743},
                                                       bad;
    struct sgx535_normalized_color0_background_image *p = calloc(1, sizeof(*p)),
                                                     *saved = calloc(1, sizeof(*p)),
                                                     *mut = calloc(1, sizeof(*p));
    struct sgx535_normalized_color0_background_bound *out = calloc(1, sizeof(*out)),
                                                     *saved_out = calloc(1, sizeof(*out));
    struct sgx535_normalized_color0_background_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_normalized_color0_launch_workspace *standalone = calloc(1, sizeof(*standalone));
    struct sgx535_task_launch_binding b, bad_b;
    struct sgx535_background_binding bb, bad_bb;
    uint8_t expected[2048];
    unsigned found = 0;
    CHECK(ir && p && saved && mut && out && saved_out && w && standalone);
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--dump")));
    r.foreground = request(ir);
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_background_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->version == 1 && p->missing_background_contracts == 15);
    literal(expected, &r.foreground, 0);
    CHECK(tile16_packet_equal(p->foreground.packet.bytes, expected, 0));
    check_background(&p->background, r.packed_background_argb, 0);
    disjoint_sections(p);
    scene_binding(p, &b, &bb);
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, out, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_background_bound_validate(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_BLOCKED);
    masks(&p->foreground, &out->launch);
    CHECK(out->version == 1 && out->missing_background_contracts == 15);
    check_background(&out->background.image, r.packed_background_argb, 1);
    CHECK(out->background.bgobjtag_word == 0x02200000 && out->background.bgobj_word == 0x200 &&
          out->background.bgdepth_word == 0x3f800000);
    CHECK(out->background.resolved_relocations == 5 &&
          out->background.bank.bank_base_va_bytes == 0x40000000 &&
          out->background.bank.entry_offset_bytes == 0x1600);
    CHECK(!memcmp(&out->background.binding, &bb, sizeof(bb)));
    for (unsigned i = 0; i < out->launch.launch.stages[2].count; i++) {
        const struct sgx535_engine121_action *a = &out->launch.launch.stages[2].actions[i];
        if (a->offset == 0x4c4) {
            CHECK(a->value == 0x02200000);
            found++;
        }
    }
    CHECK(found == 1);
    /* The foreground source and launch code are still exactly its task10. Only
  * the canonical background words enter raster-prefix construction. */
    CHECK(sgx535_normalized_color0_tile16_launch_build(&r.foreground, &p->foreground, &b,
                                                       &w->result.launch,
                                                       standalone) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(out->launch.launch.bytes, w->result.launch.launch.bytes, 2048));
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < out->launch.launch.stages[i].count; j++) {
            const struct sgx535_engine121_action *a = &out->launch.launch.stages[i].actions[j];
            const struct sgx535_engine121_action *c = &w->result.launch.launch.stages[i].actions[j];
            if (a->offset == 0x4c4 && i == 2)
                CHECK(c->value == 0);
            else
                CHECK(!memcmp(a, c, sizeof(*a)));
        }
    *saved = *p;
    *saved_out = *out;
    if (argc == 2) {
        save("color0-background-image.bin", p, sizeof(*p));
        save("color0-background-bound.bin", out, sizeof(*out));
    }
    for (size_t i = 0; i < sizeof(*p); i += 41) {
        *mut = *p;
        ((uint8_t *)mut)[i] ^= 1;
        CHECK(sgx535_normalized_color0_background_validate(&r, mut, w) == SGX535_TASK_INVALID);
        CHECK(sgx535_normalized_color0_background_bind(&r, mut, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    for (size_t i = 0; i < sizeof(*out); i += 43) {
        ((uint8_t *)out)[i] ^= 1;
        CHECK(sgx535_normalized_color0_background_bound_validate(&r, p, &b, &bb, out, w) ==
              SGX535_TASK_INVALID);
        ((uint8_t *)out)[i] ^= 1;
    }
    for (unsigned mode = 0; mode < 16; mode++) {
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
            bad_bb.resources[2].gpu_va_bytes = 0x32001000;
        if (mode == 10)
            bad_bb.resources[2].access = 3;
        if (mode == 11)
            bad_bb.resources[2].allocation_bytes = 8192;
        if (mode == 12)
            bad_b.roles[1].gpu_va_bytes = 0x32000000;
        if (mode == 13)
            bad_bb.pds_exec_base = 0x20000020;
        if (mode == 14)
            bad_bb.bif_3d_req_base = 0x32000000;
        if (mode == 15)
            bad_b.roles[0].allocation_bytes = 8192;
        CHECK(sgx535_normalized_color0_background_bind(&r, p, &bad_b, &bad_bb, out, w) < 0);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    /* Request/image identity, not an ordinary pair-struct cast or policy flag. */
    for (unsigned mode = 0; mode < 4; mode++) {
        bad = r;
        if (mode == 0)
            bad.version = 0;
        if (mode == 1)
            bad.reserved[0] = 1;
        if (mode == 2)
            bad.foreground.profile = 7;
        if (mode == 3)
            bad.foreground.clip_position[2][3] = 0;
        CHECK(sgx535_normalized_color0_background_build(&bad, p, w) < 0);
        CHECK(!memcmp(p, saved, sizeof(*p)));
    }
    CHECK(sgx535_normalized_color0_background_build(&r, p, (void *)p) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, (void *)&bb, w) ==
          SGX535_TASK_INVALID);
    fail_alloc = 1;
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_NOMEM);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    fail_alloc = 0;
    ir[4].source[0] = 3;
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, out, w) == SGX535_TASK_INVALID);
    CHECK(!memcmp(out, saved_out, sizeof(*out)));
    ir[4].source[0] = 0;
    /* Ninety-six live values then47 MAD and one MIN reductions: TEMP97,1264B code,
  * event1280..1312. BG1536..1544 stays disjoint, with unchanged raw task words. */
    memset(ir, 0, 512 * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (unsigned i = 1; i < 96; i++)
        ir[i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_CONSTANT, 0x3e000001 + i, 0, 0, {0, 0, 0}};
    for (unsigned i = 0; i < 47; i++)
        ir[96 + i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_MAD, 0, 0, 0, {i ? 95 + i : 0, 1 + i * 2, 2 + i * 2}};
    ir[143] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MIN, 0, 0, 0, {142, 95, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[144 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {143, 0, 0}};
    r.foreground.fragment_count = 148;
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->foreground.fragment_code_bytes == 1264 && p->foreground.fragment_temp_words == 97 &&
          p->foreground.event_use_offset_bytes == 1280 &&
          p->foreground.fragment_use_extent_bytes == 1312);
    disjoint_sections(p);
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, out, w) == SGX535_TASK_BLOCKED);
    check_background(&out->background.image, r.packed_background_argb, 1);
    masks(&p->foreground, &out->launch);
    /* Exact packet capacity, independent of BG space:146 definitions+4outputs
  * lower to160 instruction pairs. A further MOV exceeds the2048B packet. */
    memset(ir, 0, 512 * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (unsigned i = 1; i < 146; i++)
        ir[i] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {i - 1, 0, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[146 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {145, 0, 0}};
    r.foreground.fragment_count = 150;
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->foreground.fragment_code_bytes == 1280 &&
          p->foreground.event_use_offset_bytes == 1280 && p->foreground.packet.byte_count == 2040);
    disjoint_sections(p);
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, out, w) == SGX535_TASK_BLOCKED);
    *saved = *p;
    *saved_out = *out;
    ir[146] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {145, 0, 0}};
    for (unsigned i = 0; i < 4; i++)
        ir[147 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {146, 0, 0}};
    r.foreground.fragment_count = 151;
    CHECK(sgx535_normalized_color0_background_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    CHECK(sgx535_normalized_color0_background_bind(&r, p, &b, &bb, out, w) ==
          SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(out, saved_out, sizeof(*out)));
    /* Old standalone constructor must not accept the new image's version or
  * treat its fixed offset as an admissible caller-selected raw address. */
    struct sgx535_background_request old = {
        .version = 2, .core_id = 0x01130000, .revision = 0x00010201, .width = 16, .height = 16};
    struct sgx535_background_image old_out = {0}, old_work;
    CHECK(sgx535_background_build(&old, &old_out, &old_work) == SGX535_TASK_UNSUPPORTED);
    printf(
        "normalized COLOR0+background: %u checks; complete CPU composition, all native contracts retained\n",
        checks);
    free(standalone);
    free(w);
    free(saved_out);
    free(out);
    free(mut);
    free(saved);
    free(p);
    free(ir);
    return 0;
}
