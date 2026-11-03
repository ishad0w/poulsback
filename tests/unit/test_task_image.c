// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_image.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static struct sgx535_task_request request(void)
{
    return (struct sgx535_task_request){1,
                                        1,
                                        0x01130000,
                                        0x00010201,
                                        {{0xbf000000, 0x3f000000, 0, 0x3f800000},
                                         {0x3f000000, 0x3f000000, 0, 0x3f800000},
                                         {0xbf000000, 0xbef00000, 0, 0x3f800000}},
                                        0xffff00ff,
                                        {0, 0, 0}};
}
int main(void)
{
    struct sgx535_task_request r = request(), bad;
    struct sgx535_task_image image, before, changed;
    uint32_t random = 0x237569da;
    size_t i;
    unsigned int j;
    memset(&image, 0xa5, sizeof(image));
    before = image;
    assert(sgx535_task_image_build(NULL, &image) == SGX535_TASK_INVALID);
    assert(!memcmp(&before, &image, sizeof(image)));
    assert(sgx535_task_image_build(&r, NULL) == SGX535_TASK_INVALID);
    assert(sgx535_task_image_build(&r, &image) == SGX535_TASK_BLOCKED);
    assert(sgx535_task_image_validate(&r, &image) == SGX535_TASK_BLOCKED);
    assert(image.missing_contracts == 0x7f && image.byte_count == 72);
    /* Independent exact small-integer viewport expectation. The third Y is47. */
    assert(word(image.bytes) == 0x41800000 && word(image.bytes + 4) == 0x41800000);
    assert(word(image.bytes + 16) == 0x42400000 && word(image.bytes + 20) == 0x41800000);
    assert(word(image.bytes + 32) == 0x41800000 && word(image.bytes + 36) == 0x423c0000);
    for (i = 0; i < 3; i++) {
        assert(word(image.bytes + 16 * i + 8) == 0x3f000000);
        assert(word(image.bytes + 16 * i + 12) == 0x3f800000);
    }
    assert(word(image.bytes + 48) == 0xa0000000);
    assert(word(image.bytes + 52) == 0x28a13001);
    assert(word(image.bytes + 56) == 0xa0200000);
    assert(word(image.bytes + 60) == 0xfb275000);
    assert(image.vertex_words == 4 && image.vertex_stride_bytes == 16);
    assert(image.dma_descriptor == 0x80600003 && image.dma_word_count == 4);
    for (i = 0; i < image.relocation_count; i++) {
        const struct sgx535_task_reloc *p = &image.relocations[i];
        const struct sgx535_task_section *s = &image.sections[p->section];
        assert(p->kind == SGX535_TASK_RELOC_COPY_SECTION && p->role < 8);
        assert(p->section_byte_offset + p->span_bytes <= s->byte_count);
        assert(s->byte_offset + s->byte_count <= image.byte_count);
        assert(p->resource_byte_offset + p->span_bytes <=
               image.requirements[p->role].minimum_bytes);
        assert(!(p->resource_byte_offset % p->alignment));
    }
    /* Arbitrary colors are data, independently decode every immediate bit.
	 * They cannot alter destination/opcode/END or choose a known scene. */
    for (i = 0; i < 4096; i++) {
        uint32_t lo, hi, color;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        r.packed_argb = random;
        assert(sgx535_task_image_build(&r, &image) == SGX535_TASK_BLOCKED);
        lo = word(image.bytes + 64);
        hi = word(image.bytes + 68);
        color = (lo & 0x1fffff) | ((hi >> 4) & 31) << 21 | ((hi >> 12) & 63) << 26;
        assert(color == random && !(lo & ~0x1fffffu));
        assert((hi & ~0x3f1f0u) == 0xfca40001);
    }
    r = request();
    assert(sgx535_task_image_build(&r, &image) == SGX535_TASK_BLOCKED);
    /* Every output byte, including padding/unused bytes/missing mask, is bound. */
    for (i = 0; i < sizeof(image); i++) {
        changed = image;
        ((uint8_t *)&changed)[i] ^= 1;
        assert(sgx535_task_image_validate(&r, &changed) == SGX535_TASK_INVALID);
    }
    for (j = 0; j < 14; j++) {
        bad = r;
        switch (j) {
        case 0:
            bad.version++;
            break;
        case 1:
            bad.profile++;
            break;
        case 2:
            bad.core_id++;
            break;
        case 3:
            bad.revision++;
            break;
        case 4:
            bad.reserved[0] = 1;
            break;
        case 5:
            bad.reserved[1] = 1;
            break;
        case 6:
            bad.reserved[2] = 1;
            break;
        case 7:
            bad.clip_position[0][3] = 0;
            break;
        case 8:
            bad.clip_position[1][3] = 0x40000000;
            break;
        case 9:
            bad.clip_position[2][0] = 0x7fc00000;
            break;
        case 10:
            bad.clip_position[0][1] = 0x7f800000;
            break;
        case 11:
            bad.clip_position[0][2] = 0x40000000;
            break;
        case 12:
            bad.clip_position[1][0] = 0xbf800001;
            break;
        default:
            bad.clip_position[2][3] = 0xbf800000;
            break;
        }
        before = image;
        assert(sgx535_task_image_build(&bad, &image) == SGX535_TASK_UNSUPPORTED);
        assert(!memcmp(&before, &image, sizeof(image)));
    }
    /* Deliberate stable input/output overlap is supported. */
    {
        union {
            struct sgx535_task_request request;
            struct sgx535_task_image image;
        } u;
        u.request = r;
        assert(sgx535_task_image_build(&u.request, &u.image) == SGX535_TASK_BLOCKED);
        assert(!memcmp(&u.image, &image, sizeof(image)));
    }
    puts(
        "task image: bounded known sections/placements, 4096 colors, all-byte corruption and refusal PASS; executable fields BLOCKED");
    return 0;
}
