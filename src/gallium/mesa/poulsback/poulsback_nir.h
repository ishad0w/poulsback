// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_NIR_H
#define POULSBACK_NIR_H
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_normalized.h"
struct pipe_screen;
struct nir_shader;
/* Internal compiler configuration only, independent of API/render caps and
 * screen publication. VS follows Mesa's CPU draw/NIR-to-TGSI path; FS keeps
 * intrinsic IO for the strict fragment adapter. Options have static lifetime. */
void poulsback_nir_screen_init(struct pipe_screen *);
/* Standard structural passes on transferred, driver-owned valid FS NIR.
 * No floating arithmetic optimization, floating-constraint change, guessed uniform
 * layout or unrelated unsupported-instruction pruning. Idempotent with finalize_nir.
 * Constant scalar32 iadd/imul/ishl trees used as load_uniform offsets are
 * folded only under a checked non-wrapping address model. Only now-unused
 * nodes in that proven tree are removed; shared and unrelated uses remain.
 * The strict adapter still decides whether the complete result is supported. */
void poulsback_nir_finalize_fragment(struct nir_shader *);
/* Own a per-draw clone with statically addressed FS CB0 loads replaced by
 * their exact 32-bit words. The complete original is validated first; no FP
 * evaluation, optimization, layout guessing or mutation of the bound shader.
 * Byte count must be DWORD-aligned and every referenced word must be present,
 * including dead loads. Caller serializes shader/constant ownership, owns the
 * returned ralloc shader, and must revalidate its chosen native profile.
 * Errors leave *out unchanged. This supplies no uniform upload/native caps. */
int poulsback_nir_specialize_fragment(const struct nir_shader *,
                                      enum sgx535_fragment_uniform_layout, const void *, size_t,
                                      struct nir_shader **out);

struct poulsback_nir_solid_fragment {
    struct sgx535_fragment_instruction instruction[SGX535_NIR_MAX_INSTRUCTIONS];
    struct sgx535_fragment_normalized_candidate program;
    uint32_t instruction_count;
};
/* Compile an already specialized zero-input/no-uniform graph. All original
 * nodes are validated; no unused unsupported operation is pruned. The result
 * owns its scalar IR and existing normalized construction, with qualification
 * fields false. Errors preserve output; no driver capability is selected. */
enum sgx535_nir_status poulsback_nir_solid_fragment_ir(const struct nir_shader *,
                                                       enum sgx535_fragment_uniform_layout,
                                                       struct poulsback_nir_solid_fragment *);
/* Checked zero-input source before per-draw capture. MAD graphs require a
 * uniform load. Direct uniforms retain their checked logical DWORD extent;
 * this immutable CSO validation supplies no values or upload authority. */
enum sgx535_nir_status
poulsback_nir_solid_fragment_source_ir(const struct nir_shader *,
                                       enum sgx535_fragment_uniform_layout,
                                       struct poulsback_nir_solid_fragment *);
#endif
