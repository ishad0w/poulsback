// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_RAW_SIGN_NORMALIZED_H
#define POULSBACK_SGX535_FRAGMENT_RAW_SIGN_NORMALIZED_H
#include "sgx535_fragment_raw_sign.h"
#include "sgx535_fragment_normalized.h"

/* Complete opt-in COLOR0 raw-sign -> clamp -> scaled PACK candidate. RAW_XOR
 * must use constant80000000 as its second source; RAW_AND must use constant
 * 7fffffff. A nonfinite constant is admitted only when every actual use is
 * that RAW_AND mask operand. It may not flow as a floating value, even via a
 * MOV, and an unused nonfinite literal still refuses. Explicit floating NaN
 * and Inf constants remain refused. Computed nonfinite values, actual bitwise
 * execution, MIN/MAX, SCALE/rounding, interpolation and delivery remain
 * unqualified. No old compiler/profile gains these scalar operations.
 *
 * The source stream and binding are copied before sleeping compilation. Errors
 * preserve output; stable allocated input/output ranges may overlap. Large
 * results belong off stack. All normalized missing/qualification fields and
 * the always-refusing native admission function retain their old meaning. */
enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_normalized_candidate *);

/* The old raw-sign NIR API still rejects every floating-control flag. Only
 * this additional entry accepts nir_fp_preserve_inf, and only on fneg/fabs,
 * lowered to the exact sign XOR/AND reference. Other floating controls,
 * execution modes, wrap flags and unsupported/dead instructions still refuse.
 * No floating constraint is changed in the immutable source graph. This is a
 * construction/reference policy, not a qualification of GPU numeric effects.
 * Existing source-layout, uniform and pairwise output-overlap checks apply. */
enum sgx535_nir_status
sgx535_nir_fragment_raw_sign_inf_ir(const struct nir_shader *, enum sgx535_fragment_uniform_layout,
                                    struct sgx535_fragment_instruction *, size_t capacity,
                                    size_t *instruction_count, struct sgx535_fragment_linkage *);

struct sgx535_fragment_dri_raw_sign_normalized_nir_program {
    struct sgx535_fragment_normalized_candidate program;
    struct sgx535_fragment_linkage source_linkage;
    enum sgx535_fragment_interpolation resolved_interpolation;
};
/* One complete real-NIR -> validated live SSA -> matched COLOR0 -> normalized
 * PACK composition. Original driver-base/interpolation provenance is retained;
 * only the private compiler binding is canonical base0/SMOOTH. FLAT still needs
 * replicated geometry at a separately checked backend. No geometry, uniform
 * upload, packet, native publication or CPU fragment evaluation is performed.
 * All unsupported/dead source instructions refuse before liveness pruning.
 * Errors preserve output, which must be disjoint from the stable NIR graph. */
enum sgx535_nir_status sgx535_nir_project_dri_color0_raw_sign_normalized_candidate(
    const struct nir_shader *, enum sgx535_fragment_uniform_layout,
    enum sgx535_fragment_interpolation color_mode,
    struct sgx535_fragment_dri_raw_sign_normalized_nir_program *);
/* Export the same checked live scalar SSA and original semantic sidecar for
 * a separately checked packet builder. The full original stream is validated
 * before pruning; preserve_inf is accepted only for the existing sign policy.
 * Transactional output must be disjoint from the entire stable NIR graph.
 * Unused instructions are zero. No native admission or uniform upload. */
enum sgx535_nir_status sgx535_nir_dri_color0_raw_sign_normalized_ir(
    const struct nir_shader *, enum sgx535_fragment_uniform_layout,
    enum sgx535_fragment_interpolation, struct sgx535_fragment_dri_nir_ir *);
#endif
