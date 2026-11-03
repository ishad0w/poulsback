// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef MSVDX_TRANSPORT_H
#define MSVDX_TRANSPORT_H
#include "msvdx_firmware.h"
#include "msvdx_proto.h"
#include <stdbool.h>

#define MSVDX_MODEL_MAX_POLLS 1000u
enum msvdx_model_phase {
    MSVDX_MODEL_COLD,
    MSVDX_MODEL_READY,
    MSVDX_MODEL_SUBMITTED,
    MSVDX_MODEL_COMPLETED,
    MSVDX_MODEL_HOLD
};
enum msvdx_model_status {
    MSVDX_MODEL_OK,
    MSVDX_MODEL_ARGUMENT,
    MSVDX_MODEL_STATE,
    MSVDX_MODEL_FIRMWARE,
    MSVDX_MODEL_IO,
    MSVDX_MODEL_TIMEOUT,
    MSVDX_MODEL_READBACK,
    MSVDX_MODEL_SIGNATURE,
    MSVDX_MODEL_PACKET,
    MSVDX_MODEL_STALE,
    MSVDX_MODEL_FULL,
    MSVDX_MODEL_LIMIT
};
enum msvdx_model_action {
    MSVDX_MODEL_RESET,
    MSVDX_MODEL_COMMS,
    MSVDX_MODEL_RAM_READY,
    MSVDX_MODEL_UPLOAD,
    MSVDX_MODEL_VERIFY,
    MSVDX_MODEL_PC,
    MSVDX_MODEL_START,
    MSVDX_MODEL_SIGNATURE_POLL,
    MSVDX_MODEL_RING_WORD,
    MSVDX_MODEL_PUBLISH_VISIBILITY,
    MSVDX_MODEL_WRITE_INDEX,
    MSVDX_MODEL_KICK,
    MSVDX_MODEL_DRAIN_POLL,
    MSVDX_MODEL_READ_VISIBILITY
};
struct msvdx_model_token {
    uint64_t owner, epoch;
    uint32_t sequence;
};
struct msvdx_model_fault {
    uint64_t operation;         /* One-based absolute model operation; zero disables. */
    bool uncertain;             /* Failure may have mutated even when no value was stored. */
    uint32_t ready_after_polls; /* Each wait, one-based; zero means never. */
    uint32_t signature_value;
    uint32_t corrupt_readback_word; /* One-based over code then data; zero disables. */
};
/* CLOSED CPU fake provider: all RAM/register/ring effects are fields below.
 * There are no function pointers, MMIO, ioctl, GPU mapping or vendor entry
 * points. Caller serializes the whole object and retains stable input bytes.
 * Firmware bytes must be separate from the complete fake-provider object.
 * Do not reinterpret a model token, wait, completion or HOLD as a physical
 * certificate. A physical backend needs its own reviewed API and admission.
 * Fault/settings fields are test controls, never a userspace wire interface. */
struct msvdx_fake_mtx {
    struct msvdx_fake_mtx *self;
    uint64_t owner, epoch, operations;
    struct msvdx_fw_geometry geometry;
    struct msvdx_model_fault fault;
    enum msvdx_model_phase phase;
    enum msvdx_model_status primary_error;
    enum msvdx_model_action last_action;
    uint32_t code[MSVDX_FW_MAX_REGION_BYTES / 4u];
    uint32_t data[MSVDX_FW_MAX_REGION_BYTES / 4u];
    uint32_t ring[MSVDX_HOST_RING_WORDS];
    uint32_t read_index, write_index, last_sequence, pc, polls;
    struct msvdx_model_token active;
    bool running, retained, visibility_before_index, index_before_kick;
};
/* Fresh zero-initialized object only; HOLD has no reset/reinit/release API.
 * Geometry is CPU policy and is validated completely by load before effects. */
enum msvdx_model_status msvdx_model_init(struct msvdx_fake_mtx *, uint64_t owner, uint64_t epoch,
                                         struct msvdx_fw_geometry);
enum msvdx_model_status msvdx_model_load(struct msvdx_fake_mtx *, const uint8_t *, size_t,
                                         uint32_t poll_budget);
/* One exact render32 packet. No sequence wrap/reuse within the model epoch.
 * Field serialization does not validate addresses, BOs or decode commands.
 * Rejection before any ring mutation keeps READY; uncertain/partial effects
 * retain the active token and enter terminal HOLD. FULL makes no effects. */
enum msvdx_model_status msvdx_model_submit(struct msvdx_fake_mtx *, struct msvdx_model_token,
                                           const uint8_t *, size_t);
/* Fake firmware observation only. Exact epoch/fence, completion ID, HOST_INT
 * and fully consumed ring snapshot are required by this single-job policy.
 * Bad/stale/error replies while a job is active enter HOLD; they never release
 * it. HOST_INT is not, by itself, certified hardware retirement. */
enum msvdx_model_status msvdx_model_receive(struct msvdx_fake_mtx *, uint64_t epoch,
                                            const uint8_t *, size_t, uint32_t read_snapshot);
/* Completion, drain and read visibility are separate model gates. Both waits
 * are bounded; timeout cannot signal success, clear HOLD or recycle a token. */
enum msvdx_model_status msvdx_model_retire(struct msvdx_fake_mtx *, struct msvdx_model_token,
                                           uint32_t poll_budget);
#endif
