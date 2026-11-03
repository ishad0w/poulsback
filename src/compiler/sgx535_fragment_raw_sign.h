// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_RAW_SIGN_H
#define POULSBACK_SGX535_FRAGMENT_RAW_SIGN_H
#include "sgx535_fragment_linkage.h"

/* Opt-in STATIC/CPU candidate: raw32 XOR/AND nodes supplement the existing
 * scalar IR. The ordinary compiler and NIR entrypoints still refuse those
 * nodes. Only RAW_F32_WORDS output is constructed; no PACK numeric profile.
 * FNEG reference toggles bit31, FABS reference clears bit31. Those raw-word
 * references preserve NaN payloads, Inf and zero magnitudes without any
 * floating rounding step. They do not prove the GPU raw-word/IEEE semantics.
 * All existing qualifier fields stay zero and native admission still refuses.
 * Existing validation, resource bounds and transactional output rules apply. */
enum sgx535_nir_status
sgx535_fragment_compile_raw_sign_candidate(const struct sgx535_fragment_instruction *, size_t,
                                           struct sgx535_fragment_program *);
enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_sign_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_dri_color0_program *);

/* Same actual Mesa graph validation, source linkage and uniform addressing
 * contract as sgx535_nir_fragment_options_ir. Only this entry selects raw sign lowering.
 * Floating-control/wrap flags, unsupported/dead operations and output overlap
 * still refuse. Requires a linkage sidecar; source graph is never changed. */
enum sgx535_nir_status sgx535_nir_fragment_raw_sign_ir(const struct nir_shader *,
                                                       enum sgx535_fragment_uniform_layout,
                                                       struct sgx535_fragment_instruction *,
                                                       size_t capacity, size_t *instruction_count,
                                                       struct sgx535_fragment_linkage *);
#endif
