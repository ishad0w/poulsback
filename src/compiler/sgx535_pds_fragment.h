// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_PDS_FRAGMENT_H
#define POULSBACK_SGX535_PDS_FRAGMENT_H
#include "sgx535_compiler_compat.h"
#include "sgx535_iterator.h"
#include "sgx535_use_code_bank.h"

#define SGX535_PDS_FRAGMENT_MAX_ITERATORS 10U
#define SGX535_PDS_FRAGMENT_MAX_WORDS 28U
enum sgx535_pds_fragment_status {
    SGX535_PDS_FRAGMENT_OK = 0,
    SGX535_PDS_FRAGMENT_ERR_ARGUMENT = -1,
    SGX535_PDS_FRAGMENT_ERR_RANGE = -2,
    SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED = -3
};
struct sgx535_pds_fragment {
    /* Opaque CPU words. Only a separately checked trusted task producer may
     * form executable codebase/attribute controls; this is no launch UAPI. */
    uint32_t task_control[3];
    uint32_t iterator_count;
    struct sgx535_iterator_f32 iterator[SGX535_PDS_FRAGMENT_MAX_ITERATORS];
};
struct sgx535_pds_fragment_program {
    uint32_t iterator_count, data_words, total_words;
    uint32_t words[SGX535_PDS_FRAGMENT_MAX_WORDS];
};
struct sgx535_texture2d_nearest;
struct sgx535_texture2d_cpu_binding;
#define SGX535_PDS_FRAGMENT_SAMPLE_WORDS 16U
struct sgx535_pds_fragment_sample {
    uint32_t task_control[3], coordinate_slot;
};
struct sgx535_pds_fragment_sample_task {
    struct sgx535_use_code_bank_input code;
    uint32_t word_count, temp_register_count, phase_count;
    uint32_t coordinate_slot, texture_issue_count, use_issue_count;
    uint32_t last_texture_issue; /* Zero-based index; the sole terminal issue is 0. */
};
/* One-phase CPU task projection for the checked scalar sampled body. Code
 * describes the whole allocation; the complete body must fit from entry.
 * Exactly one terminal texture issue and no separate USE iterator are admitted.
 * TEMP extent4..124 matches the four precolored UNPACKs and scalar allocator.
 * Code-bank projection does not authenticate allocation ownership, mappings,
 * upload or execution. Output is transactional; stable objects may overlap. */
enum sgx535_pds_fragment_status
sgx535_pds_fragment_sample_task_project(const struct sgx535_pds_fragment_sample_task *,
                                        struct sgx535_pds_fragment_sample *);
struct sgx535_pds_fragment_sample_program {
    uint32_t primary_dwords, data_words, total_words;
    uint32_t words[SGX535_PDS_FRAGMENT_SAMPLE_WORDS];
};
/* One nonprojected smooth-F32 GENERIC vec2 coordinate and one packed sample
 * result at PA0. There is no separate USE-coordinate record or issue merging.
 * The producer owns the task dependency fields and the last-texture marker;
 * task entry/allocation controls otherwise remain trusted opaque CPU input.
 * The tuple/address input is CPU-only and grants no native admission.
 * Stable input/output may overlap; failure preserves every output byte. */
enum sgx535_pds_fragment_status sgx535_pds_fragment_sample_encode(
    const struct sgx535_pds_fragment_sample *, const struct sgx535_texture2d_nearest *,
    const struct sgx535_texture2d_cpu_binding *, struct sgx535_pds_fragment_sample_program *);
/* Canonical no-texture F32 producer profile. Slots are distinct, in caller
 * emission order. Exactly the last descriptor has last=1; it additionally
 * carries the producer's dependency bit. Unused descriptors/words are zero.
 * No interpolation, task safety, texture, cache or execution claim is made.
 * Failure preserves output; valid stable input/output objects may overlap. */
enum sgx535_pds_fragment_status sgx535_pds_fragment_encode(const struct sgx535_pds_fragment *,
                                                           struct sgx535_pds_fragment_program *);
enum sgx535_pds_fragment_status
sgx535_pds_fragment_decode(const struct sgx535_pds_fragment_program *,
                           struct sgx535_pds_fragment *);
#endif
