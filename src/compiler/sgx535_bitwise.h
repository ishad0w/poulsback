// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_BITWISE_H
#define POULSBACK_SGX535_BITWISE_H
#include "sgx535_compiler_compat.h"

/* Static full-word, scalar TEMP-only profile. No immediate, rotation,
 * inversion, partial write, predicate, scheduling or MOE controls. */
enum sgx535_bitwise_opcode { SGX535_BITWISE_AND, SGX535_BITWISE_XOR };
enum sgx535_bitwise_status {
    SGX535_BITWISE_OK = 0,
    SGX535_BITWISE_ERR_ARGUMENT = -1,
    SGX535_BITWISE_ERR_RANGE = -2,
    SGX535_BITWISE_ERR_UNSUPPORTED = -3
};
struct sgx535_bitwise {
    enum sgx535_bitwise_opcode opcode;
    uint32_t destination_temp;
    uint32_t source_temp[2];
    uint32_t end;
};
/* TEMP0..123, repeat1, skip-invalid, no source modifiers. Errors preserve
 * output. Stable allocated inputs/outputs may overlap. Encoding does not
 * qualify execution, raw-word delivery or floating API semantics. */
enum sgx535_bitwise_status sgx535_bitwise_encode(const struct sgx535_bitwise *, uint32_t[2]);
enum sgx535_bitwise_status sgx535_bitwise_decode(const uint32_t[2], struct sgx535_bitwise *);

struct sgx535_const_ushr {
    uint32_t destination_temp, source_temp, bit_count;
};
/* Separate full raw32 logical shift with fixed immediate1/2/4/8/16/31.
 * Direct TEMP0..123, repeat1 and SKIPINV; no END, predicate, source modifier,
 * rotation, inversion, partial write, scheduling or index controls. Caller
 * establishes identity MOE increments and disabled format controls. One
 * instruction is two DWORDs, word0 first; static construction does not
 * establish execution or native arithmetic. Other counts remain unsupported.
 * Errors preserve outputs; valid stable allocations may overlap. The old
 * AND/XOR profile and restricted integer SHR31 interface are unchanged. */
enum sgx535_bitwise_status sgx535_const_ushr_encode(const struct sgx535_const_ushr *, uint32_t[2]);
enum sgx535_bitwise_status sgx535_const_ushr_decode(const uint32_t[2], struct sgx535_const_ushr *);

enum sgx535_raw_ieq_status {
    SGX535_RAW_IEQ_OK = 0,
    SGX535_RAW_IEQ_ERR_ARGUMENT = -1,
    SGX535_RAW_IEQ_ERR_RANGE = -2
};
struct sgx535_raw_ieq {
    uint32_t destination_temp;
    uint32_t source_temp[2];
};
/* Complete scalar raw32 equality construction, producing canonical 0/~0.
 * TEMP0..123; fixed repeat1 and SKIPINV; no caller predicate or header fields.
 * P1 is scratch from the unconditional TEST through both immediate writes.
 * The whole program must exclude other predicate uses, kill, CFG, derivatives
 * and helper-lane consumers. This CPU construction grants no native admission.
 * TEST consumes both inputs before destination writes, permitting aliases.
 * Three instructions, no END; all errors preserve output. */
enum sgx535_raw_ieq_status sgx535_raw_ieq_encode(const struct sgx535_raw_ieq *, uint32_t[6]);

enum sgx535_f32_less_candidate_status {
    SGX535_F32_LESS_CANDIDATE_OK = 0,
    SGX535_F32_LESS_CANDIDATE_ERR_ARGUMENT = -1,
    SGX535_F32_LESS_CANDIDATE_ERR_RANGE = -2
};
struct sgx535_f32_less_candidate {
    uint32_t destination_temp;
    uint32_t source_temp[2];
};
/* Source-bound F32 IFSUB TEST/LESS word construction, then the same fixed
 * P1 -> zero/all-ones interval as raw IEQ. TEMP0..123; no source modifiers,
 * caller predicate/format controls or END. All errors preserve output and
 * stable input/output allocations may overlap. Three instructions consume
 * both inputs before either result write. The whole-program P1 and active-
 * lane restrictions above apply. This is encoding only: native NaN, signed
 * zero, subnormal and difference-rounding/FTZ behavior remains unqualified.
 * No NIR operation, arithmetic qualification or native admission is enabled. */
enum sgx535_f32_less_candidate_status
sgx535_f32_less_candidate_encode(const struct sgx535_f32_less_candidate *, uint32_t[6]);
#endif
