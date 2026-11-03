// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pbe_state.h"
#include "sgx535_pixel_event_reference.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int event_reference(const struct sgx535_pbe_state *p)
{
    uint8_t pds[128], use[16];
    for (unsigned i = 0; i < 32; i++)
        for (unsigned j = 0; j < 4; j++)
            pds[i * 4u + j] = (uint8_t)(p->pixel_pds_words[i] >> (j * 8u));
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 4; j++)
            use[i * 4u + j] = (uint8_t)(p->event_use_words[i] >> (j * 8u));
    return p->pixel_pds_data_words == 16 && p->pixel_pds_word_count == 32 &&
           sgx535_pixel_event_reference_check(pds, sizeof(pds), use, sizeof(use), 0, 0, 0,
                                              0x00200000u);
}
int main(void)
{
    struct sgx535_pbe_surface s = {64, 64, 256, 0, 0, 16384}, bad;
    struct sgx535_pbe_state p, before;
    uint32_t pitch, x, y;
    unsigned checks = 0, i;
    assert(!sgx535_pbe_state_project(&s, &p));
    assert(p.words[0] == 0x001f8000 && p.words[1] == 0 && p.words[2] == 0 &&
           p.words[3] == 0x0003f03f && p.required_bytes == 16384);
    assert(event_reference(&p));
    {
        static const struct {
            struct sgx535_pbe_surface surface;
            uint32_t word0, word3, extent;
        } literal[] = {{{16, 16, 128, 0, 0, 4096}, 0x000f8000, 0x0000f00f, 1984},
                       {{64, 64, 256, 0, 0, 16384}, 0x001f8000, 0x0003f03f, 16384},
                       {{2048, 3, 8192, 0, 0, 24576}, 0x03ff8000, 0x000027ff, 24576}};
        static const uint32_t rejected_pitch[] = {8320, 32768};
        for (i = 0; i < sizeof(literal) / sizeof(literal[0]); i++) {
            assert(!sgx535_pbe_state_project(&literal[i].surface, &p));
            assert(event_reference(&p));
            assert(p.words[0] == literal[i].word0 && p.words[1] == 0 && p.words[2] == 0 &&
                   p.words[3] == literal[i].word3 && p.required_bytes == literal[i].extent);
            /* Layout [27:26] and pack mode [31:28] remain the selected zero
             * values, even at the maximum representable linear pitch. */
            assert(!(p.words[0] & 0xfc000000u));
        }
        before = p;
        for (i = 0; i < sizeof(rejected_pitch) / sizeof(rejected_pitch[0]); i++) {
            bad = (struct sgx535_pbe_surface){16, 3, rejected_pitch[i], 0, 0, UINT64_MAX};
            assert(sgx535_pbe_state_project(&bad, &p) == -1);
            assert(!memcmp(&before, &p, sizeof(p)));
        }
    }
    for (pitch = 128; pitch <= 8192; pitch += 128) {
        for (x = 0; x < 8; x++)
            for (y = 0; y < 8; y++) {
                uint32_t width = pitch / 4 - x;
                if (width > 4096 - x)
                    width = 4096 - x;
                s = (struct sgx535_pbe_surface){width, 31, pitch, x, y, (uint64_t)(y + 31) * pitch};
                assert(!sgx535_pbe_state_project(&s, &p));
                checks++;
                /* Decode field domains and independently reconstruct inclusive
             * coordinates and last addressed byte, rather than round-tripping
             * through a production decoder. word1 stays an unresolved address. */
                assert((p.words[0] & 0xffffu) == 0x8000 && !(p.words[0] & 0xfc000000u));
                assert((((p.words[0] >> 16) * 2u) + 2u) * 4u == pitch);
                assert((p.words[2] & 4095u) == x && (p.words[2] >> 12) == y);
                assert((p.words[3] & 4095u) + 1u == x + width);
                assert((p.words[3] >> 12) + 1u == y + 31u);
                assert(p.required_bytes == (y + 30u) * pitch + (x + width) * 4u);
                assert(!p.words[1]);
            }
    }
    s = (struct sgx535_pbe_surface){65, 31, 384, 3, 7, 32768};
    assert(!sgx535_pbe_state_project(&s, &p));
    before = p;
    for (i = 0; i < 11; i++) {
        bad = s;
        switch (i) {
        case 0:
            bad.width = 0;
            break;
        case 1:
            bad.height = 0;
            break;
        case 2:
            bad.origin_x = 4096;
            break;
        case 3:
            bad.origin_y = UINT32_MAX;
            break;
        case 4:
            bad.stride_bytes = 256;
            break;
        case 5:
            bad.stride_bytes = 385;
            break;
        case 6:
            bad.stride_bytes = 65536;
            break;
        case 7:
            bad.allocation_bytes = p.required_bytes - 1u;
            break;
        case 8:
            bad.width = UINT32_MAX;
            break;
        case 9:
            bad.height = UINT32_MAX;
            break;
        default:
            bad.stride_bytes = 0;
            break;
        }
        assert(sgx535_pbe_state_project(&bad, &p) == -1);
        assert(!memcmp(&before, &p, sizeof(p)));
    }
    assert(sgx535_pbe_state_project(NULL, &p) == -1);
    assert(sgx535_pbe_state_project(&s, NULL) == -1);
    {
        union {
            struct sgx535_pbe_surface input;
            struct sgx535_pbe_state output;
        } u;
        u.input = s;
        assert(!sgx535_pbe_state_project(&u.input, &u.output));
        assert(!memcmp(&before, &u.output, sizeof(before)));
    }
    printf(
        "PASS %u PBE field layouts: pitch/origin/full extent/refusal; channel/ISA/execution unqualified\n",
        checks);
    return 0;
}
