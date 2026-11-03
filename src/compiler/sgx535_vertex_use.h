// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_VERTEX_USE_H
#define SGX535_VERTEX_USE_H
#ifdef __KERNEL__
#include <linux/types.h>
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#else
#include <stdint.h>
#endif

/* A bounded word projection of one retained vertex producer. It is not an
 * executable shader contract. Direct register numbers must stay below 128;
 * the historical producer's wrapping beyond that bound is refused. */
#define SGX535_VERTEX_USE_MAX_WORDS 128u
#define SGX535_VERTEX_USE_MAX_INSTRUCTIONS 9u
enum sgx535_vertex_use_status {
    SGX535_VERTEX_USE_OK,
    SGX535_VERTEX_USE_INVALID,
    SGX535_VERTEX_USE_RANGE
};
struct sgx535_vertex_use_projection {
    uint32_t input_words;
    uint32_t instruction_count;
    uint32_t words[2u * SGX535_VERTEX_USE_MAX_INSTRUCTIONS];
};
/* Copies the input-bank word projection in chunks of at most sixteen and
 * appends the producer's opaque terminal pair. Repeat/terminal execution,
 * vertex origin, PDS linkage, address units and retirement remain unqualified.
 * Refusal leaves the complete destination unchanged. */
enum sgx535_vertex_use_status sgx535_vertex_use_project(uint32_t input_words,
                                                        struct sgx535_vertex_use_projection *out);
#endif
