// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_VARYING_PACKET_H
#define SGX535_VARYING_PACKET_H
#include "sgx535_task_packet.h"

/* Version 4 is reserved for a separately typed depth construction. */
#define SGX535_VARYING_PACKET_VERSION 5u
#define SGX535_VARYING_W1_COLOR0 2u
#define SGX535_VARYING_SMOOTH 1u
/* The ordinary CPU vertex stage has already selected and copied the flat
 * attribute. This is an equality-checked transport, not native flat shading. */
#define SGX535_VARYING_FLAT_REPLICATED 2u
#define SGX535_VARYING_MISSING_INPUT_LINKAGE (1u << 7)
#define SGX535_VARYING_MISSING_PACK_NUMERICS (1u << 8)
#define SGX535_VARYING_MISSING_ALL 0x1ffu

struct sgx535_varying_request {
    uint32_t version, profile, core_id, revision;
    uint32_t clip_position[3][4]; /* IEEE binary32; W exactly +1. */
    uint32_t rgba[3][4];          /* Finite nonnegative binary32, bounded to [0,1]. */
    uint32_t interpolation, reserved[3];
};
struct sgx535_varying_workspace {
    struct sgx535_task_packet base, result;
};

/* Complete source-derived 64x64 triangle: eight vertex output words, one
 * four-component iterator and a generic PRIMARY->TEMP->packed OUTPUT program.
 * This is word construction, not proof of interpolation, normalized PACK,
 * perspective, task effects or physical execution. No depth/texture/blend.
 * FLAT_REPLICATED requires all three complete RGBA vectors to be bit-identical
 * in the same finite [0,1] input domain. It emits the existing smooth iterator
 * and state; no native provoking-vertex or interpolation effect is inferred.
 * No input/output/workspace overlap is accepted. Refusals preserve output;
 * workspace is scratch. Existing packet versions 2 and 3 remain unchanged. */
enum sgx535_task_status sgx535_varying_packet_build(const struct sgx535_varying_request *,
                                                    struct sgx535_task_packet *,
                                                    struct sgx535_varying_workspace *);
enum sgx535_task_status sgx535_varying_packet_validate(const struct sgx535_varying_request *,
                                                       const struct sgx535_task_packet *,
                                                       struct sgx535_varying_workspace *);
#endif
