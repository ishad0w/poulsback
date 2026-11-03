// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_transport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)
static const uint8_t firmware[] = {2,  0,  0,    0,    3,  0,  0,  0,  2,  0,  0,  0,
                                   12, 0,  0x88, 0x82, 1,  2,  3,  4,  5,  6,  7,  8,
                                   9,  10, 11,   12,   13, 14, 15, 16, 17, 18, 19, 20};
static struct msvdx_fake_mtx *fresh(void)
{
    struct msvdx_fake_mtx *m = calloc(1, sizeof(*m));
    struct msvdx_fw_geometry g = {8, 32, 32};
    CHECK(m);
    CHECK(msvdx_model_init(m, 41, 29, g) == MSVDX_MODEL_OK);
    return m;
}
static struct msvdx_fake_mtx *loaded(void)
{
    struct msvdx_fake_mtx *m = fresh();
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 3) == MSVDX_MODEL_OK);
    CHECK(m->phase == MSVDX_MODEL_READY && m->running && m->pc == MSVDX_FW_CODE_BASE);
    CHECK(m->code[0] == UINT32_C(0x04030201) && m->code[2] == UINT32_C(0x0c0b0a09));
    CHECK(m->data[3] == UINT32_C(0x100f0e0d) && m->data[4] == UINT32_C(0x14131211));
    return m;
}
static void render(uint8_t packet[32], uint32_t sequence)
{
    struct msvdx_message x;
    memset(&x, 0, sizeof(x));
    x.message_id = MSVDX_RENDER_ID;
    x.payload.render.fence_value = sequence;
    CHECK(msvdx_message_encode(&x, MSVDX_HOST_TO_MTX, packet, 32) == MSVDX_OK);
}
static void completed(uint8_t packet[12], uint32_t sequence, uint32_t flags)
{
    struct msvdx_message x;
    memset(&x, 0, sizeof(x));
    x.message_id = MSVDX_COMPLETED_ID;
    x.payload.completed.fence_value = sequence;
    x.payload.completed.flags = flags;
    CHECK(msvdx_message_encode(&x, MSVDX_MTX_TO_HOST, packet, 12) == MSVDX_OK);
}
static void load_faults(void)
{
    struct msvdx_fake_mtx *m = loaded();
    uint64_t operations = m->operations, at;
    unsigned uncertain, word;
    free(m);
    for (uncertain = 0; uncertain < 2; ++uncertain)
        for (at = 1; at <= operations; ++at) {
            m = fresh();
            m->fault.operation = at;
            m->fault.uncertain = uncertain != 0;
            CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 3) == MSVDX_MODEL_IO);
            CHECK(m->operations == at && m->primary_error == MSVDX_MODEL_IO);
            CHECK(m->phase == (at == 1 && !uncertain ? MSVDX_MODEL_COLD : MSVDX_MODEL_HOLD));
            CHECK(m->retained == (at != 1 || uncertain != 0));
            if (m->phase == MSVDX_MODEL_HOLD) {
                CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 3) == MSVDX_MODEL_STATE);
                CHECK(msvdx_model_init(m, 41, 30, m->geometry) == MSVDX_MODEL_STATE);
            }
            free(m);
        }
    for (word = 1; word <= 5; ++word) {
        m = fresh();
        m->fault.corrupt_readback_word = word;
        CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 3) == MSVDX_MODEL_READBACK);
        CHECK(m->phase == MSVDX_MODEL_HOLD && m->retained);
        free(m);
    }
    m = fresh();
    m->fault.ready_after_polls = 0;
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 7) == MSVDX_MODEL_TIMEOUT);
    CHECK(m->polls == 7 && m->phase == MSVDX_MODEL_HOLD);
    free(m);
    m = fresh();
    m->fault.ready_after_polls = 2;
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 1) == MSVDX_MODEL_TIMEOUT);
    CHECK(m->polls == 1 && m->phase == MSVDX_MODEL_HOLD);
    free(m);
    m = fresh();
    m->fault.signature_value ^= 1u;
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 3) == MSVDX_MODEL_SIGNATURE);
    CHECK(m->phase == MSVDX_MODEL_HOLD && m->running);
    free(m);
    m = fresh();
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware) - 1u, 3) == MSVDX_MODEL_FIRMWARE);
    CHECK(m->operations == 0 && m->phase == MSVDX_MODEL_COLD);
    CHECK(msvdx_model_load(m, firmware, sizeof(firmware), 0) == MSVDX_MODEL_ARGUMENT);
    CHECK(msvdx_model_load(m, (const uint8_t *)m->code, sizeof(firmware), 3) ==
          MSVDX_MODEL_ARGUMENT);
    free(m);
}
static void submit_faults(void)
{
    struct msvdx_model_token t = {41, 29, 17};
    struct msvdx_fake_mtx *m = loaded();
    uint64_t begin = m->operations, count, i;
    uint8_t packet[32];
    unsigned uncertain;
    render(packet, 17);
    m->read_index = m->write_index = 97;
    CHECK(msvdx_model_submit(m, t, packet, sizeof(packet)) == MSVDX_MODEL_OK);
    CHECK(m->write_index == 8 && m->visibility_before_index && m->index_before_kick);
    CHECK(m->ring[97] == 12 && !m->ring[98] && !m->ring[99]);
    count = m->operations - begin;
    CHECK(count == 14);
    free(m);
    for (uncertain = 0; uncertain < 2; ++uncertain)
        for (i = 1; i <= count; ++i) {
            m = loaded();
            m->read_index = m->write_index = 97;
            m->fault.operation = m->operations + i;
            m->fault.uncertain = uncertain != 0;
            CHECK(msvdx_model_submit(m, t, packet, sizeof(packet)) == MSVDX_MODEL_IO);
            CHECK(m->phase == (i == 1 && !uncertain ? MSVDX_MODEL_READY : MSVDX_MODEL_HOLD));
            CHECK(m->retained == (i != 1 || uncertain != 0));
            if (m->retained) {
                CHECK(m->active.sequence == 17 && m->active.owner == 41);
                CHECK(msvdx_model_submit(m, t, packet, sizeof(packet)) == MSVDX_MODEL_STATE);
            }
            CHECK(m->write_index == (i == 14 ? 8u : 97u));
            free(m);
        }
    m = loaded();
    m->read_index = 1;
    m->write_index = 0;
    begin = m->operations;
    CHECK(msvdx_model_submit(m, t, packet, sizeof(packet)) == MSVDX_MODEL_FULL);
    CHECK(m->operations == begin && !m->retained);
    free(m);
}
static void replies(void)
{
    uint8_t packet[32], reply[12];
    struct msvdx_model_token t = {41, 29, 1};
    struct msvdx_fake_mtx *m;
    unsigned variant, i;
    render(packet, 1);
    completed(reply, 1, 0x4000);
    for (variant = 0; variant < 8; ++variant) {
        uint8_t bad[12];
        memcpy(bad, reply, sizeof(bad));
        m = loaded();
        CHECK(msvdx_model_submit(m, t, packet, 32) == MSVDX_MODEL_OK);
        if (variant == 1)
            bad[4] = 2;
        if (variant == 2)
            bad[1] = MSVDX_PANIC_ID;
        if (variant == 3)
            bad[1] = MSVDX_FAILED_ID;
        if (variant == 4)
            bad[9] = 0;
        if (variant == 5)
            bad[0] = 0;
        CHECK(msvdx_model_receive(m, variant ? 29 : 30, bad, variant == 6 ? 11u : 12u,
                                  variant == 7 ? 99u : m->write_index) != MSVDX_MODEL_OK);
        CHECK(m->phase == MSVDX_MODEL_HOLD && m->retained && m->active.sequence == 1);
        CHECK(msvdx_model_retire(m, t, 3) == MSVDX_MODEL_STATE);
        free(m);
    }
    for (variant = 0; variant < 4; ++variant) {
        m = loaded();
        CHECK(msvdx_model_submit(m, t, packet, 32) == MSVDX_MODEL_OK);
        CHECK(msvdx_model_receive(m, 29, reply, 12, m->write_index) == MSVDX_MODEL_OK);
        CHECK(m->retained && m->phase == MSVDX_MODEL_COMPLETED);
        if (variant < 2)
            m->fault.operation = m->operations + variant + 1u;
        if (variant == 2)
            m->fault.ready_after_polls = 0;
        if (variant == 3)
            ++t.epoch;
        CHECK(msvdx_model_retire(m, t, 2) != MSVDX_MODEL_OK);
        CHECK(m->phase == MSVDX_MODEL_HOLD && m->retained);
        t.epoch = 29;
        free(m);
    }
    m = loaded();
    CHECK(msvdx_model_submit(m, t, packet, 32) == MSVDX_MODEL_OK);
    CHECK(msvdx_model_receive(m, 29, reply, 12, m->write_index) == MSVDX_MODEL_OK);
    CHECK(msvdx_model_receive(m, 29, reply, 12, m->write_index) == MSVDX_MODEL_STALE);
    CHECK(m->phase == MSVDX_MODEL_HOLD && m->retained && m->active.sequence == 1);
    CHECK(msvdx_model_retire(m, t, 3) == MSVDX_MODEL_STATE);
    free(m);
    m = loaded();
    for (i = 1; i <= 128; ++i) {
        t.sequence = i;
        render(packet, i);
        completed(reply, i, 0x4000);
        CHECK(msvdx_model_submit(m, t, packet, 32) == MSVDX_MODEL_OK);
        CHECK(msvdx_model_retire(m, t, 3) == MSVDX_MODEL_STATE);
        CHECK(msvdx_model_receive(m, 29, reply, 12, m->write_index) == MSVDX_MODEL_OK);
        CHECK(m->retained);
        CHECK(msvdx_model_retire(m, t, 3) == MSVDX_MODEL_OK);
        CHECK(!m->retained && m->phase == MSVDX_MODEL_READY && !m->active.sequence);
        CHECK(msvdx_model_receive(m, 29, reply, 12, m->write_index) == MSVDX_MODEL_STATE);
        CHECK(!m->retained && m->phase == MSVDX_MODEL_READY && !m->active.sequence);
        CHECK(msvdx_model_submit(m, t, packet, 32) == MSVDX_MODEL_STALE);
    }
    free(m);
}
int main(void)
{
    load_faults();
    submit_faults();
    replies();
    printf(
        "PASS CLOSED CPU fake MTX transport: %u checks; no MMIO, firmware execution or GPU certificate\n",
        checks);
    return 0;
}
