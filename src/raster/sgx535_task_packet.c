// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_task_packet_internal.h"
#include "sgx535_packet_internal.h"
#include "sgx535_clip_w1.h"
#include "sgx535_engine121.h"
#include "sgx535_vertex_pds.h"
#include "sgx535_vertex_use.h"
#include "sgx535_limm_fields.h"
#include "sgx535_pbe_state.h"
#include "sgx535_pixel_event.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

/* Resource offsets are intentionally distinct from byte-stream offsets.
 * These are generated program/data placements, never application controls. */
enum section_id {
    VERTICES,
    INDICES,
    VERTEX_USE,
    FRAGMENT_USE,
    STATE_USE,
    EVENT_USE,
    VERTEX_PDS,
    FRAGMENT_PDS,
    SECONDARY_PDS,
    STATE_PDS,
    TERMINATE_PDS,
    EVENT_PDS,
    MTE_STATE,
    TERMINATE_STATE,
    PBE_STATE,
    TA_STREAM
};

static int overlaps(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}

static void word(uint8_t *bytes, uint32_t value)
{
    unsigned int i;
    for (i = 0; i < 4; i++)
        bytes[i] = (uint8_t)(value >> (i * 8u));
}

static uint32_t private_offset(const struct sgx535_task_packet *p, uint32_t role)
{
    if (p->version == SGX535_TASK_PAIR_PACKET_VERSION &&
        p->profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT &&
        (role == SGX535_TASK_PDS || role == SGX535_TASK_USE_VERTEX ||
         role == SGX535_TASK_USE_FRAGMENT || role == SGX535_TASK_VERTEX_DATA))
        return SGX535_TASK_PAIR_SECOND_PRIVATE_OFFSET;
    return 0;
}

static void section(struct sgx535_task_packet *p, uint32_t id, uint32_t kind, uint32_t role,
                    uint32_t offset, uint32_t align, const uint32_t *words, uint32_t count)
{
    uint32_t i, start = (p->byte_count + 15u) & ~15u;
    p->sections[id] = (struct sgx535_task_packet_section){
        kind, start, count * 4u, role, offset + private_offset(p, role), align};
    for (i = 0; i < count; i++)
        word(p->bytes + start + i * 4u, words[i]);
    p->byte_count = start + count * 4u;
}

static void relocation(struct sgx535_task_packet *p, uint32_t kind, uint32_t s, uint32_t offset,
                       uint32_t role, uint32_t target, uint32_t bytes, uint32_t align,
                       uint32_t mask, uint32_t right, uint32_t left)
{
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        kind, s, offset, role, target + private_offset(p, role), bytes, align, mask, right, left};
}

static void address(struct sgx535_task_packet *p, uint32_t s, uint32_t offset, uint32_t role,
                    uint32_t target, uint32_t bytes, uint32_t align, uint32_t mask, uint32_t right,
                    uint32_t left)
{
    relocation(p, SGX535_PACKET_DRI_OFFSET, s, offset, role, target, bytes, align, mask, right,
               left);
}

static void use_link(struct sgx535_task_packet *p, uint32_t s, uint32_t offset, uint32_t role,
                     uint32_t target, uint32_t bytes)
{
    relocation(p, SGX535_PACKET_USE_BANK, s, offset, role, target, bytes, 16, 0xf, 0, 0);
    relocation(p, SGX535_PACKET_USE_ENTRY, s, offset, role, target, bytes, 16, 0xf0, 15, 4);
    relocation(p, SGX535_PACKET_USE_ENTRY, s, offset, role, target, bytes, 16, 0x7ff00, 4, 8);
}

/* The complete state-program producer copies primary words to outputs and
 * emits the state-specific terminal pair. Its terminal register operand is
 * the emitted word count; it is not the vertex producer's zero operand. */
static void state_use(uint32_t count, uint32_t words[4])
{
    words[0] = 0xa0000000u;
    words[1] = 0x28a10001u | (count - 1u) << 12;
    words[2] = 0xa0200000u | count << 7;
    words[3] = 0xfb274000u;
}

static int state_pds(uint32_t count, uint32_t words[15])
{
    struct sgx535_vertex_dma_plan dma;
    /* Both fixed records fit one direct DMA descriptor. */
    if (sgx535_vertex_dma_build(count, &dma) != SGX535_VERTEX_DMA_OK || dma.transfer_count != 1)
        return -1;
    memset(words, 0, 15u * sizeof(*words));
    words[1] = dma.transfers[0].descriptor_word;
    words[2] = 0x00200000u;
    words[12] = 0x07030223u;
    words[13] = 0x07042345u;
    words[14] = 0xaf000000u;
    return 0;
}

static enum sgx535_task_status construct_words(const struct sgx535_task_request *input,
                                               const uint32_t *vertices, uint32_t vertex_count,
                                               uint32_t width, uint32_t height,
                                               struct sgx535_task_packet *p,
                                               struct sgx535_task_packet_word_workspace *w)
{
    int mask_set;
    uint32_t stride = (width * 4u + 127u) & ~127u;
    uint32_t target_bytes = stride * height;
    static const uint32_t state[10] = {0x54c1, 0x01d00000, 0,          3u << 16,   12u << 24,
                                       0,      0,          0x04001000, 0x00010000, 0};
    static const uint32_t mask_state[11] = {0x54c5, 0x07f00100, 0x02000000, 0,          0, 0,
                                            0,      0,          0x04001000, 0x00010000, 0};
    static const uint32_t initial_ta[17] = {0x40000000, 0x0c02a203, 0x81400003u, 0,          0, 0,
                                            0x02000103, 0x60000000, 0x0c02a201,  0xc0000000u};
    uint32_t mte_offset = 416, term_offset = 464, pbe_offset = 480, ta_offset = 512;
    uint32_t index_offset = 64, index_words = 2;
    uint32_t terminate_pds_offset = 224, event_pds_offset = 288;
    uint32_t i;
    memset(w, 0, sizeof(*w));
    w->request = *input;
    mask_set = w->request.version == SGX535_TASK_PAIR_PACKET_VERSION &&
               (w->request.profile == SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT ||
                w->request.profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT);
    w->surface = (struct sgx535_pbe_surface){width, height, stride, 0, 0, target_bytes};
    memcpy(w->ta, initial_ta, sizeof(w->ta));
    w->indices[0] = 0x00010000u;
    w->indices[1] = 2;
    w->term[0] = 0x2000;
    w->term[1] = ((width + 15u) >> 4) << 16 | ((height + 15u) >> 4);
    w->secondary[0] = 0xaf000000u;
    if ((w->request.version != SGX535_TASK_PACKET_VERSION &&
         w->request.version != SGX535_TASK_PACKET_DRI_VERSION &&
         w->request.version != SGX535_WINDOW_COLOR0_PACKET_VERSION &&
         w->request.version != SGX535_TASK_PAIR_PACKET_VERSION) ||
        (!mask_set &&
         w->request.profile != (w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION
                                    ? SGX535_WINDOW_COLOR0_PROFILE
                                : w->request.version == SGX535_TASK_PAIR_PACKET_VERSION
                                    ? SGX535_TASK_PAIR_W1_PACKED_CONSTANT
                                    : SGX535_TASK_W1_PACKED_CONSTANT)) ||
        vertex_count != (mask_set                                                ? 10u
                         : w->request.version == SGX535_TASK_PAIR_PACKET_VERSION ? 6u
                                                                                 : 3u) ||
        (mask_set && (width != 16 || height != 16)) ||
        !((width == 64u && height == 64u) ||
          ((w->request.version == SGX535_TASK_PAIR_PACKET_VERSION ||
            w->request.version == SGX535_TASK_PACKET_DRI_VERSION) &&
           width == 16u && height == 16u)) ||
        w->request.core_id != SGX535_ENGINE121_CORE_ID ||
        w->request.revision != SGX535_ENGINE121_REVISION || w->request.reserved[0] ||
        w->request.reserved[1] || w->request.reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    if (sgx535_vertex_use_project(4, &w->vertex) != SGX535_VERTEX_USE_OK ||
        sgx535_vertex_pds_project(4, 4, &w->vpds) != SGX535_VERTEX_DMA_OK ||
        sgx535_pbe_state_project(&w->surface, &w->pbe) ||
        sgx535_engine121_scene_build(width, height, &w->scene) != SGX535_ENGINE121_OK ||
        (w->request.version != SGX535_TASK_PACKET_VERSION &&
         sgx535_pixel_event_build(SGX535_PIXEL_EVENT_PTOFF_PBE, &w->event)))
        return SGX535_TASK_UNSUPPORTED;

    if (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
        w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
        w->request.version == SGX535_TASK_PAIR_PACKET_VERSION) {
        mte_offset = 448;
        term_offset = 496;
        pbe_offset = 512;
        ta_offset = 544;
    }
    if (w->request.version == SGX535_TASK_PAIR_PACKET_VERSION) {
        /* The selected triangle-list producer takes an index count, not a
         * primitive count. Six U16 entries form two triangles; stride and
         * per-vertex PDS/USE output counts are unchanged. The VBO placement
         * leaves both the full 96-byte vertex span and 12-byte index span
         * disjoint without inferring an extra backing owner. */
        w->ta[2] = 0x81400006u;
        w->indices[1] = 0x00030002u;
        w->indices[2] = 0x00050004u;
        index_offset = 128;
        index_words = 3;
    }
    if (mask_set) {
        static const uint32_t stream[17] = {
            0x40000000, 0x0c02a203, 0x81400006u, 0,           0,          0,
            0x02000103, 0x40000000, 0x0c02a203,  0x81400006u, 0,          0,
            0,          0x02000103, 0x60000000,  0x0c02a201,  0xc0000000u};
        /* The quad follows all six arbitrary ordinary vertices. Its indices
         * select those four records through the same16-byte DMA stride. Both
         * index lists fit the one disjoint aggregate index section. */
        w->indices[0] = 0x00070006;
        w->indices[1] = 0x00090008;
        w->indices[2] = 0x00070008;
        w->indices[3] = 0x00010000;
        w->indices[4] = 0x00030002;
        w->indices[5] = 0x00050004;
        index_offset = 160;
        index_words = 6;
        terminate_pds_offset = 288;
        event_pds_offset = 352;
        mte_offset = 512;
        term_offset = 608;
        pbe_offset = 624;
        ta_offset = 672;
        memcpy(w->ta, stream, sizeof(stream));
        memcpy(w->mte, state, sizeof(state));
        memcpy(w->mte + 12, mask_state, sizeof(mask_state));
    }
    memset(p, 0, sizeof(*p));
    p->version = w->request.version;
    p->profile = w->request.profile;
    p->core_id = w->request.core_id;
    p->revision = w->request.revision;
    p->width = width;
    p->height = height;
    p->stride_bytes = stride;
    p->packed_argb = w->request.packed_argb;
    p->missing_contracts = SGX535_PACKET_MISSING_ALL;
    p->section_count = SGX535_TASK_PACKET_SECTIONS;
    p->ta_stream_section = TA_STREAM;
    p->event_pds_section = EVENT_PDS;
    sgx535_limm_compose(1, 0, w->request.packed_argb, 1, w->fs);
    /* Constant-fragment producer: no iterator, one output, empty secondary.
     * 07000345 is this path's actual producer word, not the older generic
     * codec's distinct 07000365 template. */
    w->fpds[8] = 0x20;
    w->fpds[12] = 0x07000345;
    w->fpds[13] = 0xaf000000u;
    /* Presence1: ordinary triangle, depth disabled, no stencil/color mask or
     * blend. Shader group40, output words80, MTE control400, output count1000
     * and selector4000 follow the four-word passthrough profile. These fields
     * are source construction; effects/profile joining remain explicit. */
    state_use(mask_set ? 11 : 10, w->suse);
    state_use(2, w->suse + 8);
    if (mask_set)
        state_use(10, w->suse + 4);
    if (state_pds(mask_set ? 11 : 10, w->spds) || (mask_set && state_pds(10, w->spds + 16)))
        return SGX535_TASK_UNSUPPORTED;
    section(p, VERTICES, SGX535_PACKET_VERTICES, SGX535_TASK_VERTEX_DATA, 0, 16, vertices,
            vertex_count * 4u);
    section(p, INDICES, SGX535_PACKET_INDICES, SGX535_TASK_VERTEX_DATA, index_offset, 2, w->indices,
            index_words);
    section(p, VERTEX_USE, SGX535_PACKET_VERTEX_USE, SGX535_TASK_USE_VERTEX, 0, 32, w->vertex.words,
            4);
    section(p, FRAGMENT_USE, SGX535_PACKET_FRAGMENT_USE, SGX535_TASK_USE_FRAGMENT, 0, 32, w->fs, 2);
    section(p, STATE_USE, SGX535_PACKET_STATE_USE, SGX535_TASK_USE_VERTEX, 32, 32, w->suse, 12);
    section(p, EVENT_USE, SGX535_PACKET_EVENT_USE, SGX535_TASK_USE_FRAGMENT, 32, 32,
            (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
             w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
             w->request.version == SGX535_TASK_PAIR_PACKET_VERSION)
                ? w->event.use_words
                : w->pbe.event_use_words,
            (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
             w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
             w->request.version == SGX535_TASK_PAIR_PACKET_VERSION)
                ? 8
                : 4);
    section(p, VERTEX_PDS, SGX535_PACKET_VERTEX_PDS, SGX535_TASK_PDS, 0, 32, w->vpds.words, 16);
    section(p, FRAGMENT_PDS, SGX535_PACKET_FRAGMENT_PDS, SGX535_TASK_PDS, 64, 32, w->fpds, 14);
    section(p, SECONDARY_PDS, SGX535_PACKET_SECONDARY_PDS, SGX535_TASK_PDS, 128, 32, w->secondary,
            1);
    section(p, STATE_PDS, SGX535_PACKET_STATE_PDS, SGX535_TASK_PDS, 160, 32, w->spds,
            mask_set ? 31 : 15);
    if (state_pds(2, w->spds))
        return SGX535_TASK_UNSUPPORTED;
    section(p, TERMINATE_PDS, SGX535_PACKET_TERMINATE_PDS, SGX535_TASK_PDS, terminate_pds_offset,
            32, w->spds, 15);
    section(p, EVENT_PDS, SGX535_PACKET_EVENT_PDS, SGX535_TASK_PDS, event_pds_offset, 32,
            (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
             w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
             w->request.version == SGX535_TASK_PAIR_PACKET_VERSION)
                ? w->event.pds_words
                : w->pbe.pixel_pds_words,
            (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
             w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
             w->request.version == SGX535_TASK_PAIR_PACKET_VERSION)
                ? 37
                : 32);
    section(p, MTE_STATE, SGX535_PACKET_MTE_STATE, SGX535_TASK_PDS, mte_offset, 16,
            mask_set ? w->mte : state, mask_set ? 23 : 10);
    section(p, TERMINATE_STATE, SGX535_PACKET_TERMINATE_STATE, SGX535_TASK_PDS, term_offset, 16,
            w->term, 2);
    section(p, PBE_STATE, SGX535_PACKET_PBE_STATE, SGX535_TASK_PDS, pbe_offset, 16, w->pbe.words,
            4);
    section(p, TA_STREAM, SGX535_PACKET_TA_STREAM, SGX535_TASK_PDS, ta_offset, 16, w->ta,
            mask_set ? 17 : 10);

    address(p, VERTEX_PDS, 0, SGX535_TASK_VERTEX_DATA, 0, vertex_count * 16u, 16, 0xffffffffu, 0,
            0);
    use_link(p, VERTEX_PDS, 16, SGX535_TASK_USE_VERTEX, 0, 16);
    use_link(p, FRAGMENT_PDS, 0, SGX535_TASK_USE_FRAGMENT, 0, 8);
    address(p, STATE_PDS, 0, SGX535_TASK_PDS, mte_offset + (mask_set ? 48u : 0u),
            mask_set ? 44 : 40, 16, 0xffffffffu, 0, 0);
    use_link(p, STATE_PDS, 8, SGX535_TASK_USE_VERTEX, 32, 16);
    if (mask_set) {
        address(p, STATE_PDS, 64, SGX535_TASK_PDS, mte_offset, 40, 16, 0xffffffffu, 0, 0);
        use_link(p, STATE_PDS, 72, SGX535_TASK_USE_VERTEX, 48, 16);
    }
    address(p, TERMINATE_PDS, 0, SGX535_TASK_PDS, term_offset, 8, 16, 0xffffffffu, 0, 0);
    use_link(p, TERMINATE_PDS, 8, SGX535_TASK_USE_VERTEX, 64, 16);
    address(p, MTE_STATE, 8, SGX535_TASK_PDS, 128, 4, 32, 0xffffff, 4, 0);
    address(p, MTE_STATE, 16, SGX535_TASK_PDS, 64, 56, 32, 0xffffff, 4, 0);
    if (w->request.version == SGX535_TASK_PACKET_DRI_VERSION ||
        w->request.version == SGX535_WINDOW_COLOR0_PACKET_VERSION ||
        w->request.version == SGX535_TASK_PAIR_PACKET_VERSION) {
        address(p, EVENT_PDS, 8, SGX535_TASK_PDS, pbe_offset, 16, 16, 0xffffffffu, 0, 0);
        use_link(p, EVENT_PDS, 0, SGX535_TASK_USE_FRAGMENT, 32, 8);
        use_link(p, EVENT_PDS, 16, SGX535_TASK_USE_FRAGMENT, 48, 16);
    } else {
        address(p, EVENT_PDS, 0, SGX535_TASK_PDS, pbe_offset, 16, 16, 0xffffffffu, 0, 0);
        use_link(p, EVENT_PDS, 8, SGX535_TASK_USE_FRAGMENT, 32, 16);
    }
    address(p, PBE_STATE, 4, SGX535_TASK_TARGET, 0, target_bytes, 4, 0xfffffffcu, 2, 2);
    address(p, TA_STREAM, 0, SGX535_TASK_PDS, 160, 60, 32, 0xfffffff, 4, 0);
    address(p, TA_STREAM, 12, SGX535_TASK_VERTEX_DATA, index_offset,
            mask_set ? 12 : vertex_count * 2u, 2, 0xffffffffu, 0, 0);
    address(p, TA_STREAM, 20, SGX535_TASK_PDS, 0, 64, 32, 0xfffffff, 4, 0);
    if (mask_set) {
        address(p, TA_STREAM, 28, SGX535_TASK_PDS, 224, 60, 32, 0xfffffff, 4, 0);
        address(p, TA_STREAM, 40, SGX535_TASK_VERTEX_DATA, index_offset + 12, 12, 2, 0xffffffffu, 0,
                0);
        address(p, TA_STREAM, 48, SGX535_TASK_PDS, 0, 64, 32, 0xfffffff, 4, 0);
        address(p, TA_STREAM, 56, SGX535_TASK_PDS, terminate_pds_offset, 60, 32, 0xfffffff, 4, 0);
    } else {
        address(p, TA_STREAM, 28, SGX535_TASK_PDS, 224, 60, 32, 0xfffffff, 4, 0);
    }
    for (i = 0; i < SGX535_TASK_IMAGE_ROLES; i++)
        p->requirements[i] =
            (struct sgx535_task_requirement){i, SGX535_TASK_ACCESS_READ, 4096, 4096};
    p->requirements[SGX535_TASK_TARGET].minimum_bytes = (target_bytes + 4095u) & ~4095u;
    p->requirements[SGX535_TASK_TARGET].access = SGX535_TASK_ACCESS_WRITE;
    p->requirements[SGX535_TASK_SCENE].minimum_bytes = (w->scene.allocation_bytes + 4095u) & ~4095u;
    p->requirements[SGX535_TASK_SCENE].access |= SGX535_TASK_ACCESS_WRITE;
    for (i = SGX535_TASK_DPM_TABLES; i <= SGX535_TASK_DPM_PARAMETERS; i++) {
        p->requirements[i].minimum_bytes = 32u * 1024u * 1024u;
        p->requirements[i].access |= SGX535_TASK_ACCESS_WRITE;
    }
    p->requirements[SGX535_TASK_DPM_PARAMETERS].alignment = 1024u * 1024u;
    return SGX535_TASK_BLOCKED;
}

static enum sgx535_task_status construct(const struct sgx535_task_request *r,
                                         struct sgx535_task_packet *p,
                                         struct sgx535_task_packet_word_workspace *words)
{
    uint32_t vertices[12], i;
    if (r->version != SGX535_TASK_PACKET_VERSION && r->version != SGX535_TASK_PACKET_DRI_VERSION)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 3; i++)
        if (sgx535_clip_w1_encode(r->clip_position[i], 64, 64, vertices + i * 4u) !=
            SGX535_CLIP_W1_OK)
            return SGX535_TASK_UNSUPPORTED;
    return construct_words(r, vertices, 3, 64, 64, p, words);
}
enum sgx535_task_status sgx535_task_packet_clip16_seed(uint32_t core, uint32_t revision,
                                                       const uint32_t clip[3][4],
                                                       struct sgx535_task_packet *p)
{
    struct sgx535_task_packet_word_workspace words;
    struct sgx535_task_request metadata = {0};
    uint32_t positions[12], i;
    for (i = 0; i < 3; i++)
        if (sgx535_clip_w1_encode(clip[i], 16, 16, positions + i * 4u) != SGX535_CLIP_W1_OK)
            return SGX535_TASK_UNSUPPORTED;
    metadata.version = SGX535_TASK_PACKET_DRI_VERSION;
    metadata.profile = SGX535_TASK_W1_PACKED_CONSTANT;
    metadata.core_id = core;
    metadata.revision = revision;
    return construct_words(&metadata, positions, 3, 16, 16, p, &words);
}
enum sgx535_task_status
sgx535_task_packet_window_seed(uint32_t core, uint32_t revision,
                               const struct sgx535_window_color0_triangle *r,
                               struct sgx535_task_packet *p)
{
    struct sgx535_task_packet_word_workspace words;
    struct sgx535_task_request metadata = {0};
    uint32_t records[24], positions[12], i;
    if (sgx535_window_color0_encode(r, records) != SGX535_WINDOW_VERTEX_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 3; i++)
        memcpy(positions + i * 4u, records + i * 8u, 16);
    metadata.version = SGX535_WINDOW_COLOR0_PACKET_VERSION;
    metadata.profile = SGX535_WINDOW_COLOR0_PROFILE;
    metadata.core_id = core;
    metadata.revision = revision;
    return construct_words(&metadata, positions, 3, 64, 64, p, &words);
}

static enum sgx535_task_status pair_construct(const struct sgx535_task_pair_request *r,
                                              struct sgx535_task_packet *p,
                                              struct sgx535_task_packet_word_workspace *words)
{
    struct sgx535_task_request metadata = {0};
    uint32_t vertices[40], i, j, width = r->width, height = r->height;
    int mask_set = r->profile == SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT ||
                   r->profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT;
    if (r->version != SGX535_TASK_PAIR_PACKET_VERSION ||
        (r->profile != SGX535_TASK_PAIR_W1_PACKED_CONSTANT && !mask_set) || r->reserved[0] ||
        r->reserved[1] || r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    if (mask_set && (width != 16 || height != 16))
        return SGX535_TASK_UNSUPPORTED;
    if (!width && !height)
        width = height = 64;
    if (!((width == 64u && height == 64u) || (width == 16u && height == 16u)))
        return SGX535_TASK_UNSUPPORTED;
    metadata.version = r->version;
    metadata.profile = r->profile;
    metadata.core_id = r->core_id;
    metadata.revision = r->revision;
    metadata.packed_argb = r->packed_argb;
    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++)
            if (sgx535_clip_w1_encode(r->clip_position[i][j], width, height,
                                      vertices + (i * 3u + j) * 4u) != SGX535_CLIP_W1_OK)
                return SGX535_TASK_UNSUPPORTED;
    if (mask_set) {
        static const uint32_t quad[16] = {
            0, 0,          0x3f800000, 0x3f800000, 0x41800000, 0,          0x3f800000, 0x3f800000,
            0, 0x41800000, 0x3f800000, 0x3f800000, 0x41800000, 0x41800000, 0x3f800000, 0x3f800000};
        memcpy(vertices + 24, quad, sizeof(quad));
    }
    return construct_words(&metadata, vertices, mask_set ? 10 : 6, width, height, p, words);
}

static int pair_arguments(const struct sgx535_task_pair_request *r,
                          const struct sgx535_task_packet *out, const struct sgx535_task_packet *w)
{
    return r && out && w && !overlaps(w, sizeof(*w), r, sizeof(*r)) &&
           !overlaps(w, sizeof(*w), out, sizeof(*out)) &&
           !overlaps(out, sizeof(*out), r, sizeof(*r));
}

static int word_arguments(const struct sgx535_task_packet_word_workspace *words,
                          const void *request, unsigned long request_bytes,
                          const struct sgx535_task_packet *out,
                          const struct sgx535_task_packet *packet)
{
    return words && !overlaps(words, sizeof(*words), request, request_bytes) &&
           !overlaps(words, sizeof(*words), out, sizeof(*out)) &&
           !overlaps(words, sizeof(*words), packet, sizeof(*packet));
}

enum sgx535_task_status sgx535_task_pair_mask_set_packet_build_words(
    const struct sgx535_task_pair_request *r, struct sgx535_task_packet *out,
    struct sgx535_task_packet *workspace, struct sgx535_task_packet_word_workspace *words)
{
    enum sgx535_task_status result;
    if (r && r->profile != SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    if (!pair_arguments(r, out, workspace) || !word_arguments(words, r, sizeof(*r), out, workspace))
        return SGX535_TASK_INVALID;
    result = pair_construct(r, workspace, words);
    if (result == SGX535_TASK_BLOCKED)
        memcpy(out, workspace, sizeof(*out));
    return result;
}

static enum sgx535_task_status pair_build(const struct sgx535_task_pair_request *r,
                                          struct sgx535_task_packet *out,
                                          struct sgx535_task_packet *workspace)
{
    struct sgx535_task_packet_word_workspace words;
    enum sgx535_task_status result;
    if (!pair_arguments(r, out, workspace))
        return SGX535_TASK_INVALID;
    result = pair_construct(r, workspace, &words);
    if (result == SGX535_TASK_BLOCKED)
        memcpy(out, workspace, sizeof(*out));
    return result;
}

static enum sgx535_task_status pair_validate(const struct sgx535_task_pair_request *r,
                                             const struct sgx535_task_packet *p,
                                             struct sgx535_task_packet *workspace)
{
    struct sgx535_task_packet_word_workspace words;
    enum sgx535_task_status result;
    if (!pair_arguments(r, p, workspace))
        return SGX535_TASK_INVALID;
    result = pair_construct(r, workspace, &words);
    if (result != SGX535_TASK_BLOCKED)
        return result;
    return memcmp(p, workspace, sizeof(*p)) ? SGX535_TASK_INVALID : SGX535_TASK_BLOCKED;
}

enum sgx535_task_status
sgx535_task_pair_mask_set_packet_build(const struct sgx535_task_pair_request *r,
                                       struct sgx535_task_packet *out,
                                       struct sgx535_task_packet *workspace)
{
    if (r && r->profile != SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    return pair_build(r, out, workspace);
}
enum sgx535_task_status
sgx535_task_pair_mask_set_packet_validate(const struct sgx535_task_pair_request *r,
                                          const struct sgx535_task_packet *p,
                                          struct sgx535_task_packet *workspace)
{
    if (r && r->profile != SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    return pair_validate(r, p, workspace);
}
enum sgx535_task_status sgx535_task_pair_packet_build(const struct sgx535_task_pair_request *r,
                                                      struct sgx535_task_packet *out,
                                                      struct sgx535_task_packet *workspace)
{
    if (r && r->profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    return r && r->profile == SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT
               ? sgx535_task_pair_mask_set_packet_build(r, out, workspace)
               : pair_build(r, out, workspace);
}
enum sgx535_task_status sgx535_task_pair_packet_validate(const struct sgx535_task_pair_request *r,
                                                         const struct sgx535_task_packet *p,
                                                         struct sgx535_task_packet *workspace)
{
    if (r && r->profile == SGX535_TASK_PAIR_SECOND_MASK_SET_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    return r && r->profile == SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT
               ? sgx535_task_pair_mask_set_packet_validate(r, p, workspace)
               : pair_validate(r, p, workspace);
}

enum sgx535_task_status
sgx535_task_packet_build_words(const struct sgx535_task_request *r, struct sgx535_task_packet *out,
                               struct sgx535_task_packet *workspace,
                               struct sgx535_task_packet_word_workspace *words)
{
    enum sgx535_task_status result;
    if (!r || !out || !workspace || overlaps(workspace, sizeof(*workspace), r, sizeof(*r)) ||
        overlaps(workspace, sizeof(*workspace), out, sizeof(*out)) ||
        !word_arguments(words, r, sizeof(*r), out, workspace))
        return SGX535_TASK_INVALID;
    result = construct(r, workspace, words);
    if (result == SGX535_TASK_BLOCKED)
        memcpy(out, workspace, sizeof(*out));
    return result;
}

enum sgx535_task_status sgx535_task_packet_build(const struct sgx535_task_request *r,
                                                 struct sgx535_task_packet *out,
                                                 struct sgx535_task_packet *workspace)
{
    struct sgx535_task_packet_word_workspace words;
    return sgx535_task_packet_build_words(r, out, workspace, &words);
}

enum sgx535_task_status sgx535_task_packet_validate(const struct sgx535_task_request *r,
                                                    const struct sgx535_task_packet *p,
                                                    struct sgx535_task_packet *workspace)
{
    struct sgx535_task_packet_word_workspace words;
    enum sgx535_task_status result;
    if (!r || !p || !workspace || overlaps(workspace, sizeof(*workspace), r, sizeof(*r)) ||
        overlaps(workspace, sizeof(*workspace), p, sizeof(*p)))
        return SGX535_TASK_INVALID;
    result = construct(r, workspace, &words);
    if (result != SGX535_TASK_BLOCKED)
        return result;
    return memcmp(p, workspace, sizeof(*p)) ? SGX535_TASK_INVALID : SGX535_TASK_BLOCKED;
}
