// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* SGX535 vertex programs (docs/compiler.md#vertex-programs).
 *
 * - A program is scalar F32 SSA: each instruction defines one value, or writes
 *   one output register. Operands are earlier values, attribute registers (PA,
 *   filled per vertex by the vertex PDS's DMA) or constant registers (SA,
 *   filled per draw by a SECONDARY task). Literals are constants too: the
 *   caller places them in SA.
 * - The encoder allocates temporaries by last use, takes PA, SA and
 *   temporaries as direct operands (MAD's first source: a temporary or PA, so
 *   the encoder swaps its factors or moves the operand first) and ends with
 *   emitvtx.end.freep. Encodings follow EMGD's vertex programs as the rev121
 *   decoder reads them (docs/re/emgd-linux/vertex-path.md).
 * - The vertex PDS (EMGD's form): one DMA per element into PA, from a source
 *   address plus vertex index × stride, then the USE task. The SECONDARY PDS:
 *   a constant block DMAed into SA in lines of at most sixteen dwords, then
 *   the nop.end task.
 *
 * Word construction only: no device address, upload or execution. */
#ifndef SGX535_VERTEX_H
#define SGX535_VERTEX_H
#include "sgx535_compiler_compat.h"

#define SGX535_VERTEX_MAX_INSTRUCTIONS 384u
#define SGX535_VERTEX_MAX_CODE (2u * SGX535_VERTEX_MAX_INSTRUCTIONS + 2u) /* instructions */
#define SGX535_VERTEX_TEMPS 48u
#define SGX535_VERTEX_ATTRIBUTES 64u /* PA registers */
#define SGX535_VERTEX_CONSTANTS 128u /* SA registers; sa0 is reserved */
#define SGX535_VERTEX_OUTPUTS 44u    /* POSITION and ten 4-dword texture coordinates */

enum sgx535_vertex_status {
    SGX535_VERTEX_OK = 0,
    SGX535_VERTEX_INVALID = -1,
    SGX535_VERTEX_UNSUPPORTED = -2,
    SGX535_VERTEX_RANGE = -3
};
enum sgx535_vertex_source_kind {
    SGX535_VERTEX_VALUE,     /* an earlier instruction's value */
    SGX535_VERTEX_ATTRIBUTE, /* PA register */
    SGX535_VERTEX_CONSTANT   /* SA register */
};
struct sgx535_vertex_source {
    uint8_t kind, negate, absolute;
    uint16_t index;
};
enum sgx535_vertex_opcode {
    SGX535_VERTEX_MOV, /* source 0 */
    SGX535_VERTEX_MAD, /* source 0 × source 1 + source 2 */
    SGX535_VERTEX_MIN,
    SGX535_VERTEX_MAX,
    SGX535_VERTEX_RCP,
    SGX535_VERTEX_RSQ,
    SGX535_VERTEX_LOG,    /* log2 */
    SGX535_VERTEX_EXP,    /* exp2 */
    SGX535_VERTEX_UNORM8  /* + lane 0..3: that byte of source 0 (a PA register) / 255 */
};
struct sgx535_vertex_instruction {
    uint8_t opcode;
    int8_t output; /* -1: defines a value; else the output register it writes */
    struct sgx535_vertex_source source[3];
};
struct sgx535_vertex_code {
    uint32_t words[2u * SGX535_VERTEX_MAX_CODE];
    uint32_t instruction_count, temps;
};
/* Encodes COUNT instructions, then the emit. Every output register below
 * OUTPUTS is written exactly once; values are used after their definition.
 * Refusal leaves OUT unchanged. */
enum sgx535_vertex_status sgx535_vertex_encode(const struct sgx535_vertex_instruction *,
                                               uint32_t count, uint32_t outputs,
                                               struct sgx535_vertex_code *out);

/* The USE task words of a vertex program with TEMPS temporaries whose
 * DOUTU0 address fields are LINK (sgx535_pds_use_link.c). The task waits for
 * the PDS DMA (bit 21), as our pass-through task does. */
void sgx535_vertex_task(uint32_t link, uint32_t temps, uint32_t task[3]);

/* The vertex PDS. */
#define SGX535_VERTEX_PDS_ELEMENTS 8u
#define SGX535_VERTEX_PDS_STRIDES 4u
#define SGX535_VERTEX_PDS_MAX_WORDS 64u
struct sgx535_vertex_pds_element {
    uint32_t primary; /* first PA register, a multiple of four */
    uint32_t dwords;  /* 1..4 */
    uint32_t stride;  /* bytes; 0: every vertex reads the same dwords */
};
struct sgx535_vertex_pds_layout {
    uint32_t element_count;
    struct sgx535_vertex_pds_element element[SGX535_VERTEX_PDS_ELEMENTS];
    uint32_t task[3];
};
struct sgx535_vertex_pds {
    uint32_t data_words, word_count; /* data first, then code */
    uint32_t words[SGX535_VERTEX_PDS_MAX_WORDS];
    uint32_t source_word[SGX535_VERTEX_PDS_ELEMENTS]; /* each element's source address */
};
enum sgx535_vertex_status sgx535_vertex_pds_build(const struct sgx535_vertex_pds_layout *,
                                                  struct sgx535_vertex_pds *out);

/* The SECONDARY PDS: DWORDS (1..SGX535_VERTEX_CONSTANTS) from a constant
 * block into sa0 and up, then the USE task TASK. */
#define SGX535_VERTEX_SECONDARY_MAX_WORDS 24u
struct sgx535_vertex_secondary {
    uint32_t data_words, word_count;
    uint32_t words[SGX535_VERTEX_SECONDARY_MAX_WORDS];
    uint32_t transfer_count, source_word[2], source_offset[2];
};
enum sgx535_vertex_status sgx535_vertex_secondary_build(uint32_t dwords, const uint32_t task[3],
                                                        struct sgx535_vertex_secondary *out);

/* nop.end, the SECONDARY task's USE program. */
#define SGX535_VERTEX_NOP_END_LOW UINT32_C(0x00000000)
#define SGX535_VERTEX_NOP_END_HIGH UINT32_C(0xf8040140)
#endif
