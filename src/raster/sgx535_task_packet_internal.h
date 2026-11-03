// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TASK_PACKET_INTERNAL_H
#define SGX535_TASK_PACKET_INTERNAL_H
#include "sgx535_task_packet.h"
#include "sgx535_vertex_use.h"
#include "sgx535_vertex_pds.h"
#include "sgx535_pbe_state.h"
#include "sgx535_pixel_event.h"
#include "sgx535_engine121.h"

/* Private construction storage, disjoint from requests and both packet spans.
 * Native regeneration retains this in its existing per-owner heap workspace.
 * It is never packet payload, a new admission class or device-visible state. */
struct sgx535_task_packet_word_workspace {
    struct sgx535_task_request request;
    struct sgx535_vertex_use_projection vertex;
    struct sgx535_vertex_pds_program vpds;
    struct sgx535_pbe_state pbe;
    struct sgx535_pixel_event_program event;
    struct sgx535_engine121_scene scene;
    struct sgx535_pbe_surface surface;
    uint32_t fs[2], suse[12], spds[31], fpds[14], mte[23], ta[17], indices[6];
    uint32_t term[2], secondary[1];
};

enum sgx535_task_status sgx535_task_packet_build_words(const struct sgx535_task_request *,
                                                       struct sgx535_task_packet *,
                                                       struct sgx535_task_packet *,
                                                       struct sgx535_task_packet_word_workspace *);
enum sgx535_task_status sgx535_task_pair_mask_set_packet_build_words(
    const struct sgx535_task_pair_request *, struct sgx535_task_packet *,
    struct sgx535_task_packet *, struct sgx535_task_packet_word_workspace *);
#endif
