// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_launch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}
static const struct sgx535_task_packet_section *section(const struct sgx535_task_packet *p,
                                                        uint32_t kind)
{
    unsigned i;
    for (i = 0; i < p->section_count; i++)
        if (p->sections[i].kind == kind)
            return &p->sections[i];
    assert(0);
    return NULL;
}
static uint32_t sw(const struct sgx535_task_packet *p, const uint8_t *bytes, uint32_t kind,
                   unsigned i)
{
    const struct sgx535_task_packet_section *s = section(p, kind);
    assert(i < s->byte_count / 4u);
    return get(bytes + s->byte_offset + i * 4u);
}

/* Independent desired window positions. The first pair tiles a square and
 * puts 48 pixel centers on its shared diagonal. The second pair is disjoint
 * and has different slopes/positions. Request preparation is test-only host
 * arithmetic; production uses the separate integer binary32 converter. */
static const int xy[3][6][2] = {{{8, 8}, {56, 8}, {56, 56}, {8, 8}, {56, 56}, {8, 56}},
                                {{4, 6}, {24, 6}, {4, 28}, {40, 34}, {60, 34}, {60, 58}},
                                {{2, 2}, {14, 2}, {13, 12}, {2, 2}, {13, 12}, {2, 14}}};
static struct sgx535_task_pair_request request(unsigned scene)
{
    struct sgx535_task_pair_request r = {0};
    unsigned i;
    r.version = SGX535_TASK_PAIR_PACKET_VERSION;
    r.profile = SGX535_TASK_PAIR_W1_PACKED_CONSTANT;
    r.core_id = SGX535_ENGINE121_CORE_ID;
    r.revision = SGX535_ENGINE121_REVISION;
    r.packed_argb = 0x1264b9e7;
    if (scene == 2) {
        r.width = r.height = 16;
        r.packed_argb = 0xff37a9d5;
    }
    for (i = 0; i < 6; i++) {
        uint32_t *v = r.clip_position[i / 3u][i % 3u];
        float half = scene == 2 ? 8.0f : 32.0f;
        v[0] = bits((float)xy[scene][i][0] / half - 1.0f);
        v[1] = bits(1.0f - (float)xy[scene][i][1] / half);
        v[2] = scene == 2 ? 0 : i % 3u == 0 ? 0xbf800000u : i % 3u == 1 ? 0 : 0x3f800000u;
        v[3] = 0x3f800000u;
    }
    return r;
}

/* Exact decode of this test's integer window coordinates, independent of the
 * clip encoder. Pixel coverage uses doubled integer coordinates, not floats. */
static int twice_coordinate(uint32_t u)
{
    uint32_t m = (u & 0x7fffffu) | 0x800000u;
    unsigned shift = 149u - ((u >> 23) & 255u);
    assert(!(u & 0x80000000u) && shift < 24);
    assert(!(m & ((1u << shift) - 1u)));
    return (int)(m >> shift);
}
struct point {
    int x, y;
};
static int64_t edge(struct point a, struct point b, struct point q)
{
    return (int64_t)(b.x - a.x) * (q.y - a.y) - (int64_t)(b.y - a.y) * (q.x - a.x);
}
static int covered(struct point v[3], struct point q)
{
    unsigned i;
    int64_t area = edge(v[0], v[1], v[2]);
    if (!area)
        return 0;
    if (area < 0) {
        struct point t = v[1];
        v[1] = v[2];
        v[2] = t;
    }
    for (i = 0; i < 3; i++) {
        struct point a = v[i], b = v[(i + 1u) % 3u];
        int64_t e = edge(a, b, q);
        /* Specified candidate: centers (x+.5,y+.5), image Y down, top/left
         * edges included. This is an oracle policy, not SGX qualification. */
        int top_left = b.y < a.y || (b.y == a.y && b.x > a.x);
        if (e < 0 || (!e && !top_left))
            return 0;
    }
    return 1;
}
static void pixel_oracle(const struct sgx535_task_packet *p, unsigned scene)
{
    struct point v[2][3];
    unsigned i, x, y, seam = 0;
    for (i = 0; i < 6; i++) {
        uint32_t packed = sw(p, p->bytes, SGX535_PACKET_INDICES, i / 2u);
        unsigned index = (packed >> (16u * (i % 2u))) & 0xffffu;
        assert(index < 6);
        v[i / 3u][i % 3u] = (struct point){
            twice_coordinate(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u)),
            twice_coordinate(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u + 1u))};
    }
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            struct point q = {(int)(2u * x + 1u), (int)(2u * y + 1u)};
            int a = covered(v[0], q), b = covered(v[1], q), expected;
            if (!scene) {
                /* Independent analytical rectangle; no edge function or emitted
             * coordinates are used in this expectation. Shared pixels must
             * belong to exactly one triangle, including all diagonal ties. */
                expected = x >= 8 && x < 56 && y >= 8 && y < 56;
                if (expected && x == y) {
                    assert(a && !b);
                    seam++;
                }
            } else {
                int dx = q.x - 8, dy = q.y - 12;
                int left = dx > 0 && dy > 0 && 22 * dx + 20 * dy < 880;
                dx = q.x - 80;
                dy = q.y - 68;
                expected = left || (q.x < 120 && dy > 0 && 24 * dx - 20 * dy >= 0);
            }
            assert(a + b == expected);
        }
    if (!scene)
        assert(seam == 48);
}
static void tile16_oracle(const struct sgx535_task_pair_request *r,
                          const struct sgx535_task_packet *p)
{
    /* Literal masks from independent rational scanline intersections, with
     * no packet/clip/edge builder used to generate the expected membership.
     * Their shared edge and all outer edges miss every sample center. */
    static const uint16_t rows[2][16] = {
        {0, 0, 0x3ff8, 0x3ff0, 0x3fe0, 0x3fc0, 0x3f80, 0x1f00, 0x1e00, 0x1c00, 0x1800, 0x1000, 0, 0,
         0, 0},
        {0, 0, 4, 0xc, 0x1c, 0x3c, 0x7c, 0xfc, 0x1fc, 0x3fc, 0x7fc, 0xffc, 0x3fc, 0x1c, 0, 0}};
    struct point v[2][3];
    unsigned i, t, x, y, count[2] = {0, 0}, background = 0;
    uint32_t lo = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 0);
    uint32_t hi = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 1);
    uint32_t color = (lo & 0x1fffffu) | ((hi >> 4) & 31u) << 21 | ((hi >> 12) & 63u) << 26;
    assert(color == r->packed_argb && !(lo & 0xffe00000u));
    assert((hi & ~0x3f1f0u) == 0xfca40001u);
    assert(p->width == 16 && p->height == 16 && p->stride_bytes == 128);
    assert(p->requirements[SGX535_TASK_TARGET].minimum_bytes == 4096);
    assert(p->requirements[SGX535_TASK_SCENE].minimum_bytes == 8192);
    assert(sw(p, p->bytes, SGX535_PACKET_PBE_STATE, 0) == 0x000f8000);
    assert(sw(p, p->bytes, SGX535_PACKET_PBE_STATE, 1) == 0);
    assert(sw(p, p->bytes, SGX535_PACKET_PBE_STATE, 2) == 0);
    assert(sw(p, p->bytes, SGX535_PACKET_PBE_STATE, 3) == 0x0000f00f);
    assert(sw(p, p->bytes, SGX535_PACKET_TERMINATE_STATE, 0) == 0x2000);
    assert(sw(p, p->bytes, SGX535_PACKET_TERMINATE_STATE, 1) == 0x00010001);
    for (i = 0; i < 6; i++) {
        unsigned index =
            (sw(p, p->bytes, SGX535_PACKET_INDICES, i / 2u) >> (16u * (i % 2u))) & 0xffffu;
        assert(index == i);
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u) == bits((float)xy[2][i][0]));
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u + 1u) ==
               bits((float)xy[2][i][1]));
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u + 2u) == 0x3f000000u);
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u + 3u) == 0x3f800000u);
        v[i / 3u][i % 3u] = (struct point){
            twice_coordinate(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u)),
            twice_coordinate(sw(p, p->bytes, SGX535_PACKET_VERTICES, index * 4u + 1u))};
    }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            struct point q = {(int)(2u * x + 1u), (int)(2u * y + 1u)};
            unsigned owners = 0, expected = 0;
            for (t = 0; t < 2; t++) {
                unsigned want = (rows[t][y] >> x) & 1u;
                assert(covered(v[t], q) == (int)want);
                for (i = 0; i < 3; i++)
                    assert(edge(v[t][i], v[t][(i + 1u) % 3u], q));
                owners += (unsigned)covered(v[t], q);
                expected += want;
                count[t] += want;
            }
            assert(owners == expected && owners <= 1);
            /* CPU-black initialization is an explicit test input, not a GPU clear. */
            assert((owners ? color : 0xff000000u) == (expected ? r->packed_argb : 0xff000000u));
            background += !owners;
        }
    assert(count[0] == 60 && count[1] == 66 && background == 130);
    for (i = 0; i < p->relocation_count; i++)
        if (p->relocations[i].role == SGX535_TASK_TARGET)
            assert(p->relocations[i].resource_byte_offset == 0 &&
                   p->relocations[i].span_bytes == 2048);
}
static void ranges(const struct sgx535_task_packet *p)
{
    unsigned i, j;
    assert(p->section_count == 16 && p->relocation_count == 29);
    assert(p->byte_count <= SGX535_TASK_PACKET_BYTES);
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
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
        const struct sgx535_task_packet_section *s = &p->sections[r->section];
        int contained = r->role == SGX535_TASK_TARGET;
        assert(r->section_byte_offset + 4u <= s->byte_count);
        assert(!(r->resource_byte_offset & (r->alignment - 1u)));
        for (j = 0; j < p->section_count; j++) {
            const struct sgx535_task_packet_section *t = &p->sections[j];
            if (t->role == r->role && r->resource_byte_offset >= t->resource_byte_offset &&
                r->resource_byte_offset + r->span_bytes <= t->resource_byte_offset + t->byte_count)
                contained = 1;
        }
        assert(contained);
    }
}
static void word_oracle(const struct sgx535_task_pair_request *r,
                        const struct sgx535_task_packet *p, unsigned scene)
{
    const struct sgx535_task_packet_section *v = section(p, SGX535_PACKET_VERTICES);
    const struct sgx535_task_packet_section *ix = section(p, SGX535_PACKET_INDICES);
    unsigned i;
    uint32_t lo = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 0);
    uint32_t hi = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 1);
    uint32_t color = (lo & 0x1fffffu) | ((hi >> 4) & 31u) << 21 | ((hi >> 12) & 63u) << 26;
    assert(color == r->packed_argb && !(lo & 0xffe00000u));
    assert((hi & ~0x3f1f0u) == 0xfca40001u);
    assert(v->byte_count == 96 && v->resource_byte_offset == 0);
    assert(ix->byte_count == 12 && ix->resource_byte_offset == 128);
    assert(sw(p, p->bytes, SGX535_PACKET_INDICES, 0) == 0x00010000);
    assert(sw(p, p->bytes, SGX535_PACKET_INDICES, 1) == 0x00030002);
    assert(sw(p, p->bytes, SGX535_PACKET_INDICES, 2) == 0x00050004);
    for (i = 0; i < 6; i++) {
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, i * 4u) == bits((float)xy[scene][i][0]));
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, i * 4u + 1u) ==
               bits((float)xy[scene][i][1]));
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, i * 4u + 2u) == (i % 3u == 0 ? 0u
                                                                        : i % 3u == 1
                                                                            ? 0x3f000000u
                                                                            : 0x3f800000u));
        assert(sw(p, p->bytes, SGX535_PACKET_VERTICES, i * 4u + 3u) == 0x3f800000u);
    }
    assert(sw(p, p->bytes, SGX535_PACKET_TA_STREAM, 2) == 0x81400006u);
    assert(sw(p, p->bytes, SGX535_PACKET_TA_STREAM, 6) == 0x02000103u);
    assert(sw(p, p->bytes, SGX535_PACKET_TA_STREAM, 9) == 0xc0000000u);
    assert(section(p, SGX535_PACKET_TA_STREAM)->byte_count == 40);
    assert(sw(p, p->bytes, SGX535_PACKET_VERTEX_PDS, 1) == 0x80600003u);
    assert(sw(p, p->bytes, SGX535_PACKET_VERTEX_PDS, 8) == 16);
    assert(sw(p, p->bytes, SGX535_PACKET_VERTEX_USE, 1) == 0x28a13001);
    for (i = 0; i < p->relocation_count; i++) {
        const struct sgx535_task_packet_reloc *rel = &p->relocations[i];
        uint32_t kind = p->sections[rel->section].kind;
        if (rel->role == SGX535_TASK_VERTEX_DATA) {
            assert(rel->mask == 0xffffffffu && !rel->right_shift && !rel->left_shift);
            if (kind == SGX535_PACKET_VERTEX_PDS)
                assert(rel->resource_byte_offset == 0 && rel->span_bytes == 96);
            else {
                assert(kind == SGX535_PACKET_TA_STREAM && rel->section_byte_offset == 12);
                assert(rel->resource_byte_offset == 128 && rel->span_bytes == 12);
            }
        }
    }
}
static void bind(const struct sgx535_task_packet *p, struct sgx535_task_launch_binding *b)
{
    static const uint32_t bases[8] = {0x20003000, 0x41001000, 0x42002000, 0x43004000,
                                      0x44000000, 0x45000000, 0x46000000, 0x30000000};
    unsigned i;
    memset(b, 0, sizeof(*b));
    b->epoch = 31;
    b->bank_index[0] = 3;
    b->bank_index[1] = 15;
    for (i = 0; i < 8; i++)
        b->roles[i] = (struct sgx535_task_launch_role){
            301u + i, bases[i], p->requirements[i].minimum_bytes, p->requirements[i].access, 0};
}
int main(void)
{
    struct sgx535_task_pair_request r = request(0), bad;
    struct sgx535_task_request legacy = {0};
    struct sgx535_task_packet *p = malloc(sizeof(*p)), *w = malloc(sizeof(*w));
    struct sgx535_task_packet *saved = malloc(sizeof(*saved)), *old = malloc(sizeof(*old));
    struct sgx535_task_launch *launch = malloc(sizeof(*launch)), *lsaved = malloc(sizeof(*lsaved));
    struct sgx535_task_launch_workspace *lw = malloc(sizeof(*lw));
    struct sgx535_task_launch_binding binding;
    unsigned scene, i, j;
    size_t n;
    assert(p && w && saved && old && launch && lsaved && lw);
    for (scene = 0; scene < 2; scene++) {
        r = request(scene);
        assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
        assert(sgx535_task_pair_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
        assert(p->version == 8 && p->profile == 5 && p->missing_contracts == 0x7f);
        ranges(p);
        word_oracle(&r, p, scene);
        pixel_oracle(p, scene);
        legacy.version = 3;
        legacy.profile = 1;
        legacy.core_id = r.core_id;
        legacy.revision = r.revision;
        legacy.packed_argb = r.packed_argb;
        memcpy(legacy.clip_position, r.clip_position[0], sizeof(legacy.clip_position));
        assert(sgx535_task_packet_build(&legacy, old, w) == SGX535_TASK_BLOCKED);
        for (i = 0; i < p->section_count; i++) {
            const struct sgx535_task_packet_section *s = &p->sections[i];
            const struct sgx535_task_packet_section *t = section(old, s->kind);
            if (s->kind == SGX535_PACKET_VERTICES || s->kind == SGX535_PACKET_INDICES ||
                s->kind == SGX535_PACKET_TA_STREAM)
                continue;
            assert(s->resource_byte_offset == t->resource_byte_offset &&
                   s->byte_count == t->byte_count);
            assert(!memcmp(p->bytes + s->byte_offset, old->bytes + t->byte_offset, s->byte_count));
        }
        bind(p, &binding);
        assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
        assert(sgx535_task_pair_launch_validate(&r, p, &binding, launch, lw) ==
               SGX535_TASK_BLOCKED);
        assert(launch->packet_version == 8 && launch->missing_packet_contracts == 0x7f);
        assert(launch->missing_launch_contracts == 0x75 && launch->resolved_relocations == 29);
        assert(launch->stages[1].count == 7 && launch->stages[1].actions[6].offset == 0x238);
        assert(launch->stages[1].actions[6].value == 0x20003220);
        assert(sw(p, launch->bytes, SGX535_PACKET_TA_STREAM, 3) == 0x43004080);
        assert(sw(p, launch->bytes, SGX535_PACKET_VERTEX_PDS, 0) == 0x43004000);
        assert(sw(p, launch->bytes, SGX535_PACKET_TA_STREAM, 0) == 0x4200030a);
        assert(launch->event_pds_va_bytes == 0x20003120);
    }
    /* Reverse winding, preserving the spec coverage and one shared-edge owner. */
    r = request(0);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 4; j++) {
            uint32_t t = r.clip_position[i][1][j];
            r.clip_position[i][1][j] = r.clip_position[i][2][j];
            r.clip_position[i][2][j] = t;
        }
    assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    pixel_oracle(p, 0);
    for (i = 0; i < 32; i++) {
        r.packed_argb = 1u << i;
        assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
        assert(sgx535_task_pair_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
        /* Every color bit remains data, including alpha and zero channels. */
        {
            uint32_t lo = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 0);
            uint32_t hi = sw(p, p->bytes, SGX535_PACKET_FRAGMENT_USE, 1);
            assert(((lo & 0x1fffffu) | ((hi >> 4) & 31u) << 21 | ((hi >> 12) & 63u) << 26) ==
                   r.packed_argb);
        }
    }
    *saved = *p;
    for (n = 0; n < sizeof(*p); n++) {
        ((uint8_t *)p)[n] ^= 1;
        assert(sgx535_task_pair_packet_validate(&r, p, w) == SGX535_TASK_INVALID);
        ((uint8_t *)p)[n] ^= 1;
    }
    for (i = 0; i < 14; i++) {
        bad = r;
        if (i < 6)
            bad.clip_position[i / 3u][i % 3u][3] = 0;
        else
            switch (i) {
            case 6:
                bad.version = 3;
                break;
            case 7:
                bad.profile = 1;
                break;
            case 8:
                bad.core_id++;
                break;
            case 9:
                bad.revision++;
                break;
            case 10:
                bad.reserved[2] = 1;
                break;
            case 11:
                bad.clip_position[1][2][0] = 0x7fc00000;
                break;
            case 12:
                bad.clip_position[1][2][1] = 0x40000000;
                break;
            default:
                bad.clip_position[1][2][2] = 0xff800000;
                break;
            }
        assert(sgx535_task_pair_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
        assert(!memcmp(saved, p, sizeof(*p)));
    }
    assert(sgx535_task_pair_packet_build(&r, p, p) == SGX535_TASK_INVALID);
    assert(sgx535_task_pair_packet_validate(&r, p, p) == SGX535_TASK_INVALID);
    assert(sgx535_task_pair_packet_build(NULL, p, w) == SGX535_TASK_INVALID);
    assert(sgx535_task_pair_packet_build(&r, NULL, w) == SGX535_TASK_INVALID);
    assert(sgx535_task_pair_packet_build(&r, p, NULL) == SGX535_TASK_INVALID);
    legacy.version = 8;
    legacy.profile = 5;
    assert(sgx535_task_packet_build(&legacy, old, w) == SGX535_TASK_UNSUPPORTED);
    assert(sgx535_task_launch_build(&legacy, p, &binding, launch, lw) == SGX535_TASK_INVALID);
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
    *lsaved = *launch;
    for (n = 0; n < sizeof(*launch); n++) {
        ((uint8_t *)launch)[n] ^= 1;
        assert(sgx535_task_pair_launch_validate(&r, p, &binding, launch, lw) ==
               SGX535_TASK_INVALID);
        ((uint8_t *)launch)[n] ^= 1;
    }
    p->missing_contracts = 0;
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_INVALID);
    assert(!memcmp(launch, lsaved, sizeof(*launch)));
    *p = *saved;
    binding.roles[SGX535_TASK_VERTEX_DATA].allocation_bytes = 4095;
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_INVALID);
    assert(!memcmp(launch, lsaved, sizeof(*launch)));
    {
        union alias {
            struct sgx535_task_pair_request r;
            struct sgx535_task_packet p;
            struct sgx535_task_launch out;
            struct sgx535_task_launch_workspace w;
        };
        union alias *u = malloc(sizeof(*u)), *copy = malloc(sizeof(*copy));
        assert(u && copy);
        memset(u, 0x6b, sizeof(*u));
        u->r = r;
        *copy = *u;
        assert(sgx535_task_pair_packet_build(&u->r, &u->p, w) == SGX535_TASK_INVALID);
        assert(!memcmp(u, copy, sizeof(*u)));
        assert(sgx535_task_pair_packet_build(&u->r, p, &u->p) == SGX535_TASK_INVALID);
        assert(!memcmp(u, copy, sizeof(*u)));
        bind(p, &binding);
        assert(sgx535_task_pair_launch_build(&u->r, p, &binding, &u->out, lw) ==
               SGX535_TASK_INVALID);
        assert(!memcmp(u, copy, sizeof(*u)));
        free(copy);
        free(u);
    }
    /* Extents are additive internal input; zero/zero keeps every old byte. */
    r = request(0);
    assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    *saved = *p;
    bind(p, &binding);
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
    *lsaved = *launch;
    r.width = r.height = 64;
    assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    assert(!memcmp(p, saved, sizeof(*p)));
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
    assert(!memcmp(launch, lsaved, sizeof(*launch)));
    r = request(2);
    assert(sgx535_task_pair_packet_build(&r, p, w) == SGX535_TASK_BLOCKED);
    assert(sgx535_task_pair_packet_validate(&r, p, w) == SGX535_TASK_BLOCKED);
    assert(p->missing_contracts == 0x7f);
    ranges(p);
    tile16_oracle(&r, p);
    bind(p, &binding);
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
    assert(sgx535_task_pair_launch_validate(&r, p, &binding, launch, lw) == SGX535_TASK_BLOCKED);
    assert(launch->missing_launch_contracts == 0x75);
    assert(launch->stages[SGX535_TASK_LAUNCH_RASTER].actions[3].offset == 0x410);
    assert(launch->stages[SGX535_TASK_LAUNCH_RASTER].actions[3].value == 0); /* RENDBOX2, as EMGD */
    assert(launch->stages[SGX535_TASK_LAUNCH_RASTER].actions[4].offset == 0x414);
    assert(launch->stages[SGX535_TASK_LAUNCH_RASTER].actions[4].value == 0x100);
    *saved = *p;
    *lsaved = *launch;
    for (i = 0; i < 10; i++) {
        static const uint32_t unsupported[10][2] = {
            {0, 16},  {16, 0}, {0, 64},  {64, 0},  {16, 64},
            {64, 16}, {1, 1},  {15, 15}, {17, 17}, {UINT32_MAX, UINT32_MAX}};
        bad = r;
        bad.width = unsupported[i][0];
        bad.height = unsupported[i][1];
        assert(sgx535_task_pair_packet_build(&bad, p, w) == SGX535_TASK_UNSUPPORTED);
        assert(!memcmp(p, saved, sizeof(*p)));
        assert(sgx535_task_pair_launch_build(&bad, p, &binding, launch, lw) ==
               SGX535_TASK_UNSUPPORTED);
        assert(!memcmp(launch, lsaved, sizeof(*launch)));
    }
    p->width = 64;
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_INVALID);
    assert(!memcmp(launch, lsaved, sizeof(*launch)));
    *p = *saved;
    binding.roles[SGX535_TASK_TARGET].allocation_bytes = 4095;
    assert(sgx535_task_pair_launch_build(&r, p, &binding, launch, lw) == SGX535_TASK_INVALID);
    assert(!memcmp(launch, lsaved, sizeof(*launch)));
    puts(
        "PASS two-triangle single packet: retained 64x64 bytes, independent 16x16 words/all 256 pixels and distinct 64x64 coordinates/shared edge, typed launch, transactional refusal; hardware BLOCKED");
    free(lw);
    free(lsaved);
    free(launch);
    free(old);
    free(saved);
    free(w);
    free(p);
    return 0;
}
