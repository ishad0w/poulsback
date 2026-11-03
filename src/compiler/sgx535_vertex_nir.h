// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* NIR vertex shaders as SGX535 vertex programs (docs/compiler.md#vertex-programs).
 *
 * - sgx535_nir_vertex_ir: a clone of the state tracker's NIR, IO lowered to
 *   vec4 slots, scalarized and optimized, read as one basic block of F32
 *   operations: input I component C is PA 4I + C; CB0 dwords and literals are
 *   SA registers (sa0 reserved); POSITION and up to 16 other outputs are
 *   kept, each by its location.
 *   Float comparisons give exact 0.0/1.0 values (MAD, MAX and MIN, no
 *   predicates), and selects blend by them. Loops with a constant bound are
 *   unrolled and ifs whose sides are straight code become selects. Anything
 *   else (other control flow, integers, indirect access, other intrinsics) is
 *   refused and named in REFUSAL.
 * - sgx535_vertex_nir_link: the program for one draw. Attribute components an
 *   element does not supply read GL's defaults (0, 0, 0, 1), a UNORM8 element's
 *   are unpacked first (unpckf32u8.scale); the outputs are
 *   POSITION, then the texture coordinates the fragment stage iterates, in its
 *   order (COLOR0 clamped to [0, 1]); dead code goes; the result is encoded
 *   (sgx535_vertex_encode). */
#ifndef SGX535_VERTEX_NIR_H
#define SGX535_VERTEX_NIR_H
#include "sgx535_vertex.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif

#define SGX535_VERTEX_INPUTS 16u
enum sgx535_vertex_constant_kind {
    SGX535_VERTEX_RESERVED, /* sa0: zero until memory constants exist */
    SGX535_VERTEX_WORD,     /* a CB0 dword */
    SGX535_VERTEX_LITERAL   /* F32 bits */
};
struct sgx535_vertex_constant {
    uint32_t kind, value;
};
/* An output other than POSITION: LOCATION a gl_varying_slot, WRITTEN its
 * components written (a mask); CLAMP: COLOR0, clamped to [0, 1]. */
#define SGX535_VERTEX_NIR_OUTPUTS 16u
struct sgx535_vertex_nir_output {
    uint16_t location;
    uint8_t written, clamp;
    struct sgx535_vertex_source source[4];
};
struct sgx535_vertex_nir_program {
    struct sgx535_vertex_instruction instruction[SGX535_VERTEX_MAX_INSTRUCTIONS];
    uint32_t count;
    struct sgx535_vertex_source position[4];
    bool has_position;
    uint32_t output_count;
    struct sgx535_vertex_nir_output output[SGX535_VERTEX_NIR_OUTPUTS];
    uint32_t input_count;                         /* highest input read + 1 */
    uint32_t input_components[SGX535_VERTEX_INPUTS]; /* masks of components read */
    struct sgx535_vertex_constant constant[SGX535_VERTEX_CONSTANTS];
    uint32_t constant_count, zero, one; /* zero, one: SA of 0.0 and 1.0 */
    const char *refusal;
};
struct nir_shader;
enum sgx535_vertex_status sgx535_nir_vertex_ir(const struct nir_shader *,
                                               struct sgx535_vertex_nir_program *out);
/* COMPONENTS[i]: input I's element: its components (1..4), F32 dwords in PA
 * 4I.. unless SGX535_VERTEX_PACKED: UNORM8 bytes in the one dword PA 4I
 * (with SGX535_VERTEX_BGRA, blue first), unpacked to [0, 1]. */
#define SGX535_VERTEX_PACKED 0x10u
#define SGX535_VERTEX_BGRA 0x20u
/* A texture coordinate after POSITION: the first COMPONENTS (1-4) of the
 * output at LOCATION, which the program must write. */
#define SGX535_VERTEX_TCS 10u
struct sgx535_vertex_tc {
    uint16_t location;
    uint8_t components;
};
enum sgx535_vertex_status sgx535_vertex_nir_link(const struct sgx535_vertex_nir_program *,
                                                 const uint8_t components[SGX535_VERTEX_INPUTS],
                                                 const struct sgx535_vertex_tc *tc,
                                                 uint32_t tc_count, struct sgx535_vertex_code *out);
#endif
