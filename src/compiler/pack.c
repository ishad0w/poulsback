// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pack.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static enum sgx535_pack_status pack_direct_encode(const struct sgx535_pack_f32_u8 *input,
                                                  uint32_t formats, uint32_t components,
                                                  uint32_t words[2])
{
    struct sgx535_pack_f32_u8 op;
    uint32_t result[2];
    unsigned i;

    if (!input || !words)
        return SGX535_PACK_ERR_ARGUMENT;
    op = *input;
    if ((unsigned)op.destination.bank > 1u)
        return SGX535_PACK_ERR_UNSUPPORTED;
    for (i = 0; i < 2; ++i)
        if ((unsigned)op.source[i].bank > 3u)
            return SGX535_PACK_ERR_UNSUPPORTED;
    if (op.destination.number > (op.destination.bank == SGX535_MOV_TEMP ? 123u : 127u) ||
        op.write_mask > 15u || op.scale > 1u || op.end > 1u)
        return SGX535_PACK_ERR_RANGE;
    for (i = 0; i < 2; ++i)
        if (op.source[i].number > (op.source[i].bank == SGX535_MOV_TEMP ? 123u : 127u))
            return SGX535_PACK_ERR_RANGE;
    if (op.write_mask == 7 || op.write_mask == 11 || op.write_mask == 13 || op.write_mask == 14)
        return SGX535_PACK_ERR_UNSUPPORTED;
    result[0] = op.destination.number << 21 | op.source[0].number << 7 | op.source[1].number |
                (uint32_t)op.source[0].bank << 30 | (uint32_t)op.source[1].bank << 28 |
                op.scale << 18 | components;
    result[1] = UINT32_C(0x40a00000) | formats | (uint32_t)op.destination.bank |
                op.write_mask << 2 | op.end << 18;
    memcpy(words, result, sizeof(result));
    return SGX535_PACK_OK;
}

enum sgx535_pack_status sgx535_pack_f32_u8_encode(const struct sgx535_pack_f32_u8 *input,
                                                  uint32_t words[2])
{
    return pack_direct_encode(input, UINT32_C(0x0c00), 0, words);
}

enum sgx535_pack_status sgx535_pack_f32_u8_decode(const uint32_t words[2],
                                                  struct sgx535_pack_f32_u8 *output)
{
    struct sgx535_pack_f32_u8 op;
    uint32_t canonical[2];

    if (!words || !output)
        return SGX535_PACK_ERR_ARGUMENT;
    op.destination =
        (struct sgx535_mov_operand){(enum sgx535_mov_bank)(words[1] & 1u), (words[0] >> 21) & 127u};
    op.source[0] = (struct sgx535_mov_operand){(enum sgx535_mov_bank)((words[0] >> 30) & 3u),
                                               (words[0] >> 7) & 127u};
    op.source[1] =
        (struct sgx535_mov_operand){(enum sgx535_mov_bank)((words[0] >> 28) & 3u), words[0] & 127u};
    op.scale = (words[0] >> 18) & 1u;
    op.write_mask = (words[1] >> 2) & 15u;
    op.end = (words[1] >> 18) & 1u;
    if (sgx535_pack_f32_u8_encode(&op, canonical) != SGX535_PACK_OK || canonical[0] != words[0] ||
        canonical[1] != words[1])
        return SGX535_PACK_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_PACK_OK;
}

enum sgx535_pack_status
sgx535_pack_f32_u8_channel_encode(const struct sgx535_pack_f32_u8_channel *input, uint32_t words[2])
{
    struct sgx535_pack_f32_u8_channel op;
    struct sgx535_pack_f32_u8 direct;
    uint32_t result[2];
    enum sgx535_pack_status ret;
    if (!input || !words)
        return SGX535_PACK_ERR_ARGUMENT;
    op = *input;
    if (op.byte_index > 3U)
        return SGX535_PACK_ERR_RANGE;
    direct = (struct sgx535_pack_f32_u8){
        op.destination, {op.source, {SGX535_MOV_TEMP, 0}}, 1U << op.byte_index, op.scale, op.end};
    ret = sgx535_pack_f32_u8_encode(&direct, result);
    if (ret != SGX535_PACK_OK)
        return ret;
    result[0] |= UINT32_C(0x20000000);
    result[1] |= UINT32_C(0x00010000);
    memcpy(words, result, sizeof(result));
    return SGX535_PACK_OK;
}

enum sgx535_pack_status sgx535_pack_f32_u8_channel_decode(const uint32_t words[2],
                                                          struct sgx535_pack_f32_u8_channel *output)
{
    struct sgx535_pack_f32_u8 direct;
    struct sgx535_pack_f32_u8_channel op;
    uint32_t original[2], registers[2], canonical[2], byte_index;
    if (!words || !output)
        return SGX535_PACK_ERR_ARGUMENT;
    memcpy(original, words, sizeof(original));
    registers[0] = original[0] & ~UINT32_C(0x20000000);
    registers[1] = original[1] & ~UINT32_C(0x00010000);
    if (sgx535_pack_f32_u8_decode(registers, &direct) != SGX535_PACK_OK ||
        direct.source[1].bank != SGX535_MOV_TEMP || direct.source[1].number)
        return SGX535_PACK_ERR_UNSUPPORTED;
    for (byte_index = 0; byte_index < 4U; byte_index++)
        if (direct.write_mask == (1U << byte_index))
            break;
    if (byte_index == 4U)
        return SGX535_PACK_ERR_UNSUPPORTED;
    op = (struct sgx535_pack_f32_u8_channel){direct.destination, direct.source[0], byte_index,
                                             direct.scale, direct.end};
    if (sgx535_pack_f32_u8_channel_encode(&op, canonical) != SGX535_PACK_OK ||
        memcmp(original, canonical, sizeof(original)))
        return SGX535_PACK_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_PACK_OK;
}

enum sgx535_pack_status
sgx535_unpack_u8_f32_component_encode(const struct sgx535_unpack_u8_f32_component *input,
                                      uint32_t words[2])
{
    struct sgx535_unpack_u8_f32_component op;
    struct sgx535_pack_f32_u8 direct;
    if (!input || !words)
        return SGX535_PACK_ERR_ARGUMENT;
    op = *input;
    if (op.byte_index > 3u)
        return SGX535_PACK_ERR_RANGE;
    direct = (struct sgx535_pack_f32_u8){op.destination, {op.source, op.source}, 15, 1, op.end};
    return pack_direct_encode(&direct, UINT32_C(0x0180), op.byte_index << 16 | op.byte_index << 14,
                              words);
}

enum sgx535_pack_status
sgx535_unpack_u8_f32_component_decode(const uint32_t words[2],
                                      struct sgx535_unpack_u8_f32_component *output)
{
    struct sgx535_unpack_u8_f32_component op;
    uint32_t canonical[2];
    if (!words || !output)
        return SGX535_PACK_ERR_ARGUMENT;
    op.destination =
        (struct sgx535_mov_operand){(enum sgx535_mov_bank)(words[1] & 1u), (words[0] >> 21) & 127u};
    op.source = (struct sgx535_mov_operand){(enum sgx535_mov_bank)((words[0] >> 30) & 3u),
                                            (words[0] >> 7) & 127u};
    op.byte_index = (words[0] >> 16) & 3u;
    op.end = (words[1] >> 18) & 1u;
    if (sgx535_unpack_u8_f32_component_encode(&op, canonical) != SGX535_PACK_OK ||
        memcmp(words, canonical, sizeof(canonical)))
        return SGX535_PACK_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_PACK_OK;
}
