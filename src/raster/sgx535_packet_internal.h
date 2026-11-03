// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_PACKET_INTERNAL_H
#define SGX535_PACKET_INTERNAL_H
#include "sgx535_varying_packet.h"
#include "sgx535_window_vertex.h"
/* Construction internals only. These entries validate the complete typed
 * window record before shared non-position assembly. Outputs/workspaces are
 * disjoint scratch owned by the enclosing public constructor, not unchecked
 * packets to patch or hardware/admission interfaces. */
enum sgx535_task_status sgx535_task_packet_window_seed(uint32_t, uint32_t,
                                                       const struct sgx535_window_color0_triangle *,
                                                       struct sgx535_task_packet *);
enum sgx535_task_status sgx535_varying_window_seed(uint32_t, uint32_t,
                                                   const struct sgx535_window_color0_triangle *,
                                                   struct sgx535_task_packet *,
                                                   struct sgx535_varying_workspace *);
/* Fixed16 clip-W1 seed for the separate normalized COLOR0 task. Ordinary
 * task/varying frontdoors continue to construct their fixed64 profile. */
enum sgx535_task_status sgx535_task_packet_clip16_seed(uint32_t, uint32_t, const uint32_t[3][4],
                                                       struct sgx535_task_packet *);
enum sgx535_task_status sgx535_varying_clip16_seed(const struct sgx535_varying_request *,
                                                   struct sgx535_task_packet *,
                                                   struct sgx535_varying_workspace *);
#endif
