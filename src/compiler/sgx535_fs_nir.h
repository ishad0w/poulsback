// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* st/mesa's fragment NIR -> the fragment backend's IR (docs/compiler.md#fragment-backend). The
 * adapter lowers a private clone: texture projectors and rectangles, sin and
 * cos by our own polynomial, everything else by NIR's options; per-pixel ifs
 * are flattened into selects and discards; integers and booleans become
 * floats (GLSL 1.20's ranges); what remains branches only on conditions that
 * are uniform per task. */
#ifndef POULSBACK_SGX535_FS_NIR_H
#define POULSBACK_SGX535_FS_NIR_H
#include "sgx535_fs.h"

struct nir_shader;

/* Mesa's packed-uniforms cap: dword addresses or vec4 slots. */
enum sgx535_fs_uniforms {
    SGX535_FS_UNIFORMS_DWORDS,
    SGX535_FS_UNIFORMS_VEC4,
};

struct sgx535_fs_nir {
    struct sgx535_fs_instruction ir[SGX535_FS_MAX_INSTRUCTIONS];
    uint32_t count;
    struct sgx535_fs_key key; /* varyings and per-sampler result forms */
    /* The gl_varying_slot each TC slot iterates, in location order. */
    uint16_t varying_location[SGX535_FS_MAX_VARYINGS];
    uint32_t sampler_mask;
    /* The refusal's site: a NIR op or intrinsic name, or a short reason. */
    char reason[64];
};

enum sgx535_fs_status sgx535_fs_from_nir(const struct nir_shader *, enum sgx535_fs_uniforms,
                                         struct sgx535_fs_nir *);
#endif
