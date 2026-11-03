// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_FRAGMENT_H
#define POULSBACK_SGX535_FRAGMENT_H
#include "sgx535_iterator.h"
#include "sgx535_linear.h"

#define SGX535_FRAGMENT_INPUT_SLOTS 10u
#define SGX535_FRAGMENT_UNIFORM_WORDS 128u

enum sgx535_fragment_opcode {
    SGX535_FRAGMENT_CONSTANT,
    SGX535_FRAGMENT_INPUT,
    SGX535_FRAGMENT_UNIFORM,
    SGX535_FRAGMENT_MOV,
    SGX535_FRAGMENT_MAD,
    SGX535_FRAGMENT_MIN,
    SGX535_FRAGMENT_MAX,
    SGX535_FRAGMENT_RCP,
    SGX535_FRAGMENT_OUTPUT,
    SGX535_FRAGMENT_RSQ,     /* Appended: old numeric values remain unchanged. */
    SGX535_FRAGMENT_RAW_XOR, /* Separate opt-in raw-sign candidate only. */
    SGX535_FRAGMENT_RAW_AND,
    SGX535_FRAGMENT_RAW_IEQ,           /* Opt-in scalar raw32 -> canonical 0/~0 only. */
    SGX535_FRAGMENT_FRACT,             /* Unary source duplicated into the FRC word profile. */
    SGX535_FRAGMENT_RAW_USHR_IMM,      /* Raw-mask candidate: source0 >> fixed immediate. */
    SGX535_FRAGMENT_SAMPLE2D_COMPONENT /* IR2-only; ordinary compilers refuse. */
};
enum sgx535_fragment_output_profile {
    SGX535_FRAGMENT_RAW_F32_WORDS,
    SGX535_FRAGMENT_NORMALIZED_U8, /* Refused: numerical conversion unqualified. */
    SGX535_FRAGMENT_U8_WORD_PROJECTION
};
struct sgx535_fragment_instruction {
    enum sgx535_fragment_opcode opcode;
    uint32_t immediate; /* CONSTANT payload or RAW_USHR_IMM bit count. */
    uint32_t index;     /* INPUT slot, UNIFORM word, OUTPUT component. */
    uint32_t component; /* INPUT component; otherwise zero. */
    uint32_t source[3]; /* Earlier scalar definitions; unused fields zero. */
};

#define SGX535_FRAGMENT_SAMPLED_SLOTS 32u
#define SGX535_FRAGMENT_SAMPLED_BGRA8_U8 1u
struct nir_shader;
struct sgx535_fragment_sampled_input {
    uint32_t texture_slot, sampler_slot, coordinate_slot, coordinate_components;
    uint32_t interpolation, result_format, result_dwords, reserved;
};
/* One nondependent texture result. SAMPLE2D_COMPONENT has declaration index0,
 * component0..3 and identical earlier direct GENERIC F32 INPUT sources0/1.
 * The declaration's interpolation is SMOOTH (0), slot means original GENERIC
 * driver_location0..9, and all four result components occur exactly once.
 * This validates copied symbolic IR only; it creates no code or image binding. */
enum sgx535_nir_status
sgx535_fragment_sampled_validate(const struct sgx535_fragment_instruction *, size_t,
                                 const struct sgx535_fragment_sampled_input *);
/* Actual normalized owned NIR, same one-block IO rules as the existing adapter.
 * Outputs are disjoint and transactional. Ordinary fragment adapters retain
 * texture refusal; this entry creates no native shader or resource permission. */
enum sgx535_nir_status sgx535_nir_fragment_sampled_ir(const struct nir_shader *,
                                                      struct sgx535_fragment_instruction *, size_t,
                                                      size_t *,
                                                      struct sgx535_fragment_sampled_input *);
struct sgx535_fragment_input {
    uint32_t slot, components, primary_base;
    struct sgx535_iterator_words iterator;
};
struct sgx535_fragment_program {
    struct sgx535_nir_program encoding;
    struct sgx535_fragment_input input[SGX535_FRAGMENT_INPUT_SLOTS];
    uint32_t input_count, primary_register_count;
    uint32_t uniform_words_used[4];    /* Exact SECONDARY word indices. */
    uint32_t secondary_register_count; /* Highest referenced word + one. */
    enum sgx535_fragment_output_profile output_profile;
    /* Static word construction does not qualify arithmetic, PDS population,
     * uniform upload/cache, raster output delivery or any GPU execution. */
    uint32_t arithmetic_qualified, linkage_qualified, output_qualified;
};

/* One scalar SSA stream, final four OUTPUT nodes in component order 0..3.
 * INPUT slots 0..9 have F32 prefix components 1..4, sorted by logical slot;
 * each gets consecutive PRIMARY words, matching the bounded static producer.
 * UNIFORM indices address 32-bit SECONDARY words. No spilling, CPU evaluation,
 * texture sampling, qualified numeric packing or native publication is implemented.
 * The explicit U8 word projection selects four producer-compatible single-byte
 * PACKs into OUTPUT0 (logical RGBA -> physical bytes 2,1,0,3), scale one.
 * Its numeric conversion and PBE/raster configuration remain unqualified.
 * All errors preserve output; stable allocated input/output may overlap. */
enum sgx535_nir_status sgx535_fragment_compile(const struct sgx535_fragment_instruction *, size_t,
                                               enum sgx535_fragment_output_profile,
                                               struct sgx535_fragment_program *);

struct sgx535_texture2d_primary_plan;
/* Conditional CPU-only sampled body: the caller owns one completed packed PA0
 * and the exact four checked UNPACKs. Their TEMP destinations are precolored
 * before any SSA allocation, including SAMPLE definitions encountered later.
 * PDS-only coordinate loads do not become USE F32 PRIMARY inputs. Reuses the
 * ordinary allocator/ALU/U8 OUTPUT projection and keeps all qualification0.
 * No sample-arrival/DRC wait, task-control authority, image or native admission
 * is supplied. Stable allocated inputs/output may overlap; errors preserve it. */
enum sgx535_nir_status
sgx535_fragment_compile_sampled_candidate(const struct sgx535_fragment_instruction *, size_t,
                                          const struct sgx535_fragment_sampled_input *,
                                          const struct sgx535_texture2d_primary_plan *,
                                          struct sgx535_fragment_program *);

/* Separate CPU-only raw integer equality candidate. RAW_IEQ expands to an
 * internal TEST/P1/immediate-write interval; no caller predicate/word fields.
 * Entire stream is straight-line with no kill/helper/gradient/texture/CFG
 * operations. The same explicit raw-mask candidate accepts RAW_USHR_IMM with
 * immediate1/2/4/8/16/31, one raw32 source and all unused fields zero. It emits
 * two DWORDs with no predicate or END controls. Old ordinary/raw-sign
 * compilers retain refusal of both operations. Total physical words
 * remain bounded by the existing program array, independently of IR count.
 * Source/outputs are transactional; no numerical/native qualification. */
enum sgx535_nir_status
sgx535_fragment_compile_raw_ieq_candidate(const struct sgx535_fragment_instruction *, size_t,
                                          struct sgx535_fragment_program *);

/* Always rejects this unqualified transport profile, including a forged
 * qualification flag. The compiler cannot grant hardware publication. */
enum sgx535_nir_status sgx535_fragment_native_admission(const struct sgx535_fragment_program *);

struct sgx535_fragment_linkage;
/* Separate matched DRI COLOR0 result: no generic-prefix iterator descriptor
 * is valid for this producer. Logical RGBA components read PRIMARY2/1/0/3;
 * the producer allocates all four words even if only a prefix is loaded.
 * This describes word construction, not GPU-populated values or PACK effects. */
struct sgx535_fragment_dri_color0_program {
    struct sgx535_nir_program encoding;
    uint32_t logical_components, primary_register_count;
    uint32_t primary_component[4];
    uint32_t uniform_words_used[4], secondary_register_count;
    enum sgx535_fragment_output_profile output_profile;
    uint32_t arithmetic_qualified, linkage_qualified, output_qualified;
};
/* Exactly one smooth COLOR0 vec4 binding with original driver_base zero,
 * dense IR slot0 and unused linkage entries zero. A nonzero original NIR
 * base remains refused here, even though the ordinary linked adapter retains
 * that base after densifying IR; this API does not normalize its provenance. All used IR INPUTs must address slot0; no caller register/bank/address
 * controls are accepted. SSA validation/allocation/emission are shared with
 * the old generic compiler, whose ABI and output words remain unchanged.
 * Unsupported linkage/profile, malformed IR and allocation errors preserve
 * output. Stable, fully allocated input/linkage/output storage may overlap. */
enum sgx535_nir_status sgx535_fragment_compile_dri_color0(
    const struct sgx535_fragment_instruction *, size_t, const struct sgx535_fragment_linkage *,
    enum sgx535_fragment_output_profile, struct sgx535_fragment_dri_color0_program *);
/* Always refuses, including forged qualification fields. */
enum sgx535_nir_status
sgx535_fragment_dri_color0_native_admission(const struct sgx535_fragment_dri_color0_program *);

/* Actual Mesa26.2.4, optional external dependency. Normalized single-block
 * fragment NIR: no variables, scalar/vec2..4 F32, generic input slots,
 * smooth pixel barycentrics, static uniform WORD offsets, final DATA0/COLOR vec4.
 * Caller owns lowering; this function never changes the input graph. */
enum sgx535_nir_status sgx535_nir_project_fragment(const struct nir_shader *,
                                                   enum sgx535_fragment_output_profile,
                                                   struct sgx535_fragment_program *);

/* The same validated normalized NIR projection, retaining its scalar IR for
 * explicit userspace wire translation. No native-word reverse decoding.
 * Capacity is bounded by SGX535_NIR_MAX_INSTRUCTIONS. Both outputs are
 * transactional and must be separate from each other and the stable NIR graph.
 * RSQ can appear here; a consumer must refuse it when its wire ABI lacks it.
 * This does not supply native execution or numerical qualification. */
enum sgx535_nir_status sgx535_nir_fragment_ir(const struct nir_shader *,
                                              struct sgx535_fragment_instruction *, size_t capacity,
                                              size_t *instruction_count);

struct sgx535_fragment_nir_options;
/* Explicit CPU-only constant USHR projection. Each original count is an
 * actual32-bit load_const, resolved through its source swizzle, in the same
 * fixed count subset as RAW_USHR_IMM. Dynamic/other counts and signed shifts
 * refuse, including dead unsupported instructions. No Boolean grammar is
 * widened; the original ordinary entrypoints continue to refuse USHR.
 * Existing limits, immutable source and transactional output rules apply. */
enum sgx535_nir_status sgx535_nir_fragment_const_ushr_options_ir(
    const struct nir_shader *, const struct sgx535_fragment_nir_options *,
    struct sgx535_fragment_instruction *, size_t, size_t *, struct sgx535_fragment_linkage *);
#endif
