// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_ITERATOR_H
#define POULSBACK_SGX535_ITERATOR_H

#include "sgx535_compiler_compat.h"

enum sgx535_iterator_status {
    SGX535_ITERATOR_OK = 0,
    SGX535_ITERATOR_ERR_ARGUMENT = -1,
    SGX535_ITERATOR_ERR_RANGE = -2,
    SGX535_ITERATOR_ERR_UNSUPPORTED = -3
};

/* Deliberately bounded generic F32 issue descriptor. The slot limit is this
 * codec's policy, not a claimed hardware maximum or a shader semantic map.
 */
#define SGX535_ITERATOR_F32_MAX_SLOT 9u
struct sgx535_iterator_f32 {
    uint32_t slot;
    uint32_t components;
    uint32_t last;
};
struct sgx535_iterator_words {
    uint32_t issue;
    uint32_t tag_layer;
};

/* CPU serializer only: no PDS MOVS generation, primary-attribute allocation,
 * interpolation-mode selection, dependency control or task launch. No texture
 * issue, special position/color semantic, F16 or C10 profile is accepted.
 * Errors preserve output; valid stable input/output objects may overlap.
 */
enum sgx535_iterator_status sgx535_iterator_f32_encode(const struct sgx535_iterator_f32 *,
                                                       struct sgx535_iterator_words *);
enum sgx535_iterator_status sgx535_iterator_f32_decode(const struct sgx535_iterator_words *,
                                                       struct sgx535_iterator_f32 *);

#endif
