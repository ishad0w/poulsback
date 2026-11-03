// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_LINEAR_H
#define POULSBACK_SGX535_LINEAR_H
#include "sgx535_compiler_compat.h"
#define SGX535_NIR_MAX_INSTRUCTIONS 512u
#define SGX535_NIR_MAX_WORDS (SGX535_NIR_MAX_INSTRUCTIONS * 2u)
#define SGX535_NIR_TEMP_COUNT 124u

enum sgx535_nir_status {
    SGX535_NIR_OK = 0,
    SGX535_NIR_ERR_ARGUMENT = -1,
    SGX535_NIR_ERR_UNSUPPORTED = -2,
    SGX535_NIR_ERR_LIMIT = -3,
    SGX535_NIR_ERR_SSA = -4,
    SGX535_NIR_ERR_PRESSURE = -5,
    SGX535_NIR_ERR_NOMEM = -6
};
struct sgx535_nir_program {
    uint32_t words[SGX535_NIR_MAX_WORDS];
    uint32_t word_count;
    uint32_t peak_live_temps;
    uint32_t temp_register_count;
};
struct sgx535_nir_f32_projection {
    struct sgx535_nir_program encoding;
    uint32_t arithmetic_qualified; /* Always zero. */
};

enum sgx535_linear_opcode {
    SGX535_LINEAR_CONSTANT,
    SGX535_LINEAR_MOV,
    SGX535_LINEAR_MAD,
    SGX535_LINEAR_MIN,
    SGX535_LINEAR_MAX,
    SGX535_LINEAR_RCP,
    SGX535_LINEAR_OUTPUT
};
enum sgx535_linear_profile { SGX535_LINEAR_INTEGER, SGX535_LINEAR_F32_WORD_PROJECTION };
struct sgx535_linear_instruction {
    enum sgx535_linear_opcode opcode;
    uint32_t immediate; /* CONSTANT only; otherwise zero. */
    uint32_t source[3]; /* Earlier instruction indices; unused fields zero. */
};
/* Scalar32, one linear block, final OUTPUT0, at most 512 instructions.
 * Source indices identify SSA definitions. OUTPUT is not a definition.
 * TEMP0..123, last-use allocation, no spilling. This is rawword projection,
 * not a floating evaluator or graphics API backend. No output on failure.
 */
enum sgx535_nir_status sgx535_linear_compile(const struct sgx535_linear_instruction *, size_t,
                                             enum sgx535_linear_profile,
                                             struct sgx535_nir_program *);

/* Optional actual Mesa26.2.4 adapters, not linked in the default CPU library.
 * Valid stable allocated NIR graphs are required. No lowering/input mutation.
 */
struct nir_shader;
enum sgx535_nir_status sgx535_nir_compile(const struct nir_shader *, struct sgx535_nir_program *);
enum sgx535_nir_status sgx535_nir_project_f32(const struct nir_shader *,
                                              struct sgx535_nir_f32_projection *);
#endif
