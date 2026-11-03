// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event_reference.h"
#include "sgx535_task_launch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        checks++;                                                                                  \
    } while (0)
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static const struct sgx535_task_packet_section *section(const struct sgx535_task_packet *p,
                                                        uint32_t kind)
{
    uint32_t i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    abort();
}
static uint32_t sw(const struct sgx535_task_packet *p, const uint8_t *b, uint32_t kind, uint32_t i)
{
    const struct sgx535_task_packet_section *s = section(p, kind);
    CHECK(i < s->byte_count / 4u);
    return word(b + s->byte_offset + i * 4u);
}
static void make_binding(const struct sgx535_task_packet *p, struct sgx535_task_launch_binding *b)
{
    const uint32_t bases[8] = {0x20003000, 0x41010000, 0x42012000, 0x43003000,
                               0x44000000, 0x45000000, 0x46000000, 0x30000000};
    uint32_t i;
    memset(b, 0, sizeof(*b));
    b->epoch = 19;
    b->bank_index[0] = 3;
    b->bank_index[1] = 15;
    for (i = 0; i < 8; i++)
        b->roles[i] = (struct sgx535_task_launch_role){
            101u + i, bases[i], p->requirements[i].minimum_bytes, p->requirements[i].access, 0};
}
static void refuse(const struct sgx535_task_request *r, const struct sgx535_task_packet *p,
                   const struct sgx535_task_launch_binding *b, struct sgx535_task_launch *out,
                   struct sgx535_task_launch_workspace *w)
{
    struct sgx535_task_launch *saved = malloc(sizeof(*saved));
    CHECK(saved != NULL);
    memcpy(saved, out, sizeof(*saved));
    CHECK(sgx535_task_launch_build(r, p, b, out, w) == SGX535_TASK_INVALID);
    CHECK(memcmp(saved, out, sizeof(*saved)) == 0);
    free(saved);
}
int main(int argc, char **argv)
{
    struct sgx535_task_request r = {3,
                                    1,
                                    SGX535_ENGINE121_CORE_ID,
                                    SGX535_ENGINE121_REVISION,
                                    {{0xbf000000, 0xbf000000, 0, 0x3f800000},
                                     {0x3f000000, 0xbf000000, 0, 0x3f800000},
                                     {0, 0x3f000000, 0, 0x3f800000}},
                                    0xff123456,
                                    {0}};
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *p2 = malloc(sizeof(*p2));
    struct sgx535_task_packet *pw = malloc(sizeof(*pw));
    struct sgx535_task_launch *out = malloc(sizeof(*out)), *saved = malloc(sizeof(*saved));
    struct sgx535_task_launch_workspace *w = malloc(sizeof(*w));
    struct sgx535_task_launch_binding b, bad;
    size_t i;
    uint32_t j, unknown = 0;
    const uint32_t ta_offsets[7] = {0x204, 0x218, 0x23c, 0x240, 0x244, 0x250, 0x238};
    const uint32_t ta_values[7] = {0, 0x400000, 0x82, 0x358637bd, 0x358637bd, 0x88, 0x20003220};
    CHECK(p && p2 && pw && out && saved && w);
    CHECK(sgx535_task_packet_build(&r, p, pw) == SGX535_TASK_BLOCKED);
    if (argc == 2 && strcmp(argv[1], "--dump-v2") == 0) {
        r.version = 2;
        CHECK(sgx535_task_packet_build(&r, p, pw) == SGX535_TASK_BLOCKED);
        CHECK(fwrite(p, 1, sizeof(*p), stdout) == sizeof(*p));
        return 0;
    }
    CHECK(argc == 1);
    make_binding(p, &b);
    CHECK(p->version == 3 && p->section_count == 16 && p->relocation_count == 29);
    CHECK(sgx535_task_launch_build(&r, p, &b, out, w) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_task_launch_validate(&r, p, &b, out, w) == SGX535_TASK_BLOCKED);
    CHECK(out->missing_packet_contracts == SGX535_PACKET_MISSING_ALL);
    CHECK(out->missing_launch_contracts ==
          (SGX535_LAUNCH_MISSING_ALL &
           ~(SGX535_LAUNCH_MISSING_EVENT_PROFILE | SGX535_LAUNCH_MISSING_CALLER_SCALARS)));
    CHECK(out->epoch == 19 && out->resolved_relocations == 29);
    CHECK(out->ta_stream_va_bytes == 0x20003220 && out->event_pds_va_bytes == 0x20003120);
    CHECK(out->stages[0].count == 3 && out->stages[1].count == 7 && out->stages[2].count == 26);
    CHECK(out->stages[0].actions[0].offset == 0xab8 &&
          out->stages[0].actions[0].value == 0x20000000);
    CHECK(out->stages[0].actions[1].offset == 0xa18 &&
          out->stages[0].actions[1].value == 0x00820000);
    CHECK(out->stages[0].actions[2].offset == 0xa48 &&
          out->stages[0].actions[2].value == 0x02840000);
    for (j = 0; j < 7; j++) {
        CHECK(out->stages[1].actions[j].offset == ta_offsets[j]);
        CHECK(out->stages[1].actions[j].value == ta_values[j]);
    }
    for (j = 0; j < out->stages[2].count; j++) {
        const struct sgx535_engine121_action *a = &out->stages[2].actions[j];
        uint32_t mask = out->stages[2].unknown_value_masks[j];
        CHECK(a->kind == SGX535_ENGINE121_WRITE && a->mask == 0);
        if (mask)
            unknown++;
        /* Independent literal oracle from the complete attribute initializer
         * -> tracked-pointer -> narrowing caller -> raster producer chain.
         * The selected state values do not enable a clear or depth storage. */
        if (a->offset == 0x4bc)
            CHECK(a->value == 0x200 && mask == 0);
        if (a->offset == 0x4b8)
            CHECK(a->value == 0x3f800000 && mask == 0);
        if (a->offset == 0x480)
            CHECK(a->value == 0xf0000);
        if (a->offset == 0xa5c)
            CHECK(a->value == 0x20003120);
        if (a->offset == 0xa60)
            CHECK(a->value == 4);
    }
    CHECK(unknown == 0);
    CHECK(out->missing_launch_contracts == 0x75);
    CHECK(out->missing_launch_contracts & SGX535_LAUNCH_MISSING_BACKGROUND_PROFILE);
    CHECK(out->missing_launch_contracts & SGX535_LAUNCH_MISSING_READSET);
    CHECK(out->missing_launch_contracts & SGX535_LAUNCH_MISSING_COLD_SOURCE);
    CHECK(out->missing_launch_contracts & SGX535_LAUNCH_MISSING_REGISTER_EFFECTS);
    CHECK(section(p, SGX535_PACKET_EVENT_PDS)->byte_count == 148);
    CHECK(section(p, SGX535_PACKET_EVENT_USE)->byte_count == 32);
    CHECK(section(p, SGX535_PACKET_MTE_STATE)->resource_byte_offset == 448);
    const struct sgx535_task_packet_section *epds = section(p, SGX535_PACKET_EVENT_PDS);
    const struct sgx535_task_packet_section *euse = section(p, SGX535_PACKET_EVENT_USE);
    CHECK(sgx535_pixel_event_reference_check(p->bytes + epds->byte_offset, epds->byte_count,
                                             p->bytes + euse->byte_offset, euse->byte_count, 1, 0,
                                             0, 0x00200000u));
    CHECK(sgx535_pixel_event_reference_check(out->bytes + epds->byte_offset, epds->byte_count,
                                             out->bytes + euse->byte_offset, euse->byte_count, 1,
                                             0x20003200u, 0x0002022fu, 0x0022032fu));
    /* Nonzero PDS and USE allocation offsets: high bits have different actual
     * consumers. The 24-bit field's coincidence with subtraction is no proof. */
    CHECK(sw(p, out->bytes, SGX535_PACKET_TA_STREAM, 0) == 0x4200030a);
    CHECK(sw(p, out->bytes, SGX535_PACKET_MTE_STATE, 2) == 0x308);
    CHECK(out->missing_launch_contracts & SGX535_LAUNCH_MISSING_PDS_DOMAIN);
    CHECK(out->roles[SGX535_TASK_TARGET].access == SGX535_TASK_ACCESS_WRITE);
    *saved = *out;
    for (i = 0; i < sizeof(*out); i++) {
        ((uint8_t *)out)[i] ^= 1;
        CHECK(sgx535_task_launch_validate(&r, p, &b, out, w) == SGX535_TASK_INVALID);
        ((uint8_t *)out)[i] ^= 1;
    }
    CHECK(memcmp(saved, out, sizeof(*out)) == 0);
    for (j = 0; j < 8; j++) {
        bad = b;
        bad.roles[j].reserved = 1;
        refuse(&r, p, &bad, out, w);
        bad = b;
        bad.roles[j].allocation_bytes -= 4096;
        refuse(&r, p, &bad, out, w);
        bad = b;
        bad.roles[j].gpu_va_bytes++;
        refuse(&r, p, &bad, out, w);
        bad = b;
        bad.roles[j].access ^= SGX535_TASK_ACCESS_WRITE;
        refuse(&r, p, &bad, out, w);
    }
    for (j = 0; j < 10; j++) {
        bad = b;
        switch (j) {
        case 0:
            bad.epoch = 0;
            break;
        case 1:
            bad.reserved[1] = 1;
            break;
        case 2:
            bad.bank_index[0] = 2;
            break;
        case 3:
            bad.bank_index[1] = 16;
            break;
        case 4:
            bad.bank_index[1] = bad.bank_index[0];
            break;
        case 5:
            bad.roles[1].storage_id = bad.roles[2].storage_id;
            break;
        case 6:
            bad.roles[2].gpu_va_bytes = bad.roles[1].gpu_va_bytes;
            break;
        case 7: /* Past the bank end. */
            bad.roles[1].gpu_va_bytes = 0x4107f000;
            bad.roles[1].allocation_bytes = 8192;
            break;
        case 8:
            bad.roles[3].gpu_va_bytes = UINT64_C(0x100000000);
            break;
        default:
            bad.roles[0].gpu_va_bytes = 0x2ffff000;
            bad.roles[0].allocation_bytes = 8192;
            break;
        }
        refuse(&r, p, &bad, out, w);
    }
    p->missing_contracts = 0;
    refuse(&r, p, &b, out, w);
    p->missing_contracts = SGX535_PACKET_MISSING_ALL;
    CHECK(sgx535_task_launch_build(&r, p, &b, &w->result, w) == SGX535_TASK_INVALID);
    CHECK(sgx535_task_launch_build(NULL, p, &b, out, w) == SGX535_TASK_INVALID);
    CHECK(memcmp(out, saved, sizeof(*out)) == 0);
    {
        union alias {
            struct sgx535_task_request r;
            struct sgx535_task_packet p;
            struct sgx535_task_launch_binding b;
            struct sgx535_task_launch out;
        };
        union alias *u = malloc(sizeof(*u)), *snapshot = malloc(sizeof(*snapshot));
        CHECK(u && snapshot);
        memset(u, 0x6b, sizeof(*u));
        u->r = r;
        *snapshot = *u;
        CHECK(sgx535_task_launch_build(&u->r, p, &b, &u->out, w) == SGX535_TASK_INVALID);
        CHECK(memcmp(u, snapshot, sizeof(*u)) == 0);
        u->p = *p;
        *snapshot = *u;
        CHECK(sgx535_task_launch_build(&r, &u->p, &b, &u->out, w) == SGX535_TASK_INVALID);
        CHECK(memcmp(u, snapshot, sizeof(*u)) == 0);
        u->b = b;
        *snapshot = *u;
        CHECK(sgx535_task_launch_build(&r, p, &u->b, &u->out, w) == SGX535_TASK_INVALID);
        CHECK(memcmp(u, snapshot, sizeof(*u)) == 0);
        free(snapshot);
        free(u);
    }
    /* v2 remains separate: event-profile field cannot be promoted by masks. */
    r.version = 2;
    CHECK(sgx535_task_packet_build(&r, p2, pw) == SGX535_TASK_BLOCKED);
    CHECK(sgx535_task_launch_build(&r, p2, &b, out, w) == SGX535_TASK_BLOCKED);
    CHECK(out->resolved_relocations == 26 &&
          out->missing_launch_contracts == SGX535_LAUNCH_MISSING_ALL);
    unknown = 0;
    for (j = 0; j < out->stages[2].count; j++) {
        const struct sgx535_engine121_action *a = &out->stages[2].actions[j];
        uint32_t mask = out->stages[2].unknown_value_masks[j];
        if (a->offset == 0x4bc) {
            CHECK(a->value == 0x200 && mask == 0xff);
            unknown++;
        }
        if (a->offset == 0x4b8) {
            CHECK(a->value == 0 && mask == 0xffffffffu);
            unknown++;
        }
        if (a->offset == 0xa60) {
            CHECK(a->value == 0 && mask == 0x3f);
            unknown++;
        }
    }
    CHECK(unknown == 3);
    CHECK(section(p2, SGX535_PACKET_EVENT_PDS)->byte_count == 128);
    epds = section(p2, SGX535_PACKET_EVENT_PDS);
    euse = section(p2, SGX535_PACKET_EVENT_USE);
    CHECK(sgx535_pixel_event_reference_check(p2->bytes + epds->byte_offset, epds->byte_count,
                                             p2->bytes + euse->byte_offset, euse->byte_count, 0, 0,
                                             0, 0x00200000u));
    CHECK(sgx535_pixel_event_reference_check(out->bytes + epds->byte_offset, epds->byte_count,
                                             out->bytes + euse->byte_offset, euse->byte_count, 0,
                                             0x200031e0u, 0, 0x0022022fu));
    CHECK(section(p2, SGX535_PACKET_PBE_STATE)->resource_byte_offset == 480);
    CHECK(out->ta_stream_va_bytes == 0x20003200);
    printf(
        "PASS task launch: %lu checks; v2/v3 separated, canonical bounds/29relocs/7TA/26raster, unknown physical fields refused\n",
        checks);
    free(w);
    free(saved);
    free(out);
    free(pw);
    free(p2);
    free(p);
    return 0;
}
