// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_normalized_color0_mask_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static int fail_alloc;
static uint32_t *drift_at;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t size)
{
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
static void put(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> (i * 8));
}
#include "normalized_mask_list_reference.h"
static const uint32_t clips[2][3][4] = {{{0xbf400000, 0x3f400000, 0, 0x3f800000},
                                         {0x3f400000, 0x3f400000, 0, 0x3f800000},
                                         {0x3f200000, 0xbf000000, 0, 0x3f800000}},
                                        {{0xbf400000, 0x3f400000, 0, 0x3f800000},
                                         {0x3f200000, 0xbf000000, 0, 0x3f800000},
                                         {0xbf400000, 0xbf400000, 0, 0x3f800000}}};
static const uint32_t windows[2][3][4] = {{{0x40000000, 0x40000000, 0x3f000000, 0x3f800000},
                                           {0x41600000, 0x40000000, 0x3f000000, 0x3f800000},
                                           {0x41500000, 0x41400000, 0x3f000000, 0x3f800000}},
                                          {{0x40000000, 0x40000000, 0x3f000000, 0x3f800000},
                                           {0x41500000, 0x41400000, 0x3f000000, 0x3f800000},
                                           {0x40000000, 0x41600000, 0x3f000000, 0x3f800000}}};
static struct sgx535_normalized_color0_mask_list_request
request(struct sgx535_normalized_color0_mask_list_triangle *t,
        struct sgx535_fragment_instruction *ir, uint32_t count)
{
    struct sgx535_normalized_color0_mask_list_request r = {.version = 12,
                                                           .profile = 11,
                                                           .core_id = 0x01130000,
                                                           .revision = 0x00010201,
                                                           .triangles = t,
                                                           .triangle_count = count,
                                                           .interpolation = 1,
                                                           .fragment = ir,
                                                           .fragment_count = 8};
    memset(t, 0, SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES * sizeof(*t));
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    for (unsigned i = 0; i < SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES; i++) {
        memcpy(t[i].clip_position, clips[i % 2], sizeof(t[i].clip_position));
        for (unsigned j = 0; j < 3; j++) {
            t[i].rgba[j][i % 2] = 0x3f800000;
            t[i].rgba[j][3] = 0x3f800000;
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4].index = i;
        ir[i + 4].source[0] = i;
    }
    return r;
}
static struct sgx535_normalized_color0_background_request
first_request(const struct sgx535_normalized_color0_mask_list_request *r)
{
    struct sgx535_normalized_color0_background_request f = {.version = 2,
                                                            .packed_background_argb = 0xff1d2743};
    f.foreground.version = 11;
    f.foreground.profile = 10;
    f.foreground.core_id = r->core_id;
    f.foreground.revision = r->revision;
    f.foreground.interpolation = r->interpolation;
    f.foreground.fragment = r->fragment;
    f.foreground.fragment_count = r->fragment_count;
    memcpy(f.foreground.clip_position, r->triangles[0].clip_position,
           sizeof(f.foreground.clip_position));
    memcpy(f.foreground.rgba, r->triangles[0].rgba, sizeof(f.foreground.rgba));
    return f;
}
static uint32_t expected_size(uint32_t count)
{
    return ((96u * count + 64u + ((3u * count + 7u) / 2u) * 4u + 15u) & ~15u) + 1012u;
}
static void literal(uint8_t bytes[2048], const struct sgx535_normalized_color0_mask_list_request *r,
                    struct sgx535_task_packet_section sections[16],
                    struct sgx535_task_packet_reloc relocs[40], int bound)
{
    uint8_t seed[2048];
    uint32_t count = r->triangle_count, vertices = 3u * count, vbytes = 96u * count + 64u;
    struct sgx535_normalized_color0_background_request first = first_request(r);
    mask_literal(seed, &first.foreground, bound);
    memset(bytes, 0, 2048);
    memcpy(sections, mask_sections, sizeof(mask_sections));
    memcpy(relocs, mask_relocs, sizeof(mask_relocs));
    uint32_t end = 0;
    for (unsigned i = 0; i < 16; i++) {
        sections[i].byte_offset = (end + 15u) & ~15u;
        if (i == 0)
            sections[i].byte_count = vbytes;
        if (i == 1) {
            sections[i].byte_count = ((vertices + 7u) / 2u) * 4u;
            sections[i].resource_byte_offset = vbytes;
        }
        end = sections[i].byte_offset + sections[i].byte_count;
        CHECK(end <= 2048);
        uint8_t *p = bytes + sections[i].byte_offset;
        if (i == 0) {
            for (unsigned a = 0; a < count; a++)
                for (unsigned b = 0; b < 3; b++) {
                    for (unsigned c = 0; c < 4; c++)
                        put(p + a * 96u + b * 32u + c * 4u, windows[a % 2][b][c]);
                    for (unsigned c = 0; c < 4; c++)
                        put(p + a * 96u + b * 32u + 16u + c * 4u, r->triangles[a].rgba[b][c]);
                }
            memcpy(p + count * 96u, seed + 96, 64);
        } else if (i == 1) {
            memcpy(p, seed + 160, 12);
            for (unsigned j = 0; j < vertices; j++) {
                p[12u + j * 2u] = (uint8_t)j;
                p[13u + j * 2u] = (uint8_t)(j >> 8);
            }
        } else
            memcpy(p, seed + mask_sections[i].byte_offset, sections[i].byte_count);
    }
    relocs[0].span_bytes = 96u * count;
    relocs[4].resource_byte_offset = 96u * count;
    relocs[34].resource_byte_offset = vbytes;
    relocs[37].resource_byte_offset = vbytes + 12u;
    relocs[37].span_bytes = vertices * 2u;
    put(bytes + sections[15].byte_offset + 36u, 0x81400000u | vertices);
    if (bound) {
        put(bytes + sections[6].byte_offset + 64u, 0x40002000u + 96u * count);
        put(bytes + sections[15].byte_offset + 12u, 0x40002000u + vbytes);
        put(bytes + sections[15].byte_offset + 40u, 0x40002000u + vbytes + 12u);
    }
    CHECK(end == expected_size(count));
}
static void qualification(const struct sgx535_normalized_color0_mask_list_task *p,
                          const struct sgx535_normalized_color0_launch *out)
{
    const struct sgx535_fragment_normalized_candidate *f = &p->task.fragment;
    CHECK(p->task.packet.version == 12 && p->task.packet.profile == 11 &&
          p->task.packet.missing_contracts == 0x1ff);
    CHECK(f->missing_contracts == 0x3ff && !f->arithmetic_qualified && !f->numerical_qualified &&
          !f->linkage_qualified);
    CHECK(!f->output_qualified && !f->hardware_qualified && !f->nonfinite_behavior_qualified);
    CHECK(sgx535_fragment_normalized_native_admission(f) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(out->launch.packet_version == 12 && out->launch.missing_packet_contracts == 0x1ff &&
          out->launch.missing_launch_contracts == 0x75);
    CHECK(out->fragment_missing_contracts == 0x3ff && !out->arithmetic_qualified &&
          !out->numerical_qualified && !out->linkage_qualified);
    CHECK(!out->output_qualified && !out->hardware_qualified && !out->nonfinite_behavior_qualified);
}
static void save(const char *dir, const char *name, const void *p, size_t bytes)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wbx");
    CHECK(f);
    CHECK(fwrite(p, 1, bytes, f) == bytes);
    CHECK(!fclose(f));
}
int main(int argc, char **argv)
{
    struct sgx535_normalized_color0_mask_list_triangle *t =
        calloc(SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES, sizeof(*t));
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_normalized_color0_mask_list_task *p = calloc(1, sizeof(*p)),
                                                   *old = calloc(1, sizeof(*p)),
                                                   *mut = calloc(1, sizeof(*p));
    struct sgx535_normalized_color0_mask_list_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_normalized_color0_mask_list_launch_workspace *lw = calloc(1, sizeof(*lw));
    struct sgx535_normalized_color0_launch *out = calloc(1, sizeof(*out)),
                                           *old_out = calloc(1, sizeof(*out));
    struct sgx535_normalized_color0_mask_list_background_image *bp = calloc(1, sizeof(*bp)),
                                                               *old_bp = calloc(1, sizeof(*bp));
    struct sgx535_normalized_color0_mask_list_background_workspace *bw = calloc(1, sizeof(*bw));
    struct sgx535_normalized_color0_background_bound *bound = calloc(1, sizeof(*bound)),
                                                     *old_bound = calloc(1, sizeof(*bound));
    struct sgx535_normalized_color0_background_image *single = calloc(1, sizeof(*single));
    struct sgx535_normalized_color0_background_workspace *sw = calloc(1, sizeof(*sw));
    struct sgx535_normalized_color0_mask_list_request r, bad;
    struct sgx535_normalized_color0_mask_list_background_request br, bad_br;
    struct sgx535_task_launch_binding b, bad_b;
    struct sgx535_background_binding bb, bad_bb;
    struct sgx535_task_packet_section sections[16];
    struct sgx535_task_packet_reloc relocs[40];
    uint8_t expected[2048], background[192];
    unsigned last = 0;
    CHECK(argc == 1 || argc == 2);
    CHECK(t && ir && p && old && mut && w && lw && out && old_out && bp && old_bp && bw && bound &&
          old_bound && single && sw);
    r = request(t, ir, 1);
    struct sgx535_normalized_color0_background_request first = first_request(&r);
    CHECK(sgx535_normalized_color0_mask_background_build(&first, single, sw) ==
          SGX535_TASK_BLOCKED);
    mask_binding(single, &b, &bb);
    CHECK(sgx535_normalized_color0_mask_background_bind(&first, single, &b, &bb, bound, sw) ==
          SGX535_TASK_BLOCKED);
    for (unsigned count = 1; count <= SGX535_NORMALIZED_COLOR0_MASK_LIST_INPUT_TRIANGLES; count++) {
        r = request(t, ir, count);
        *old = *p;
        enum sgx535_task_status status = sgx535_normalized_color0_mask_list_build(&r, p, w);
        if (expected_size(count) > 2048) {
            CHECK(status == SGX535_TASK_UNSUPPORTED);
            CHECK(!memcmp(p, old, sizeof(*p)));
            continue;
        }
        CHECK(status == SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_color0_mask_list_validate(&r, p, w) == SGX535_TASK_BLOCKED);
        last = count;
        CHECK(p->triangle_count == count && p->vertex_count == 3u * count &&
              p->foreground_vertex_bytes == 96u * count);
        CHECK(p->mask_vertex_offset_bytes == 96u * count &&
              p->vertex_storage_bytes == 96u * count + 64u);
        CHECK(p->index_offset_bytes == p->vertex_storage_bytes &&
              p->foreground_index_offset_bytes == p->index_offset_bytes + 12);
        CHECK(p->foreground_index_bytes == 6u * count &&
              p->index_storage_bytes == ((3u * count + 7u) / 2u) * 4u);
        CHECK(p->task.packet.byte_count == expected_size(count));
        CHECK(p->task.packet.section_count == 16 && p->task.packet.relocation_count == 40);
        const uint8_t *ta = p->task.packet.bytes + p->task.packet.sections[15].byte_offset;
        CHECK(get(ta + 8) == 0x81400006 && get(ta + 36) == (0x81400000u | 3u * count));
        CHECK(get(ta + 24) == 0x02000103 && get(ta + 52) == 0x04000203);
        literal(expected, &r, sections, relocs, 0);
        CHECK(mask_reference_equal(p->task.packet.bytes, expected, sections, 0));
        CHECK(!memcmp(p->task.packet.sections, sections, sizeof(sections)));
        CHECK(!memcmp(p->task.packet.relocations, relocs, sizeof(relocs)));
        CHECK(sgx535_normalized_color0_mask_list_launch_build(&r, p, &b, out, lw) ==
              SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_color0_mask_list_launch_validate(&r, p, &b, out, lw) ==
              SGX535_TASK_BLOCKED);
        literal(expected, &r, sections, relocs, 1);
        CHECK(mask_reference_equal(out->launch.bytes, expected, sections, 1));
        qualification(p, out);
        CHECK(out->launch.resolved_relocations == 40 &&
              out->launch.ta_stream_va_bytes == 0x200002b0 &&
              out->launch.event_pds_va_bytes == 0x200001a0);
        br = (struct sgx535_normalized_color0_mask_list_background_request){
            .version = 3, .packed_background_argb = 0xff1d2743, .foreground = r};
        CHECK(sgx535_normalized_color0_mask_list_background_build(&br, bp, bw) ==
              SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_color0_mask_list_background_validate(&br, bp, bw) ==
              SGX535_TASK_BLOCKED);
        CHECK(!memcmp(&p->task, &bp->foreground.task, sizeof(p->task)));
        CHECK(sgx535_normalized_color0_mask_list_background_bind(&br, bp, &b, &bb, bound, bw) ==
              SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_color0_mask_list_background_bound_validate(
                  &br, bp, &b, &bb, bound, bw) == SGX535_TASK_BLOCKED);
        CHECK(bound->version == 3 && bound->missing_background_contracts == 15 &&
              bound->background.image.missing_contracts == 15);
        CHECK(mask_reference_equal(bound->launch.launch.bytes, expected, sections, 1));
        mask_background_literal(background, 0xff1d2743, 1);
        CHECK(!memcmp(bound->background.image.bytes, background, 192));
        qualification(p, &bound->launch);
        if (count == 1) {
            CHECK(!memcmp(single->foreground.packet.bytes, p->task.packet.bytes, 2048));
            CHECK(!memcmp(single->foreground.packet.sections, p->task.packet.sections,
                          sizeof(sections)));
            CHECK(!memcmp(single->foreground.packet.relocations, p->task.packet.relocations,
                          sizeof(relocs)));
        }
        if (argc == 2 && (count == 1 || count == 2 || count == 9)) {
            char name[64];
            int n = snprintf(name, sizeof(name), "list-%u.packet.bound.bin", count);
            CHECK(n > 0 && (size_t)n < sizeof(name));
            save(argv[1], name, bound->launch.launch.bytes, 2048);
        }
    }
    CHECK(last == 9);
    r = request(t, ir, 2);
    br = (struct sgx535_normalized_color0_mask_list_background_request){
        .version = 3, .packed_background_argb = 0xff1d2743, .foreground = r};
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_BLOCKED);
    *old = *p;
    CHECK(sgx535_normalized_color0_mask_list_launch_build(&r, p, &b, out, lw) ==
          SGX535_TASK_BLOCKED);
    *old_out = *out;
    CHECK(sgx535_normalized_color0_mask_list_background_build(&br, bp, bw) == SGX535_TASK_BLOCKED);
    *old_bp = *bp;
    CHECK(sgx535_normalized_color0_mask_list_background_bind(&br, bp, &b, &bb, bound, bw) ==
          SGX535_TASK_BLOCKED);
    *old_bound = *bound;
    for (unsigned m = 0; m < 12; m++) {
        bad = r;
        if (m == 0)
            bad.version = 11;
        if (m == 1)
            bad.profile = 10;
        if (m == 2)
            bad.reserved[0] = 1;
        if (m == 3)
            bad.triangle_count = 0;
        if (m == 4)
            bad.triangle_count = 22;
        if (m == 5)
            bad.fragment_count = 4;
        if (m == 6)
            bad.triangles = NULL;
        if (m == 7)
            bad.fragment = NULL;
        if (m == 8)
            bad.interpolation = 0;
        if (m == 9)
            bad.triangles = (void *)(~(unsigned long)0 - 3u);
        if (m == 10)
            bad.fragment = (void *)(~(unsigned long)0 - 3u);
        if (m == 11)
            bad.revision++;
        CHECK(sgx535_normalized_color0_mask_list_build(&bad, p, w) < 0);
        CHECK(!memcmp(p, old, sizeof(*p)));
    }
    for (unsigned m = 0; m < 5; m++) {
        uint32_t *field = m == 0 ? &t[1].clip_position[2][3] : &t[1].rgba[1][0];
        uint32_t saved = *field;
        *field = m == 0   ? 0x40000000u
                 : m == 1 ? 0x80000000u
                 : m == 2 ? 0x7f800000u
                 : m == 3 ? 0x7fc12345u
                          : 0x3f800001u;
        CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(p, old, sizeof(*p)));
        *field = saved;
    }
    for (unsigned m = 0; m < 4; m++) {
        uint32_t *field = m == 0   ? &t[1].rgba[0][0]
                          : m == 1 ? &ir[0].component
                          : m == 2 ? &r.revision
                                   : &b.bank_index[1];
        uint32_t saved = *field;
        drift_at = field;
        if (m == 3)
            CHECK(sgx535_normalized_color0_mask_list_launch_build(&r, p, &b, out, lw) < 0);
        else
            CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) < 0);
        CHECK(!drift_at);
        CHECK(!memcmp(p, old, sizeof(*p)));
        CHECK(!memcmp(out, old_out, sizeof(*out)));
        *field = saved;
    }
    for (size_t i = 0; i < sizeof(*p); i += 89) {
        *mut = *p;
        ((uint8_t *)mut)[i] ^= 1;
        CHECK(sgx535_normalized_color0_mask_list_validate(&r, mut, w) == SGX535_TASK_INVALID);
        CHECK(sgx535_normalized_color0_mask_list_launch_build(&r, mut, &b, out, lw) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(out, old_out, sizeof(*out)));
    }
    for (unsigned m = 0; m < 7; m++) {
        bad_b = b;
        bad_bb = bb;
        bad_br = br;
        if (m == 0)
            bad_bb.epoch++;
        if (m == 1)
            bad_bb.resources[2].storage_id = b.roles[0].storage_id;
        if (m == 2)
            bad_bb.resources[2].gpu_va_bytes += 4096;
        if (m == 3)
            bad_b.roles[5].allocation_bytes = 2048;
        if (m == 4)
            bad_br.version = 2;
        if (m == 5)
            bad_br.reserved[1] = 1;
        if (m == 6)
            bad_b.roles[1].storage_id = 0;
        CHECK(sgx535_normalized_color0_mask_list_background_bind(&bad_br, bp, &bad_b, &bad_bb,
                                                                 bound, bw) < 0);
        CHECK(!memcmp(bound, old_bound, sizeof(*bound)));
    }
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, (void *)p) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_mask_list_build(&r, (void *)t, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_mask_list_background_bind(&br, bp, &b, &bb, (void *)&bb, bw) ==
          SGX535_TASK_INVALID);
    fail_alloc = 1;
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_NOMEM);
    CHECK(!memcmp(p, old, sizeof(*p)));
    fail_alloc = 0;
    r.interpolation = 2;
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_BLOCKED);
    t[1].rgba[2][0] = 0x3f000000;
    *old = *p;
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, old, sizeof(*p)));
    r = request(t, ir, 9);
    r.fragment_count = 14;
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
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(p, old, sizeof(*p)));
    r.triangle_count = 8;
    CHECK(sgx535_normalized_color0_mask_list_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->task.fragment_code_bytes == 192 && p->task.event_use_offset_bytes == 192 &&
          p->task.packet.byte_count == 1956);
    printf(
        "normalized COLOR0 mask list: %u checks; max9 only for this144B program; CPU construction/native closed\n",
        checks);
    free(sw);
    free(single);
    free(old_bound);
    free(bound);
    free(bw);
    free(old_bp);
    free(bp);
    free(old_out);
    free(out);
    free(lw);
    free(w);
    free(mut);
    free(old);
    free(p);
    free(ir);
    free(t);
    return 0;
}
