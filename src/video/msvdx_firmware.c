// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_firmware.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int overlap(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (n > UINTPTR_MAX - x || m > UINTPTR_MAX - y)
        return 1;
    return x < y + m && y < x + n;
}
enum msvdx_fw_status msvdx_fw_parse_v2(const uint8_t *bytes, size_t n, struct msvdx_fw_view *out)
{
    struct msvdx_fw_view v;
    uint64_t length, end;
    if (!bytes || !out)
        return MSVDX_FW_ARGUMENT;
    if (n < MSVDX_FW_V2_HEADER_BYTES)
        return MSVDX_FW_LENGTH;
    if (overlap(bytes, n, out, sizeof(*out)))
        return MSVDX_FW_ALIAS;
    if (le32(bytes) != 2u)
        return MSVDX_FW_VERSION;
    memset(&v, 0, sizeof(v));
    v.text_words = le32(bytes + 4);
    v.data_words = le32(bytes + 8);
    v.data_location = le32(bytes + 12);
    length = 16u + UINT64_C(4) * ((uint64_t)v.text_words + v.data_words);
    if (length != n)
        return MSVDX_FW_LENGTH;
    if (!v.text_words || !v.data_words || v.text_words > MSVDX_FW_MAX_REGION_BYTES / 4u ||
        v.data_words > MSVDX_FW_MAX_REGION_BYTES / 4u || v.data_location < MSVDX_FW_DATA_BASE ||
        (v.data_location & 3u))
        return MSVDX_FW_RANGE;
    end = (uint64_t)v.data_location - MSVDX_FW_DATA_BASE + UINT64_C(4) * v.data_words;
    if (end > MSVDX_FW_MAX_REGION_BYTES)
        return MSVDX_FW_RANGE;
    v.text_offset = 16;
    v.data_offset = 16u + (size_t)v.text_words * 4u;
    v.bytes = n;
    memcpy(out, &v, sizeof(v));
    return MSVDX_FW_OK;
}
static int geometry_valid(const struct msvdx_fw_geometry *g)
{
    return g->bank_bytes >= 4u && g->bank_bytes <= MSVDX_FW_MAX_REGION_BYTES &&
           !(g->bank_bytes & (g->bank_bytes - 1u)) && g->code_bytes && g->data_bytes &&
           !(g->code_bytes & 3u) && !(g->data_bytes & 3u) &&
           g->code_bytes <= MSVDX_FW_MAX_REGION_BYTES &&
           g->data_bytes <= MSVDX_FW_MAX_REGION_BYTES &&
           (uint64_t)g->code_bytes <= UINT64_C(8) * g->bank_bytes &&
           (uint64_t)g->data_bytes <= UINT64_C(8) * g->bank_bytes;
}
static void split(struct msvdx_fw_plan *p, enum msvdx_fw_space space, uint32_t offset,
                  uint32_t words, size_t source)
{
    while (words) {
        struct msvdx_fw_chunk *c = &p->chunks[p->count++];
        uint32_t available = (p->geometry.bank_bytes - offset % p->geometry.bank_bytes) / 4u;
        c->space = space;
        c->bank = (space == MSVDX_FW_CODE ? 0x10u : 0x18u) + offset / p->geometry.bank_bytes;
        c->region_byte_offset = offset;
        c->word_address = offset / 4u;
        c->words = words < available ? words : available;
        c->source_offset = source;
        words -= c->words;
        offset += c->words * 4u;
        source += (size_t)c->words * 4u;
    }
}
enum msvdx_fw_status msvdx_fw_plan_v2(const uint8_t *bytes, size_t n,
                                      const struct msvdx_fw_geometry *geometry,
                                      struct msvdx_fw_plan *out)
{
    struct msvdx_fw_plan p;
    enum msvdx_fw_status s;
    uint32_t data_offset;
    if (!bytes || !geometry || !out)
        return MSVDX_FW_ARGUMENT;
    if (overlap(bytes, n, out, sizeof(*out)) ||
        overlap(geometry, sizeof(*geometry), out, sizeof(*out)))
        return MSVDX_FW_ALIAS;
    if (!geometry_valid(geometry))
        return MSVDX_FW_GEOMETRY;
    memset(&p, 0, sizeof(p));
    s = msvdx_fw_parse_v2(bytes, n, &p.image);
    if (s != MSVDX_FW_OK)
        return s;
    data_offset = p.image.data_location - MSVDX_FW_DATA_BASE;
    if (p.image.text_words > geometry->code_bytes / 4u || data_offset >= geometry->data_bytes ||
        p.image.data_words > (geometry->data_bytes - data_offset) / 4u)
        return MSVDX_FW_RANGE;
    p.geometry = *geometry;
    split(&p, MSVDX_FW_CODE, 0, p.image.text_words, p.image.text_offset);
    split(&p, MSVDX_FW_DATA, data_offset, p.image.data_words, p.image.data_offset);
    memcpy(out, &p, sizeof(p));
    return MSVDX_FW_OK;
}
