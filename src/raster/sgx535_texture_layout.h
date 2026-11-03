// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TEXTURE_LAYOUT_H
#define SGX535_TEXTURE_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* SGX535 texture memory as EMGD stores it (docs/native-3d.md#texture-memory): where each level,
 * face and plane lives, and the
 * conversion of linear texel rows into it and back. CPU only: no BO, Gallium
 * or kernel state; little-endian (x86) memory.
 *
 * Twiddled (POT 2D, cube, 3D): element bits interleave y, x, z from the lowest
 * bit, an axis dropping out when its bits run out, so a non-square level is
 * min(w, h) squares along the long axis. An element is a texel, or a 4x4 block
 * for DXT. Stride (NPOT 2D, depth formats, or on request): rows padded to 32
 * texels. Levels follow each other with no padding; a mipmapped texture
 * stores the whole chain down to 1x1 (EMGD allocates it whatever the uploaded
 * levels). Cube faces follow in GL order (+X, -X, +Y, -Y, +Z, -Z), each with
 * all its levels. RGBA16F and RGBA32F are split into 2 or 4 planes, each a
 * complete texture of one F1616 or F32 element, 64-byte aligned. */

#define SGX535_TEXTURE_MAX_LEVELS 12u /* 2048 down to 1 */
#define SGX535_TEXTURE_MAX_SIZE 2048u /* 2D and cube */
#define SGX535_TEXTURE_MAX_SIZE_3D 256u

enum sgx535_texture_status { SGX535_TEXTURE_OK = 0, SGX535_TEXTURE_INVALID };

enum sgx535_texture_target { SGX535_TEXTURE_2D, SGX535_TEXTURE_CUBE, SGX535_TEXTURE_3D };

/* Texel data an upload takes or a readback writes, named as Gallium's formats
 * (component order from the lowest bit for packed formats, from the lowest
 * byte otherwise). sRGB formats use their UNORM twin; L8, A8, I8 and R8 are
 * U8, L8A8 and R8G8 are U8U8. */
enum sgx535_texel_format {
    SGX535_TEXEL_NONE = 0,
    SGX535_TEXEL_R8G8B8A8_UNORM,
    SGX535_TEXEL_B8G8R8A8_UNORM,
    SGX535_TEXEL_R8G8B8X8_UNORM,
    SGX535_TEXEL_B8G8R8X8_UNORM,
    SGX535_TEXEL_B5G6R5_UNORM,
    SGX535_TEXEL_B4G4R4A4_UNORM,
    SGX535_TEXEL_A4B4G4R4_UNORM,
    SGX535_TEXEL_B5G5R5A1_UNORM,
    SGX535_TEXEL_A1B5G5R5_UNORM,
    SGX535_TEXEL_U8,
    SGX535_TEXEL_U8U8,
    SGX535_TEXEL_Z16_UNORM,
    SGX535_TEXEL_Z24X8_UNORM,
    SGX535_TEXEL_Z32_FLOAT,
    SGX535_TEXEL_R16G16B16A16_FLOAT,
    SGX535_TEXEL_R32G32B32A32_FLOAT,
    SGX535_TEXEL_DXT1,
    SGX535_TEXEL_DXT3,
    SGX535_TEXEL_DXT5,
    SGX535_TEXEL_FORMAT_COUNT
};

/* DOUTT1 TEXTYPE (31:29) and TEXFORMAT (28:24) of the stored form. */
enum sgx535_texture_type {
    SGX535_TEXTYPE_2D = 0,
    SGX535_TEXTYPE_3D = 1,
    SGX535_TEXTYPE_CEM = 2,
    SGX535_TEXTYPE_STRIDE = 3
};

enum sgx535_texture_hw_format {
    SGX535_TEXFORMAT_U8 = 0,
    SGX535_TEXFORMAT_A4R4G4B4 = 2,
    SGX535_TEXFORMAT_A1R5G5B5 = 4,
    SGX535_TEXFORMAT_R5G6B5 = 5,
    SGX535_TEXFORMAT_U88 = 7,
    SGX535_TEXFORMAT_U16 = 9,
    SGX535_TEXFORMAT_B8G8R8A8 = 12,
    SGX535_TEXFORMAT_F1616 = 17,
    SGX535_TEXFORMAT_F32 = 18,
    SGX535_TEXFORMAT_DXT1 = 20,
    SGX535_TEXFORMAT_DXT3 = 21, /* by elimination: EMGD used 20 for DXT1, 22 for DXT5 */
    SGX535_TEXFORMAT_DXT5 = 22
};

struct sgx535_texture_layout {
    enum sgx535_texture_target target;
    enum sgx535_texel_format format;
    enum sgx535_texture_type type;
    enum sgx535_texture_hw_format hw_format;
    bool chan_replicate; /* DOUTT0 CHANREPLICATE: U8 and U88 */
    uint32_t width;      /* level 0, texels */
    uint32_t height;
    uint32_t depth;         /* 3D slices, else 1 */
    uint32_t levels;        /* 1, or the whole chain */
    uint32_t faces;         /* 6 for a cube, else 1 */
    uint32_t planes;        /* 1, 2 (RGBA16F) or 4 (RGBA32F) */
    uint32_t block;         /* texels per element side: 4 for DXT, else 1 */
    uint32_t element_bytes; /* one plane's element */
    uint32_t level_offset[SGX535_TEXTURE_MAX_LEVELS]; /* within a face */
    uint32_t level_bytes[SGX535_TEXTURE_MAX_LEVELS];
    uint32_t level_pitch[SGX535_TEXTURE_MAX_LEVELS]; /* stride row bytes, 0 when twiddled */
    uint32_t face_bytes;   /* all levels of one face: the cube face step */
    uint32_t plane_bytes;  /* the plane step */
    uint32_t border_bytes; /* the border area in front of the map, 0 without one */
    uint32_t size;         /* bytes of storage */
};

/* A region of one level: x, y in texels; z and depth in slices (3D) or faces
 * (cube, GL order), as Gallium's pipe_box. DXT regions start on a block and
 * end on a block or the level's edge. */
struct sgx535_texture_box {
    uint32_t x, y, z;
    uint32_t width, height, depth;
};

/* The layout of a texture: last_level 0 stores one level, any other the
 * whole chain. A 2D texture is stride when stride is set, when a side is not
 * a power of two, or for depth formats (as EMGD); cube and 3D need power-of-
 * two sides. Refused: sizes above the limits, NPOT cube or 3D, stride DXT,
 * depth formats outside 2D. */
enum sgx535_texture_status
sgx535_texture_layout_init(struct sgx535_texture_layout *layout, enum sgx535_texture_target target,
                           enum sgx535_texel_format format, uint32_t width, uint32_t height,
                           uint32_t depth, uint32_t last_level, bool stride);

/* A border area in front of the map (docs/native-3d.md#texture-memory):
 * 48 + 8N texels for a square N x N texture of one level and plane, twiddled
 * (304 texels before a 32 x 32 map). The map and the size move
 * by it; DOUTT2 of a CLAMP_TO_BORDER sampler names the area's start. Refused:
 * other textures (mipmapped and non-square border maps have no capture). */
enum sgx535_texture_status sgx535_texture_layout_border(struct sgx535_texture_layout *layout);

/* The border colour (one element in the stored format, element_bytes) into
 * the area's slots for the map's size: the four corners at 4 log2(N) and the
 * 4N edge texels at 52 + 4(N - 1). Other texels are untouched (EMGD leaves
 * the other sizes' slots zero). */
enum sgx535_texture_status sgx535_texture_border_fill(const struct sgx535_texture_layout *layout,
                                                      void *storage, const void *element);

/* Byte offset of a surface (DOUTT2 = storage address + offset): plane, cube
 * face (0 otherwise) and level. */
uint32_t sgx535_texture_offset(const struct sgx535_texture_layout *layout, uint32_t plane,
                               uint32_t face, uint32_t level);

/* Writes box of level from linear src rows (src_stride bytes apart; slices or
 * faces src_layer_stride apart; DXT rows are block rows) into storage
 * (layout->size bytes). src_format must have the layout's stored format; X
 * channels are stored as 255. Bytes outside the box are untouched. */
enum sgx535_texture_status
sgx535_texture_upload(const struct sgx535_texture_layout *layout, void *storage, uint32_t level,
                      const struct sgx535_texture_box *box, enum sgx535_texel_format src_format,
                      const void *src, uint32_t src_stride, uint32_t src_layer_stride);

/* Reads box of level into linear dst rows: in any format with the layout's
 * stored format (the exact inverse of an upload), or, for 16-bit colour,
 * R8G8B8A8_UNORM or B8G8R8A8_UNORM with each channel expanded by bit
 * replication. */
enum sgx535_texture_status sgx535_texture_readback(const struct sgx535_texture_layout *layout,
                                                   const void *storage, uint32_t level,
                                                   const struct sgx535_texture_box *box,
                                                   enum sgx535_texel_format dst_format, void *dst,
                                                   uint32_t dst_stride, uint32_t dst_layer_stride);

#endif
