// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_packet.h"
#include "sgx535_task_packet_internal.h"
#include "sgx535_engine121.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static const struct sgx535_task_packet_section *find(const struct sgx535_task_packet *p,
                                                     uint32_t kind)
{
    uint32_t i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    assert(0);
    return NULL;
}
static uint32_t sw(const struct sgx535_task_packet *p, uint32_t kind, uint32_t index)
{
    const struct sgx535_task_packet_section *s = find(p, kind);
    assert(index < s->byte_count / 4u);
    return get(p->bytes + s->byte_offset + index * 4u);
}
static void ranges(const struct sgx535_task_packet *p)
{
    uint32_t i, j;
    assert(p->byte_count <= SGX535_TASK_PACKET_BYTES);
    assert(p->section_count == 16 && p->relocation_count <= SGX535_TASK_PACKET_RELOCS);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        assert(s->role < SGX535_TASK_IMAGE_ROLES && s->byte_count);
        assert(s->alignment && !(s->alignment & (s->alignment - 1u)));
        assert(!(s->resource_byte_offset & (s->alignment - 1u)));
        assert(s->byte_offset + s->byte_count <= p->byte_count);
        assert(s->resource_byte_offset + s->byte_count <= p->requirements[s->role].minimum_bytes);
        for (j = i + 1; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *t = &p->sections[j];
            assert(s->byte_offset + s->byte_count <= t->byte_offset ||
                   t->byte_offset + t->byte_count <= s->byte_offset);
            if (s->role == t->role)
                assert(s->resource_byte_offset + s->byte_count <= t->resource_byte_offset ||
                       t->resource_byte_offset + t->byte_count <= s->resource_byte_offset);
        }
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        const struct sgx535_task_packet_section *s;
        int contained = r->role == SGX535_TASK_TARGET;
        assert(r->section < p->section_count && r->role < SGX535_TASK_IMAGE_ROLES);
        s = &p->sections[r->section];
        assert(!(r->section_byte_offset & 3u) && r->section_byte_offset + 4u <= s->byte_count);
        assert(r->mask && r->right_shift < 32 && r->left_shift < 32);
        assert(r->alignment && !(r->alignment & (r->alignment - 1u)));
        assert(!(r->resource_byte_offset & (r->alignment - 1u)) && r->span_bytes);
        assert(r->resource_byte_offset + r->span_bytes <= p->requirements[r->role].minimum_bytes);
        assert(!(get(p->bytes + s->byte_offset + r->section_byte_offset) & r->mask));
        for (j = 0; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *t = &p->sections[j];
            if (t->role == r->role && r->resource_byte_offset >= t->resource_byte_offset &&
                r->resource_byte_offset + r->span_bytes <= t->resource_byte_offset + t->byte_count)
                contained = 1;
        }
        assert(contained);
        for (j = i + 1; j < p->relocation_count; j++) {
            const struct sgx535_task_packet_reloc *t = &p->relocations[j];
            if (r->section == t->section && r->section_byte_offset == t->section_byte_offset)
                assert(!(r->mask & t->mask));
        }
    }
}

/* An independent consumer exercises the distinct masks with synthetic owned
 * bases. These numeric inputs are CPU oracles, never physical bank authority. */
static void fields(const struct sgx535_task_packet *p)
{
    uint32_t (*patches)[32] = calloc(SGX535_TASK_PACKET_SECTIONS, sizeof(*patches));
    const uint32_t bases[8] = {0x20000000, 0x41000000, 0x42000000, 0x43000000,
                               0x44000000, 0x45000000, 0x46000000, 0x30000000};
    uint32_t i, j, mask24 = 0, mask28 = 0;
    assert(patches);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        assert(s->byte_count <= sizeof(patches[0]));
        for (j = 0; j < s->byte_count / 4u; j++)
            patches[i][j] = get(p->bytes + s->byte_offset + 4u * j);
    }
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *r = &p->relocations[i];
        uint32_t value;
        if (r->kind == SGX535_PACKET_DRI_OFFSET)
            value = bases[r->role] + r->resource_byte_offset;
        else if (r->kind == SGX535_PACKET_USE_BANK) {
            assert(r->role == SGX535_TASK_USE_VERTEX || r->role == SGX535_TASK_USE_FRAGMENT);
            value = r->role == SGX535_TASK_USE_VERTEX ? 9u : 3u;
        } else {
            assert(r->kind == SGX535_PACKET_USE_ENTRY);
            value = r->resource_byte_offset;
        }
        patches[r->section][r->section_byte_offset / 4u] |=
            ((value >> r->right_shift) << r->left_shift) & r->mask;
        if (r->role == SGX535_TASK_PDS && r->mask == 0xffffff)
            mask24++;
        if (r->role == SGX535_TASK_PDS && r->mask == 0xfffffff)
            mask28++;
    }
    assert(mask24 == 2 && mask28 == 3);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        const uint32_t *w = patches[i];
        if (s->kind == SGX535_PACKET_VERTEX_PDS) {
            assert(w[0] == bases[3]);
            assert((w[4] & 0xf) == 9);
        } else if (s->kind == SGX535_PACKET_FRAGMENT_PDS) {
            assert((w[0] & 0xf) == 3 && w[8] == 0x20);
        } else if (s->kind == SGX535_PACKET_STATE_PDS) {
            assert(w[0] == bases[0] + 416);
            assert((w[2] & 0x7ff00) >> 8 == 2 && (w[2] & 0xf) == 9);
        } else if (s->kind == SGX535_PACKET_TERMINATE_PDS) {
            assert(w[0] == bases[0] + 464);
            assert((w[2] & 0x7ff00) >> 8 == 4);
        } else if (s->kind == SGX535_PACKET_MTE_STATE) {
            /* The source's 24-bit fields discard the2xx window bits; its
             * 28-bit TA fields retain them. Do not normalize the two. */
            assert(w[2] == 8 && w[4] == 0x0c000004);
            /* This24-bit field alone cannot distinguish OFFSET from the
             * separate PSB PDS_OFFSET operation: the base bits are masked. */
            assert(((bases[0] + 128u) >> 4 & 0xffffffu) ==
                   ((bases[0] + 128u - 0x20000000u) >> 4 & 0xffffffu));
        } else if (s->kind == SGX535_PACKET_PBE_STATE) {
            assert(w[1] == bases[5]);
        } else if (s->kind == SGX535_PACKET_EVENT_PDS) {
            assert(w[0] == bases[0] + 480 && (w[2] & 0xf) == 3);
        } else if (s->kind == SGX535_PACKET_TA_STREAM) {
            assert(w[0] == 0x4200000a && w[3] == bases[3] + 64);
            /* Replacing generic OFFSET with PDS_OFFSET would change this
             * exact source-derived28-bit TA pointer. Such normalization must
             * fail this oracle, even though the24-bit MTE field hid it. */
            assert(w[0] != (0x40000000u | ((bases[0] + 160u - 0x20000000u) >> 4 & 0x0fffffffu)));
            assert(w[5] == 0x02000000 && w[7] == 0x6200000e);
            assert(w[9] == 0xc0000000u);
        }
    }
    free(patches);
}

static void workspace_builds(const struct sgx535_task_request *source)
{
    struct word_storage {
        uint64_t before;
        struct sgx535_task_packet_word_workspace words;
        uint64_t after;
    } *storage = malloc(sizeof(*storage));
    struct sgx535_task_packet *legacy = malloc(sizeof(*legacy));
    struct sgx535_task_packet *out = malloc(sizeof(*out)), *packet = malloc(sizeof(*packet));
    union pair_overlap {
        struct sgx535_task_packet_word_workspace words;
        struct sgx535_task_pair_request request;
    } *overlap = malloc(sizeof(*overlap));
    struct sgx535_task_request r = *source;
    struct sgx535_task_pair_request pair = {.version = SGX535_TASK_PAIR_PACKET_VERSION,
                                            .profile = SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT,
                                            .core_id = source->core_id,
                                            .revision = source->revision,
                                            .packed_argb = source->packed_argb,
                                            .width = 16,
                                            .height = 16};
    assert(storage && legacy && out && packet && overlap);
    storage->before = UINT64_C(0x123456789abcdef0);
    storage->after = UINT64_C(0xfedcba9876543210);
    for (unsigned version = 2; version <= 3; version++) {
        r.version = version;
        assert(sgx535_task_packet_build(&r, legacy, packet) == SGX535_TASK_BLOCKED);
        memset(&storage->words, 0xa5, sizeof(storage->words));
        assert(sgx535_task_packet_build_words(&r, out, packet, &storage->words) ==
               SGX535_TASK_BLOCKED);
        assert(!memcmp(legacy, out, sizeof(*out)));
        assert(storage->before == UINT64_C(0x123456789abcdef0) &&
               storage->after == UINT64_C(0xfedcba9876543210));
    }
    assert(sgx535_task_packet_build_words(&r, out, packet, NULL) == SGX535_TASK_INVALID);
    assert(sgx535_task_packet_build_words(&r, out, packet,
                                          (struct sgx535_task_packet_word_workspace *)out) ==
           SGX535_TASK_INVALID);
    assert(sgx535_task_packet_build_words(&r, out, packet,
                                          (struct sgx535_task_packet_word_workspace *)packet) ==
           SGX535_TASK_INVALID);
    storage->words.request = r;
    assert(sgx535_task_packet_build_words(&storage->words.request, out, packet, &storage->words) ==
           SGX535_TASK_INVALID);
    r.reserved[1] = 1;
    assert(sgx535_task_packet_build_words(&r, out, packet, &storage->words) ==
           SGX535_TASK_UNSUPPORTED);
    assert(!memcmp(legacy, out, sizeof(*out)));
    for (unsigned triangle = 0; triangle < 2; triangle++)
        memcpy(pair.clip_position[triangle], source->clip_position, sizeof(source->clip_position));
    assert(sgx535_task_pair_mask_set_packet_build(&pair, legacy, packet) == SGX535_TASK_BLOCKED);
    assert(sgx535_task_pair_mask_set_packet_build_words(&pair, out, packet, &storage->words) ==
           SGX535_TASK_BLOCKED);
    assert(!memcmp(legacy, out, sizeof(*out)));
    assert(sgx535_task_pair_mask_set_packet_build_words(&pair, out, packet, NULL) ==
           SGX535_TASK_INVALID);
    assert(sgx535_task_pair_mask_set_packet_build_words(
               &pair, out, packet, (struct sgx535_task_packet_word_workspace *)out) ==
           SGX535_TASK_INVALID);
    assert(sgx535_task_pair_mask_set_packet_build_words(
               &pair, out, packet, (struct sgx535_task_packet_word_workspace *)packet) ==
           SGX535_TASK_INVALID);
    overlap->request = pair;
    assert(sgx535_task_pair_mask_set_packet_build_words(&overlap->request, out, packet,
                                                        &overlap->words) == SGX535_TASK_INVALID);
    pair.reserved[0] = 1;
    assert(sgx535_task_pair_mask_set_packet_build_words(&pair, out, packet, &storage->words) ==
           SGX535_TASK_UNSUPPORTED);
    assert(!memcmp(legacy, out, sizeof(*out)));
    assert(storage->before == UINT64_C(0x123456789abcdef0) &&
           storage->after == UINT64_C(0xfedcba9876543210));
    free(packet);
    free(overlap);
    free(out);
    free(legacy);
    free(storage);
}

int main(void)
{
    struct sgx535_task_request r = {2,
                                    1,
                                    SGX535_ENGINE121_CORE_ID,
                                    SGX535_ENGINE121_REVISION,
                                    {{0xbf000000, 0xbf000000, 0, 0x3f800000},
                                     {0x3f000000, 0xbf000000, 0, 0x3f800000},
                                     {0, 0x3f000000, 0, 0x3f800000}},
                                    0x12345678,
                                    {0}};
    struct sgx535_task_request bad;
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *work = malloc(sizeof(*work));
    struct sgx535_task_packet *before = malloc(sizeof(*before));
    uint8_t *b;
    size_t i;
    assert(p && work && before);
    workspace_builds(&r);
    assert(SGX535_TASK_IMAGE_VERSION == 1 && SGX535_TASK_IMAGE_BYTES == 128);
    assert(sgx535_task_packet_build(&r, p, work) == SGX535_TASK_BLOCKED);
    assert(sgx535_task_packet_validate(&r, p, work) == SGX535_TASK_BLOCKED);
    assert(p->missing_contracts == SGX535_PACKET_MISSING_ALL);
    ranges(p);
    fields(p);
    assert(sw(p, SGX535_PACKET_MTE_STATE, 0) == 0x54c1);
    assert(sw(p, SGX535_PACKET_MTE_STATE, 1) == 0x1d00000);
    assert(sw(p, SGX535_PACKET_STATE_USE, 1) == 0x28a19001);
    assert(sw(p, SGX535_PACKET_STATE_USE, 2) == 0xa0200500u);
    assert(sw(p, SGX535_PACKET_STATE_USE, 3) == 0xfb274000u);
    assert(sw(p, SGX535_PACKET_STATE_USE, 8) == 0xa0000000u);
    assert(sw(p, SGX535_PACKET_STATE_USE, 10) == 0xa0200100u);
    assert(sw(p, SGX535_PACKET_STATE_PDS, 1) == 0x81200009u);
    assert(sw(p, SGX535_PACKET_STATE_PDS, 12) == 0x07030223);
    assert(sw(p, SGX535_PACKET_SECONDARY_PDS, 0) == 0xaf000000u);
    assert(sw(p, SGX535_PACKET_TERMINATE_STATE, 0) == 0x2000);
    assert(sw(p, SGX535_PACKET_TERMINATE_STATE, 1) == 0x40004);
    assert(sw(p, SGX535_PACKET_FRAGMENT_PDS, 12) == 0x07000345);
    *before = *p;
    b = (uint8_t *)p;
    for (i = 0; i < sizeof(*p); i++) {
        b[i] ^= 1;
        assert(sgx535_task_packet_validate(&r, p, work) == SGX535_TASK_INVALID);
        b[i] ^= 1;
    }
    assert(!memcmp(before, p, sizeof(*p)));
    for (i = 0; i < 8; i++) {
        bad = r;
        switch (i) {
        case 0:
            bad.version = 1;
            break;
        case 1:
            bad.profile = 2;
            break;
        case 2:
            bad.core_id++;
            break;
        case 3:
            bad.revision++;
            break;
        case 4:
            bad.reserved[2] = 1;
            break;
        case 5:
            bad.clip_position[0][3] = 0;
            break;
        case 6:
            bad.clip_position[1][0] = 0x7fc00000;
            break;
        default:
            bad.clip_position[2][1] = 0x40000000;
            break;
        }
        assert(sgx535_task_packet_build(&bad, p, work) == SGX535_TASK_UNSUPPORTED);
        assert(!memcmp(before, p, sizeof(*p)));
    }
    assert(sgx535_task_packet_build(&r, p, p) == SGX535_TASK_INVALID);
    assert(sgx535_task_packet_validate(&r, p, p) == SGX535_TASK_INVALID);
    assert(!memcmp(before, p, sizeof(*p)));
    assert(sgx535_task_packet_build(NULL, p, work) == SGX535_TASK_INVALID);
    assert(sgx535_task_packet_build(&r, NULL, work) == SGX535_TASK_INVALID);
    assert(sgx535_task_packet_build(&r, p, NULL) == SGX535_TASK_INVALID);
    for (i = 0; i < 1000; i++) {
        r.packed_argb = r.packed_argb * 1664525u + 1013904223u;
        assert(sgx535_task_packet_build(&r, p, work) == SGX535_TASK_BLOCKED);
        assert(p->packed_argb == r.packed_argb);
        assert(sgx535_task_packet_validate(&r, p, work) == SGX535_TASK_BLOCKED);
    }
    {
        union overlapping_packet {
            struct sgx535_task_request r;
            struct sgx535_task_packet p;
        };
        union overlapping_packet *overlap = malloc(sizeof(*overlap));
        assert(overlap);
        overlap->r = r;
        assert(sgx535_task_packet_build(&overlap->r, &overlap->p, work) == SGX535_TASK_BLOCKED);
        assert(!memcmp(&overlap->p, p, sizeof(*p)));
        free(overlap);
    }
    printf(
        "PASS complete16-section packet: %zu-byte corruption oracle, disjoint spans, synthetic relocation consumer,1000generic colors; physical contracts remain BLOCKED\n",
        sizeof(*p));
    free(before);
    free(work);
    free(p);
    return 0;
}
