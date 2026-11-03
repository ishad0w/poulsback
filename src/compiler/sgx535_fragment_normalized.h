// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_NORMALIZED_H
#define POULSBACK_SGX535_FRAGMENT_NORMALIZED_H
#include "sgx535_fragment.h"
#include "sgx535_fragment_linkage.h"

/* Separate internal candidate, never SGX535_FRAGMENT_NORMALIZED_U8 admission. */
#define SGX535_NORMALIZED_FINITE_CLAMP_RTE 1u
#define SGX535_NORMALIZED_EXTRA_INSTRUCTIONS 10u
#define SGX535_NORMALIZED_MISSING_ARITHMETIC (1u << 0)
#define SGX535_NORMALIZED_MISSING_FINITE_RESULTS (1u << 1)
#define SGX535_NORMALIZED_MISSING_CLAMP (1u << 2)
#define SGX535_NORMALIZED_MISSING_SCALE (1u << 3)
#define SGX535_NORMALIZED_MISSING_ROUNDING (1u << 4)
#define SGX535_NORMALIZED_MISSING_MASKED_WRITE (1u << 5)
#define SGX535_NORMALIZED_MISSING_INPUT_LINKAGE (1u << 6)
#define SGX535_NORMALIZED_MISSING_PBE (1u << 7)
#define SGX535_NORMALIZED_MISSING_VISIBILITY (1u << 8)
#define SGX535_NORMALIZED_MISSING_RETIREMENT (1u << 9)
#define SGX535_NORMALIZED_MISSING_ALL 0x3ffu

struct sgx535_fragment_normalized_candidate {
    struct sgx535_nir_program encoding;
    uint32_t logical_components, primary_register_count, primary_component[4];
    uint32_t uniform_words_used[4], secondary_register_count;
    uint32_t source_instruction_count, lowered_instruction_count;
    uint32_t reference_policy, output_byte_lane[4], missing_contracts;
    uint32_t arithmetic_qualified, numerical_qualified, linkage_qualified;
    uint32_t output_qualified, hardware_qualified, nonfinite_behavior_qualified;
};

/* Same canonical DRI COLOR0 linkage and strict scalar SSA as the existing
 * fragment compiler. Validate the original stream before adding two constants
 * and MAX(value,+0), MIN(result,+1) for each final output, followed by SCALE=1
 * single-byte PACKs. No original operation, source edge or constant is changed.
 * All original nodes are validated, including dead ones; nonfinite constants
 * refuse. Arithmetic can still produce nonfinite values, so finite-result and
 * hardware qualifications stay unresolved. Trusted producers must preserve
 * the old NIR adapter's refusal of floating constraints absent from this IR.
 *
 * FINITE_CLAMP_RTE describes the independent CPU reference: clamp finite values
 * to [0,1], multiply the exact rational by 255, nearest-even integer rounding.
 * It is not an assertion about SGX MAX/MIN, SCALE, rounding, masked writes or
 * PBE delivery. Nonfinite hardware behavior is unknown. No rounded-mode bit is
 * invented. The old word projection and NORMALIZED_U8 refusal are unchanged.
 *
 * At most 502 original instructions fit the existing 512-instruction backend.
 * This compiler may allocate/sleep; errors preserve output, including when
 * stable fully allocated input/linkage/output storage overlaps. No CPU color
 * evaluation, native publication or packet 6/7 code-size relaxation occurs. */
enum sgx535_nir_status sgx535_fragment_compile_dri_color0_normalized_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_normalized_candidate *);

/* Same strict scalar grammar and unchanged clamp/PACK tail, without an INPUT
 * or iterator. Every original node is validated, including unused inputs,
 * which refuse. PRIMARY counts/components stay zero; statically indexed
 * uniforms retain the exact SECONDARY bitmap and highest-word extent under
 * the existing 128-DWORD bound. Constants still use LIMM; sampling and raw
 * mask extensions remain outside this entry. This is compiler metadata, not
 * an allocation for combined uniform/literal/sampler data, DMA or DMS setup.
 * The same atomic output/overlap, size and qualification rules above apply;
 * native admission remains refused. No COLOR0 entry changes its contract. */
enum sgx535_nir_status sgx535_fragment_compile_no_input_normalized_candidate(
    const struct sgx535_fragment_instruction *, size_t,
    struct sgx535_fragment_normalized_candidate *);

/* Separate CPU-only COLOR0 raw-mask policy. RAW_IEQ compares exact scalar32
 * words and produces 0/~0; dynamic RAW_AND/XOR keep their raw-word meaning.
 * A nonfinite CONSTANT is allowed only when every actual use is a raw IEQ,
 * AND or XOR operand, never an F32/MOV/OUTPUT edge; unused such words refuse.
 * Validate the whole original before any private liveness pruning. This joins
 * the existing raw-IEQ emitter to the unchanged clamp/PACK/linkage machinery,
 * not a GLSL/numerical capability. All missing/qualification fields and native
 * refusal retain their meaning. Stable fully allocated inputs are immutable
 * for the call; source/linkage/output may overlap and errors preserve output.
 * Old constructor policies and their output words are unchanged. */
enum sgx535_nir_status sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    struct sgx535_fragment_normalized_candidate *);

/* Explicit no-uniform one-diamond CPU transport policy. Original unsupported
 * operations and floating controls are checked before private clone lowering.
 * Old NIR entrypoints retain their grammar; these grant no GPU qualification. */
enum sgx535_nir_status sgx535_nir_dri_color0_raw_mask_normalized_v1_ir(
    const struct nir_shader *, enum sgx535_fragment_uniform_layout,
    enum sgx535_fragment_interpolation, struct sgx535_fragment_dri_nir_ir *);

/* Always refuses, including a forged missing mask or qualification fields. */
enum sgx535_nir_status
sgx535_fragment_normalized_native_admission(const struct sgx535_fragment_normalized_candidate *);
#endif
