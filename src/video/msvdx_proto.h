// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

#ifndef MSVDX_PROTO_H
#define MSVDX_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Legacy Poulsbo MSVDX host<->firmware envelopes: codec, stream checks and the
 * host->MTX ring planner. Pure CPU code: no allocation, I/O, device access or
 * mutable static state. Byte spans may be unaligned. A refusal writes nothing. */

#define MSVDX_RENDER_ID 0x81u
#define MSVDX_COMPLETED_ID 0xc0u
#define MSVDX_DEBLOCK_REQUIRED_ID 0xc2u
#define MSVDX_FAILED_ID 0xc5u
#define MSVDX_PANIC_ID 0xc7u
#define MSVDX_RENDER_BYTES 32u
#define MSVDX_MAX_ENVELOPE_BYTES 32u
/* Render halfword at byte 2: bits 11:0 buffer size, bits 15:12 unused. */
#define MSVDX_BUFFER_SIZE_MASK 0x0fffu
/* PTD bit 0 asks the firmware to invalidate its MMU before the job. */
#define MSVDX_PTD_INVALIDATE 1u
#define MSVDX_HOST_RING_WORDS 100u
/* Seven padding words at most, then the eight render words. */
#define MSVDX_RING_PLAN_WORDS 15u

enum msvdx_direction { MSVDX_HOST_TO_MTX = 1, MSVDX_MTX_TO_HOST = 2 };
enum msvdx_status {
    MSVDX_OK,
    MSVDX_ARGUMENT,
    MSVDX_LENGTH,
    MSVDX_UNSUPPORTED,
    MSVDX_DIRECTION,
    MSVDX_RANGE,
    MSVDX_LIMIT
};
enum msvdx_ring_status {
    MSVDX_RING_OK,
    MSVDX_RING_ARGUMENT,
    MSVDX_RING_INDEX,
    MSVDX_RING_PACKET,
    MSVDX_RING_FULL
};

struct msvdx_render_fields {
    uint16_t buffer_size;      /* bits 11:0 of the halfword at byte 2 */
    uint16_t buffer_size_high; /* bits 15:12 of that halfword, kept in place */
    uint32_t mmu_ptd, lldma_address, context, fence_value, operating_mode;
    uint16_t first_mb, last_mb; /* last: (MB rows - 1) << 8 | (MB columns - 1) */
    uint32_t flags;
};
struct msvdx_completed_fields {
    uint16_t header; /* bytes 2-3, carried uninterpreted */
    uint32_t fence_value, flags;
};
struct msvdx_deblock_fields {
    uint16_t header; /* bytes 2-3, carried uninterpreted */
    uint32_t context;
};
struct msvdx_failed_fields { /* command failed (0xc5) and hardware panic (0xc7) */
    uint16_t flags;
    uint32_t fence_value, irq_status;
};
struct msvdx_message {
    uint8_t message_id;
    union {
        struct msvdx_render_fields render;
        struct msvdx_completed_fields completed;
        struct msvdx_deblock_fields deblock;
        struct msvdx_failed_fields failed;
    } payload;
};

/* Envelope size in bytes for an ID with a layout, else 0. */
size_t msvdx_proto_envelope_size(uint8_t id);
/* Writes exactly the envelope's size; bytes past it are untouched. Output may
 * overlap the message. */
enum msvdx_status msvdx_message_encode(const struct msvdx_message *message,
                                       enum msvdx_direction direction, uint8_t *output,
                                       size_t capacity);
/* Exactly one whole envelope; replaces the whole record. Input may overlap it. */
enum msvdx_status msvdx_message_decode(const uint8_t *input, size_t bytes,
                                       enum msvdx_direction direction,
                                       struct msvdx_message *message);

/* Counts are zero on every refusal. */
struct msvdx_proto_scan {
    enum msvdx_status status;
    uint32_t messages, renders;
};
/* A concatenation of whole envelopes of one direction, nothing between them.
 * Both budgets are positive caller limits. Reads the span only. */
struct msvdx_proto_scan msvdx_proto_scan(const uint8_t *span, size_t bytes,
                                         enum msvdx_direction direction, size_t max_bytes,
                                         uint32_t max_messages);
/* Scans as host->MTX, then writes the PTD word and the fence into every render
 * envelope; invalidate_first ORs MSVDX_PTD_INVALIDATE into the first one only.
 * One fence for the whole batch. A refusal leaves the span unchanged. */
struct msvdx_proto_scan msvdx_proto_stamp_render(uint8_t *span, size_t bytes, size_t max_bytes,
                                                 uint32_t max_messages, uint32_t ptd,
                                                 uint32_t fence, bool invalidate_first);

/* Where one render envelope goes in the 100-word host->MTX ring. Indices count
 * words; one slot stays empty so that a full ring never looks empty. A wrap is
 * preceded by one padding envelope (size in bytes, ID 0, zero body) and both
 * are published by a single write-index store. Plans only: no ring memory or
 * index is written. Packet and plan may overlap. */
struct msvdx_ring_plan {
    uint32_t read_snapshot, initial_write, final_write;
    uint32_t padding_words, word_count;
    uint32_t words[MSVDX_RING_PLAN_WORDS]; /* first word_count are stored from initial_write */
};
enum msvdx_ring_status msvdx_ring_plan_render(const uint8_t *packet, size_t bytes,
                                              uint32_t read_index, uint32_t write_index,
                                              struct msvdx_ring_plan *plan);
#endif
