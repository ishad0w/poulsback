// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_bitwise.h"
#include "sgx535_usse.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif

enum sgx535_bitwise_status sgx535_bitwise_encode(const struct sgx535_bitwise *input,
                                                 uint32_t words[2])
{
    struct sgx535_bitwise op;
    uint32_t result[2];
    if (!input || !words)
        return SGX535_BITWISE_ERR_ARGUMENT;
    op = *input;
    if ((unsigned)op.opcode > SGX535_BITWISE_XOR)
        return SGX535_BITWISE_ERR_UNSUPPORTED;
    if (op.destination_temp > 123u || op.source_temp[0] > 123u || op.source_temp[1] > 123u ||
        op.end > 1u)
        return SGX535_BITWISE_ERR_RANGE;
    result[0] = op.destination_temp << 21 | op.source_temp[0] << 7 | op.source_temp[1];
    result[1] = (op.opcode == SGX535_BITWISE_AND ? UINT32_C(0x50a00000) : UINT32_C(0x58a00000)) |
                op.end << 18;
    memcpy(words, result, sizeof(result));
    return SGX535_BITWISE_OK;
}

enum sgx535_bitwise_status sgx535_bitwise_decode(const uint32_t words[2],
                                                 struct sgx535_bitwise *output)
{
    struct sgx535_bitwise op;
    uint32_t canonical[2];
    if (!words || !output)
        return SGX535_BITWISE_ERR_ARGUMENT;
    switch (words[1] & UINT32_C(0xfffbffff)) {
    case UINT32_C(0x50a00000):
        op.opcode = SGX535_BITWISE_AND;
        break;
    case UINT32_C(0x58a00000):
        op.opcode = SGX535_BITWISE_XOR;
        break;
    default:
        return SGX535_BITWISE_ERR_UNSUPPORTED;
    }
    op.destination_temp = (words[0] >> 21) & 127u;
    op.source_temp[0] = (words[0] >> 7) & 127u;
    op.source_temp[1] = words[0] & 127u;
    op.end = (words[1] >> 18) & 1u;
    if (sgx535_bitwise_encode(&op, canonical) != SGX535_BITWISE_OK || canonical[0] != words[0] ||
        canonical[1] != words[1])
        return SGX535_BITWISE_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_BITWISE_OK;
}

enum sgx535_bitwise_status sgx535_const_ushr_encode(const struct sgx535_const_ushr *input,
                                                    uint32_t output[2])
{
    struct sgx535_const_ushr selected;
    uint32_t words[2];
    if (!input || !output)
        return SGX535_BITWISE_ERR_ARGUMENT;
    selected = *input;
    if (selected.destination_temp > 123u || selected.source_temp > 123u)
        return SGX535_BITWISE_ERR_RANGE;
    if (selected.bit_count != 1u && selected.bit_count != 2u && selected.bit_count != 4u &&
        selected.bit_count != 8u && selected.bit_count != 16u && selected.bit_count != 31u)
        return SGX535_BITWISE_ERR_UNSUPPORTED;
    words[0] = UINT32_C(0x20000000) | selected.destination_temp << 21 | selected.source_temp << 7 |
               selected.bit_count;
    words[1] = UINT32_C(0x68a10000);
    memcpy(output, words, sizeof(words));
    return SGX535_BITWISE_OK;
}

enum sgx535_bitwise_status sgx535_const_ushr_decode(const uint32_t input[2],
                                                    struct sgx535_const_ushr *output)
{
    struct sgx535_const_ushr selected;
    uint32_t words[2], canonical[2];
    if (!input || !output)
        return SGX535_BITWISE_ERR_ARGUMENT;
    memcpy(words, input, sizeof(words));
    selected.destination_temp = (words[0] >> 21) & 127u;
    selected.source_temp = (words[0] >> 7) & 127u;
    selected.bit_count = words[0] & 127u;
    if (sgx535_const_ushr_encode(&selected, canonical) != SGX535_BITWISE_OK ||
        memcmp(words, canonical, sizeof(words)))
        return SGX535_BITWISE_ERR_UNSUPPORTED;
    memcpy(output, &selected, sizeof(selected));
    return SGX535_BITWISE_OK;
}

static bool predicate_mask_materialize(uint32_t destination_temp, uint32_t words[6])
{
    struct sgx535_limm immediate = {SGX535_LIMM_TEMP, destination_temp, 0, 0};
    if (sgx535_limm_encode(&immediate, words + 2) != SGX535_LIMM_OK)
        return false;
    immediate.immediate = UINT32_MAX;
    if (sgx535_limm_encode(&immediate, words + 4) != SGX535_LIMM_OK)
        return false;
    /* Immediate MOV encodes positive P1 as physical2 in bits9..11,
     * disjoint from the full32 immediate payload.
     * Both MOVs leave P1 intact. The interval is internal, never caller-owned. */
    words[5] |= UINT32_C(2) << 9;
    return true;
}

enum sgx535_raw_ieq_status sgx535_raw_ieq_encode(const struct sgx535_raw_ieq *input,
                                                 uint32_t output[6])
{
    struct sgx535_raw_ieq selected;
    uint32_t words[6];
    if (!input || !output)
        return SGX535_RAW_IEQ_ERR_ARGUMENT;
    selected = *input;
    if (selected.destination_temp >= 124 || selected.source_temp[0] >= 124 ||
        selected.source_temp[1] >= 124)
        return SGX535_RAW_IEQ_ERR_RANGE;
    /* Integer IXOR TEST: no data destination, P1 destination, ZERO,
     * scalar mask1, unpredicated, SKIPINV, no TEST-only/END/scheduling controls.
     * Bank0 TEMP operands occupy the independently joined source fields. */
    words[0] = UINT32_C(0x001c8000) | selected.source_temp[0] << 7 | selected.source_temp[1];
    words[1] = UINT32_C(0x48801184);
    if (!predicate_mask_materialize(selected.destination_temp, words))
        return SGX535_RAW_IEQ_ERR_RANGE;
    memcpy(output, words, sizeof(words));
    return SGX535_RAW_IEQ_OK;
}

enum sgx535_f32_less_candidate_status
sgx535_f32_less_candidate_encode(const struct sgx535_f32_less_candidate *input, uint32_t output[6])
{
    struct sgx535_f32_less_candidate selected;
    uint32_t words[6];
    if (!input || !output)
        return SGX535_F32_LESS_CANDIDATE_ERR_ARGUMENT;
    selected = *input;
    if (selected.destination_temp >= 124 || selected.source_temp[0] >= 124 ||
        selected.source_temp[1] >= 124)
        return SGX535_F32_LESS_CANDIDATE_ERR_RANGE;
    /* Selected F32 IFSUB TEST: LT condition, P1 destination and scalar C0.
     * Unmodified, unindexed TEMP sources use bank0 and 7-bit fields. This
     * source-derived template specifies encoding, not native FP semantics. */
    words[0] = UINT32_C(0x00138000) | selected.source_temp[0] << 7 | selected.source_temp[1];
    words[1] = UINT32_C(0x48801684);
    if (!predicate_mask_materialize(selected.destination_temp, words))
        return SGX535_F32_LESS_CANDIDATE_ERR_RANGE;
    memcpy(output, words, sizeof(words));
    return SGX535_F32_LESS_CANDIDATE_OK;
}
