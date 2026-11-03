// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Shared raw field composition. Callers validate bank/register/end first.
 * These words do not authenticate execution, task linkage or termination. */
#ifndef POULSBACK_SGX535_LIMM_FIELDS_H
#define POULSBACK_SGX535_LIMM_FIELDS_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 sgx535_limm_word;
#else
#include <stdint.h>
typedef uint32_t sgx535_limm_word;
#endif

static inline sgx535_limm_word sgx535_limm_data_low(sgx535_limm_word bits)
{
    return bits & 0x001fffffU;
}

static inline sgx535_limm_word sgx535_limm_data_high(sgx535_limm_word bits)
{
    return ((bits >> 21) & 31U) << 4 | ((bits >> 26) & 63U) << 12;
}

static inline sgx535_limm_word sgx535_limm_data_decode(const sgx535_limm_word words[2])
{
    return sgx535_limm_data_low(words[0]) | ((words[1] >> 4) & 31U) << 21 |
           ((words[1] >> 12) & 63U) << 26;
}

static inline void sgx535_limm_compose(sgx535_limm_word bank, sgx535_limm_word reg,
                                       sgx535_limm_word immediate, sgx535_limm_word end,
                                       sgx535_limm_word words[2])
{
    words[0] = sgx535_limm_data_low(immediate) | reg << 21;
    words[1] = 0xfca00000U | bank | end << 18 | sgx535_limm_data_high(immediate);
}
#endif
