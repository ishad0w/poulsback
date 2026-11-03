// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_launch.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long checks;
static int fail_allocation;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t bytes)
{
    return fail_allocation ? NULL : __real_calloc(n, bytes);
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
static struct sgx535_compiled_color0_request request(struct sgx535_fragment_instruction *ir)
{
    struct sgx535_compiled_color0_request r = {SGX535_COMPILED_COLOR0_PACKET_VERSION,
                                               SGX535_COMPILED_COLOR0_PROFILE,
                                               SGX535_ENGINE121_CORE_ID,
                                               SGX535_ENGINE121_REVISION,
                                               {{0xbf000000, 0xbf000000, 0, 0x3f800000},
                                                {0x3f000000, 0xbf000000, 0, 0x3f800000},
                                                {0, 0x3f000000, 0, 0x3f800000}},
                                               {{0x3f800000, 0, 0, 0x3f800000},
                                                {0, 0x3f800000, 0, 0x3f800000},
                                                {0, 0, 0x3f800000, 0x3f800000}},
                                               ir,
                                               8,
                                               SGX535_VARYING_SMOOTH,
                                               {0}};
    unsigned i;
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4].index = i;
        ir[i + 4].source[0] = i;
    }
    return r;
}
static const struct sgx535_task_packet_section *section(const struct sgx535_task_packet *p,
                                                        unsigned kind)
{
    unsigned i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    abort();
}
static void binding(const struct sgx535_task_packet *p, struct sgx535_task_launch_binding *b)
{
    const uint32_t bases[8] = {0x20003000, 0x41010000, 0x42012000, 0x43003000,
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
static void refuse(struct sgx535_compiled_color0_request *r, struct sgx535_task_packet *p,
                   struct sgx535_compiled_color0_workspace *w, enum sgx535_task_status expected)
{
    struct sgx535_task_packet *saved = malloc(sizeof(*saved));
    CHECK(saved);
    *saved = *p;
    CHECK(sgx535_compiled_color0_packet_build(r, p, w) == expected);
    CHECK(!memcmp(saved, p, sizeof(*p)));
    free(saved);
}
static void replicated_flat(void)
{
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct flat_fixture {
        struct sgx535_compiled_color0_request r, bad;
        struct sgx535_task_launch_binding binding;
    } *f = malloc(sizeof(*f));
    struct sgx535_compiled_color0_workspace *w = malloc(sizeof(*w));
    struct sgx535_compiled_color0_launch_workspace *lw = malloc(sizeof(*lw));
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *smooth = malloc(sizeof(*smooth));
    struct sgx535_task_launch *launch = malloc(sizeof(*launch)), *saved = malloc(sizeof(*saved));
    static const uint32_t rgba[4] = {0x3e000001, 0x3f000001, 0x3f400001, 0x3f200001};
    unsigned i, j;
    CHECK(ir && f && w && lw && p && smooth && launch && saved);
    f->r = request(ir);
    /* A generic channel permutation prevents special casing identity output.
     * All four inputs and outputs remain live symbolic SSA. */
    for (j = 0; j < 4; j++)
        ir[4u + j].source[0] = (j + 1u) % 4u;
    for (i = 0; i < 3; i++)
        memcpy(f->r.rgba[i], rgba, sizeof(rgba));
    CHECK(sgx535_compiled_color0_packet_build(&f->r, smooth, w) == SGX535_TASK_BLOCKED);
    binding(smooth, &f->binding);
    CHECK(sgx535_compiled_color0_launch_build(&f->r, smooth, &f->binding, saved, lw) ==
          SGX535_TASK_BLOCKED);
    f->r.interpolation = SGX535_VARYING_FLAT_REPLICATED;
    CHECK(sgx535_compiled_color0_packet_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_compiled_color0_packet_validate(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(p, smooth, sizeof(*p)) && p->missing_contracts == 0x1ff);
    CHECK(sgx535_compiled_color0_launch_build(&f->r, p, &f->binding, launch, lw) ==
          SGX535_TASK_BLOCKED);
    CHECK(sgx535_compiled_color0_launch_validate(&f->r, p, &f->binding, launch, lw) ==
          SGX535_TASK_BLOCKED);
    CHECK(!memcmp(launch, saved, sizeof(*launch)) && launch->missing_packet_contracts == 0x1ff);
    for (i = 1; i < 3; i++)
        for (j = 0; j < 4; j++) {
            f->bad = f->r;
            f->bad.rgba[i][j] ^= 1u;
            refuse(&f->bad, p, w, SGX535_TASK_UNSUPPORTED);
            CHECK(sgx535_compiled_color0_packet_validate(&f->bad, p, w) == SGX535_TASK_UNSUPPORTED);
            CHECK(sgx535_compiled_color0_launch_build(&f->bad, p, &f->binding, launch, lw) ==
                  SGX535_TASK_UNSUPPORTED);
            CHECK(sgx535_compiled_color0_launch_validate(&f->bad, p, &f->binding, launch, lw) ==
                  SGX535_TASK_UNSUPPORTED);
            CHECK(!memcmp(launch, saved, sizeof(*launch)));
        }
    f->bad = f->r;
    f->bad.interpolation = 3;
    refuse(&f->bad, p, w, SGX535_TASK_UNSUPPORTED);
    fail_allocation = 1;
    refuse(&f->r, p, w, SGX535_TASK_NOMEM);
    CHECK(sgx535_compiled_color0_launch_build(&f->r, p, &f->binding, launch, lw) ==
          SGX535_TASK_NOMEM);
    fail_allocation = 0;
    CHECK(!memcmp(launch, saved, sizeof(*launch)));
    free(saved);
    free(launch);
    free(smooth);
    free(p);
    free(lw);
    free(w);
    free(f);
    free(ir);
}
int main(void)
{
    static const uint32_t literal[16] = {0xa0000100, 0x28a10000, 0xa0200080, 0x28a10000,
                                         0xa0400000, 0x28a10000, 0xa0600180, 0x28a10000,
                                         0x20040000, 0x40a10c11, 0x20040080, 0x40a10c09,
                                         0x20040100, 0x40a10c05, 0x20040180, 0x40a50c21};
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_fragment_instruction *saved_ir =
        malloc(SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    struct sgx535_compiled_color0_workspace *w = malloc(sizeof(*w));
    struct sgx535_varying_workspace *oldw = malloc(sizeof(*oldw));
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *saved = malloc(sizeof(*saved)),
                              *old = malloc(sizeof(*old));
    struct sgx535_task_launch *launch = malloc(sizeof(*launch)),
                              *launch_saved = malloc(sizeof(*launch_saved));
    struct sgx535_compiled_color0_launch_workspace *lw = malloc(sizeof(*lw));
    struct sgx535_task_launch_binding b, bad_binding;
    struct sgx535_compiled_color0_request r, bad;
    struct sgx535_varying_request old_request = {0};
    const struct sgx535_task_packet_section *fs, *event;
    size_t i;
    unsigned j;
    CHECK(ir && saved_ir && w && oldw && p && saved && old && launch && launch_saved && lw);
    r = request(ir);
    memcpy(saved_ir, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    CHECK(sgx535_compiled_color0_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_compiled_color0_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(saved_ir, ir, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir)));
    CHECK(p->version == 6 && p->profile == 3 && p->missing_contracts == 0x1ff);
    CHECK(p->section_count == 16 && p->relocation_count == 29);
    fs = section(p, SGX535_PACKET_FRAGMENT_USE);
    event = section(p, SGX535_PACKET_EVENT_USE);
    CHECK(fs->resource_byte_offset == 0 && fs->byte_count == 64 &&
          event->resource_byte_offset == 128);
    for (j = 0; j < 16; j++)
        CHECK(word(p->bytes + fs->byte_offset + j * 4u) == literal[j]);
    old_request.version = 5;
    old_request.profile = 2;
    old_request.core_id = r.core_id;
    old_request.revision = r.revision;
    old_request.interpolation = 1;
    memcpy(old_request.clip_position, r.clip_position, sizeof(r.clip_position));
    memcpy(old_request.rgba, r.rgba, sizeof(r.rgba));
    CHECK(sgx535_varying_packet_build(&old_request, old, oldw) == SGX535_TASK_BLOCKED);
    old->version = 6;
    old->profile = 3;
    CHECK(!memcmp(p, old, sizeof(*p))); /* All old5 fields/bytes preserved. */
    *saved = *p;
    for (i = 0; i < sizeof(*p); i++) {
        ((uint8_t *)p)[i] ^= 1;
        CHECK(sgx535_compiled_color0_packet_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
        ((uint8_t *)p)[i] ^= 1;
    }
    CHECK(!memcmp(p, saved, sizeof(*p)));
    binding(p, &b);
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, launch, lw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_compiled_color0_launch_validate(&r, p, &b, launch, lw) == SGX535_TASK_BLOCKED);
    CHECK(launch->packet_version == 6 && launch->missing_packet_contracts == 0x1ff &&
          launch->epoch == 31);
    CHECK(launch->missing_launch_contracts ==
          (SGX535_LAUNCH_MISSING_ALL &
           ~(SGX535_LAUNCH_MISSING_EVENT_PROFILE | SGX535_LAUNCH_MISSING_CALLER_SCALARS)));
    CHECK(launch->resolved_relocations == 29 && launch->bank_index[0] == 3 &&
          launch->bank_index[1] == 15);
    CHECK(launch->stages[0].count == 3 && launch->stages[1].count == 7 &&
          launch->stages[2].count == 26);
    CHECK(launch->ta_stream_va_bytes == b.roles[SGX535_TASK_PDS].gpu_va_bytes +
                                            p->sections[p->ta_stream_section].resource_byte_offset);
    CHECK(launch->event_pds_va_bytes == b.roles[SGX535_TASK_PDS].gpu_va_bytes +
                                            p->sections[p->event_pds_section].resource_byte_offset);
    for (j = 0; j < 16; j++)
        CHECK(word(launch->bytes + fs->byte_offset + j * 4u) == literal[j]);
    *launch_saved = *launch;
    for (i = 0; i < sizeof(*launch); i++) {
        ((uint8_t *)launch)[i] ^= 1;
        CHECK(sgx535_compiled_color0_launch_validate(&r, p, &b, launch, lw) == SGX535_TASK_INVALID);
        ((uint8_t *)launch)[i] ^= 1;
    }
    for (j = 0; j < 8; j++) {
        bad_binding = b;
        bad_binding.roles[j].storage_id = 0;
        CHECK(sgx535_compiled_color0_launch_build(&r, p, &bad_binding, launch, lw) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(launch, launch_saved, sizeof(*launch)));
    }
    bad_binding = b;
    bad_binding.bank_index[0] = bad_binding.bank_index[1];
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &bad_binding, launch, lw) ==
          SGX535_TASK_INVALID);
    CHECK(!memcmp(launch, launch_saved, sizeof(*launch)));
    p->missing_contracts = 0;
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, launch, lw) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(launch, launch_saved, sizeof(*launch)));
    *p = *saved;
    /* Exact128B limit: one input and eleven identity MOVs, then four outputs.
     * This tests the backend independently of frontend copy propagation. */
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (j = 1; j < 13; j++) {
        ir[j].opcode = SGX535_FRAGMENT_MOV;
        ir[j].source[0] = j - 1;
    }
    for (j = 0; j < 4; j++) {
        ir[j + 12].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[j + 12].index = j;
        ir[j + 12].source[0] = 11;
    }
    r.fragment_count = 16;
    CHECK(sgx535_compiled_color0_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    fs = section(p, SGX535_PACKET_FRAGMENT_USE);
    CHECK(fs->byte_count == 128 && fs->resource_byte_offset + fs->byte_count == 128);
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, launch, lw) == SGX535_TASK_BLOCKED);
    CHECK(launch->missing_packet_contracts == 0x1ff && launch->resolved_relocations == 29);
    ir[12] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MOV, 0, 0, 0, {11, 0, 0}};
    for (j = 0; j < 4; j++) {
        ir[j + 13] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, j, 0, {12, 0, 0}};
    }
    r.fragment_count = 17;
    refuse(&r, p, w, SGX535_TASK_UNSUPPORTED);
    r = request(ir);
    ir[4] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x3f800000, 0, 0, {0}};
    ir[5] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MAD, 0, 0, 0, {0, 4, 1}};
    for (j = 0; j < 4; j++)
        ir[j + 6] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_OUTPUT, 0, j, 0, {j ? j : 5, 0, 0}};
    r.fragment_count = 10;
    refuse(&r, p, w, SGX535_TASK_UNSUPPORTED);
    CHECK(w->program.encoding.temp_register_count > 4);
    r = request(ir);
    ir[1].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[1].component = 0;
    ir[1].index = 3;
    refuse(&r, p, w, SGX535_TASK_UNSUPPORTED);
    CHECK(w->program.secondary_register_count == 4);
    r = request(ir);
    ir[0].index = 1;
    refuse(&r, p, w, SGX535_TASK_UNSUPPORTED);
    r = request(ir);
    ir[4].source[0] = 7;
    refuse(&r, p, w, SGX535_TASK_UNSUPPORTED);
    r = request(ir);
    *p = *saved;
    for (j = 0; j < 9; j++) {
        bad = r;
        if (j == 0)
            bad.version = 5;
        if (j == 1)
            bad.profile = 2;
        if (j == 2)
            bad.interpolation = 2;
        if (j == 3)
            bad.reserved[0] = 1;
        if (j == 4)
            bad.core_id = 0;
        if (j == 5)
            bad.revision = 0;
        if (j == 6)
            bad.clip_position[0][3] = 0x40000000;
        if (j == 7)
            bad.rgba[0][0] = 0x80000000;
        if (j == 8)
            bad.rgba[0][0] = 0x7fc12345;
        refuse(&bad, p, w, SGX535_TASK_UNSUPPORTED);
    }
    bad = r;
    bad.fragment = NULL;
    refuse(&bad, p, w, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment_count = 513;
    refuse(&bad, p, w, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment = (const void *)(uintptr_t)(UINTPTR_MAX - 7);
    refuse(&bad, p, w, SGX535_TASK_INVALID);
    bad = r;
    bad.fragment = (const void *)p;
    refuse(&bad, p, w, SGX535_TASK_INVALID);
    CHECK(sgx535_compiled_color0_packet_build(&r, (void *)w, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, (void *)lw, lw) == SGX535_TASK_INVALID);
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, (void *)ir, lw) == SGX535_TASK_INVALID);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    *launch_saved = *launch;
    fail_allocation = 1;
    refuse(&r, p, w, SGX535_TASK_NOMEM);
    CHECK(sgx535_compiled_color0_launch_build(&r, p, &b, launch, lw) == SGX535_TASK_NOMEM);
    fail_allocation = 0;
    CHECK(!memcmp(launch, launch_saved, sizeof(*launch)));
    replicated_flat();
    free(lw);
    free(launch_saved);
    free(launch);
    free(old);
    free(saved);
    free(p);
    free(oldw);
    free(w);
    free(saved_ir);
    free(ir);
    printf(
        "PASS compiled COLOR0 complete packet/launch: %lu checks; PA4/SA0/TEMP4/128B, symbolic IR only, all hardware qualifiers retained\n",
        checks);
    return 0;
}
