// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_GENERIC_F32_H
#define POULSBACK_SGX535_FRAGMENT_GENERIC_F32_H
#include "sgx535_fragment_normalized.h"

/* A separate physical-linkage candidate for unchanged logical COLOR0 IR.
 * The matched generic F32 coordinate0 delivers RGBA through PA0/1/2/3.
 * All four PAs remain allocated even if only some components are read.
 * Whole SSA validation, finite/raw-sign mask policy, final clamp/PACK,
 * source dataflow, uniform ledger and transactional output remain unchanged.
 * This is not the unrelated generic-prefix iterator ABI. The packet must
 * construct matching dense vertex output, OUTSELECTS, TEXSIZE and PDS issue.
 * Internal storage/interpolation, TEXFLOAT/default precision, general W,
 * PACK/numerics and native admission remain unqualified. */
enum sgx535_nir_status sgx535_fragment_compile_generic_f32_normalized_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_normalized_candidate *);

struct sgx535_fragment_generic_f32_secondary_candidate {
    struct sgx535_fragment_normalized_candidate program;
    uint32_t logical_uniform_words_used[4], logical_uniform_extent;
    uint32_t physical_sa_words, reserved_sa_words, uniform_sa_words;
    uint32_t literal_sa_words, sampler_state_sa_words;
};

/* Separate ordinary-SECONDARY construction: logical uniform i reads physical
 * SA(i+1), with SA0 reserved only for a nonempty uniform prefix. The complete
 * physical extent is bounded by128 DWORDs, so logical index127 refuses even
 * when dead. The source grammar and generic-F32 PA mapping stay unchanged.
 * Program bitmap/count describe physical SA; the separate logical ledger
 * retains caller uniform indices and padding. LIMM literals, including the
 * clamp tail, consume no SA. Sampling, state and memory constants remain
 * unsupported; no additional occupancy can be hidden in this ledger.
 *
 * Whole original validation precedes the private index mapping. Stable fully
 * allocated inputs/linkage/output may overlap; errors preserve output. All
 * missing/qualification fields remain unchanged. This does not provide DMA,
 * PDS/task setup, cache visibility, an upload or native admission. The old
 * generic-F32 compiler keeps its original logical-SA contract and words. */
enum sgx535_nir_status sgx535_fragment_compile_generic_f32_secondary_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_generic_f32_secondary_candidate *);
#endif
