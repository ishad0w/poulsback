// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The USSE instruction encoder (docs/compiler.md#fragment-backend): one record per instruction,
 * two words out. Every form here prints as intended through the rev121
 * decoder (docs/re/sgx535/usse-instruction-contracts.md).
 * Encoding checks representability only; it says nothing about execution. */
#ifndef POULSBACK_SGX535_USE_ENCODE_H
#define POULSBACK_SGX535_USE_ENCODE_H
#include "sgx535_compiler_compat.h"

enum sgx535_use_bank {
    SGX535_USE_R,   /* temporaries r0-r123 */
    SGX535_USE_O,   /* outputs o0-o127 */
    SGX535_USE_PA,  /* primary attributes */
    SGX535_USE_SA,  /* secondary attributes */
    SGX535_USE_C,   /* special constants c0-c63: c48 0.0, c52 1.0 */
    SGX535_USE_G,   /* special registers g0-g63: g16 the facing */
    SGX535_USE_IMM, /* immediate #0-127 */
    SGX535_USE_I,   /* internal registers i0-i3 */
};
#define SGX535_USE_NEG 1u
#define SGX535_USE_ABS 2u
#define SGX535_USE_C_ZERO 48u
#define SGX535_USE_C_ONE 52u
#define SGX535_USE_G_FACING 16u
#define SGX535_USE_TEMPS 124u

struct sgx535_use_operand {
    uint8_t bank, number, mod; /* mod: SGX535_USE_NEG | SGX535_USE_ABS */
};

enum sgx535_use_opcode {
    SGX535_USE_FMAD, /* dst = src0 * src1 + src2 */
    SGX535_USE_FFRC, /* dst = src1 - floor(src2) */
    SGX535_USE_FMIN, /* src1, src2 */
    SGX535_USE_FMAX,
    SGX535_USE_FRCP, /* src1 */
    SGX535_USE_FRSQ,
    SGX535_USE_FLOG, /* base 2 */
    SGX535_USE_FEXP,
    SGX535_USE_FDSX, /* src1 = src2, a temporary or PA */
    SGX535_USE_FDSY,
    SGX535_USE_FDP,        /* sum over repeat of src1[i] * src2[i] */
    SGX535_USE_MOV,        /* src1 */
    SGX535_USE_LIMM,       /* immediate, dst r or o */
    SGX535_USE_TEST,       /* alu(src1, src2) tested into p[pdst] */
    SGX535_USE_PCKU8F32,   /* bytes of mask from src1, src2 (F32 -> U8) */
    SGX535_USE_UNPCKF32U8, /* byte lane of src1 -> F32 */
    SGX535_USE_SMP,        /* coordinates src0, state words src1, LOD src2 */
    SGX535_USE_WDF,
    SGX535_USE_BR, /* relative target, in instructions */
    SGX535_USE_BA, /* absolute target */
    SGX535_USE_NOP,
    SGX535_USE_SOP2, /* integer blend */
    SGX535_USE_SOPWM,
    SGX535_USE_ATST8,
    SGX535_USE_DEPTHF,
    SGX535_USE_PCOEFF,
    SGX535_USE_AND, /* bitwise, src1 op src2 */
    SGX535_USE_OR,
    SGX535_USE_XOR,
};

/* TEST conditions on the ALU result: six forms. */
enum sgx535_use_test {
    SGX535_USE_TEST_GT, /* sign clear and non-zero */
    SGX535_USE_TEST_GE, /* sign clear or zero */
    SGX535_USE_TEST_EQ,
    SGX535_USE_TEST_LT,
    SGX535_USE_TEST_LE,
    SGX535_USE_TEST_NE,
};
/* TEST's ALU: F32 operations, or the bitwise ones. */
enum sgx535_use_test_alu {
    SGX535_USE_ALU_FADD,
    SGX535_USE_ALU_FSUB,
    SGX535_USE_ALU_FMUL,
    SGX535_USE_ALU_FMIN,
    SGX535_USE_ALU_FMAX,
    SGX535_USE_ALU_AND,
    SGX535_USE_ALU_OR,
    SGX535_USE_ALU_XOR,
};

/* Predicates. EPRED families (float, mov, test, pack, smp, branches): 0 none,
 * 1-4 p0-p3, 5 !p0, 6 !p1. SPRED families (sop2, sopwm, atst8, depthf,
 * pcoeff, bitwise): 0 none, 1 p0, 2 p1, 3 !p0. */
struct sgx535_use_instruction {
    uint8_t opcode;
    uint8_t pred;
    uint8_t skipinv, syncs, nosched, end;
    uint8_t repeat;    /* 1-16 iterations; 0 is 1 */
    uint8_t writemask; /* 1-15: RMSKCNT as a write mask, RCNTSEL clear (EMGD's packs) */
    uint8_t mask;      /* PCK byte mask (bit n = byte n), SOPWM write mask, UNPCK lane */
    uint8_t scale;     /* PCK/UNPCK: [0,1] <-> 0-255 (0-65535 for U16) */
    uint8_t format;    /* UNPCK's source: 0 U8, 3 U16, 5 F16 (PCK's formats); the lane
                        * is the byte it starts at */
    struct sgx535_use_operand dst, src[3];
    uint32_t immediate; /* LIMM data; BR/BA target */
    /* TEST */
    uint8_t test, alu, pdst, writeback;
    /* SMP: cdim 0-2 (u, uv, uvs), lodm 0-3 (none, bias, replace, gradients) */
    uint8_t cdim, lodm, drc;
    /* SOP2 and SOPWM, as named in use-encode.py */
    uint8_t csel1, csel2, cmod1, cmod2, asel1, asel2, amod1, amod2, cop, aop, src1mod, asrc1mod,
        adstmod;
    /* ATST8/DEPTHF/PCOEFF */
    uint8_t twosided, optdwd, nofeedback, pdsten;
    /* AND/OR/XOR: src2 inverted (SRC2INV) */
    uint8_t invert;
};

enum sgx535_use_status {
    SGX535_USE_OK = 0,
    SGX535_USE_ERR_ARGUMENT = -1,
    SGX535_USE_ERR_RANGE = -2,
    SGX535_USE_ERR_UNSUPPORTED = -3,
};
enum sgx535_use_status sgx535_use_encode(const struct sgx535_use_instruction *, uint32_t[2]);
/* IN encoded after the COUNT instructions of WORDS (room for CAPACITY), COUNT
 * advanced; SGX535_USE_ERR_RANGE when full. */
enum sgx535_use_status sgx535_use_append(uint32_t *words, uint32_t *count, uint32_t capacity,
                                         const struct sgx535_use_instruction *in);
#endif
