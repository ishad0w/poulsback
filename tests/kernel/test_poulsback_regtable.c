// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Independent checks of the SUBMIT register table; literals, not the table. */
#include "../../kernel/engine/poulsback_regtable.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "regtable check %u line %d: %s\n", checks, __LINE__, #x);              \
        }                                                                                          \
    } while (0)

static enum poulsback_reg_status one(sgx535_u32 phase, sgx535_u32 offset, sgx535_u32 value,
                                     int has_va, sgx535_u64 va, sgx535_u32 *out)
{
    struct poulsback_reg_request r = {offset, value, (sgx535_u32)has_va, va};
    sgx535_u32 seen = 0;
    return poulsback_reg_resolve(phase, &r, &seen, out);
}

int main(void)
{
    /* Prefix registers and BO addresses, including a background object in a
     * PARAMETER BO (BGOBJTAG relocation, docs/kernel/execution.md). */
    static const struct {
        sgx535_u32 offset, value;
        int bo;
        sgx535_u64 va;
    } ta[] = {{0x204, 0, 0, 0},          {0x218, 0x400000, 0, 0},   {0x23c, 0x82, 0, 0},
              {0x240, 0x358637bd, 0, 0}, {0x244, 0x358637bd, 0, 0}, {0x250, 0x88, 0, 0},
              {0x238, 0, 1, 0x20001680}},
      raster[] = {
          {0x400, 0xffff80, 0, 0},   {0x404, 0, 0, 0},          {0x40c, 0, 0, 0},
          {0x410, 0x120012, 0, 0},   {0x414, 0x100, 0, 0},      {0x418, 1, 0, 0},
          {0x41c, 0xda24260, 0, 0},  {0x420, 0, 0, 0},          {0x424, 0xfbf4, 0, 0},
          {0x42c, 0, 0, 0},          {0x480, 0x4513d0, 0, 0},   {0xcb0, 0, 1, 0x50000000},
          {0x484, 0, 1, 0x50000000}, {0x488, 0, 0, 0},          {0x48c, 0, 0, 0},
          {0x490, 0, 0, 0},          {0x494, 0xff, 0, 0},       {0x4c4, 0x2000000, 1, 0x34200000},
          {0x4bc, 0x200, 0, 0},      {0x4b8, 0x3f800000, 0, 0}, {0x4c8, 0x88, 0, 0},
          {0x4dc, 0, 0, 0},          {0x800, 1, 0, 0},          {0xa5c, 0, 1, 0x200001a0},
          {0xa60, 4, 0, 0},          {0xa64, 0x4fff, 0, 0}};
    static const sgx535_u32 forbidden[] = {0x200, 0x428, 0x080, 0x630, 0x620, 0x624,
                                           0x694, 0xc78, 0xc84, 0xc70, 0xc00, 0xa0c,
                                           0x130, 0x134, 0x69c, 0xab8, 0x220, 0x408};
    sgx535_u32 v, seen, i;

    seen = 0;
    for (i = 0; i < sizeof(ta) / sizeof(ta[0]); i++) {
        struct poulsback_reg_request r = {ta[i].offset, ta[i].value, (sgx535_u32)ta[i].bo,
                                          ta[i].va};
        CHECK(poulsback_reg_resolve(POULSBACK_REG_TA, &r, &seen, &v) == POULSBACK_REG_OK);
        CHECK(v == (ta[i].bo ? (sgx535_u32)ta[i].va : ta[i].value));
    }
    CHECK(poulsback_reg_complete(POULSBACK_REG_TA, seen));
    seen = 0;
    for (i = 0; i < sizeof(raster) / sizeof(raster[0]); i++) {
        struct poulsback_reg_request r = {raster[i].offset, raster[i].value,
                                          (sgx535_u32)raster[i].bo, raster[i].va};
        CHECK(poulsback_reg_resolve(POULSBACK_REG_RASTER, &r, &seen, &v) == POULSBACK_REG_OK);
    }
    CHECK(poulsback_reg_complete(POULSBACK_REG_RASTER, seen));
    /* Only a complete phase submits: none of its registers, or any one left
     * out, is incomplete (the job would run on the last job's state). */
    CHECK(!poulsback_reg_complete(POULSBACK_REG_TA, 0));
    CHECK(!poulsback_reg_complete(POULSBACK_REG_RASTER, 0));
    CHECK(!poulsback_reg_complete(7, ~0U));
    for (sgx535_u32 skip = 0; skip < sizeof(ta) / sizeof(ta[0]); skip++) {
        seen = 0;
        for (i = 0; i < sizeof(ta) / sizeof(ta[0]); i++) {
            struct poulsback_reg_request r = {ta[i].offset, ta[i].value, (sgx535_u32)ta[i].bo,
                                              ta[i].va};
            if (i != skip)
                CHECK(poulsback_reg_resolve(POULSBACK_REG_TA, &r, &seen, &v) == POULSBACK_REG_OK);
        }
        CHECK(!poulsback_reg_complete(POULSBACK_REG_TA, seen));
    }
    for (sgx535_u32 skip = 0; skip < sizeof(raster) / sizeof(raster[0]); skip++) {
        seen = 0;
        for (i = 0; i < sizeof(raster) / sizeof(raster[0]); i++) {
            struct poulsback_reg_request r = {raster[i].offset, raster[i].value,
                                              (sgx535_u32)raster[i].bo, raster[i].va};
            if (i != skip)
                CHECK(poulsback_reg_resolve(POULSBACK_REG_RASTER, &r, &seen, &v) ==
                      POULSBACK_REG_OK);
        }
        CHECK(!poulsback_reg_complete(POULSBACK_REG_RASTER, seen));
    }
    /* They are the whole table: a phase sets at most its slot count. */
    CHECK(sizeof(ta) / sizeof(ta[0]) == POULSBACK_REG_TA_SLOTS);
    CHECK(sizeof(raster) / sizeof(raster[0]) == POULSBACK_REG_RASTER_SLOTS);
    {
        unsigned known[2] = {0, 0};
        for (sgx535_u32 offset = 0; offset < 0x2000; offset += 4)
            for (sgx535_u32 phase = POULSBACK_REG_TA; phase <= POULSBACK_REG_RASTER; phase++)
                known[phase] += one(phase, offset, 0, 0, 0, &v) != POULSBACK_REG_UNKNOWN;
        CHECK(known[POULSBACK_REG_TA] == POULSBACK_REG_TA_SLOTS &&
              known[POULSBACK_REG_RASTER] == POULSBACK_REG_RASTER_SLOTS);
    }
    /* The dumped values come out as the old path wrote them. */
    CHECK(!one(POULSBACK_REG_RASTER, 0xcb0, 0, 1, 0x50000000, &v) && v == 0x50000000);
    CHECK(!one(POULSBACK_REG_RASTER, 0x484, 0, 1, 0x50000000, &v) && v == 0);
    CHECK(!one(POULSBACK_REG_RASTER, 0x484, 0, 1, 0x50001000, &v) && v == 0x1000);
    CHECK(!one(POULSBACK_REG_RASTER, 0xa5c, 0, 1, 0x200001a0, &v) && v == 0x200001a0);
    /* BGOBJTAG: TSPDATASIZE kept, the vertex pointer in 16-byte units from the
     * 3D request base 0x30000000 (EMGD: 0x02061d00 over base 0xd8000000). */
    CHECK(!one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x34200000, &v) && v == 0x2420000);
    CHECK(!one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x3ffffff0, &v) && v == 0x2ffffff);
    CHECK(!one(POULSBACK_REG_RASTER, 0x4c4, 0, 0, 0, &v) && v == 0);

    /* A register set twice in one phase. */
    {
        struct poulsback_reg_request r = {0x400, 1, 0, 0};
        seen = 0;
        CHECK(poulsback_reg_resolve(POULSBACK_REG_RASTER, &r, &seen, &v) == POULSBACK_REG_OK);
        CHECK(poulsback_reg_resolve(POULSBACK_REG_RASTER, &r, &seen, &v) ==
              POULSBACK_REG_DUPLICATE);
    }
    /* Kicks, resets, routing, DPM, enables, code banks, scene: never a job's. */
    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        CHECK(one(POULSBACK_REG_TA, forbidden[i], 0, 0, 0, &v) == POULSBACK_REG_UNKNOWN);
        CHECK(one(POULSBACK_REG_RASTER, forbidden[i], 0, 0, 0, &v) == POULSBACK_REG_UNKNOWN);
    }
    CHECK(one(POULSBACK_REG_TA, 0x400, 0, 0, 0, &v) == POULSBACK_REG_UNKNOWN);
    CHECK(one(POULSBACK_REG_RASTER, 0x238, 0, 1, 0x20000000, &v) == POULSBACK_REG_UNKNOWN);
    CHECK(one(7, 0x204, 0, 0, 0, &v) == POULSBACK_REG_UNKNOWN);

    /* Value rules. */
    CHECK(one(POULSBACK_REG_TA, 0x218, 0x01400000, 0, 0, &v) == POULSBACK_REG_BAD_VALUE);
    CHECK(one(POULSBACK_REG_TA, 0x204, 5, 1, 0x60000000, &v) == POULSBACK_REG_UNEXPECTED_BO);
    CHECK(one(POULSBACK_REG_TA, 0x238, 0x20001680, 0, 0, &v) == POULSBACK_REG_NEEDS_BO);
    CHECK(one(POULSBACK_REG_TA, 0x238, 4, 1, 0x20001680, &v) == POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_TA, 0x238, 0, 1, 0x20001682, &v) == POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_TA, 0x238, 0, 1, 0x100000000ULL, &v) == POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0xa5c, 0, 1, 0x200001a8, &v) == POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0x410, 0, 0, 0, &v) == POULSBACK_REG_OK && v == 0);
    CHECK(one(POULSBACK_REG_RASTER, 0x410, 0x120012, 1, 0x60000000, &v) ==
          POULSBACK_REG_UNEXPECTED_BO);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2200000, 0, 0, &v) == POULSBACK_REG_BAD_VALUE);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 0, 0, &v) == POULSBACK_REG_NEEDS_BO);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000010, 1, 0x34200000, &v) ==
          POULSBACK_REG_BAD_VALUE);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x32000000, &v) ==
          POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x40000000, &v) ==
          POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x34200008, &v) ==
          POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0x4c4, 0x2000000, 1, 0x60000000, &v) ==
          POULSBACK_REG_BAD_ADDRESS);
    CHECK(one(POULSBACK_REG_RASTER, 0x484, 0x1000, 0, 0, &v) == POULSBACK_REG_NEEDS_BO);
    CHECK(one(POULSBACK_REG_RASTER, 0x484, 0, 0, 0, &v) == POULSBACK_REG_OK && v == 0);
    CHECK(one(POULSBACK_REG_RASTER, 0xcb0, 1, 1, 0x50000000, &v) == POULSBACK_REG_BAD_ADDRESS);

    printf("regtable: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
