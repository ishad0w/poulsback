// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* CPU codecs for a bounded direct-register SGX535 instruction profile.
 * This interface describes raw words, not arithmetic or hardware execution.
 * Every failing call preserves its destination. Objects must be valid and
 * stable during calls; instruction inputs and outputs may overlap.
 */
#ifndef POULSBACK_SGX535_USSE_H
#define POULSBACK_SGX535_USSE_H
#include "sgx535_compiler_compat.h"

#define SGX535_SHADER_WORD_COUNT 2u
#define SGX535_SHADER_BYTE_COUNT 8u
#define SGX535_LIMM_WORD_COUNT 2u
#define SGX535_LIMM_TEMP_MAX 123u
#define SGX535_LIMM_OUTPUT_MAX 127u
#define SGX535_MOV_WORD_COUNT 2u
#define SGX535_F32_WORD_COUNT 2u

enum sgx535_shader_status {
    SGX535_SHADER_OK = 0,
    SGX535_SHADER_ERR_ARGUMENT = -1,
    SGX535_SHADER_ERR_UNSUPPORTED = -2,
    SGX535_SHADER_ERR_BOUNDS = -3
};
enum sgx535_limm_bank { SGX535_LIMM_TEMP = 0, SGX535_LIMM_OUTPUT = 1 };
enum sgx535_limm_status {
    SGX535_LIMM_OK = 0,
    SGX535_LIMM_ERR_ARGUMENT = -1,
    SGX535_LIMM_ERR_RANGE = -2,
    SGX535_LIMM_ERR_UNSUPPORTED = -3
};
struct sgx535_limm {
    enum sgx535_limm_bank bank;
    uint32_t register_number;
    uint32_t immediate;
    uint32_t end;
};
/* PRIMARY and SECONDARY are source-only direct banks. Register numbering does
 * not establish PDS attribute/constant linkage or cache visibility.
 */
enum sgx535_mov_bank {
    SGX535_MOV_TEMP = 0,
    SGX535_MOV_OUTPUT = 1,
    SGX535_MOV_PRIMARY = 2,
    SGX535_MOV_SECONDARY = 3
};
enum sgx535_mov_status {
    SGX535_MOV_OK = 0,
    SGX535_MOV_ERR_ARGUMENT = -1,
    SGX535_MOV_ERR_RANGE = -2,
    SGX535_MOV_ERR_UNSUPPORTED = -3
};
struct sgx535_mov_operand {
    enum sgx535_mov_bank bank;
    uint32_t number;
};
struct sgx535_mov {
    struct sgx535_mov_operand destination;
    struct sgx535_mov_operand source;
    uint32_t end;
};
enum sgx535_f32_bank { SGX535_F32_TEMP = 0, SGX535_F32_OUTPUT = 1 };
enum sgx535_f32_opcode {
    SGX535_F32_MAD = 0,
    SGX535_F32_MIN = 1,
    SGX535_F32_MAX = 2,
    SGX535_F32_RCP = 3,
    SGX535_F32_RSQ = 4, /* Static reciprocal-square-root word profile only. */
    /* Two-source F32 s1-floor(s2) construction, not arithmetic qualification. */
    SGX535_F32_FRC = 5
};
enum sgx535_f32_status {
    SGX535_F32_OK = 0,
    SGX535_F32_ERR_ARGUMENT = -1,
    SGX535_F32_ERR_RANGE = -2,
    SGX535_F32_ERR_UNSUPPORTED = -3
};
struct sgx535_f32_operand {
    enum sgx535_f32_bank bank;
    uint32_t number;
};
struct sgx535_f32 {
    enum sgx535_f32_opcode opcode;
    struct sgx535_f32_operand destination;
    struct sgx535_f32_operand source[3];
    uint32_t end;
};
enum sgx535_store_opcode { SGX535_STORE_STAD = 0, SGX535_STORE_STLD = 1 };
enum sgx535_store_status {
    SGX535_STORE_OK = 0,
    SGX535_STORE_ERR_ARGUMENT = -1,
    SGX535_STORE_ERR_RANGE = -2,
    SGX535_STORE_ERR_UNSUPPORTED = -3
};
struct sgx535_store {
    enum sgx535_store_opcode opcode;
    uint32_t base_temp;
    uint32_t offset_temp;
    uint32_t data_temp;
};

enum sgx535_limm_status sgx535_limm_encode(const struct sgx535_limm *, uint32_t[2]);
enum sgx535_limm_status sgx535_limm_decode(const uint32_t[2], struct sgx535_limm *);
enum sgx535_mov_status sgx535_mov_encode(const struct sgx535_mov *, uint32_t[2]);
enum sgx535_mov_status sgx535_mov_decode(const uint32_t[2], struct sgx535_mov *);
enum sgx535_f32_status sgx535_f32_encode(const struct sgx535_f32 *, uint32_t[2]);
enum sgx535_f32_status sgx535_f32_decode(const uint32_t[2], struct sgx535_f32 *);
enum sgx535_store_status sgx535_store_encode(const struct sgx535_store *, uint32_t[2]);
enum sgx535_store_status sgx535_store_decode(const uint32_t[2], struct sgx535_store *);
enum sgx535_shader_status sgx535_shader_encode_argb(uint32_t, uint32_t[2]);
enum sgx535_shader_status sgx535_shader_decode_argb(const uint32_t[2], uint32_t *);
enum sgx535_shader_status sgx535_shader_pack_le(const uint32_t[2], unsigned char *, size_t);
enum sgx535_shader_status sgx535_shader_unpack_le(const unsigned char *, size_t, uint32_t[2]);
#endif
