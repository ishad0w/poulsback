// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

#include "msvdx_proto.h"
#include <string.h>

struct layout {
    uint8_t id, bytes;
    enum msvdx_direction direction;
};
/* Byte 0 is the envelope size in bytes, byte 1 its ID; fields little-endian. */
static const struct layout layouts[] = {
    {MSVDX_RENDER_ID, 32, MSVDX_HOST_TO_MTX},          {MSVDX_COMPLETED_ID, 12, MSVDX_MTX_TO_HOST},
    {MSVDX_DEBLOCK_REQUIRED_ID, 8, MSVDX_MTX_TO_HOST}, {MSVDX_FAILED_ID, 12, MSVDX_MTX_TO_HOST},
    {MSVDX_PANIC_ID, 12, MSVDX_MTX_TO_HOST},
};

static const struct layout *find(uint8_t id)
{
    size_t i;
    for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
        if (layouts[i].id == id)
            return &layouts[i];
    return NULL;
}
static int direction_valid(enum msvdx_direction d)
{
    return d == MSVDX_HOST_TO_MTX || d == MSVDX_MTX_TO_HOST;
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

size_t msvdx_proto_envelope_size(uint8_t id)
{
    const struct layout *l = find(id);
    return l ? l->bytes : 0;
}

enum msvdx_status msvdx_message_encode(const struct msvdx_message *message,
                                       enum msvdx_direction direction, uint8_t *output,
                                       size_t capacity)
{
    uint8_t e[MSVDX_MAX_ENVELOPE_BYTES];
    const struct layout *l;
    if (!message || !output || !direction_valid(direction))
        return MSVDX_ARGUMENT;
    l = find(message->message_id);
    if (!l)
        return MSVDX_UNSUPPORTED;
    if (l->direction != direction)
        return MSVDX_DIRECTION;
    if (capacity < l->bytes)
        return MSVDX_LENGTH;
    /* Build the whole envelope first: the output may overlap the message. */
    memset(e, 0, sizeof(e));
    e[0] = l->bytes;
    e[1] = l->id;
    if (l->id == MSVDX_RENDER_ID) {
        const struct msvdx_render_fields *r = &message->payload.render;
        if (r->buffer_size & ~MSVDX_BUFFER_SIZE_MASK ||
            r->buffer_size_high & MSVDX_BUFFER_SIZE_MASK)
            return MSVDX_RANGE;
        put16(e + 2, (uint32_t)r->buffer_size | r->buffer_size_high);
        put32(e + 4, r->mmu_ptd);
        put32(e + 8, r->lldma_address);
        put32(e + 12, r->context);
        put32(e + 16, r->fence_value);
        put32(e + 20, r->operating_mode);
        put16(e + 24, r->first_mb);
        put16(e + 26, r->last_mb);
        put32(e + 28, r->flags);
    } else if (l->id == MSVDX_COMPLETED_ID) {
        put16(e + 2, message->payload.completed.header);
        put32(e + 4, message->payload.completed.fence_value);
        put32(e + 8, message->payload.completed.flags);
    } else if (l->id == MSVDX_DEBLOCK_REQUIRED_ID) {
        put16(e + 2, message->payload.deblock.header);
        put32(e + 4, message->payload.deblock.context);
    } else {
        put16(e + 2, message->payload.failed.flags);
        put32(e + 4, message->payload.failed.fence_value);
        put32(e + 8, message->payload.failed.irq_status);
    }
    memcpy(output, e, l->bytes);
    return MSVDX_OK;
}

enum msvdx_status msvdx_message_decode(const uint8_t *input, size_t bytes,
                                       enum msvdx_direction direction,
                                       struct msvdx_message *message)
{
    struct msvdx_message m;
    const struct layout *l;
    if (!input || !message || !direction_valid(direction))
        return MSVDX_ARGUMENT;
    if (bytes < 2)
        return MSVDX_LENGTH;
    l = find(input[1]);
    if (!l)
        return MSVDX_UNSUPPORTED;
    if (l->direction != direction)
        return MSVDX_DIRECTION;
    if (bytes != l->bytes || input[0] != bytes)
        return MSVDX_LENGTH;
    memset(&m, 0, sizeof(m));
    m.message_id = l->id;
    if (l->id == MSVDX_RENDER_ID) {
        struct msvdx_render_fields *r = &m.payload.render;
        uint16_t size = le16(input + 2);
        r->buffer_size = size & MSVDX_BUFFER_SIZE_MASK;
        r->buffer_size_high = size & ~MSVDX_BUFFER_SIZE_MASK & 0xffffu;
        r->mmu_ptd = le32(input + 4);
        r->lldma_address = le32(input + 8);
        r->context = le32(input + 12);
        r->fence_value = le32(input + 16);
        r->operating_mode = le32(input + 20);
        r->first_mb = le16(input + 24);
        r->last_mb = le16(input + 26);
        r->flags = le32(input + 28);
    } else if (l->id == MSVDX_COMPLETED_ID) {
        m.payload.completed.header = le16(input + 2);
        m.payload.completed.fence_value = le32(input + 4);
        m.payload.completed.flags = le32(input + 8);
    } else if (l->id == MSVDX_DEBLOCK_REQUIRED_ID) {
        m.payload.deblock.header = le16(input + 2);
        m.payload.deblock.context = le32(input + 4);
    } else {
        m.payload.failed.flags = le16(input + 2);
        m.payload.failed.fence_value = le32(input + 4);
        m.payload.failed.irq_status = le32(input + 8);
    }
    memcpy(message, &m, sizeof(m));
    return MSVDX_OK;
}

static struct msvdx_proto_scan refuse(enum msvdx_status status)
{
    struct msvdx_proto_scan r = {status, 0, 0};
    return r;
}

struct msvdx_proto_scan msvdx_proto_scan(const uint8_t *span, size_t bytes,
                                         enum msvdx_direction direction, size_t max_bytes,
                                         uint32_t max_messages)
{
    struct msvdx_proto_scan r = {MSVDX_OK, 0, 0};
    size_t at = 0;
    if (!span || !max_bytes || !max_messages || !direction_valid(direction))
        return refuse(MSVDX_ARGUMENT);
    if (!bytes)
        return refuse(MSVDX_LENGTH);
    if (bytes > max_bytes)
        return refuse(MSVDX_LIMIT);
    while (at < bytes) {
        struct msvdx_message m;
        enum msvdx_status s;
        size_t n;
        if (r.messages == max_messages)
            return refuse(MSVDX_LIMIT);
        if (bytes - at < 2)
            return refuse(MSVDX_LENGTH);
        n = span[at];
        if (n < 2 || n > bytes - at)
            return refuse(MSVDX_LENGTH);
        s = msvdx_message_decode(span + at, n, direction, &m);
        if (s != MSVDX_OK)
            return refuse(s);
        ++r.messages;
        r.renders += m.message_id == MSVDX_RENDER_ID;
        at += n;
    }
    return r;
}

struct msvdx_proto_scan msvdx_proto_stamp_render(uint8_t *span, size_t bytes, size_t max_bytes,
                                                 uint32_t max_messages, uint32_t ptd,
                                                 uint32_t fence, bool invalidate_first)
{
    struct msvdx_proto_scan r =
        msvdx_proto_scan(span, bytes, MSVDX_HOST_TO_MTX, max_bytes, max_messages);
    size_t at;
    if (r.status != MSVDX_OK)
        return r;
    for (at = 0; at < bytes; at += span[at])
        if (span[at + 1] == MSVDX_RENDER_ID) {
            put32(span + at + 4, at == 0 && invalidate_first ? ptd | MSVDX_PTD_INVALIDATE : ptd);
            put32(span + at + 16, fence);
        }
    return r;
}

enum msvdx_ring_status msvdx_ring_plan_render(const uint8_t *packet, size_t bytes,
                                              uint32_t read_index, uint32_t write_index,
                                              struct msvdx_ring_plan *plan)
{
    const uint32_t ring = MSVDX_HOST_RING_WORDS, envelope = MSVDX_RENDER_BYTES / 4u;
    struct msvdx_ring_plan p;
    struct msvdx_message m;
    uint32_t i;
    if (!packet || !plan)
        return MSVDX_RING_ARGUMENT;
    if (read_index >= ring || write_index >= ring)
        return MSVDX_RING_INDEX;
    if (bytes != MSVDX_RENDER_BYTES ||
        msvdx_message_decode(packet, bytes, MSVDX_HOST_TO_MTX, &m) != MSVDX_OK ||
        m.message_id != MSVDX_RENDER_ID)
        return MSVDX_RING_PACKET;
    memset(&p, 0, sizeof(p));
    /* An envelope never wraps: pad the rest of the ring first. */
    p.padding_words = write_index + envelope > ring ? ring - write_index : 0;
    p.word_count = p.padding_words + envelope;
    if (p.word_count > (read_index + ring - write_index - 1u) % ring)
        return MSVDX_RING_FULL;
    p.read_snapshot = read_index;
    p.initial_write = write_index;
    p.final_write = (write_index + p.word_count) % ring;
    if (p.padding_words)
        p.words[0] = p.padding_words * 4u; /* size in bytes, ID 0 */
    for (i = 0; i < envelope; ++i)
        p.words[p.padding_words + i] = le32(packet + 4u * i);
    memcpy(plan, &p, sizeof(p));
    return MSVDX_RING_OK;
}
