// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)
static uint8_t blob[16u + 2u * MSVDX_FW_MAX_REGION_BYTES + 1u];
static void put(uint8_t *p, uint32_t x)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(x >> (i * 8u));
}
static size_t make(uint32_t text, uint32_t data, uint32_t offset)
{
    size_t i, n = 16u + (size_t)(text + data) * 4u;
    memset(blob, 0, sizeof(blob));
    put(blob, 2);
    put(blob + 4, text);
    put(blob + 8, data);
    put(blob + 12, MSVDX_FW_DATA_BASE + offset);
    for (i = 16; i < n; ++i)
        blob[i] = (uint8_t)(i * 71u + 3u);
    return n;
}
static void plans(void)
{
    uint32_t bank;
    for (bank = 4; bank <= 4096; bank *= 2u) {
        struct msvdx_fw_geometry g = {bank, bank * 8u, bank * 8u};
        uint32_t t, d, offset;
        for (t = 1; t <= 17 && t * 4u <= g.code_bytes; ++t)
            for (d = 1; d <= 9 && d * 4u <= g.data_bytes; ++d)
                for (offset = 0; offset + d * 4u <= g.data_bytes && offset <= 36; offset += 4u) {
                    struct msvdx_fw_plan p;
                    unsigned char occupied[2][MSVDX_FW_MAX_REGION_BYTES / 4u] = {{0}};
                    size_t n = make(t, d, offset), source = 16;
                    uint32_t i, j, totals[2] = {0, 0};
                    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_OK);
                    CHECK(p.count && p.count <= MSVDX_FW_MAX_CHUNKS);
                    for (i = 0; i < p.count; ++i) {
                        const struct msvdx_fw_chunk *c = &p.chunks[i];
                        CHECK(c->space == MSVDX_FW_CODE || c->space == MSVDX_FW_DATA);
                        CHECK(c->source_offset == source);
                        CHECK(c->region_byte_offset == c->word_address * 4u);
                        CHECK(c->words);
                        for (j = 0; j < c->words; ++j) {
                            uint32_t address = c->region_byte_offset + 4u * j;
                            uint32_t id = c->space == MSVDX_FW_CODE ? 0x10u : 0x18u;
                            /* Independent per-word bank/coverage oracle. */
                            while (address >= bank) {
                                address -= bank;
                                ++id;
                            }
                            CHECK(id == c->bank);
                            CHECK(!occupied[c->space][c->word_address + j]);
                            occupied[c->space][c->word_address + j] = 1;
                            ++totals[c->space];
                        }
                        source += (size_t)c->words * 4u;
                    }
                    CHECK(source == n && totals[0] == t && totals[1] == d);
                    for (j = 0; j < g.code_bytes / 4u; ++j)
                        CHECK(occupied[0][j] == (j < t));
                    for (j = 0; j < g.data_bytes / 4u; ++j)
                        CHECK(occupied[1][j] == (j >= offset / 4u && j < offset / 4u + d));
                }
    }
}
static void invalid(void)
{
    struct msvdx_fw_view v, before;
    struct msvdx_fw_plan p, saved;
    struct msvdx_fw_geometry g = {4096, 32768, 32768};
    size_t n = make(4, 3, 8), i;
    memset(&v, 0xa5, sizeof(v));
    before = v;
    memset(&p, 0xa5, sizeof(p));
    saved = p;
    CHECK(msvdx_fw_parse_v2(NULL, n, &v) == MSVDX_FW_ARGUMENT);
    CHECK(msvdx_fw_parse_v2(blob, n, NULL) == MSVDX_FW_ARGUMENT);
    for (i = 0; i <= n + 1; ++i)
        if (i != n) {
            CHECK(msvdx_fw_parse_v2(blob, i, &v) == MSVDX_FW_LENGTH);
            CHECK(!memcmp(&v, &before, sizeof(v)));
        }
    for (i = 0; i < 256; ++i) {
        put(blob, (uint32_t)i);
        if (i != 2) {
            CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_VERSION);
            CHECK(!memcmp(&v, &before, sizeof(v)));
        }
    }
    n = make(1, 1, 0);
    put(blob + 4, UINT32_MAX);
    put(blob + 8, UINT32_MAX);
    CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_LENGTH);
    n = make(0, 1, 0);
    CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_RANGE);
    n = make(1, 1, 0);
    put(blob + 12, MSVDX_FW_DATA_BASE - 4u);
    CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_RANGE);
    put(blob + 12, MSVDX_FW_DATA_BASE + 1u);
    CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_RANGE);
    n = make(1, 1, MSVDX_FW_MAX_REGION_BYTES);
    CHECK(msvdx_fw_parse_v2(blob, n, &v) == MSVDX_FW_RANGE);
    n = make(8192, 1, 0);
    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_OK);
    n = make(8193, 1, 0);
    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_RANGE);
    p = saved;
    n = make(3, 1, 0);
    g.code_bytes = 8;
    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_RANGE);
    CHECK(!memcmp(&p, &saved, sizeof(p)));
    g.code_bytes = 32768;
    g.bank_bytes = 3;
    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_GEOMETRY);
    g.bank_bytes = 4;
    CHECK(msvdx_fw_plan_v2(blob, n, &g, &p) == MSVDX_FW_GEOMETRY);
    CHECK(!memcmp(&p, &saved, sizeof(p)));
    {
        union {
            struct msvdx_fw_plan plan;
            uint8_t bytes[sizeof(struct msvdx_fw_plan)];
        } alias;
        memcpy(alias.bytes, blob, n);
        g.bank_bytes = 4096;
        CHECK(msvdx_fw_plan_v2(alias.bytes, n, &g, &alias.plan) == MSVDX_FW_ALIAS);
        CHECK(msvdx_fw_plan_v2(blob, n, &alias.plan.geometry, &alias.plan) == MSVDX_FW_ALIAS);
    }
    {
        uint8_t unaligned[64];
        n = make(2, 2, 0);
        memcpy(unaligned + 1, blob, n);
        CHECK(msvdx_fw_parse_v2(unaligned + 1, n, &v) == MSVDX_FW_OK);
        CHECK(v.text_words == 2 && v.data_offset == 24 && v.bytes == n);
    }
}
int main(void)
{
    plans();
    invalid();
    printf("PASS firmware v2 CPU parser/bank plan: %u checks; no firmware execution\n", checks);
    return 0;
}
