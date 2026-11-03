// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_transport.h"
#include <string.h>

static int valid(const struct msvdx_fake_mtx *m)
{
    return m && m->self == m && m->owner && m->epoch;
}
static int budget(uint32_t n)
{
    return n && n <= MSVDX_MODEL_MAX_POLLS;
}
static int input_overlaps(const struct msvdx_fake_mtx *m, const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)m, b = (uintptr_t)p;
    return n > UINTPTR_MAX - b || sizeof(*m) > UINTPTR_MAX - a || (a < b + n && b < a + sizeof(*m));
}
static int token_equal(struct msvdx_model_token a, struct msvdx_model_token b)
{
    return a.owner == b.owner && a.epoch == b.epoch && a.sequence == b.sequence;
}
static enum msvdx_model_status fail(struct msvdx_fake_mtx *m, enum msvdx_model_status error,
                                    bool possible)
{
    m->primary_error = error;
    if (possible) {
        m->phase = MSVDX_MODEL_HOLD;
        m->retained = true;
    }
    return error;
}
static enum msvdx_model_status action(struct msvdx_fake_mtx *m, enum msvdx_model_action a,
                                      bool mutation, bool *possible)
{
    if (m->operations == UINT64_MAX)
        return MSVDX_MODEL_LIMIT;
    ++m->operations;
    m->last_action = a;
    if (m->operations == m->fault.operation) {
        *possible |= m->fault.uncertain;
        return MSVDX_MODEL_IO;
    }
    *possible |= mutation;
    return MSVDX_MODEL_OK;
}
static enum msvdx_model_status wait_for(struct msvdx_fake_mtx *m, enum msvdx_model_action a,
                                        uint32_t limit, bool *possible)
{
    uint32_t i;
    for (i = 1; i <= limit; ++i) {
        enum msvdx_model_status s = action(m, a, false, possible);
        ++m->polls;
        if (s != MSVDX_MODEL_OK)
            return s;
        if (m->fault.ready_after_polls && i >= m->fault.ready_after_polls)
            return MSVDX_MODEL_OK;
    }
    return MSVDX_MODEL_TIMEOUT;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
enum msvdx_model_status msvdx_model_init(struct msvdx_fake_mtx *m, uint64_t owner, uint64_t epoch,
                                         struct msvdx_fw_geometry g)
{
    if (!m || !owner || !epoch)
        return MSVDX_MODEL_ARGUMENT;
    if (m->self || m->phase != MSVDX_MODEL_COLD)
        return MSVDX_MODEL_STATE;
    memset(m, 0, sizeof(*m));
    m->self = m;
    m->owner = owner;
    m->epoch = epoch;
    m->geometry = g;
    m->fault.ready_after_polls = 1;
    m->fault.signature_value = MSVDX_FW_SIGNATURE;
    return MSVDX_MODEL_OK;
}
enum msvdx_model_status msvdx_model_load(struct msvdx_fake_mtx *m, const uint8_t *bytes, size_t n,
                                         uint32_t limit)
{
    struct msvdx_fw_plan p;
    enum msvdx_model_status s;
    bool possible = false;
    uint32_t i, pass, ordinal = 0;
    if (!valid(m) || !bytes || !budget(limit) || input_overlaps(m, bytes, n))
        return MSVDX_MODEL_ARGUMENT;
    if (m->phase != MSVDX_MODEL_COLD)
        return MSVDX_MODEL_STATE;
    if (msvdx_fw_plan_v2(bytes, n, &m->geometry, &p) != MSVDX_FW_OK)
        return MSVDX_MODEL_FIRMWARE;
    s = action(m, MSVDX_MODEL_RESET, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->running = false;
    s = action(m, MSVDX_MODEL_COMMS, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->read_index = m->write_index = 0;
    /* Copy all banks, then independently read every word before PC/start. */
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < p.count; ++i) {
            const struct msvdx_fw_chunk *c = &p.chunks[i];
            uint32_t j;
            uint32_t *ram = c->space == MSVDX_FW_CODE ? m->code : m->data;
            for (j = 0; j < c->words; ++j) {
                uint32_t word = le32(bytes + c->source_offset + (size_t)j * 4u);
                uint32_t at = c->word_address + j;
                s = wait_for(m, MSVDX_MODEL_RAM_READY, limit, &possible);
                if (s != MSVDX_MODEL_OK)
                    return fail(m, s, possible);
                s = action(m, pass ? MSVDX_MODEL_VERIFY : MSVDX_MODEL_UPLOAD, !pass, &possible);
                if (s != MSVDX_MODEL_OK)
                    return fail(m, s, possible);
                if (!pass)
                    ram[at] = word;
                else {
                    uint32_t readback = ram[at];
                    if (++ordinal == m->fault.corrupt_readback_word)
                        readback ^= 1u;
                    if (readback != word)
                        return fail(m, MSVDX_MODEL_READBACK, possible);
                }
            }
        }
    s = action(m, MSVDX_MODEL_PC, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->pc = MSVDX_FW_CODE_BASE;
    s = action(m, MSVDX_MODEL_START, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->running = true;
    s = wait_for(m, MSVDX_MODEL_SIGNATURE_POLL, limit, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    if (m->fault.signature_value != MSVDX_FW_SIGNATURE)
        return fail(m, MSVDX_MODEL_SIGNATURE, possible);
    m->phase = MSVDX_MODEL_READY;
    return MSVDX_MODEL_OK;
}
enum msvdx_model_status msvdx_model_submit(struct msvdx_fake_mtx *m, struct msvdx_model_token t,
                                           const uint8_t *packet, size_t n)
{
    struct msvdx_ring_plan p;
    struct msvdx_message message;
    enum msvdx_ring_status r;
    enum msvdx_model_status s;
    uint32_t i;
    bool possible = false;
    if (!valid(m) || !packet)
        return MSVDX_MODEL_ARGUMENT;
    if (m->phase != MSVDX_MODEL_READY)
        return MSVDX_MODEL_STATE;
    if (t.owner != m->owner || t.epoch != m->epoch || !t.sequence || t.sequence <= m->last_sequence)
        return MSVDX_MODEL_STALE;
    if (msvdx_message_decode(packet, n, MSVDX_HOST_TO_MTX, &message) != MSVDX_OK ||
        message.message_id != MSVDX_RENDER_ID || message.payload.render.fence_value != t.sequence)
        return MSVDX_MODEL_PACKET;
    r = msvdx_ring_plan_render(packet, n, m->read_index, m->write_index, &p);
    if (r != MSVDX_RING_OK)
        return r == MSVDX_RING_FULL ? MSVDX_MODEL_FULL : MSVDX_MODEL_PACKET;
    m->visibility_before_index = m->index_before_kick = false;
    for (i = 0; i < p.word_count; ++i) {
        s = action(m, MSVDX_MODEL_RING_WORD, true, &possible);
        if (s != MSVDX_MODEL_OK) {
            if (possible)
                m->active = t;
            return fail(m, s, possible);
        }
        m->active = t;
        m->ring[(p.initial_write + i) % MSVDX_HOST_RING_WORDS] = p.words[i];
    }
    s = action(m, MSVDX_MODEL_PUBLISH_VISIBILITY, false, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->visibility_before_index = true;
    s = action(m, MSVDX_MODEL_WRITE_INDEX, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->write_index = p.final_write;
    m->index_before_kick = true;
    s = action(m, MSVDX_MODEL_KICK, true, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, possible);
    m->last_sequence = t.sequence;
    m->phase = MSVDX_MODEL_SUBMITTED;
    m->retained = true;
    return MSVDX_MODEL_OK;
}
enum msvdx_model_status msvdx_model_receive(struct msvdx_fake_mtx *m, uint64_t epoch,
                                            const uint8_t *packet, size_t n, uint32_t consumed)
{
    struct msvdx_message message;
    if (!valid(m) || !packet)
        return MSVDX_MODEL_ARGUMENT;
    if (m->phase == MSVDX_MODEL_COMPLETED)
        return fail(m, MSVDX_MODEL_STALE, true);
    if (m->phase != MSVDX_MODEL_SUBMITTED)
        return MSVDX_MODEL_STATE;
    if (epoch != m->epoch)
        return fail(m, MSVDX_MODEL_STALE, true);
    if (msvdx_message_decode(packet, n, MSVDX_MTX_TO_HOST, &message) != MSVDX_OK ||
        message.message_id != MSVDX_COMPLETED_ID)
        return fail(m, MSVDX_MODEL_PACKET, true);
    if (message.payload.completed.fence_value != m->active.sequence)
        return fail(m, MSVDX_MODEL_STALE, true);
    if (!(message.payload.completed.flags & UINT32_C(0x4000)) ||
        consumed >= MSVDX_HOST_RING_WORDS || consumed != m->write_index)
        return fail(m, MSVDX_MODEL_PACKET, true);
    m->read_index = consumed;
    m->phase = MSVDX_MODEL_COMPLETED;
    return MSVDX_MODEL_OK;
}
enum msvdx_model_status msvdx_model_retire(struct msvdx_fake_mtx *m, struct msvdx_model_token t,
                                           uint32_t limit)
{
    enum msvdx_model_status s;
    bool possible = true;
    if (!valid(m) || !budget(limit))
        return MSVDX_MODEL_ARGUMENT;
    if (m->phase != MSVDX_MODEL_COMPLETED)
        return MSVDX_MODEL_STATE;
    if (!token_equal(t, m->active))
        return fail(m, MSVDX_MODEL_STALE, true);
    s = wait_for(m, MSVDX_MODEL_DRAIN_POLL, limit, &possible);
    if (s == MSVDX_MODEL_OK)
        s = wait_for(m, MSVDX_MODEL_READ_VISIBILITY, limit, &possible);
    if (s != MSVDX_MODEL_OK)
        return fail(m, s, true);
    memset(&m->active, 0, sizeof(m->active));
    m->retained = false;
    m->phase = MSVDX_MODEL_READY;
    return MSVDX_MODEL_OK;
}
