// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* A vertex shader CSO on the GPU (docs/graphics.md#gpu-vertex-shading): its
 * NIR read once into a vertex program (sgx535_vertex_nir.h); per
 * vertex-element layout and texture coordinates a variant, compiled on first
 * use; per draw the SECONDARY constant block from CB0. */
#ifndef POULSBACK_VERTEX_H
#define POULSBACK_VERTEX_H
#include "poulsback_frame.h"
#include "sgx535_vertex_nir.h"
#include <stdbool.h>
#include <stdint.h>

struct nir_shader;
struct poulsback_vertex_shader;
/* NULL when the backend refuses the shader (*REFUSAL names the construct) or
 * memory runs out (*REFUSAL "memory"). The NIR is only read. */
struct poulsback_vertex_shader *poulsback_vertex_shader_create(const struct nir_shader *,
                                                               const char **refusal);
void poulsback_vertex_shader_destroy(struct poulsback_vertex_shader *);
/* Inputs read: the highest one plus one. */
uint32_t poulsback_vertex_shader_inputs(const struct poulsback_vertex_shader *);
/* The program for elements with COMPONENTS (input I's) and the TC_COUNT
 * texture coordinates TC, as for sgx535_vertex_nir_link; NULL when it does
 * not link or encode. */
const struct poulsback_frame_gpu_program *
poulsback_vertex_shader_program(struct poulsback_vertex_shader *,
                                const uint8_t components[SGX535_VERTEX_INPUTS],
                                const struct sgx535_vertex_tc *tc, uint32_t tc_count);
/* The constant block from CB0 (BYTES at CB0) into OUT (SGX535_VERTEX_CONSTANTS
 * words): its length, or 0 when the shader reads a word CB0 lacks. */
uint32_t poulsback_vertex_shader_constants(const struct poulsback_vertex_shader *, const void *cb0,
                                           uint32_t bytes, uint32_t *out);
#endif
