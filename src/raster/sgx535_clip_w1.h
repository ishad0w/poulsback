// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_CLIP_W1_H
#define SGX535_CLIP_W1_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define SGX535_CLIP_W1_MAX_DIMENSION 4096u

enum sgx535_clip_w1_status {
    SGX535_CLIP_W1_OK = 0,
    SGX535_CLIP_W1_INVALID,
    SGX535_CLIP_W1_UNSUPPORTED
};

/* Internal mathematical candidate, not a hardware VBO/PDS wire contract.
 * The provider must separately qualify coordinate origin, vertex input and
 * storage layout. All words are IEEE binary32 bit patterns. Only finite XYZ
 * in [-1,1], W exactly +1, origin (0,0) and default depth [0,1] are supported.
 * Computes exact rational ((X+1)*width/2, (1-Y)*height/2, (Z+1)/2, 1), each
 * rounded once to binary32, nearest with ties to even. No FPU is used.
 * Subnormal inputs and signed zero are accepted. Output zero is positive.
 * On failure output is untouched; clip and output may be the same array.
 */
enum sgx535_clip_w1_status sgx535_clip_w1_encode(const uint32_t clip[4], uint32_t width,
                                                 uint32_t height, uint32_t window[4]);

#endif
