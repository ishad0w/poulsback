// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// Interface contract: docs/native-2d.md

#ifndef SGX535_2D_ENCODER_H
#define SGX535_2D_ENCODER_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* One fill or one copy as the SGX535 2D core's slave-port words: CPU only.
 * The encoder validates, clips and encodes; it maps, submits and reads
 * nothing. Every command is FENCE ... FLUSH and stands alone; its size word is
 * the second-last word. */
#define SGX535_2D_RASTER_FILL_WORDS 8u
#define SGX535_2D_RASTER_COPY_WORDS 11u
/* Offsets are relative to TWOD_REQ_BASE, inside this domain. */
#define SGX535_2D_RASTER_ADDRESS_BYTES 0x10000000ull
/* The core reads the 15-bit stride field as signed. */
#define SGX535_2D_RASTER_MAX_STRIDE 16383u

enum sgx535_2d_raster_format {
    SGX535_2D_RASTER_ARGB8888 = 1,
    SGX535_2D_RASTER_XRGB8888 = 2,
    SGX535_2D_RASTER_RGB565 = 3
};

/* X11 GX functions in protocol order. */
enum sgx535_2d_raster_rop {
    SGX535_2D_RASTER_CLEAR = 0,
    SGX535_2D_RASTER_AND = 1,
    SGX535_2D_RASTER_AND_REVERSE = 2,
    SGX535_2D_RASTER_COPY = 3,
    SGX535_2D_RASTER_AND_INVERTED = 4,
    SGX535_2D_RASTER_NOOP = 5,
    SGX535_2D_RASTER_XOR = 6,
    SGX535_2D_RASTER_OR = 7,
    SGX535_2D_RASTER_NOR = 8,
    SGX535_2D_RASTER_EQUIV = 9,
    SGX535_2D_RASTER_INVERT = 10,
    SGX535_2D_RASTER_OR_REVERSE = 11,
    SGX535_2D_RASTER_COPY_INVERTED = 12,
    SGX535_2D_RASTER_OR_INVERTED = 13,
    SGX535_2D_RASTER_NAND = 14,
    SGX535_2D_RASTER_SET = 15
};

enum sgx535_2d_raster_status {
    SGX535_2D_RASTER_OK = 0,
    SGX535_2D_RASTER_INVALID_ARGUMENT = 1,
    SGX535_2D_RASTER_UNSUPPORTED = 2,
    SGX535_2D_RASTER_OUT_OF_RANGE = 3,
    SGX535_2D_RASTER_BAD_ALIGNMENT = 4,
    SGX535_2D_RASTER_UNPROVEN_ALIAS = 5,
    SGX535_2D_RASTER_NO_SPACE = 6
};

/* storage_id names the backing object (trusted caller; 0 is refused): equal
 * ids are one object, different ids never alias. offset_bytes is pixel
 * (0, 0) relative to TWOD_REQ_BASE; allocation_bytes are accessible from it. */
struct sgx535_2d_raster_surface {
    uint64_t storage_id;
    uint64_t offset_bytes;
    uint64_t allocation_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    enum sgx535_2d_raster_format format;
};

/* Half-open [x, x + width) x [y, y + height); empty when either extent is 0. */
struct sgx535_2d_raster_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

/* clip may be NULL; it clips destination coordinates. On OK, *written is
 * SGX535_2D_RASTER_FILL_WORDS (or _COPY_WORDS) words at the start of out, or 0
 * with out untouched when nothing is left after clipping (any capacity). On
 * any error out and *written are unchanged. Inputs, out and written must not
 * overlap. */
enum sgx535_2d_raster_status sgx535_2d_raster_fill(const struct sgx535_2d_raster_surface *dst,
                                                   const struct sgx535_2d_raster_rect *rect,
                                                   const struct sgx535_2d_raster_rect *clip,
                                                   uint32_t pixel, enum sgx535_2d_raster_rop rop,
                                                   uint32_t *out, size_t capacity, size_t *written);

/* Same storage_id: one view only, copied as if through a temporary. */
enum sgx535_2d_raster_status sgx535_2d_raster_copy(const struct sgx535_2d_raster_surface *src,
                                                   const struct sgx535_2d_raster_rect *src_rect,
                                                   const struct sgx535_2d_raster_surface *dst,
                                                   const struct sgx535_2d_raster_rect *dst_rect,
                                                   const struct sgx535_2d_raster_rect *clip,
                                                   enum sgx535_2d_raster_rop rop, uint32_t *out,
                                                   size_t capacity, size_t *written);

#endif
