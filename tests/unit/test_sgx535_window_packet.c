// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_launch.h"
#include "sgx535_clip_w1.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static int fail_allocation;
static unsigned clip_calls;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t bytes)
{
    return fail_allocation ? NULL : __real_calloc(n, bytes);
}
enum sgx535_clip_w1_status __wrap_sgx535_clip_w1_encode(const uint32_t input[4], uint32_t width,
                                                        uint32_t height, uint32_t output[4]);
enum sgx535_clip_w1_status __wrap_sgx535_clip_w1_encode(const uint32_t input[4], uint32_t width,
                                                        uint32_t height, uint32_t output[4])
{
    (void)input;
    (void)width;
    (void)height;
    (void)output;
    clip_calls++;
    return SGX535_CLIP_W1_UNSUPPORTED;
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        assert(x);                                                                                 \
    } while (0)

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(value >> (i * 8u));
}
static struct sgx535_window_color0_request request(struct sgx535_fragment_instruction ir[8])
{
    struct sgx535_window_color0_request r = {SGX535_WINDOW_COLOR0_PACKET_VERSION,
                                             SGX535_WINDOW_COLOR0_PROFILE,
                                             SGX535_ENGINE121_CORE_ID,
                                             SGX535_ENGINE121_REVISION,
                                             {{{0x41800000, 0x41800000, 0x3e800000, 0x3f000000},
                                               {0x42400000, 0x41800000, 0x3f400000, 0x3e800000},
                                               {0x42000000, 0x42400000, 0x3f000000, 0x40000000}},
                                              {{0x3f800000, 0, 0, 0x3f800000},
                                               {0, 0x3f000000, 0, 0x3f800000},
                                               {0, 0, 0x3e800000, 0x3f800000}},
                                              0},
                                             ir,
                                             8,
                                             {0}};
    unsigned i;
    memset(ir, 0, 8 * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4].index = i;
        ir[i + 4].source[0] = (i + 1u) % 4u;
    }
    return r;
}
static const struct sgx535_task_packet_section *section(const struct sgx535_task_packet *p,
                                                        uint32_t kind)
{
    unsigned i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    abort();
}
static void binding(const struct sgx535_task_packet *p, struct sgx535_task_launch_binding *b)
{
    static const uint32_t bases[8] = {0x20003000, 0x41010000, 0x42012000, 0x43003000,
                                      0x44000000, 0x45000000, 0x46000000, 0x30000000};
    unsigned i;
    memset(b, 0, sizeof(*b));
    b->epoch = 31;
    b->bank_index[0] = 3;
    b->bank_index[1] = 15;
    for (i = 0; i < 8; i++)
        b->roles[i] = (struct sgx535_task_launch_role){
            101u + i, bases[i], p->requirements[i].minimum_bytes, p->requirements[i].access, 0};
}
/* Independent arithmetic consumer: full byte OFFSET, 16-byte USE entries
 * within a 512KiB bank, and separate low/high entry fields. Synthetic bindings
 * establish no physical ownership or code visibility. */
static void projection(const struct sgx535_task_packet *p,
                       const struct sgx535_task_launch_binding *b,
                       const struct sgx535_task_launch *out)
{
    uint8_t expected[SGX535_TASK_PACKET_BYTES];
    unsigned i, kinds[4] = {0};
    memcpy(expected, p->bytes, sizeof(expected));
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        const struct sgx535_task_packet_section *s = &p->sections[r->section];
        uint64_t va = b->roles[r->role].gpu_va_bytes + r->resource_byte_offset;
        uint32_t value, offset = s->byte_offset + r->section_byte_offset;
        CHECK(r->kind >= 1 && r->kind <= 3);
        kinds[r->kind]++;
        if (r->kind == SGX535_PACKET_DRI_OFFSET)
            value = (uint32_t)((va >> r->right_shift) << r->left_shift);
        else {
            uint32_t units = (uint32_t)(va & 0x7ffffu) / 16u;
            uint32_t bank = b->bank_index[r->role - SGX535_TASK_USE_VERTEX];
            value = r->kind == SGX535_PACKET_USE_BANK
                        ? bank
                        : bank | (units / 2048u) * 16u | (units % 2048u) * 256u;
        }
        put(expected + offset, (word(expected + offset) & ~r->mask) | (value & r->mask));
    }
    CHECK(kinds[1] == 11 && kinds[2] == 6 && kinds[3] == 12);
    CHECK(!memcmp(out->bytes, expected, sizeof(expected)));
    CHECK(out->packet_version == 7 && out->epoch == 31 && out->resolved_relocations == 29);
    CHECK(out->missing_packet_contracts == 0x1ff && out->missing_launch_contracts == 0x75);
    CHECK(out->stages[0].count == 3 && out->stages[1].count == 7 && out->stages[2].count == 26);
    CHECK(out->stages[0].actions[0].offset == 0xab8 &&
          out->stages[0].actions[0].value == 0x20000000);
    CHECK(out->bank_base[0] == 0x41000000 && out->bank_base[1] == 0x42000000);
    CHECK(out->ta_stream_va_bytes == 0x20003220 && out->event_pds_va_bytes == 0x20003120);
    for (i = 0; i < 3; i++) {
        unsigned j;
        for (j = 0; j < out->stages[i].count; j++)
            CHECK(!out->stages[i].unknown_value_masks[j]);
    }
}
static void payload(const struct sgx535_window_color0_request *r,
                    const struct sgx535_task_packet *p)
{
    const struct sgx535_task_packet_section *v = section(p, SGX535_PACKET_VERTICES);
    unsigned i, j;
    CHECK(p->version == 7 && p->profile == 4 && p->section_count == 16 &&
          p->relocation_count == 29);
    CHECK(p->width == 64 && p->height == 64 && p->stride_bytes == 256 &&
          p->missing_contracts == 0x1ff);
    CHECK(!v->byte_offset && v->byte_count == 96 && v->role == SGX535_TASK_VERTEX_DATA &&
          !v->resource_byte_offset);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++) {
            CHECK(word(p->bytes + v->byte_offset + i * 32u + j * 4u) ==
                  r->vertices.window_xyz_reciprocal_w[i][j]);
            CHECK(word(p->bytes + v->byte_offset + i * 32u + 16u + j * 4u) ==
                  r->vertices.rgba[i][j]);
        }
}
static void refuses(const struct sgx535_window_color0_request *r, struct sgx535_task_packet *p,
                    const struct sgx535_task_launch_binding *b, struct sgx535_task_launch *out,
                    struct sgx535_compiled_color0_workspace *w,
                    struct sgx535_compiled_color0_launch_workspace *lw,
                    enum sgx535_task_status expected)
{
    struct sgx535_task_packet saved = *p;
    struct sgx535_task_launch saved_launch = *out;
    CHECK(sgx535_window_color0_packet_build(r, p, w) == expected);
    CHECK(!memcmp(p, &saved, sizeof(saved)));
    CHECK(sgx535_window_color0_launch_build(r, p, b, out, lw) == expected);
    CHECK(!memcmp(out, &saved_launch, sizeof(saved_launch)));
}
int main(void)
{
    struct sgx535_fragment_instruction ir[8], saved_ir[8];
    struct sgx535_window_color0_request r = request(ir), bad, saved_request;
    struct sgx535_compiled_color0_request old = {0};
    struct sgx535_compiled_color0_workspace *w = malloc(sizeof(*w));
    struct sgx535_compiled_color0_launch_workspace *lw = malloc(sizeof(*lw));
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *saved = malloc(sizeof(*saved));
    struct sgx535_task_launch *out = malloc(sizeof(*out)), *saved_out = malloc(sizeof(*saved_out));
    struct sgx535_task_launch_binding b, bad_binding;
    const struct sgx535_task_packet_section *v;
    unsigned i, j;
    CHECK(w && lw && p && saved && out && saved_out);
    saved_request = r;
    memcpy(saved_ir, ir, sizeof(ir));
    CHECK(sgx535_window_color0_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_window_color0_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(&r, &saved_request, sizeof(r)) && !memcmp(ir, saved_ir, sizeof(ir)));
    payload(&r, p);
    binding(p, &b);
    CHECK(sgx535_window_color0_launch_build(&r, p, &b, out, lw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_window_color0_launch_validate(&r, p, &b, out, lw) == SGX535_TASK_BLOCKED);
    projection(p, &b, out);
    *saved = *p;
    *saved_out = *out;
    CHECK(!clip_calls); /* A packet7 build must never fabricate a W1 seed. */
    /* Dynamic binary32 values are already projected; only the full vertex
     * section may change. In particular, reciprocalW does not get inverted. */
    v = section(p, SGX535_PACKET_VERTICES);
    for (i = 0; i < 32; i++) {
        r = saved_request;
        r.vertices.window_xyz_reciprocal_w[2][0] = 0x42000000u + i * 17u;
        r.vertices.window_xyz_reciprocal_w[2][3] = 0x3e800000u + i * 97u;
        r.vertices.rgba[1][2] = 0x3e000000u + i * 13u;
        CHECK(sgx535_window_color0_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
        payload(&r, p);
        CHECK(!memcmp(p, saved, (size_t)((uint8_t *)p->bytes - (uint8_t *)p)));
        CHECK(!memcmp(p->bytes + v->byte_count, saved->bytes + v->byte_count,
                      sizeof(p->bytes) - v->byte_count));
        CHECK(sgx535_window_color0_launch_build(&r, p, &b, out, lw) == SGX535_TASK_BLOCKED);
        projection(p, &b, out);
    }
    CHECK(!clip_calls);
    r = saved_request;
    *p = *saved;
    *out = *saved_out;
    /* Old typed input cannot select the new representation, and packet7 is
     * fully regenerated: source, qualifiers, sections, relocs and padding. */
    old.version = 6;
    old.profile = 3;
    old.core_id = r.core_id;
    old.revision = r.revision;
    old.fragment = ir;
    old.fragment_count = 8;
    old.interpolation = SGX535_VARYING_SMOOTH;
    for (i = 0; i < 3; i++)
        old.clip_position[i][3] = 0x3f800000;
    CHECK(sgx535_compiled_color0_packet_validate(&old, p, w) == SGX535_TASK_UNSUPPORTED);
    CHECK(clip_calls == 1u); /* Verify that the rejecting W1 wrapper is active. */
    for (i = 0; i < sizeof(*p); i++) {
        ((uint8_t *)p)[i] ^= 1;
        CHECK(sgx535_window_color0_packet_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
        ((uint8_t *)p)[i] ^= 1;
    }
    for (i = 0; i < p->section_count; i++) {
        p->bytes[p->sections[i].byte_offset] ^= 1;
        CHECK(sgx535_window_color0_launch_build(&r, p, &b, out, lw) == SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
        p->bytes[p->sections[i].byte_offset] ^= 1;
    }
    for (i = 0; i < 3; i++) {
        out->stages[i].unknown_value_masks[0] = 1;
        CHECK(sgx535_window_color0_launch_validate(&r, p, &b, out, lw) == SGX535_TASK_INVALID);
        *out = *saved_out;
    }
    for (i = 0; i < 13; i++) {
        bad = r;
        switch (i) {
        case 0:
            bad.version = 6;
            break;
        case 1:
            bad.profile = 3;
            break;
        case 2:
            bad.core_id = 0;
            break;
        case 3:
            bad.revision = 0;
            break;
        case 4:
            bad.reserved[2] = 1;
            break;
        case 5:
            bad.vertices.window_xyz_reciprocal_w[2][0] = 0x42800001;
            break;
        case 6:
            bad.vertices.window_xyz_reciprocal_w[2][2] = 0x7fc12345;
            break;
        case 7:
            bad.vertices.window_xyz_reciprocal_w[2][3] = 0;
            break;
        case 8:
            bad.vertices.window_xyz_reciprocal_w[2][3] = 0xbf800000;
            break;
        case 9:
            bad.vertices.rgba[2][3] = 0x80000000;
            break;
        case 10:
            bad.vertices.rgba[2][3] = 0x3f800001;
            break;
        case 11:
            bad.vertices.replicated_flat = 2;
            break;
        default:
            bad.vertices.replicated_flat = 1;
            break;
        }
        refuses(&bad, p, &b, out, w, lw, SGX535_TASK_UNSUPPORTED);
    }
    bad = r;
    bad.fragment = NULL;
    refuses(&bad, p, &b, out, w, lw, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment_count = 513;
    refuses(&bad, p, &b, out, w, lw, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment = (const void *)(uintptr_t)(UINTPTR_MAX - 7);
    refuses(&bad, p, &b, out, w, lw, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment = (const void *)p;
    refuses(&bad, p, &b, out, w, lw, SGX535_TASK_INVALID);
    CHECK(sgx535_window_color0_packet_build(&r, (void *)w, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_window_color0_launch_build(&r, p, &b, (void *)lw, lw) == SGX535_TASK_INVALID);
    CHECK(sgx535_window_color0_launch_build(&r, p, &b, (void *)ir, lw) == SGX535_TASK_INVALID);
    CHECK(!memcmp(p, saved, sizeof(*p)) && !memcmp(out, saved_out, sizeof(*out)));
    ir[1].opcode = SGX535_FRAGMENT_UNIFORM;
    refuses(&r, p, &b, out, w, lw, SGX535_TASK_UNSUPPORTED);
    memcpy(ir, saved_ir, sizeof(ir));
    ir[0].opcode = 0xffffffffu;
    refuses(&r, p, &b, out, w, lw, SGX535_TASK_UNSUPPORTED);
    memcpy(ir, saved_ir, sizeof(ir));
    fail_allocation = 1;
    refuses(&r, p, &b, out, w, lw, SGX535_TASK_NOMEM);
    fail_allocation = 0;
    for (i = 0; i < 8; i++) {
        bad_binding = b;
        bad_binding.roles[i].allocation_bytes -= 4096;
        CHECK(sgx535_window_color0_launch_build(&r, p, &bad_binding, out, lw) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(out, saved_out, sizeof(*out)));
    }
    bad_binding = b;
    bad_binding.bank_index[1] = bad_binding.bank_index[0];
    CHECK(sgx535_window_color0_launch_build(&r, p, &bad_binding, out, lw) == SGX535_TASK_INVALID);
    CHECK(!memcmp(out, saved_out, sizeof(*out)));
    /* Replicated flat uses the same smooth iterator only after equality. */
    for (i = 1; i < 3; i++)
        memcpy(r.vertices.rgba[i], r.vertices.rgba[0], sizeof(r.vertices.rgba[i]));
    CHECK(sgx535_window_color0_packet_build(&r, saved, w) == SGX535_TASK_BLOCKED);
    r.vertices.replicated_flat = 1;
    CHECK(sgx535_window_color0_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    for (i = 1; i < 3; i++)
        for (j = 0; j < 4; j++) {
            bad = r;
            bad.vertices.rgba[i][j] ^= 1;
            refuses(&bad, p, &b, out, w, lw, SGX535_TASK_UNSUPPORTED);
        }
    free(saved_out);
    free(out);
    free(saved);
    free(p);
    free(lw);
    free(w);
    printf(
        "PASS typed window packet7: %u CPU checks, complete16 sections/29 relocations, masks1ff/75 retained\n",
        checks);
    return 0;
}
