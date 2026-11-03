// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_COMPARE_H
#define POULSBACK_SGX535_FRAGMENT_COMPARE_H
#include "sgx535_fragment_bool32.h"

/* The existing Boolean/select grammar plus original signed IGE with at least
 * one direct constant32 operand. On a private clone, sign-normalized bit runs
 * lower exactly to the existing raw masks/equality grammar. No floating
 * arithmetic, new instruction, wire profile or native admission is selected.
 * Constant-offset float32/int32 uniforms retain their original metadata and
 * load unchanged SECONDARY words using the explicit existing address layout.
 * No numeric conversion, unsigned/bool/narrow load or physical DMA is admitted.
 * Older entries retain float-only uniform loads.
 * Dynamic IGE, other comparisons, malformed shapes/controls and all existing
 * unsupported original operations refuse, including dead ones. Existing
 * options, scalar/program budgets and transactional output rules still apply.
 * Source is valid Mesa NIR and remains unchanged. */
enum sgx535_nir_status sgx535_nir_fragment_compare_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);

/* Opt-in extension of that grammar: ordered IEEE32 FEQ and raw integer INE
 * complement the existing checked FNEU/IEQ consumers on the private clone.
 * NaNs compare unequal, both signed zeros compare equal; no floating
 * subtraction or native comparison is introduced. The pure-diamond entry
 * shares this validated equality grammar. Older entries keep their refusals.
 * Also accepts ordered F32 FLT with at least one direct
 * constant32 operand. Checked raw IEEE32 sign/exponent/mantissa words lower on
 * a private clone through the existing constant IGE, masks and equality.
 * NaNs are false, both signed zeros compare equal, and subnormals/infinities
 * retain their source ordering. Preserve-inf/NaN controls are accepted; other
 * floating controls remain refused. This is a CPU construction contract, not
 * a native floating TEST, precision, input visibility or capability proof.
 * Dynamic FLT, malformed/dead unsupported originals and existing limits still
 * refuse transactionally. Older entries keep their FLT refusal unchanged. */
enum sgx535_nir_status sgx535_nir_fragment_flt_const_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);
#endif
