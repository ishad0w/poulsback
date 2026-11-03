// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_iterator.h"

/* Canonical generic F32 issue fields. Keep these separate from the opaque
 * no-iterator task controls and the eventual typed shader linkage owner.
 */
#define ITERATOR_BASE UINT32_C(0x0d00000f)
#define ITERATOR_LAST UINT32_C(0x02000000)

enum sgx535_iterator_status sgx535_iterator_f32_encode(const struct sgx535_iterator_f32 *input,
                                                       struct sgx535_iterator_words *output)
{
    struct sgx535_iterator_f32 descriptor;
    struct sgx535_iterator_words result;

    if (!input || !output)
        return SGX535_ITERATOR_ERR_ARGUMENT;
    descriptor = *input;
    if (descriptor.slot > SGX535_ITERATOR_F32_MAX_SLOT || descriptor.components < 1 ||
        descriptor.components > 4 || descriptor.last > 1)
        return SGX535_ITERATOR_ERR_RANGE;
    result.issue = ITERATOR_BASE | (descriptor.slot << 12) | ((descriptor.components - 1) << 22);
    if (descriptor.last)
        result.issue |= ITERATOR_LAST;
    result.tag_layer = UINT32_MAX;
    *output = result;
    return SGX535_ITERATOR_OK;
}

enum sgx535_iterator_status sgx535_iterator_f32_decode(const struct sgx535_iterator_words *input,
                                                       struct sgx535_iterator_f32 *output)
{
    struct sgx535_iterator_words words, canonical;
    struct sgx535_iterator_f32 result;

    if (!input || !output)
        return SGX535_ITERATOR_ERR_ARGUMENT;
    words = *input;
    result.slot = (words.issue >> 12) & UINT32_C(15);
    result.components = ((words.issue >> 22) & UINT32_C(3)) + 1;
    result.last = (words.issue >> 25) & UINT32_C(1);
    if (sgx535_iterator_f32_encode(&result, &canonical) != SGX535_ITERATOR_OK ||
        words.issue != canonical.issue || words.tag_layer != canonical.tag_layer)
        return SGX535_ITERATOR_ERR_UNSUPPORTED;
    *output = result;
    return SGX535_ITERATOR_OK;
}
