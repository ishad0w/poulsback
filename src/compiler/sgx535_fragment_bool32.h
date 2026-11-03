// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_BOOL32_H
#define POULSBACK_SGX535_FRAGMENT_BOOL32_H
#include "sgx535_fragment_linkage.h"

/* Opt-in CPU construction only. On a private clone, use Mesa's real bool1 to
 * bool32 pass (false=0, true=~0u), then reuse full-word TEMP AND. Original
 * iand/mov/vector boolean cones and b2b1/b2b32/b2f32/b2i32 are checked first;
 * a 32-bit boolean input must be proved canonical, never merely nonzero.
 * Comparisons, selects, phi, control flow and unknown boolean producers refuse,
 * including dead ones. All old APIs remain unchanged. The source must be valid
 * Mesa NIR and outputs must be separate stable allocated objects as in the
 * existing adapter contract. Errors preserve every output and source graph.
 * No native, numerical, output, linkage or public-screen admission is granted.
 */
enum sgx535_nir_status sgx535_nir_fragment_bool32_iand_ir(const struct nir_shader *,
                                                          enum sgx535_fragment_uniform_layout,
                                                          struct sgx535_fragment_instruction *,
                                                          size_t, size_t *,
                                                          struct sgx535_fragment_linkage *);

/* Separate CPU-only extension: the same whole-original canonical cone checks
 * also allow inot, lowered to full-word TEMP XOR with UINT32_MAX. The iand
 * entry above retains its refusal of inot. Comparisons, TEST result masks,
 * selects, CFG and all native/capability admission remain outside this API. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_inot_ir(const struct nir_shader *,
                                                          enum sgx535_fragment_uniform_layout,
                                                          struct sgx535_fragment_instruction *,
                                                          size_t, size_t *,
                                                          struct sgx535_fragment_linkage *);

/* Separate CPU-only extension: original IEQ is bool1 over two raw32 integer
 * sources. Whole-original validation still refuses other comparisons, CFG,
 * texture/kill, derivative/helper consumers and unknown Boolean producers,
 * even dead ones. A private clone uses real Mesa bool-to-int32 lowering;
 * RAW_IEQ then means canonical 0/~0, with no caller predicate controls.
 * Old iand/inot APIs retain their IEQ refusal. Compile only with the explicit
 * raw_ieq candidate; no wire ABI, physical or capability admission follows. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_ir(const struct nir_shader *,
                                                         enum sgx535_fragment_uniform_layout,
                                                         struct sgx535_fragment_instruction *,
                                                         size_t, size_t *,
                                                         struct sgx535_fragment_linkage *);

/* Separate CPU-only superset of the IEQ profile: additionally admit IOR only
 * over the whole-original canonical bool1/0-or-~0 cone. A private clone lowers
 * IOR to ~(~a & ~b), preserving original vector source swizzles, before the
 * same real Mesa bool-to-int32 pass and checked IEQ/AND/XOR adapter. No new IR
 * opcode or encoder is introduced. The old entrypoints still refuse IOR.
 * Unknown producers, noncanonical masks, controls, CFG and helper/texture
 * consumers remain refused even when dead. Errors preserve all outputs and
 * the original shader. No floating, wire, physical or capability admission. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_ior_ir(const struct nir_shader *,
                                                         enum sgx535_fragment_uniform_layout,
                                                         struct sgx535_fragment_instruction *,
                                                         size_t, size_t *,
                                                         struct sgx535_fragment_linkage *);

/* CPU-only superset: original bool1 FNEU compares two raw IEEE32 words.
 * Resolve original swizzles, then classify signed zero and both NaN classes
 * with integer masks/IEQ/Boolean operations on the checked private clone.
 * NaNs compare unequal, including equal payloads; +/-zero compare equal.
 * Only preserve-inf/preserve-NaN control bits are supported. Other FP controls,
 * shader execution modes, original lowered fneu32 and non-32-bit operands
 * refuse, even when dead. Existing limits (512 IR,1024 words,124 TEMP),
 * transactional output rules and old entrypoint refusals remain unchanged.
 * This uses the raw IEQ compiler; no native FP instruction, renderer/capability
 * qualification, public wire ABI or physical admission follows. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_fneu_ir(const struct nir_shader *,
                                                          enum sgx535_fragment_uniform_layout,
                                                          struct sgx535_fragment_instruction *,
                                                          size_t, size_t *,
                                                          struct sgx535_fragment_linkage *);

/* CPU-only superset: original BCSEL has a validated canonical bool1 condition
 * and two raw32 payloads. All original source swizzles are resolved before
 * widening that condition to 0/~0; (mask & left) | (~mask & right) preserves
 * every payload bit, including NaNs, signed zero and integer representations.
 * Canonical-result provenance requires both original branches canonical,
 * including the unselected branch. Original b32csel, non-bool1 conditions,
 * non-32-bit payloads/results, controls, CFG and unknown producers refuse.
 * Limits/output transactionality remain those of the existing raw IEQ path.
 * All old entrypoints retain BCSEL refusal. No new IR/native opcode, floating
 * operation, public ABI, physical execution or capability admission follows. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_bcsel_ir(const struct nir_shader *,
                                                           enum sgx535_fragment_uniform_layout,
                                                           struct sgx535_fragment_instruction *,
                                                           size_t, size_t *,
                                                           struct sgx535_fragment_linkage *);

/* The complete existing BCSEL/compare/Boolean grammar with the same explicit
 * uniform units and IO precision policy as the ordinary fragment adapter.
 * F32 accepts retained mediump annotations on actual 32-bit IO only; it does
 * not relax width, access metadata, original-graph, FP or canonicality checks.
 * All per-feature entries above remain STRICT. No new opcode, wire profile,
 * native execution or numerical qualification follows from this CPU entry. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);
/* Explicit CPU-only extension of the checked linear compare grammar: F32
 * floor preserves signed zeros, infinities and NaN words, truncates fractional
 * mantissa bits with five checked constant logical shifts, then subtracts one
 * only for negative nonintegral finite values. Whole-original source checks,
 * fixed IR/word/TEMP budgets and transactional outputs still apply. Original
 * shifts, CFG and dead unsupported operations refuse. Older entries retain
 * floor refusal. This is no native arithmetic or capability qualification. */
enum sgx535_nir_status sgx535_nir_fragment_floor_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);

/* Outside-only CPU candidate: one pure if/else diamond with checked32-bit
 * phis. The complete original graph, canonical Boolean provenance and bounded
 * uniform metadata are checked before a single clone and Mesa's finite
 * peephole-select pass. Nested CFG/effects/unknown/dead unsupported source and
 * dynamic ordered comparisons still refuse. Existing entries keep CFG refusal;
 * this declares no screen/caps/native/wire or numerical admission. */
enum sgx535_nir_status sgx535_nir_fragment_pure_diamond_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);
#endif
