// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_LINKAGE_H
#define POULSBACK_SGX535_FRAGMENT_LINKAGE_H
#include "sgx535_fragment.h"

enum sgx535_fragment_semantic { SGX535_FRAGMENT_COLOR, SGX535_FRAGMENT_GENERIC };
enum sgx535_fragment_interpolation {
    SGX535_FRAGMENT_SMOOTH,
    SGX535_FRAGMENT_FLAT,
    SGX535_FRAGMENT_COLOR_INTERPOLATION /* Mesa COLOR follows raster flatshade. */
};
struct sgx535_fragment_varying {
    enum sgx535_fragment_semantic semantic;
    uint32_t index, driver_base, components;
    enum sgx535_fragment_interpolation interpolation;
};
struct sgx535_fragment_linkage {
    uint32_t count;
    /* Array index is the dense logical INPUT slot. COLOR0/1 precede GENERICn;
     * semantic and index together identify a varying, not index alone. */
    struct sgx535_fragment_varying varying[SGX535_FRAGMENT_INPUT_SLOTS];
};

/* Gallium constant buffer 0 is always bytes of unchanged DWORDs. Only the
 * addressing units of load_uniform differ with pipe_caps.packed_uniforms.
 * This choice is explicit; neither NIR values nor ranges identify the layout. */
enum sgx535_fragment_uniform_layout {
    SGX535_FRAGMENT_UNIFORM_DWORDS,
    SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS
};

enum sgx535_fragment_io_precision_policy { SGX535_FRAGMENT_IO_STRICT, SGX535_FRAGMENT_IO_F32 };
struct sgx535_fragment_nir_options {
    enum sgx535_fragment_uniform_layout uniform_layout;
    enum sgx535_fragment_io_precision_policy io_precision;
};

/* General immutable F32 transport options. STRICT retains the older adapters'
 * exact IO contracts. F32 accepts the GLSL mediump annotation on otherwise
 * supported 32-bit inputs/outputs without changing their bit size or source
 * metadata. It neither rounds to 16 bits nor admits packed high-half access,
 * unknown IO flags, additional operations or physical precision guarantees.
 * GLSL ES precision qualifiers specify minima; this CPU construction is not
 * qualification of the hardware's range, precision or rounding. Complete
 * source/SSA/IO validation and transactional output rules remain unchanged.
 * Options are copied before output publication; callers serialize their input.
 * Older entry points always retain STRICT. */
enum sgx535_nir_status sgx535_nir_fragment_options_ir(const struct nir_shader *,
                                                      const struct sgx535_fragment_nir_options *,
                                                      struct sgx535_fragment_instruction *,
                                                      size_t capacity, size_t *instruction_count,
                                                      struct sgx535_fragment_linkage *);

struct sgx535_fragment_dri_nir_program {
    struct sgx535_fragment_dri_color0_program program;
    struct sgx535_fragment_linkage source_linkage;
    enum sgx535_fragment_interpolation resolved_interpolation;
};
struct sgx535_fragment_dri_nir_ir {
    struct sgx535_fragment_instruction instruction[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t instruction_count;
    struct sgx535_fragment_linkage source_linkage;
    enum sgx535_fragment_interpolation resolved_interpolation;
};
/* Same validated normalization as project_dri_color0, exporting symbolic SSA
 * instead of native words for a separately checked backend. Original linkage
 * provenance remains intact; the IR uses dense slot0. The matched U8 producer
 * is validated before publication. This large output belongs off stack and
 * must be disjoint from the stable NIR graph. Errors preserve it completely;
 * unused instructions are zero. No GPU permission or address is exported. */
enum sgx535_nir_status sgx535_nir_dri_color0_ir(const struct nir_shader *,
                                                enum sgx535_fragment_uniform_layout,
                                                enum sgx535_fragment_interpolation color_mode,
                                                struct sgx535_fragment_dri_nir_ir *);
/* Actual immutable NIR -> matched DRI COLOR0 word construction. This entry
 * obtains IR and original-base linkage together from the same graph before
 * making a separate canonical base0 binding. source_linkage is never rewritten.
 * After full graph/IR validation, only this new bridge removes scalar nodes
 * unreachable from the four outputs, retaining order and remapping SSA edges.
 * Validated identity MOVs alias their earlier scalar source transitively; no
 * arithmetic or FP rewrite occurs. Even dead unsupported source operations
 * are refused before normalization. Old adapter streams remain unchanged.
 * Runtime color_mode must be SMOOTH or FLAT and agree with an explicit source
 * qualifier. COLOR_INTERPOLATION resolves to either from raster state. Original
 * source and resolved modes remain in the sidecar; only the compiler-local word
 * binding uses SMOOTH. FLAT requires bit-identical replicated attributes before
 * backend admission; this graph-only function does not verify geometry or
 * implement a native flat iterator. Unresolved or mismatched modes refuse.
 * Other old adapter/producer restrictions are unchanged. Errors preserve the
 * single output. The output must be disjoint from the stable NIR graph; this
 * is a caller precondition, not graph-wide pointer authentication. No CPU
 * fragment evaluation, native admission or packet dispatch is implemented. */
enum sgx535_nir_status sgx535_nir_project_dri_color0(const struct nir_shader *,
                                                     enum sgx535_fragment_uniform_layout,
                                                     enum sgx535_fragment_interpolation color_mode,
                                                     enum sgx535_fragment_output_profile,
                                                     struct sgx535_fragment_dri_nir_program *);
#endif
