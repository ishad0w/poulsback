// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
#include "sgx535_normalized_color0_task.h"
#include "sgx535_vertex_dma.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static int fail_after = -1;
static void (*allocation_mutation)(void);
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t bytes)
{
    if (!fail_after)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    if (allocation_mutation) {
        void (*mutate)(void) = allocation_mutation;
        allocation_mutation = NULL;
        mutate();
    }
    return __real_calloc(n, bytes);
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        assert(x);                                                                                 \
    } while (0)
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
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
static struct sgx535_normalized_color0_request request(struct sgx535_fragment_instruction *ir)
{
    struct sgx535_normalized_color0_request r = {
        .version = 9,
        .profile = 6,
        .core_id = SGX535_ENGINE121_CORE_ID,
        .revision = SGX535_ENGINE121_REVISION,
        .vertices = {{{0x41000000, 0x42400000, 0x3e800000, 0x3f000000},
                      {0x42600000, 0x42300000, 0x3f000000, 0x3f800000},
                      {0x42000000, 0x41000000, 0x3f400000, 0x40000000}},
                     {{0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000},
                      {0x3f000000, 0x3f400000, 0x3e800000, 0x3f800000},
                      {0x3f400000, 0x3e800000, 0x3f000000, 0x3f800000}},
                     0},
        .fragment = ir,
        .fragment_count = 8};
    unsigned i;
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
        ir[i + 4u].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 4u].index = i;
        ir[i + 4u].source[0] = i;
    }
    return r;
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
static void masks(const struct sgx535_normalized_color0_task *p,
                  const struct sgx535_normalized_color0_launch *l)
{
    CHECK(p->packet.version == 9 && p->packet.profile == 6 && p->packet.missing_contracts == 0x1ff);
    CHECK(p->fragment.missing_contracts == 0x3ff && p->fragment.reference_policy == 1);
    CHECK(!p->fragment.arithmetic_qualified && !p->fragment.numerical_qualified &&
          !p->fragment.linkage_qualified);
    CHECK(!p->fragment.output_qualified && !p->fragment.hardware_qualified &&
          !p->fragment.nonfinite_behavior_qualified);
    CHECK(sgx535_fragment_normalized_native_admission(&p->fragment) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(l->launch.packet_version == 9 && l->launch.missing_packet_contracts == 0x1ff &&
          l->launch.missing_launch_contracts == 0x75 && l->fragment_missing_contracts == 0x3ff);
    CHECK(l->reference_policy == 1 && !l->arithmetic_qualified && !l->numerical_qualified &&
          !l->linkage_qualified);
    CHECK(!l->output_qualified && !l->hardware_qualified && !l->nonfinite_behavior_qualified);
}
static void extents(const struct sgx535_task_packet *p)
{
    unsigned i, j;
    CHECK(p->section_count == 16 && p->relocation_count == 29 && p->byte_count <= 2048);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        CHECK(s->byte_count && s->alignment && !(s->resource_byte_offset % s->alignment));
        CHECK(s->byte_offset <= p->byte_count && s->byte_count <= p->byte_count - s->byte_offset);
        CHECK(s->resource_byte_offset + s->byte_count <= p->requirements[s->role].minimum_bytes);
        for (j = 0; j < i; j++) {
            const struct sgx535_task_packet_section *t = &p->sections[j];
            CHECK(s->byte_offset >= t->byte_offset + t->byte_count ||
                  t->byte_offset >= s->byte_offset + s->byte_count);
            if (s->role == t->role)
                CHECK(s->resource_byte_offset >= t->resource_byte_offset + t->byte_count ||
                      t->resource_byte_offset >= s->resource_byte_offset + s->byte_count);
        }
    }
}
static void references(const struct sgx535_task_packet *p, unsigned code_bytes,
                       unsigned event_offset)
{
    unsigned i, fragment = 0, event0 = 0, event1 = 0;
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        unsigned kind = p->sections[r->section].kind;
        if (r->role != SGX535_TASK_USE_FRAGMENT)
            continue;
        if (kind == SGX535_PACKET_FRAGMENT_PDS) {
            CHECK(!r->resource_byte_offset && r->span_bytes == code_bytes);
            fragment++;
        } else {
            CHECK(kind == SGX535_PACKET_EVENT_PDS);
            if (!r->section_byte_offset) {
                CHECK(r->resource_byte_offset == event_offset && r->span_bytes == 8);
                event0++;
            } else {
                CHECK(r->section_byte_offset == 16 &&
                      r->resource_byte_offset == event_offset + 16 && r->span_bytes == 16);
                event1++;
            }
        }
    }
    CHECK(fragment == 3 && event0 == 3 && event1 == 3);
}
static void refuse(const struct sgx535_normalized_color0_request *r,
                   struct sgx535_normalized_color0_task *p,
                   struct sgx535_normalized_color0_workspace *w, enum sgx535_task_status expected)
{
    struct sgx535_normalized_color0_task *saved = malloc(sizeof(*saved));
    CHECK(saved);
    *saved = *p;
    CHECK(sgx535_normalized_color0_task_build(r, p, w) == expected);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    free(saved);
}
static void chain(struct sgx535_fragment_instruction *ir, unsigned count)
{
    unsigned i, definitions = count - 4u;
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (i = 1; i < definitions; i++) {
        ir[i].opcode = SGX535_FRAGMENT_MOV;
        ir[i].source[0] = i - 1u;
    }
    for (i = 0; i < 4; i++)
        ir[definitions + i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_OUTPUT, 0, i, 0, {definitions - 1u, 0, 0}};
}
/* n distinct live scalars (COLOR0.r and constants) force TEMPn before MIN can release
 * either source. This independent lifetime argument predicts count n+1. */
static unsigned pressure(struct sgx535_fragment_instruction *ir, unsigned n)
{
    unsigned i, last = 0;
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (i = 1; i < n; i++)
        ir[i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_CONSTANT, 0x3e000001u + i, 0, 0, {0, 0, 0}};
    for (i = 1; i < n; i++) {
        ir[n + i - 1u] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MIN, 0, 0, 0, {last, i, 0}};
        last = n + i - 1u;
    }
    for (i = 0; i < 4; i++)
        ir[2u * n - 1u + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {last, 0, 0}};
    return 2u * n + 3u;
}

static uint32_t *pa0_mutable_uniform;
static struct sgx535_fragment_instruction *pa0_mutable_ir;
static void pa0_change_uniform(void)
{
    pa0_mutable_uniform[0] ^= 1u;
}
static void pa0_change_ir(void)
{
    pa0_mutable_ir[0].index ^= 1u;
}
static void pa0_extents(const struct sgx535_task_packet *p)
{
    CHECK(p->section_count == 16 && p->relocation_count <= 40 && p->byte_count <= 2048);
    for (unsigned i = 0; i < 16; i++) {
        const struct sgx535_task_packet_section *a = &p->sections[i];
        CHECK(a->resource_byte_offset + a->byte_count <= 4096 &&
              a->byte_offset + a->byte_count <= p->byte_count &&
              !(a->resource_byte_offset & (a->alignment - 1u)));
        for (unsigned j = 0; j < i; j++) {
            const struct sgx535_task_packet_section *b = &p->sections[j];
            CHECK(a->byte_offset >= b->byte_offset + b->byte_count ||
                  b->byte_offset >= a->byte_offset + a->byte_count);
            if (a->role == b->role)
                CHECK(a->resource_byte_offset >= b->resource_byte_offset + b->byte_count ||
                      b->resource_byte_offset >= a->resource_byte_offset + a->byte_count);
        }
    }
}
static void pa0_candidate(void)
{
    static const struct {
        uint32_t s, transfers, first, second, dms, budget;
    } controls[] = {{2, 1, 0x80000001, 0, 0x00078000, 394},
                    {16, 1, 0x8000000f, 0, 0x00078000, 394},
                    {5, 1, 0x80000004, 0, 0x00078000, 394},
                    {17, 2, 0x01e0000f, 0x80001000, 0x00078000, 394},
                    {31, 2, 0x01e0000f, 0x81c0100e, 0x00078000, 394},
                    {32, 1, 0x80e00037, 0, 0x00078000, 394},
                    {33, 1, 0x8140002a, 0, 0x000b8000, 384},
                    {128, 1, 0x80e000f7, 0, 0x00138000, 341}};
    struct sgx535_normalized_pa0_workspace *w = calloc(1, sizeof(*w));
    struct sgx535_normalized_pa0_launch_workspace *lw = calloc(1, sizeof(*lw));
    struct sgx535_normalized_pa0_task *p = calloc(1, sizeof(*p)), *saved = malloc(sizeof(*saved));
    struct sgx535_normalized_color0_launch *l = calloc(1, sizeof(*l)), *sl = malloc(sizeof(*sl));
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_task_launch_binding b;
    uint32_t uniforms[128];
    struct sgx535_normalized_pa0_request r = {
        .policy = SGX535_NORMALIZED_PA0_RESERVED_SA0_V1,
        .core_id = SGX535_ENGINE121_CORE_ID,
        .revision = SGX535_ENGINE121_REVISION,
        .clip_position = {{0xbf000000, 0xbf000000, 0, 0x3f800000},
                          {0x3f000000, 0xbf000000, 0, 0x3f800000},
                          {0, 0x3f000000, 0, 0x3f800000}},
        .fragment = ir,
        .fragment_count = 8,
        .uniforms = uniforms,
        .uniform_word_count = 128};
    CHECK(w && lw && p && saved && l && sl && ir);
    for (unsigned i = 0; i < 128; i++)
        uniforms[i] = 0x7fc00000u + i;
    uniforms[0] = 0x80000000u;
    for (unsigned i = 0; i < 4; i++) {
        ir[i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_UNIFORM, .index = i};
        ir[4 + i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i, 0, 0}};
    }
    for (unsigned k = 0; k < sizeof(controls) / sizeof(controls[0]); k++) {
        uint32_t s = controls[k].s;
        for (unsigned i = 0; i < 4; i++)
            ir[i].index = s == 2 ? 0 : i;
        ir[0].index = s == 5 || s == 2 ? 0 : s - 2;
        CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_pa0_task_validate(&r, p, w) == SGX535_TASK_BLOCKED);
        CHECK(p->task.packet.version == 16 && p->task.packet.profile == 15 &&
              p->task.packet.missing_contracts == 0x1ff &&
              p->task.fragment.missing_contracts == 0x3ff && !p->task.fragment.hardware_qualified &&
              !p->task.fragment.primary_register_count && p->task.fragment_temp_words == 7 &&
              p->physical_sa_words == s && p->logical_uniform_extent == s - 1 &&
              p->uniform_sa_words == s - 1 && p->reserved_sa_words == 1 && !p->literal_sa_words &&
              !p->sampler_state_sa_words && p->task.fragment.secondary_register_count == s &&
              p->dms_word == controls[k].dms && p->dms_sa_budget_words == controls[k].budget);
        /* Independent register-field reads; no encoder roundtrip supplies these expected indices. */
        for (unsigned i = 0; i < 4; i++) {
            uint32_t low = p->task.fragment.encoding.words[2 * i];
            CHECK((low >> 30) == 3 && ((low >> 7) & 127u) == ir[i].index + 1u);
            CHECK(p->logical_uniform_words_used[ir[i].index / 32] & (1u << (ir[i].index % 32)));
            CHECK(p->task.fragment.uniform_words_used[(ir[i].index + 1) / 32] &
                  (1u << ((ir[i].index + 1) % 32)));
        }
        const struct sgx535_task_packet *packet = &p->task.packet;
        const struct sgx535_task_packet_section *primary =
            section(packet, SGX535_PACKET_FRAGMENT_PDS);
        const struct sgx535_task_packet_section *secondary =
            section(packet, SGX535_PACKET_SECONDARY_PDS);
        const struct sgx535_task_packet_section *event = section(packet, SGX535_PACKET_EVENT_USE);
        const struct sgx535_task_packet_section *mte = section(packet, SGX535_PACKET_MTE_STATE);
        CHECK(primary->resource_byte_offset == 64 && primary->byte_count == 56 &&
              word(packet->bytes + primary->byte_offset + 48) == 0x07000345 &&
              word(packet->bytes + primary->byte_offset + 52) == 0xaf000000 &&
              !word(packet->bytes + primary->byte_offset + 36)); /* No iterator. */
        CHECK(p->secondary_resource_offset_bytes == 608 && secondary->resource_byte_offset == 608 &&
              p->secondary_data_words == 16 && p->secondary_code_offset_bytes == 64 &&
              p->secondary_capacity_bytes == 96 && p->upload_resource_offset_bytes == 704 &&
              p->upload_bytes == 4 * s && secondary->byte_count == 96 + 4 * s &&
              p->secondary_program_bytes == (controls[k].transfers == 2 ? 80u : 76u));
        if (word(packet->bytes + secondary->byte_offset + 4) != controls[k].first ||
            word(packet->bytes + secondary->byte_offset + 36) != controls[k].second)
            fprintf(stderr, "PA0 DMA S%u actual %08x/%08x expected %08x/%08x\n", s,
                    word(packet->bytes + secondary->byte_offset + 4),
                    word(packet->bytes + secondary->byte_offset + 36), controls[k].first,
                    controls[k].second);
        CHECK(word(packet->bytes + secondary->byte_offset + 4) == controls[k].first &&
              word(packet->bytes + secondary->byte_offset + 36) == controls[k].second &&
              word(packet->bytes + secondary->byte_offset + 64) == 0x07030223);
        if (controls[k].transfers == 2)
            CHECK(word(packet->bytes + secondary->byte_offset + 68) == 0x07601763);
        CHECK(word(packet->bytes + secondary->byte_offset + p->secondary_program_bytes - 8) ==
                  0x07042345 &&
              word(packet->bytes + secondary->byte_offset + p->secondary_program_bytes - 4) ==
                  0xaf000000);
        CHECK(!word(packet->bytes + secondary->byte_offset + 12) &&
              !word(packet->bytes + secondary->byte_offset + 40));
        CHECK(!word(packet->bytes + secondary->byte_offset + 96)); /* Reserved SA0. */
        for (unsigned i = 1; i < s; i++)
            CHECK(word(packet->bytes + secondary->byte_offset + 96 + 4 * i) == uniforms[i - 1]);
        for (unsigned i = p->secondary_program_bytes; i < 96; i++)
            CHECK(!packet->bytes[secondary->byte_offset + i]);
        CHECK(event->byte_count == 40 && p->dummy_use_bytes == 8 &&
              p->dummy_use_offset_bytes == event->resource_byte_offset + 32 &&
              !(p->dummy_use_offset_bytes & 31u) &&
              !word(packet->bytes + event->byte_offset + 32) &&
              word(packet->bytes + event->byte_offset + 36) == 0xf8040140 &&
              word(packet->bytes + mte->byte_offset + 8) == 0x10000000 &&
              word(packet->bytes + mte->byte_offset + 12) == controls[k].dms);
        pa0_extents(packet);
        binding(packet, &b);
        CHECK(sgx535_normalized_pa0_launch_build(&r, p, &b, l, lw) == SGX535_TASK_BLOCKED);
        CHECK(sgx535_normalized_pa0_launch_validate(&r, p, &b, l, lw) == SGX535_TASK_BLOCKED);
        CHECK(l->launch.missing_packet_contracts == 0x1ff &&
              l->launch.missing_launch_contracts == 0x75 &&
              l->fragment_missing_contracts == 0x3ff && !l->hardware_qualified &&
              word(l->launch.bytes + secondary->byte_offset) == 0x20003000 + 704 &&
              word(l->launch.bytes + mte->byte_offset + 8) ==
                  (0x10000000u | ((0x20003000u + 608u) >> 4 & 0xffffffu)));
        if (controls[k].transfers == 2)
            CHECK(word(l->launch.bytes + secondary->byte_offset + 32) == 0x20003000 + 704 + 64);
        unsigned dma_refs = 0, dummy_refs = 0, secondary_refs = 0;
        for (unsigned i = 0; i < packet->relocation_count; i++) {
            const struct sgx535_task_packet_reloc *rel = &packet->relocations[i];
            if (packet->sections[rel->section].kind == SGX535_PACKET_SECONDARY_PDS) {
                if (rel->role == SGX535_TASK_PDS) {
                    CHECK(rel->resource_byte_offset >= 704 &&
                          rel->resource_byte_offset + rel->span_bytes <= 704 + 4 * s);
                    dma_refs++;
                } else {
                    CHECK(rel->role == SGX535_TASK_USE_FRAGMENT && rel->span_bytes == 8 &&
                          rel->resource_byte_offset == p->dummy_use_offset_bytes);
                    dummy_refs++;
                }
            }
            if (packet->sections[rel->section].kind == SGX535_PACKET_MTE_STATE &&
                rel->section_byte_offset == 8) {
                CHECK(rel->resource_byte_offset == 608 &&
                      rel->span_bytes == p->secondary_program_bytes);
                secondary_refs++;
            }
        }
        CHECK(dma_refs == controls[k].transfers && dummy_refs == 3 && secondary_refs == 1);
        *saved = *p;
        *sl = *l;
        for (unsigned i = 0; i < sizeof(*p); i += 71) {
            ((unsigned char *)p)[i] ^= 1;
            CHECK(sgx535_normalized_pa0_task_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
            CHECK(sgx535_normalized_pa0_launch_build(&r, p, &b, l, lw) == SGX535_TASK_UNSUPPORTED);
            CHECK(!memcmp(l, sl, sizeof(*l)));
            ((unsigned char *)p)[i] ^= 1;
        }
        for (unsigned i = 0; i < packet->relocation_count; i++) {
            p->task.packet.relocations[i].resource_byte_offset ^= 4;
            CHECK(sgx535_normalized_pa0_task_validate(&r, p, w) == SGX535_TASK_UNSUPPORTED);
            p->task.packet.relocations[i].resource_byte_offset ^= 4;
        }
        CHECK(!memcmp(p, saved, sizeof(*p)));
    }
    /* S1 is the shared direct descriptor's exact boundary. This shader policy
     * has either S0 or at least reserved0 + one referenced word (S2). */
    struct sgx535_vertex_dma_plan one;
    CHECK(sgx535_vertex_dma_build(1, &one) == SGX535_VERTEX_DMA_OK && one.transfer_count == 1 &&
          one.transfers[0].word_count == 1 && one.transfers[0].descriptor_word == 0x80000000);
    *saved = *p;
    ir[0].index = 127;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED &&
          !memcmp(p, saved, sizeof(*p)));
    ir[0].index = 126;
    r.constant_flags = 0x80;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    r.constant_flags = 0;
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[0].source[0] = 0;
    r.uniform_word_count = 126;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED);
    r.uniform_word_count = 128;
    pa0_mutable_uniform = uniforms;
    allocation_mutation = pa0_change_uniform;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED &&
          !memcmp(p, saved, sizeof(*p)));
    pa0_mutable_ir = ir;
    allocation_mutation = pa0_change_ir;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED &&
          !memcmp(p, saved, sizeof(*p)));
    ir[0].index = 126;
    for (unsigned i = 0; i < 6; i++) {
        fail_after = (int)i;
        CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_NOMEM &&
              !memcmp(p, saved, sizeof(*p)));
        fail_after = -1;
    }
    CHECK(sgx535_normalized_pa0_task_build(&r, (void *)w, w) == SGX535_TASK_INVALID);
    struct sgx535_normalized_pa0_request bad = r;
    bad.uniforms = (void *)p;
    CHECK(sgx535_normalized_pa0_task_build(&bad, p, w) == SGX535_TASK_INVALID);
    bad = r;
    bad.uniforms = (void *)(uintptr_t)(UINTPTR_MAX - 3);
    CHECK(sgx535_normalized_pa0_task_build(&bad, p, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_pa0_launch_build(&r, p, &b, (void *)lw, lw) == SGX535_TASK_INVALID);
    /* Hard2048 bound independently predicted from all sixteen section sizes:
     * S128, eighty original nodes =>2040; one more node =>2056. */
    chain(ir, 80);
    ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[0].index = 126;
    r.fragment_count = 80;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_BLOCKED &&
          p->task.packet.byte_count == 2040);
    *saved = *p;
    chain(ir, 81);
    ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[0].index = 126;
    r.fragment_count = 81;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_UNSUPPORTED &&
          !memcmp(p, saved, sizeof(*p)));
    /* No uniforms means no reserved upload, dummy or secondary allocation. */
    chain(ir, 5);
    ir[0].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[0].immediate = 0x3f000000;
    r.fragment_count = 5;
    r.uniforms = NULL;
    r.uniform_word_count = 0;
    CHECK(sgx535_normalized_pa0_task_build(&r, p, w) == SGX535_TASK_BLOCKED &&
          !p->physical_sa_words && !p->reserved_sa_words && !p->upload_bytes &&
          !p->dummy_use_bytes);
    CHECK(section(&p->task.packet, SGX535_PACKET_SECONDARY_PDS)->byte_count == 4 &&
          section(&p->task.packet, SGX535_PACKET_SECONDARY_PDS)->resource_byte_offset == 128 &&
          section(&p->task.packet, SGX535_PACKET_EVENT_USE)->byte_count == 32);
    free(ir);
    free(sl);
    free(l);
    free(saved);
    free(p);
    free(lw);
    free(w);
    puts(
        "PASS PA0 ordinary SA candidate: logical/physical registers, raw copied prefix, DMA/PDS/DMS, complete relocation, hard bounds, drift and atomic refusal; CPU only");
}

int main(void)
{
    /* The normalized fragment literal and event semantic reference are
     * independent of the compiler, packet and launch builders. */
    static const uint32_t literal[36] = {
        0xa0000100, 0x28a10000, 0xa0200080, 0x28a10000, 0xa0400000, 0x28a10000,
        0xa0600180, 0x28a10000, 0x00800000, 0xfca00000, 0x00a00000, 0xfca0f1c0,
        0x00c00004, 0x18a00200, 0x00000305, 0x18a00000, 0x00c00084, 0x18a00200,
        0x00200305, 0x18a00000, 0x00c00104, 0x18a00200, 0x00400305, 0x18a00000,
        0x00c00184, 0x18a00200, 0x00600305, 0x18a00000, 0x20040000, 0x40a10c11,
        0x20040080, 0x40a10c09, 0x20040100, 0x40a10c05, 0x20040180, 0x40a50c21};
    static const struct {
        unsigned n, code, event, allocation, low, high, dms;
    } resource[] = {{8, 232, 256, 48, 0x48000000, 0x20, 0x30030004},
                    {12, 296, 320, 64, 0x68000000, 0x20, 0x2a020004},
                    {16, 360, 384, 80, 0x88000000, 0x20, 0x1a010004},
                    {40, 744, 768, 176, 0x48000000, 0x21, 0x0a000004},
                    {64, 1128, 1152, 272, 0x08000000, 0x22, 0x0a000004}};
    struct sgx535_fragment_instruction *ir = malloc(SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    struct sgx535_normalized_color0_task *p = malloc(sizeof(*p)), *saved = malloc(sizeof(*saved));
    struct sgx535_normalized_color0_workspace *w = malloc(sizeof(*w));
    struct sgx535_normalized_color0_launch *l = malloc(sizeof(*l)),
                                           *saved_l = malloc(sizeof(*saved_l));
    struct sgx535_normalized_color0_launch_workspace *lw = malloc(sizeof(*lw));
    struct inputs {
        struct sgx535_fragment_instruction copy[8];
        struct sgx535_normalized_color0_request r, bad;
        struct sgx535_task_launch_binding b, bad_b;
        struct sgx535_window_color0_request old_r;
    } *f = malloc(sizeof(*f));
    struct sgx535_compiled_color0_workspace *old_w = malloc(sizeof(*old_w));
    struct sgx535_task_packet *old_p = malloc(sizeof(*old_p)),
                              *old_saved = malloc(sizeof(*old_saved));
    const struct sgx535_task_packet_section *fs, *event, *fpds, *mte, *epds;
    unsigned i, j;
    CHECK(f && ir && p && saved && w && l && saved_l && lw && old_w && old_p && old_saved);
    memset(f, 0, sizeof(*f));
    f->r = request(ir);
    memcpy(f->copy, ir, sizeof(f->copy));
    f->old_r.version = 7;
    f->old_r.profile = 4;
    f->old_r.core_id = f->r.core_id;
    f->old_r.revision = f->r.revision;
    f->old_r.vertices = f->r.vertices;
    f->old_r.fragment = ir;
    f->old_r.fragment_count = 8;
    CHECK(sgx535_window_color0_packet_build(&f->old_r, old_saved, old_w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_task_validate(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(f->copy, ir, sizeof(f->copy)));
    fs = section(&p->packet, SGX535_PACKET_FRAGMENT_USE);
    event = section(&p->packet, SGX535_PACKET_EVENT_USE);
    fpds = section(&p->packet, SGX535_PACKET_FRAGMENT_PDS);
    mte = section(&p->packet, SGX535_PACKET_MTE_STATE);
    epds = section(&p->packet, SGX535_PACKET_EVENT_PDS);
    CHECK(p->fragment_code_bytes == 144 && p->fragment_temp_words == 7 &&
          p->dms_temp_allocation_bytes == 32);
    CHECK(p->event_use_offset_bytes == 160 && p->event_use_bytes == 32 &&
          p->fragment_use_extent_bytes == 192);
    CHECK(fs->byte_count == 144 && !fs->resource_byte_offset && event->resource_byte_offset == 160);
    CHECK(p->packet.byte_count == 904 &&
          word(p->packet.bytes + fpds->byte_offset + 4) == 0x38000000);
    CHECK(word(p->packet.bytes + fpds->byte_offset + 32) == 0x20 &&
          word(p->packet.bytes + mte->byte_offset + 12) == 0x30030004);
    for (i = 0; i < 36; i++)
        CHECK(word(p->packet.bytes + fs->byte_offset + i * 4u) == literal[i]);
    CHECK(epds->byte_count == 148 && event->byte_count == 32);
    CHECK(sgx535_pixel_event_reference_check(p->packet.bytes + epds->byte_offset, epds->byte_count,
                                             p->packet.bytes + event->byte_offset,
                                             event->byte_count, 1, 0, 0, 0x00200000u));
    extents(&p->packet);
    references(&p->packet, 144, 160);
    binding(&p->packet, &f->b);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_launch_validate(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    CHECK(l->launch.resolved_relocations == 29 && l->launch.stages[0].count == 3 &&
          l->launch.stages[1].count == 7 && l->launch.stages[2].count == 26);
    /* Fragment VA42012000 / bank base42000000, bank15. Event starts at
     * +a0 and its second task at+b0: sixteen-byte entries120a/120b. */
    CHECK(word(l->launch.bytes + fpds->byte_offset) == 0x000a002f);
    CHECK(word(l->launch.bytes + epds->byte_offset) == 0x00020a2f);
    CHECK(word(l->launch.bytes + epds->byte_offset + 16) == 0x00220b2f);
    CHECK(sgx535_window_color0_packet_build(&f->old_r, old_p, old_w) == SGX535_TASK_BLOCKED);
    CHECK(!memcmp(old_p, old_saved, sizeof(*old_p)) &&
          section(old_p, SGX535_PACKET_FRAGMENT_USE)->byte_count == 64 &&
          section(old_p, SGX535_PACKET_EVENT_USE)->resource_byte_offset == 128);
    *saved = *p;
    *saved_l = *l;
    for (i = 0; i < sizeof(*p); i += 37) {
        ((uint8_t *)p)[i] ^= 1;
        CHECK(sgx535_normalized_color0_task_validate(&f->r, p, w) == SGX535_TASK_UNSUPPORTED);
        CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) ==
              SGX535_TASK_UNSUPPORTED);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
        ((uint8_t *)p)[i] ^= 1;
    }
    p->fragment.missing_contracts = 0;
    p->fragment.hardware_qualified = 1;
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_UNSUPPORTED);
    *p = *saved;
    for (i = 0; i < sizeof(*l); i += 43) {
        ((uint8_t *)l)[i] ^= 1;
        CHECK(sgx535_normalized_color0_launch_validate(&f->r, p, &f->b, l, lw) ==
              SGX535_TASK_INVALID);
        ((uint8_t *)l)[i] ^= 1;
    }
    for (i = 0; i < 8; i++) {
        f->bad_b = f->b;
        f->bad_b.roles[i].storage_id = 0;
        CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->bad_b, l, lw) ==
              SGX535_TASK_INVALID);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
    }
    f->bad_b = f->b;
    f->bad_b.roles[2].gpu_va_bytes = 0x42017f80;
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->bad_b, l, lw) == SGX535_TASK_INVALID);
    CHECK(!memcmp(l, saved_l, sizeof(*l))); /* Event crosses the task's32KiB entry segment. */
    f->bad_b = f->b;
    f->bad_b.bank_index[0] = f->bad_b.bank_index[1];
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->bad_b, l, lw) == SGX535_TASK_INVALID);
    for (i = 0; i < sizeof(resource) / sizeof(resource[0]); i++) {
        f->r.fragment_count = pressure(ir, resource[i].n);
        CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
        CHECK(p->fragment_temp_words == resource[i].n + 1 &&
              p->fragment_code_bytes == resource[i].code &&
              p->dms_temp_allocation_bytes == resource[i].allocation &&
              p->event_use_offset_bytes == resource[i].event);
        fpds = section(&p->packet, SGX535_PACKET_FRAGMENT_PDS);
        mte = section(&p->packet, SGX535_PACKET_MTE_STATE);
        CHECK(word(p->packet.bytes + fpds->byte_offset + 4) == resource[i].low &&
              word(p->packet.bytes + fpds->byte_offset + 32) == resource[i].high &&
              word(p->packet.bytes + mte->byte_offset + 12) == resource[i].dms);
        extents(&p->packet);
        references(&p->packet, resource[i].code, resource[i].event);
        CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
        masks(p, l);
    }
    /* Three-source reductions keep96 scalars live before the first MAD.
     * TEMP97 exercises both high TEMP bits while the complete code still
     * fits:96 definitions +48 reductions +14 normalized tail =158 pairs. */
    (void)pressure(ir, 96);
    for (i = 0; i < 47; i++)
        ir[96 + i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_MAD, 0, 0, 0, {i ? 95u + i : 0, 1u + i * 2u, 2u + i * 2u}};
    ir[143] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MIN, 0, 0, 0, {142, 95, 0}};
    for (i = 0; i < 4; i++)
        ir[144 + i] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {143, 0, 0}};
    f->r.fragment_count = 148;
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->fragment_temp_words == 97 && p->fragment_code_bytes == 1264 &&
          p->dms_temp_allocation_bytes == 400 && p->event_use_offset_bytes == 1280);
    fpds = section(&p->packet, SGX535_PACKET_FRAGMENT_PDS);
    mte = section(&p->packet, SGX535_PACKET_MTE_STATE);
    CHECK(word(p->packet.bytes + fpds->byte_offset + 4) == 0x08000000 &&
          word(p->packet.bytes + fpds->byte_offset + 32) == 0x23 &&
          word(p->packet.bytes + mte->byte_offset + 12) == 0x0a000004);
    extents(&p->packet);
    references(&p->packet, 1264, 1280);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    /* 150 original nodes lower to160*8=1280 code bytes: final payload2040.
     * One further MOV needs1288 code bytes and the aligned payload2056. */
    chain(ir, 150);
    f->r.fragment_count = 150;
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->fragment_code_bytes == 1280 && p->event_use_offset_bytes == 1280 &&
          p->packet.byte_count == 2040);
    extents(&p->packet);
    references(&p->packet, 1280, 1280);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    *saved_l = *l;
    chain(ir, 151);
    f->r.fragment_count = 151;
    refuse(&f->r, p, w, SGX535_TASK_UNSUPPORTED);
    CHECK(w->result.fragment.encoding.word_count == 322);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_UNSUPPORTED);
    CHECK(!memcmp(l, saved_l, sizeof(*l)));
    /* Arbitrary supported scalar operations survive lowering, independent of
     * triangle/color values: MAD, MIN, RCP and RSQ before four distinct outputs. */
    f->r = request(ir);
    ir[4] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x3f800000, 0, 0, {0}};
    ir[5] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0, 0, 0, {0}};
    ir[6] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MAD, 0, 0, 0, {0, 4, 5}};
    ir[7] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MIN, 0, 0, 0, {6, 4, 0}};
    ir[8] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RCP, 0, 0, 0, {4, 0, 0}};
    ir[9] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RSQ, 0, 0, 0, {4, 0, 0}};
    for (i = 0; i < 4; i++) {
        static const unsigned sources[4] = {7, 1, 8, 9};
        ir[10 + i] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_OUTPUT, 0, i, 0, {sources[i], 0, 0}};
    }
    f->r.fragment_count = 14;
    f->r.vertices.window_xyz_reciprocal_w[0][0] = 0x41880000;
    f->r.vertices.rgba[1][2] = 0x3e000001;
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(p->fragment_code_bytes == 192 && p->fragment.source_instruction_count == 14 &&
          p->fragment.lowered_instruction_count == 24);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    f->r = request(ir);
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    *saved = *p;
    for (i = 0; i < 9; i++) {
        f->bad = f->r;
        if (i == 0)
            f->bad.version = 7;
        if (i == 1)
            f->bad.profile = 4;
        if (i == 2)
            f->bad.reserved[2] = 1;
        if (i == 3)
            f->bad.core_id++;
        if (i == 4)
            f->bad.vertices.window_xyz_reciprocal_w[1][3] = 0;
        if (i == 5)
            f->bad.vertices.rgba[2][3] = 0x7fc00000;
        if (i == 6)
            f->bad.vertices.replicated_flat = 1;
        if (i == 7) {
            ir[2].opcode = SGX535_FRAGMENT_UNIFORM;
            ir[2].component = 0;
            ir[2].index = 0;
        }
        if (i == 8)
            ir[4].source[0] = 4;
        refuse(&f->bad, p, w, SGX535_TASK_UNSUPPORTED);
        f->r = request(ir);
    }
    for (i = 0; i < 4; i++) {
        static const uint32_t nonfinite[4] = {0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001};
        ir[2] =
            (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, nonfinite[i], 0, 0, {0}};
        refuse(&f->r, p, w, SGX535_TASK_UNSUPPORTED);
        f->r = request(ir);
    }
    /* Constant overflow may still produce infinity at runtime. Encoding is
     * accepted, and the explicit finite-result/nonfinite qualifiers stay0. */
    ir[0] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0x7f7fffff, 0, 0, {0}};
    ir[1] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MAD, 0, 0, 0, {0, 0, 0}};
    CHECK(sgx535_normalized_color0_task_build(&f->r, p, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_BLOCKED);
    masks(p, l);
    f->r = request(ir);
    *p = *saved;
    f->bad = f->r;
    f->bad.fragment = NULL;
    refuse(&f->bad, p, w, SGX535_TASK_INVALID);
    f->bad = f->r;
    f->bad.fragment_count = 513;
    refuse(&f->bad, p, w, SGX535_TASK_INVALID);
    f->bad = f->r;
    f->bad.fragment = (const void *)(uintptr_t)(UINTPTR_MAX - 7);
    refuse(&f->bad, p, w, SGX535_TASK_INVALID);
    f->bad = f->r;
    f->bad.fragment = (const void *)p;
    refuse(&f->bad, p, w, SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_task_build(&f->r, (void *)w, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, (void *)lw, lw) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, (void *)ir, lw) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, (void *)p, lw) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, p, (void *)lw, l, lw) ==
          SGX535_TASK_INVALID);
    CHECK(sgx535_normalized_color0_launch_build(&f->r, (void *)lw, &f->b, l, lw) ==
          SGX535_TASK_INVALID);
    CHECK(!memcmp(p, saved, sizeof(*p)));
    for (j = 0; j < 3; j++) {
        *saved_l = *l;
        fail_after = (int)j;
        refuse(&f->r, p, w, SGX535_TASK_NOMEM);
        fail_after = (int)j;
        CHECK(sgx535_normalized_color0_launch_build(&f->r, p, &f->b, l, lw) == SGX535_TASK_NOMEM);
        CHECK(!memcmp(l, saved_l, sizeof(*l)));
        fail_after = -1;
    }
    pa0_candidate();
    free(f);
    free(old_saved);
    free(old_p);
    free(old_w);
    free(lw);
    free(saved_l);
    free(l);
    free(w);
    free(saved);
    free(p);
    free(ir);
    printf(
        "PASS normalized COLOR0 task/launch: %u checks; literal code/event/relocations, TEMP7..97, bounded growth; all qualifiers retained\n",
        checks);
    return 0;
}
