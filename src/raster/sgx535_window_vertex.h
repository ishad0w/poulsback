// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_WINDOW_VERTEX_H
#define SGX535_WINDOW_VERTEX_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Coordinator-reserved internal packet/profile identities, not UAPI. */
#define SGX535_WINDOW_COLOR0_PACKET_VERSION 7u
#define SGX535_WINDOW_COLOR0_PROFILE 4u

/* Trusted CPU output of an already clipped/projected vertex pipeline. This is
 * a distinct representation from homogeneous clip positions. It supplies no
 * native interpolation, sample/edge, cache, publication or execution proof. */
struct sgx535_window_color0_triangle {
    uint32_t window_xyz_reciprocal_w[3][4];
    uint32_t rgba[3][4];
    uint32_t replicated_flat; /* Exactly 0/1; 1 requires bit-identical colors. */
};
enum sgx535_window_vertex_status {
    SGX535_WINDOW_VERTEX_OK,
    SGX535_WINDOW_VERTEX_INVALID,
    SGX535_WINDOW_VERTEX_UNSUPPORTED
};
/* Fixed 64x64/default-depth COLOR0 CPU candidate, matching the existing packet
 * record: windowXYZ and reciprocalW, then four COLOR0 words, 32 bytes/vertex.
 * XYZ must be finite and in [0,64],[0,64],[0,1]; signed zero is preserved.
 * ReciprocalW is any positive finite nonzero binary32. Colors retain the
 * existing finite nonnegative [0,1] policy, including refusal of negative zero.
 * Values are copied bit-for-bit: no divide, viewport transform, color scaling,
 * sample bias or FPU operation occurs. Typed producer provenance is required;
 * value checks cannot distinguish a mistakenly supplied homogeneous vector.
 * Source and destination may overlap; failure preserves all output bytes.
 * This interface does not reinterpret or relax any existing packet/UAPI. */
enum sgx535_window_vertex_status
sgx535_window_color0_encode(const struct sgx535_window_color0_triangle *, uint32_t output[24]);
#endif
