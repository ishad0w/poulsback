// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pack.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void literals_and_mutations(void)
{
    static const struct {
        struct sgx535_pack_f32_u8 input;
        uint32_t words[2];
    } cases[] = {
        {{{SGX535_MOV_OUTPUT, 0}, {{SGX535_MOV_TEMP, 0}, {SGX535_MOV_TEMP, 1}}, 3, 1, 0},
         {UINT32_C(0x00040001), UINT32_C(0x40a00c0d)}},
        {{{SGX535_MOV_OUTPUT, 0}, {{SGX535_MOV_TEMP, 2}, {SGX535_MOV_TEMP, 3}}, 12, 1, 0},
         {UINT32_C(0x00040103), UINT32_C(0x40a00c31)}},
        {{{SGX535_MOV_OUTPUT, 2}, {{SGX535_MOV_PRIMARY, 3}, {SGX535_MOV_SECONDARY, 5}}, 5, 0, 1},
         {UINT32_C(0xb0400185), UINT32_C(0x40a40c15)}}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t words[2];
        unsigned bit;
        struct sgx535_pack_f32_u8 decoded;
        CHECK(sgx535_pack_f32_u8_encode(&cases[i].input, words) == SGX535_PACK_OK);
        CHECK(memcmp(words, cases[i].words, sizeof(words)) == 0);
        CHECK(sgx535_pack_f32_u8_decode(cases[i].words, &decoded) == SGX535_PACK_OK);
        CHECK(memcmp(&decoded, &cases[i].input, sizeof(decoded)) == 0);
        for (bit = 0; bit < 64; ++bit) {
            uint32_t altered[2] = {words[0], words[1]}, canonical[2];
            decoded = cases[i].input;
            altered[bit / 32] ^= UINT32_C(1) << (bit % 32);
            if (sgx535_pack_f32_u8_decode(altered, &decoded) == SGX535_PACK_OK) {
                CHECK(sgx535_pack_f32_u8_encode(&decoded, canonical) == SGX535_PACK_OK);
                CHECK(memcmp(altered, canonical, sizeof(canonical)) == 0);
            } else {
                CHECK(memcmp(&decoded, &cases[i].input, sizeof(decoded)) == 0);
            }
        }
    }
}

static void profile_fields(void)
{
    static const uint32_t masks[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15};
    unsigned db, a, b, di, ai, bi;
    size_t m;
    uint32_t scale, end;
    for (db = 0; db < 2; ++db)
        for (a = 0; a < 4; ++a)
            for (b = 0; b < 4; ++b) {
                const uint32_t dst[] = {0, db ? 127u : 123u};
                const uint32_t first[] = {0, 3, a ? 127u : 123u};
                const uint32_t second[] = {0, 3, b ? 127u : 123u};
                for (di = 0; di < 2; ++di)
                    for (ai = 0; ai < 3; ++ai)
                        for (bi = 0; bi < 3; ++bi)
                            for (m = 0; m < sizeof(masks) / sizeof(masks[0]); ++m)
                                for (scale = 0; scale < 2; ++scale)
                                    for (end = 0; end < 2; ++end) {
                                        struct sgx535_pack_f32_u8
                                            op = {{(enum sgx535_mov_bank)db, dst[di]},
                                                  {{(enum sgx535_mov_bank)a, first[ai]},
                                                   {(enum sgx535_mov_bank)b, second[bi]}},
                                                  masks[m],
                                                  scale,
                                                  end},
                                            decoded;
                                        uint32_t words[2];
                                        CHECK(sgx535_pack_f32_u8_encode(&op, words) ==
                                              SGX535_PACK_OK);
                                        CHECK(sgx535_pack_f32_u8_decode(words, &decoded) ==
                                              SGX535_PACK_OK);
                                        CHECK(memcmp(&op, &decoded, sizeof(op)) == 0);
                                    }
            }
}

static void negative_and_aliases(void)
{
    struct sgx535_pack_f32_u8 good = {
        {SGX535_MOV_OUTPUT, 0}, {{SGX535_MOV_TEMP, 0}, {SGX535_MOV_TEMP, 1}}, 3, 1, 1};
    uint32_t words[2], guard[2] = {23, 45};
    unsigned i;
    CHECK(sgx535_pack_f32_u8_encode(NULL, words) == SGX535_PACK_ERR_ARGUMENT);
    CHECK(sgx535_pack_f32_u8_encode(&good, NULL) == SGX535_PACK_ERR_ARGUMENT);
    CHECK(sgx535_pack_f32_u8_decode(NULL, &good) == SGX535_PACK_ERR_ARGUMENT);
    CHECK(sgx535_pack_f32_u8_decode(words, NULL) == SGX535_PACK_ERR_ARGUMENT);
    for (i = 0; i < 13; ++i) {
        struct sgx535_pack_f32_u8 bad = good;
        enum sgx535_pack_status expected = SGX535_PACK_ERR_RANGE;
        switch (i) {
        case 0:
            bad.destination.bank = SGX535_MOV_PRIMARY;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 1:
            bad.destination.number = 128;
            break;
        case 2:
            bad.source[0].bank = (enum sgx535_mov_bank) - 1;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 3:
            bad.source[1].bank = (enum sgx535_mov_bank)4;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 4:
            bad.source[0].number = 124;
            break;
        case 5:
            bad.source[1].bank = SGX535_MOV_PRIMARY;
            bad.source[1].number = 128;
            break;
        case 6:
            bad.write_mask = 16;
            break;
        case 7:
            bad.write_mask = 7;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 8:
            bad.write_mask = 11;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 9:
            bad.write_mask = 13;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 10:
            bad.write_mask = 14;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 11:
            bad.scale = 2;
            break;
        case 12:
            bad.end = 2;
            break;
        }
        memcpy(words, guard, sizeof(guard));
        CHECK(sgx535_pack_f32_u8_encode(&bad, words) == expected);
        CHECK(memcmp(words, guard, sizeof(guard)) == 0);
    }
    {
        union {
            struct sgx535_pack_f32_u8 op;
            uint32_t words[9];
        } alias;
        alias.op = good;
        CHECK(sgx535_pack_f32_u8_encode(&alias.op, alias.words) == SGX535_PACK_OK);
        CHECK(alias.words[0] == UINT32_C(0x00040001) && alias.words[1] == UINT32_C(0x40a40c0d));
        CHECK(sgx535_pack_f32_u8_decode(alias.words, &alias.op) == SGX535_PACK_OK);
        CHECK(memcmp(&alias.op, &good, sizeof(good)) == 0);
    }
}

static void single_byte_profile(void)
{
    static const struct {
        struct sgx535_pack_f32_u8_channel op;
        uint32_t words[2];
    } vectors[] = {
        {{{SGX535_MOV_OUTPUT, 0}, {SGX535_MOV_TEMP, 0}, 2, 1, 0}, {0x20040000U, 0x40a10c11U}},
        {{{SGX535_MOV_TEMP, 3}, {SGX535_MOV_SECONDARY, 7}, 0, 0, 1}, {0xe0600380U, 0x40a50c04U}},
        {{{SGX535_MOV_OUTPUT, 0}, {SGX535_MOV_PRIMARY, 3}, 3, 1, 1}, {0xa0040180U, 0x40a50c21U}}};
    unsigned db, sb, di, si, lane, scale, end, bit;
    size_t i;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        struct sgx535_pack_f32_u8_channel decoded;
        uint32_t words[2];
        CHECK(sgx535_pack_f32_u8_channel_encode(&vectors[i].op, words) == SGX535_PACK_OK);
        CHECK(memcmp(words, vectors[i].words, sizeof(words)) == 0);
        CHECK(sgx535_pack_f32_u8_channel_decode(words, &decoded) == SGX535_PACK_OK);
        CHECK(memcmp(&decoded, &vectors[i].op, sizeof(decoded)) == 0);
        for (bit = 0; bit < 64; bit++) {
            uint32_t altered[2] = {words[0], words[1]}, canonical[2];
            decoded = vectors[i].op;
            altered[bit / 32] ^= 1U << (bit % 32);
            if (sgx535_pack_f32_u8_channel_decode(altered, &decoded) == SGX535_PACK_OK) {
                CHECK(sgx535_pack_f32_u8_channel_encode(&decoded, canonical) == SGX535_PACK_OK);
                CHECK(memcmp(altered, canonical, sizeof(canonical)) == 0);
            } else {
                CHECK(memcmp(&decoded, &vectors[i].op, sizeof(decoded)) == 0);
            }
        }
    }
    for (db = 0; db < 2; db++)
        for (sb = 0; sb < 4; sb++)
            for (di = 0; di < 3; di++)
                for (si = 0; si < 3; si++)
                    for (lane = 0; lane < 4; lane++)
                        for (scale = 0; scale < 2; scale++)
                            for (end = 0; end < 2; end++) {
                                const uint32_t destinations[] = {0, 3, db ? 127U : 123U};
                                const uint32_t sources[] = {0, 3, sb ? 127U : 123U};
                                struct sgx535_pack_f32_u8_channel op = {{(enum sgx535_mov_bank)db,
                                                                         destinations[di]},
                                                                        {(enum sgx535_mov_bank)sb,
                                                                         sources[si]},
                                                                        lane,
                                                                        scale,
                                                                        end},
                                                                  decoded;
                                uint32_t words[2];
                                CHECK(sgx535_pack_f32_u8_channel_encode(&op, words) ==
                                      SGX535_PACK_OK);
                                CHECK(sgx535_pack_f32_u8_channel_decode(words, &decoded) ==
                                      SGX535_PACK_OK);
                                CHECK(memcmp(&op, &decoded, sizeof(op)) == 0);
                                /* The old profile must continue rejecting the immediate extension. */
                                {
                                    struct sgx535_pack_f32_u8 direct;
                                    CHECK(sgx535_pack_f32_u8_decode(words, &direct) ==
                                          SGX535_PACK_ERR_UNSUPPORTED);
                                }
                            }
    {
        struct sgx535_pack_f32_u8_channel op = vectors[0].op;
        uint32_t words[2] = {23, 45};
        union {
            struct sgx535_pack_f32_u8_channel op;
            uint32_t words[7];
        } alias;
        op.byte_index = 4;
        CHECK(sgx535_pack_f32_u8_channel_encode(&op, words) == SGX535_PACK_ERR_RANGE);
        CHECK(words[0] == 23 && words[1] == 45);
        CHECK(sgx535_pack_f32_u8_channel_encode(NULL, words) == SGX535_PACK_ERR_ARGUMENT);
        CHECK(sgx535_pack_f32_u8_channel_decode(NULL, &op) == SGX535_PACK_ERR_ARGUMENT);
        alias.op = vectors[0].op;
        CHECK(sgx535_pack_f32_u8_channel_encode(&alias.op, alias.words) == SGX535_PACK_OK);
        CHECK(memcmp(alias.words, vectors[0].words, sizeof(vectors[0].words)) == 0);
        CHECK(sgx535_pack_f32_u8_channel_decode(alias.words, &alias.op) == SGX535_PACK_OK);
        CHECK(memcmp(&alias.op, &vectors[0].op, sizeof(alias.op)) == 0);
    }
}

static void unpack_component_profile(void)
{
    /* Matched native PCK/UNPCK: U8->F32 format0x180, full F32 mask0xf,
     * SCALE bit18, and equal source descriptors (including component).
     * Literal words are independent of this codec's encoder/decoder.
     */
    static const struct {
        struct sgx535_unpack_u8_f32_component op;
        uint32_t words[2];
    } vectors[] = {
        {{{SGX535_MOV_TEMP, 0}, {SGX535_MOV_TEMP, 0}, 0, 0}, {0x00040000U, 0x40a001bcU}},
        {{{SGX535_MOV_TEMP, 1}, {SGX535_MOV_PRIMARY, 3}, 1, 0}, {0xa0254183U, 0x40a001bcU}},
        {{{SGX535_MOV_TEMP, 3}, {SGX535_MOV_SECONDARY, 7}, 2, 1}, {0xf0668387U, 0x40a401bcU}},
        {{{SGX535_MOV_OUTPUT, 2}, {SGX535_MOV_OUTPUT, 5}, 3, 0}, {0x5047c285U, 0x40a001bdU}},
        {{{SGX535_MOV_TEMP, 123}, {SGX535_MOV_TEMP, 123}, 3, 1}, {0x0f67fdfbU, 0x40a401bcU}},
        {{{SGX535_MOV_OUTPUT, 127}, {SGX535_MOV_PRIMARY, 127}, 0, 1}, {0xafe43fffU, 0x40a401bdU}}};
    size_t i;
    unsigned bit, db, sb, byte, end;
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        struct sgx535_unpack_u8_f32_component decoded;
        uint32_t words[2];
        CHECK(sgx535_unpack_u8_f32_component_encode(&vectors[i].op, words) == SGX535_PACK_OK);
        CHECK(memcmp(words, vectors[i].words, sizeof(words)) == 0);
        CHECK(sgx535_unpack_u8_f32_component_decode(words, &decoded) == SGX535_PACK_OK);
        CHECK(memcmp(&decoded, &vectors[i].op, sizeof(decoded)) == 0);
        for (bit = 0; bit < 64; bit++) {
            uint32_t changed[2] = {words[0], words[1]};
            bool allowed = (bit >= 21 && bit <= 27) || bit == 32 || bit == 50;
            changed[bit / 32] ^= UINT32_C(1) << (bit % 32);
            /* A single bit can change only destination or END. Source banks,
             * registers and byte selectors are paired and cannot diverge.
             */
            if ((changed[1] & 1u) == 0 && ((changed[0] >> 21) & 127u) > 123u)
                allowed = false;
            decoded = vectors[i].op;
            CHECK(sgx535_unpack_u8_f32_component_decode(changed, &decoded) ==
                  (allowed ? SGX535_PACK_OK : SGX535_PACK_ERR_UNSUPPORTED));
            if (!allowed)
                CHECK(memcmp(&decoded, &vectors[i].op, sizeof(decoded)) == 0);
        }
    }
    for (db = 0; db < 2; db++)
        for (sb = 0; sb < 4; sb++)
            for (byte = 0; byte < 4; byte++)
                for (end = 0; end < 2; end++) {
                    struct sgx535_unpack_u8_f32_component op = {{(enum sgx535_mov_bank)db,
                                                                 db ? 127u : 123u},
                                                                {(enum sgx535_mov_bank)sb,
                                                                 sb ? 127u : 123u},
                                                                byte,
                                                                end},
                                                          decoded;
                    struct sgx535_pack_f32_u8 old;
                    uint32_t words[2];
                    CHECK(sgx535_unpack_u8_f32_component_encode(&op, words) == SGX535_PACK_OK);
                    CHECK(sgx535_unpack_u8_f32_component_decode(words, &decoded) == SGX535_PACK_OK);
                    CHECK(memcmp(&op, &decoded, sizeof(op)) == 0);
                    CHECK(sgx535_pack_f32_u8_decode(words, &old) == SGX535_PACK_ERR_UNSUPPORTED);
                }
    for (i = 0; i < 10; i++) {
        struct sgx535_unpack_u8_f32_component bad = vectors[0].op;
        uint32_t words[2] = {23, 45};
        enum sgx535_pack_status expected = SGX535_PACK_ERR_RANGE;
        switch (i) {
        case 0:
            bad.destination.bank = SGX535_MOV_PRIMARY;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 1:
            bad.source.bank = (enum sgx535_mov_bank) - 1;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 2:
            bad.destination.number = 124;
            break;
        case 3:
            bad.destination = (struct sgx535_mov_operand){SGX535_MOV_OUTPUT, 128};
            break;
        case 4:
            bad.source.number = 124;
            break;
        case 5:
            bad.source = (struct sgx535_mov_operand){SGX535_MOV_PRIMARY, 128};
            break;
        case 6:
            bad.byte_index = 4;
            break;
        case 7:
            bad.end = 2;
            break;
        case 8:
            bad.destination.bank = (enum sgx535_mov_bank) - 1;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        case 9:
            bad.source.bank = (enum sgx535_mov_bank)4;
            expected = SGX535_PACK_ERR_UNSUPPORTED;
            break;
        }
        CHECK(sgx535_unpack_u8_f32_component_encode(&bad, words) == expected);
        CHECK(words[0] == 23 && words[1] == 45);
    }
    {
        union {
            struct sgx535_unpack_u8_f32_component op;
            uint32_t words[6];
        } alias;
        struct sgx535_unpack_u8_f32_component guard = vectors[0].op;
        uint32_t words[2] = {23, 45};
        CHECK(sgx535_unpack_u8_f32_component_encode(NULL, words) == SGX535_PACK_ERR_ARGUMENT);
        CHECK(sgx535_unpack_u8_f32_component_encode(&guard, NULL) == SGX535_PACK_ERR_ARGUMENT);
        CHECK(words[0] == 23 && words[1] == 45);
        CHECK(sgx535_unpack_u8_f32_component_decode(NULL, &guard) == SGX535_PACK_ERR_ARGUMENT);
        CHECK(memcmp(&guard, &vectors[0].op, sizeof(guard)) == 0);
        CHECK(sgx535_unpack_u8_f32_component_decode(words, NULL) == SGX535_PACK_ERR_ARGUMENT);
        alias.op = vectors[3].op;
        CHECK(sgx535_unpack_u8_f32_component_encode(&alias.op, alias.words) == SGX535_PACK_OK);
        CHECK(memcmp(alias.words, vectors[3].words, sizeof(vectors[3].words)) == 0);
        CHECK(sgx535_unpack_u8_f32_component_decode(alias.words, &alias.op) == SGX535_PACK_OK);
        CHECK(memcmp(&alias.op, &vectors[3].op, sizeof(alias.op)) == 0);
        alias.op = vectors[2].op;
        CHECK(sgx535_unpack_u8_f32_component_encode(&alias.op, alias.words + 2) == SGX535_PACK_OK);
        CHECK(memcmp(alias.words + 2, vectors[2].words, sizeof(vectors[2].words)) == 0);
        CHECK(sgx535_unpack_u8_f32_component_decode(alias.words + 2, &alias.op) == SGX535_PACK_OK);
        CHECK(memcmp(&alias.op, &vectors[2].op, sizeof(alias.op)) == 0);
    }
}

int main(void)
{
    literals_and_mutations();
    profile_fields();
    negative_and_aliases();
    single_byte_profile();
    unpack_component_profile();
    printf("F32/U8 PACK word CPU checks: %u; numerical/PBE/hardware semantics unqualified\n",
           checks);
    return 0;
}
