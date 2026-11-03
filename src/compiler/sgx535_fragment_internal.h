// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_INTERNAL_H
#define POULSBACK_SGX535_FRAGMENT_INTERNAL_H
#include "sgx535_fragment_linkage.h"
struct nir_block;
struct nir_alu_instr;
struct nir_instr;
#ifndef __KERNEL__
#include <stdbool.h>
#endif

/* Shared construction for the separate raw-sign compiler candidates. This
 * private profile selection grants neither numerical nor native admission. */
enum sgx535_nir_status sgx535_fragment_dri_raw_sign_profile(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    enum sgx535_fragment_output_profile, struct sgx535_fragment_dri_color0_program *);

/* Separate raw32 mask/equality profile, shared by the new normalized CPU
 * constructor. Old raw-sign and generic-F32 policies remain unchanged. */
enum sgx535_nir_status sgx535_fragment_dri_raw_mask_profile(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    enum sgx535_fragment_output_profile, struct sgx535_fragment_dri_color0_program *);

/* Logical COLOR0 remains unchanged; this selected physical link uses a complete
 * generic F32 coordinate0 with PA0/1/2/3, not primary packed color's BGR map. */
enum sgx535_nir_status sgx535_fragment_dri_generic_f32_profile(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    enum sgx535_fragment_output_profile, struct sgx535_fragment_dri_color0_program *);

/* Private shared SSA normalization, called only after complete source/IR
 * validation. No unsupported node is pruned into acceptance. */
enum sgx535_nir_status sgx535_fragment_dri_live_ir(struct sgx535_fragment_instruction *, size_t *,
                                                   bool *, uint32_t *, bool raw_sign);

/* Same private normalization after full raw-mask constructor validation;
 * RAW_IEQ gains arity2 only here. No unsupported original node is pruned.
 * Caller owns valid, disjoint IR/count/live/remap storage for the whole call. */
enum sgx535_nir_status sgx535_fragment_dri_raw_mask_live_ir(struct sgx535_fragment_instruction *,
                                                            size_t *, bool *, uint32_t *);

/* Reuse the actual strict one-block structural gate. The bool32 wrapper first
 * proves every original boolean cone, then lowers its own clone. */
const struct nir_block *sgx535_nir_fragment_linear_block(const struct nir_shader *);
enum sgx535_nir_status sgx535_nir_fragment_bool32_lowered_ir(const struct nir_shader *,
                                                             enum sgx535_fragment_uniform_layout,
                                                             struct sgx535_fragment_instruction *,
                                                             size_t, size_t *,
                                                             struct sgx535_fragment_linkage *);
enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_lowered_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *,
    bool int32_uniforms);
/* The checked caller validates its complete original graph before lowering
 * the private clone. Only this explicit extension permits fixed-count USHR
 * in that lowered clone; old Boolean adapters retain their refusal. */
enum sgx535_nir_status sgx535_nir_fragment_bool32_ieq_const_ushr_lowered_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *,
    bool int32_uniforms);
/* Only the checked pure-diamond caller may mark original Boolean/phi defs
 * opaque; ordinary instructions still use the canonical adapter contracts. */
enum sgx535_nir_status
sgx535_nir_fragment_pure_original_options_contract(const struct nir_instr *const *, const bool *,
                                                   size_t,
                                                   const struct sgx535_fragment_nir_options *);
/* Fixed comparison composition: validate original IGE, then lower only the
 * private clone before the existing Boolean passes. Not a generic extension. */
bool sgx535_nir_ige_const_contract(const struct nir_alu_instr *);
void sgx535_nir_lower_ige_const(struct nir_shader *);
enum sgx535_nir_status sgx535_nir_fragment_bool32_compare_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);
#endif
