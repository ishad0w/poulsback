// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_clip_w1.h"

/* A binary32 in [-1,1] is an integer multiple of 2^-149. Six little-endian
 * limbs hold 1 +/- input and multiplication by the bounded viewport extent.
 * The final denominator is 2^150. This preserves even the least subnormal
 * input through the single final rounding, without host floating arithmetic.
 */
#define LIMBS 6u
struct wide {
    uint32_t limb[LIMBS];
};

static uint32_t bit(const struct wide *value, unsigned index)
{
    return (value->limb[index / 32u] >> (index % 32u)) & 1u;
}

static uint32_t round_binary32(const struct wide *value)
{
    unsigned high = LIMBS * 32u, shift, index;
    uint32_t significant = 0, sticky = 0, exponent;

    while (high && !bit(value, high - 1u))
        high--;
    if (!high)
        return 0;
    high--;
    /* A nonzero 1 +/- input is >= 2^-24, so the output is normal.
	 * high >= 125; multiplication by an extent <=4096 gives high <=162.
	 */
    shift = high - 23u;
    for (index = 0; index < 24u; index++)
        significant |= bit(value, shift + index) << index;
    for (index = 0; index + 1u < shift; index++)
        sticky |= bit(value, index);
    if (bit(value, shift - 1u) && (sticky || (significant & 1u)))
        significant++;
    if (significant == 0x1000000u) {
        significant >>= 1;
        high++;
    }
    exponent = high - 23u; /* biased exponent = high - 150 + 127 */
    return (exponent << 23) | (significant & 0x7fffffu);
}

static uint32_t coordinate(uint32_t input, uint32_t extent, unsigned invert)
{
    struct wide magnitude = {{0}}, value = {{0}};
    uint32_t exponent = (input >> 23) & 0xffu;
    uint32_t significant = input & 0x7fffffu;
    uint64_t carry = 0;
    unsigned shift = 0, index;
    unsigned subtract = ((input >> 31) ^ invert) & 1u;

    if (exponent) {
        significant |= 0x800000u;
        shift = exponent - 1u;
    }
    for (index = 0; index < 24u; index++)
        if ((significant >> index) & 1u)
            magnitude.limb[(shift + index) / 32u] |= 1u << ((shift + index) % 32u);
    value.limb[149u / 32u] = 1u << (149u % 32u);
    for (index = 0; index < LIMBS; index++) {
        if (subtract) {
            uint64_t rhs = (uint64_t)magnitude.limb[index] + carry;

            carry = (uint64_t)value.limb[index] < rhs;
            value.limb[index] = (uint32_t)((uint64_t)value.limb[index] - rhs);
        } else {
            uint64_t sum = (uint64_t)value.limb[index] + magnitude.limb[index] + carry;

            value.limb[index] = (uint32_t)sum;
            carry = sum >> 32;
        }
    }
    carry = 0;
    for (index = 0; index < LIMBS; index++) {
        uint64_t product = (uint64_t)value.limb[index] * extent + carry;

        value.limb[index] = (uint32_t)product;
        carry = product >> 32;
    }
    return round_binary32(&value);
}

enum sgx535_clip_w1_status sgx535_clip_w1_encode(const uint32_t clip[4], uint32_t width,
                                                 uint32_t height, uint32_t window[4])
{
    uint32_t result[4];
    unsigned index;

    if (!clip || !window || !width || !height || width > SGX535_CLIP_W1_MAX_DIMENSION ||
        height > SGX535_CLIP_W1_MAX_DIMENSION)
        return SGX535_CLIP_W1_INVALID;
    if (clip[3] != 0x3f800000u)
        return SGX535_CLIP_W1_UNSUPPORTED;
    for (index = 0; index < 3u; index++)
        if ((clip[index] & 0x7fffffffu) > 0x3f800000u)
            return SGX535_CLIP_W1_UNSUPPORTED;
    result[0] = coordinate(clip[0], width, 0);
    result[1] = coordinate(clip[1], height, 1);
    result[2] = coordinate(clip[2], 1u, 0);
    result[3] = 0x3f800000u;
    for (index = 0; index < 4u; index++)
        window[index] = result[index];
    return SGX535_CLIP_W1_OK;
}
