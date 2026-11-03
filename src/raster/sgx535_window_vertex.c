// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_window_vertex.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int window_component(uint32_t word, uint32_t maximum)
{
    uint32_t magnitude = word & 0x7fffffffu;
    return magnitude <= maximum && (!(word >> 31) || !magnitude);
}
enum sgx535_window_vertex_status
sgx535_window_color0_encode(const struct sgx535_window_color0_triangle *in, uint32_t output[24])
{
    uint32_t result[24], i, j;
    if (!in || !output)
        return SGX535_WINDOW_VERTEX_INVALID;
    if (in->replicated_flat > 1u)
        return SGX535_WINDOW_VERTEX_UNSUPPORTED;
    for (i = 0; i < 3; i++) {
        const uint32_t *p = in->window_xyz_reciprocal_w[i];
        if (!window_component(p[0], 0x42800000u) || !window_component(p[1], 0x42800000u) ||
            !window_component(p[2], 0x3f800000u) || !p[3] || p[3] >= 0x7f800000u)
            return SGX535_WINDOW_VERTEX_UNSUPPORTED;
        for (j = 0; j < 4; j++) {
            if (in->rgba[i][j] > 0x3f800000u ||
                (in->replicated_flat && in->rgba[i][j] != in->rgba[0][j]))
                return SGX535_WINDOW_VERTEX_UNSUPPORTED;
            result[i * 8u + j] = p[j];
            result[i * 8u + 4u + j] = in->rgba[i][j];
        }
    }
    memcpy(output, result, sizeof(result));
    return SGX535_WINDOW_VERTEX_OK;
}
