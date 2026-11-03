// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Restricted instruction field composition. No device access occurs here. */
#include "sgx535_usse.h"
#include "sgx535_limm_fields.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif

struct arithmetic_layout {
    uint32_t high;
    unsigned sources;
};
static const struct arithmetic_layout arithmetic[] = {
    {UINT32_C(0x00a00000), 3}, {UINT32_C(0x18a00000), 2}, {UINT32_C(0x18a00200), 2},
    {UINT32_C(0x08a00000), 1}, {UINT32_C(0x08a00200), 1}, {UINT32_C(0x00a00600), 2}};

static bool direct_bank(unsigned bank)
{
    return bank <= 1u;
}
static bool valid_register(unsigned bank, uint32_t number)
{
    return direct_bank(bank) && number <= (bank ? 127u : 123u);
}
static bool identical(const uint32_t expected[2], const uint32_t actual[2])
{
    return expected[0] == actual[0] && expected[1] == actual[1];
}

enum sgx535_limm_status sgx535_limm_encode(const struct sgx535_limm *input, uint32_t words[2])
{
    struct sgx535_limm op;
    uint32_t result[2];
    if (!input || !words)
        return SGX535_LIMM_ERR_ARGUMENT;
    op = *input;
    if (!direct_bank((unsigned)op.bank))
        return SGX535_LIMM_ERR_UNSUPPORTED;
    if (!valid_register((unsigned)op.bank, op.register_number) || op.end > 1u)
        return SGX535_LIMM_ERR_RANGE;
    sgx535_limm_compose((uint32_t)op.bank, op.register_number, op.immediate, op.end, result);
    memcpy(words, result, sizeof(result));
    return SGX535_LIMM_OK;
}

enum sgx535_limm_status sgx535_limm_decode(const uint32_t words[2], struct sgx535_limm *output)
{
    struct sgx535_limm op;
    uint32_t canonical[2];
    if (!words || !output)
        return SGX535_LIMM_ERR_ARGUMENT;
    op.bank = (enum sgx535_limm_bank)(words[1] & 3u);
    op.register_number = (words[0] >> 21) & 127u;
    op.immediate = sgx535_limm_data_decode(words);
    op.end = (words[1] >> 18) & 1u;
    if (sgx535_limm_encode(&op, canonical) != SGX535_LIMM_OK || !identical(canonical, words))
        return SGX535_LIMM_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_LIMM_OK;
}

enum sgx535_mov_status sgx535_mov_encode(const struct sgx535_mov *input, uint32_t words[2])
{
    struct sgx535_mov op;
    uint32_t result[2];
    if (!input || !words)
        return SGX535_MOV_ERR_ARGUMENT;
    op = *input;
    if (!direct_bank((unsigned)op.destination.bank) || (unsigned)op.source.bank > 3u)
        return SGX535_MOV_ERR_UNSUPPORTED;
    if (!valid_register((unsigned)op.destination.bank, op.destination.number) ||
        op.source.number > (op.source.bank == SGX535_MOV_TEMP ? 123u : 127u) || op.end > 1u)
        return SGX535_MOV_ERR_RANGE;
    result[0] = UINT32_C(0x20000000) | op.destination.number << 21 | op.source.number << 7 |
                (uint32_t)op.source.bank << 30;
    result[1] = UINT32_C(0x28a10000) | (uint32_t)op.destination.bank | op.end << 18;
    memcpy(words, result, sizeof(result));
    return SGX535_MOV_OK;
}

enum sgx535_mov_status sgx535_mov_decode(const uint32_t words[2], struct sgx535_mov *output)
{
    struct sgx535_mov op;
    uint32_t canonical[2];
    if (!words || !output)
        return SGX535_MOV_ERR_ARGUMENT;
    op.destination =
        (struct sgx535_mov_operand){(enum sgx535_mov_bank)(words[1] & 1u), (words[0] >> 21) & 127u};
    op.source = (struct sgx535_mov_operand){(enum sgx535_mov_bank)((words[0] >> 30) & 3u),
                                            (words[0] >> 7) & 127u};
    op.end = (words[1] >> 18) & 1u;
    if (sgx535_mov_encode(&op, canonical) != SGX535_MOV_OK || !identical(canonical, words))
        return SGX535_MOV_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_MOV_OK;
}

enum sgx535_f32_status sgx535_f32_encode(const struct sgx535_f32 *input, uint32_t words[2])
{
    struct sgx535_f32 op;
    const struct arithmetic_layout *layout;
    uint32_t result[2];
    unsigned i, middle;
    if (!input || !words)
        return SGX535_F32_ERR_ARGUMENT;
    op = *input;
    if ((unsigned)op.opcode >= sizeof(arithmetic) / sizeof(arithmetic[0]) ||
        !direct_bank((unsigned)op.destination.bank))
        return SGX535_F32_ERR_UNSUPPORTED;
    layout = &arithmetic[op.opcode];
    for (i = 0; i < 3u; ++i) {
        if (i < layout->sources) {
            if (!direct_bank((unsigned)op.source[i].bank))
                return SGX535_F32_ERR_UNSUPPORTED;
        } else if (op.source[i].bank != SGX535_F32_TEMP || op.source[i].number != 0u) {
            return SGX535_F32_ERR_UNSUPPORTED;
        }
    }
    if (layout->sources == 3u && op.source[0].bank != SGX535_F32_TEMP)
        return SGX535_F32_ERR_UNSUPPORTED;
    if (op.end > 1u || !valid_register((unsigned)op.destination.bank, op.destination.number))
        return SGX535_F32_ERR_RANGE;
    for (i = 0; i < layout->sources; ++i)
        if (!valid_register((unsigned)op.source[i].bank, op.source[i].number))
            return SGX535_F32_ERR_RANGE;
    middle = layout->sources == 3u ? 1u : 0u;
    result[0] = op.destination.number << 21 | op.source[middle].number << 7 |
                (uint32_t)op.source[middle].bank << 30;
    result[1] = layout->high | (uint32_t)op.destination.bank | op.end << 18;
    if (layout->sources == 3u)
        result[0] |= op.source[0].number << 14;
    if (layout->sources >= 2u) {
        i = middle + 1u;
        result[0] |= op.source[i].number | (uint32_t)op.source[i].bank << 28;
    }
    memcpy(words, result, sizeof(result));
    return SGX535_F32_OK;
}

enum sgx535_f32_status sgx535_f32_decode(const uint32_t words[2], struct sgx535_f32 *output)
{
    struct sgx535_f32 op = {0};
    uint32_t canonical[2];
    unsigned i, middle, count = 0;
    if (!words || !output)
        return SGX535_F32_ERR_ARGUMENT;
    for (i = 0; i < sizeof(arithmetic) / sizeof(arithmetic[0]); ++i) {
        if ((words[1] & UINT32_C(0xfffbfffe)) == arithmetic[i].high) {
            op.opcode = (enum sgx535_f32_opcode)i;
            count = arithmetic[i].sources;
            break;
        }
    }
    if (!count)
        return SGX535_F32_ERR_UNSUPPORTED;
    op.destination =
        (struct sgx535_f32_operand){(enum sgx535_f32_bank)(words[1] & 1u), (words[0] >> 21) & 127u};
    op.end = (words[1] >> 18) & 1u;
    middle = count == 3u ? 1u : 0u;
    op.source[middle] = (struct sgx535_f32_operand){(enum sgx535_f32_bank)((words[0] >> 30) & 1u),
                                                    (words[0] >> 7) & 127u};
    if (count == 3u)
        op.source[0].number = (words[0] >> 14) & 127u;
    if (count >= 2u)
        op.source[middle + 1u] = (struct sgx535_f32_operand){
            (enum sgx535_f32_bank)((words[0] >> 28) & 1u), words[0] & 127u};
    if (sgx535_f32_encode(&op, canonical) != SGX535_F32_OK || !identical(canonical, words))
        return SGX535_F32_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_F32_OK;
}

enum sgx535_store_status sgx535_store_encode(const struct sgx535_store *input, uint32_t words[2])
{
    struct sgx535_store op;
    uint32_t result[2];
    if (!input || !words)
        return SGX535_STORE_ERR_ARGUMENT;
    op = *input;
    if ((unsigned)op.opcode > 1u)
        return SGX535_STORE_ERR_UNSUPPORTED;
    if (op.base_temp > 123u || op.offset_temp > 123u || op.data_temp > 123u)
        return SGX535_STORE_ERR_RANGE;
    result[0] = op.base_temp << 14 | op.offset_temp << 7 | op.data_temp;
    result[1] = UINT32_C(0xf0a00000) | (uint32_t)op.opcode << 10;
    memcpy(words, result, sizeof(result));
    return SGX535_STORE_OK;
}

enum sgx535_store_status sgx535_store_decode(const uint32_t words[2], struct sgx535_store *output)
{
    struct sgx535_store op;
    uint32_t canonical[2];
    if (!words || !output)
        return SGX535_STORE_ERR_ARGUMENT;
    op = (struct sgx535_store){(enum sgx535_store_opcode)((words[1] >> 10) & 1u),
                               (words[0] >> 14) & 127u, (words[0] >> 7) & 127u, words[0] & 127u};
    if (sgx535_store_encode(&op, canonical) != SGX535_STORE_OK || !identical(canonical, words))
        return SGX535_STORE_ERR_UNSUPPORTED;
    *output = op;
    return SGX535_STORE_OK;
}

enum sgx535_shader_status sgx535_shader_encode_argb(uint32_t argb, uint32_t words[2])
{
    const struct sgx535_limm op = {SGX535_LIMM_OUTPUT, 0, argb, 1};
    if (!words)
        return SGX535_SHADER_ERR_ARGUMENT;
    return sgx535_limm_encode(&op, words) == SGX535_LIMM_OK ? SGX535_SHADER_OK
                                                            : SGX535_SHADER_ERR_UNSUPPORTED;
}

enum sgx535_shader_status sgx535_shader_decode_argb(const uint32_t words[2], uint32_t *argb)
{
    struct sgx535_limm op;
    if (!words || !argb)
        return SGX535_SHADER_ERR_ARGUMENT;
    if (sgx535_limm_decode(words, &op) != SGX535_LIMM_OK || op.bank != SGX535_LIMM_OUTPUT ||
        op.register_number != 0u || op.end != 1u)
        return SGX535_SHADER_ERR_UNSUPPORTED;
    *argb = op.immediate;
    return SGX535_SHADER_OK;
}

enum sgx535_shader_status sgx535_shader_pack_le(const uint32_t words[2], unsigned char *bytes,
                                                size_t capacity)
{
    unsigned char packed[8];
    uint32_t ignored;
    unsigned i;
    if (!words || !bytes)
        return SGX535_SHADER_ERR_ARGUMENT;
    if (capacity < sizeof(packed))
        return SGX535_SHADER_ERR_BOUNDS;
    if (sgx535_shader_decode_argb(words, &ignored) != SGX535_SHADER_OK)
        return SGX535_SHADER_ERR_UNSUPPORTED;
    for (i = 0; i < sizeof(packed); ++i)
        packed[i] = (unsigned char)(words[i / 4u] >> (i % 4u * 8u));
    memcpy(bytes, packed, sizeof(packed));
    return SGX535_SHADER_OK;
}

enum sgx535_shader_status sgx535_shader_unpack_le(const unsigned char *bytes, size_t capacity,
                                                  uint32_t words[2])
{
    uint32_t unpacked[2] = {0}, ignored;
    unsigned i;
    if (!bytes || !words)
        return SGX535_SHADER_ERR_ARGUMENT;
    if (capacity < 8u)
        return SGX535_SHADER_ERR_BOUNDS;
    for (i = 0; i < 8u; ++i)
        unpacked[i / 4u] |= (uint32_t)bytes[i] << (i % 4u * 8u);
    if (sgx535_shader_decode_argb(unpacked, &ignored) != SGX535_SHADER_OK)
        return SGX535_SHADER_ERR_UNSUPPORTED;
    memcpy(words, unpacked, sizeof(unpacked));
    return SGX535_SHADER_OK;
}
