// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_PROGRAM_H
#define POULSBACK_PROGRAM_H
#include "sgx535_fragment.h"
#include "sgx535_fragment_linkage.h"
#include "poulsback_render_ir.h"

/* Snapshot the canonical compiler's referenced constant-buffer-0 prefix.
 * Addressing has already been translated to DWORD indices in scalar IR.
 * Validate the exact bitmap/highest-word extent against the bound byte buffer;
 * preserve every bit (including NaN payloads and signed zero), without float
 * conversion. Output and word_count publish together; they must be separate
 * from each other and the stable compiler program. Buffer/output may overlap.
 * This does not populate or qualify physical SECONDARY registers. */
enum sgx535_nir_status poulsback_fragment_uniform_snapshot(const struct sgx535_fragment_program *,
                                                           const void *buffer, size_t buffer_bytes,
                                                           uint32_t *output, size_t capacity_words,
                                                           size_t *word_count);
#endif
