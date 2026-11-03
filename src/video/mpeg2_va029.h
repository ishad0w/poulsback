// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

#ifndef MPEG2_VA029_H
#define MPEG2_VA029_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libva 0.29 MPEG-2 descriptors (i386, little-endian) checked and turned into
 * Poulsbo MSVDX register words. Pure CPU code: no allocation, I/O or mutable
 * static state. Inputs may be unaligned. A refusal writes nothing. */

#define MPEG2_VA029_PICTURE_BYTES 24u
#define MPEG2_VA029_IQ_BYTES 272u
#define MPEG2_VA029_SLICE_BYTES 28u
#define MPEG2_VA029_MAX_DIMENSION 4096u
#define MPEG2_VA029_I_PICTURE 1u
#define MPEG2_VA029_FRAME_PICTURE 3u
/* Poulsbo shift-register setup command, no flags; followed by the macroblock
 * bit offset and the slice size in bytes. */
#define MPEG2_VA029_SR_SETUP 0xb0000000u

enum mpeg2_va029_status {
    MPEG2_VA029_OK,
    MPEG2_VA029_ARGUMENT,
    MPEG2_VA029_LENGTH,
    MPEG2_VA029_UNSUPPORTED,
    MPEG2_VA029_RANGE
};

/* Raw descriptor fields, nothing validated. The extension word keeps its
 * unknown bits 31:13; the named fields are its bits 12:0. */
struct mpeg2_va029_picture {
    uint16_t horizontal_size, vertical_size;
    uint32_t forward_reference, backward_reference, coding_type, f_code, extension;
    uint8_t intra_dc_precision, picture_structure, top_field_first, frame_pred_frame_dct,
        concealment_motion_vectors, q_scale_type, intra_vlc_format, alternate_scan,
        repeat_first_field, progressive_frame, first_field;
};
struct mpeg2_va029_picture_words {
    struct mpeg2_va029_picture picture;
    uint32_t fe_pps0, fe_pps1, be_pps0, be_pps1, be_pps2, coded_picture_size;
    uint32_t coded_width, coded_height, mb_columns, mb_rows;
    uint16_t first_mb, last_mb; /* last: (rows - 1) << 8 | (columns - 1) */
};
struct mpeg2_va029_matrices {
    uint8_t intra[64], non_intra[64]; /* raster order */
};
struct mpeg2_va029_slice {
    uint32_t data_size, data_offset, data_flag, macroblock_offset, vertical_position,
        quantiser_scale_code, intra_slice_flag;
};

enum mpeg2_va029_status mpeg2_va029_picture_read(const uint8_t *in, size_t bytes,
                                                 struct mpeg2_va029_picture *out);
/* progressive is the decode context's flag, not the picture's progressive_frame.
 * Limits are 1..MPEG2_VA029_MAX_DIMENSION; the 4096 bound is CPU policy, the
 * hardware's 7-bit MB counts stop at 2048 pixels. Coding types 1..3. */
enum mpeg2_va029_status mpeg2_va029_picture_words(const uint8_t *in, size_t bytes, bool progressive,
                                                  uint32_t max_width, uint32_t max_height,
                                                  struct mpeg2_va029_picture_words *out);
/* Starts from previous; a nonzero load flag replaces that matrix (zigzag to
 * raster). Chroma matrices are refused. All three may overlap. */
enum mpeg2_va029_status mpeg2_va029_iq_update(const uint8_t *in, size_t bytes,
                                              const struct mpeg2_va029_matrices *previous,
                                              struct mpeg2_va029_matrices *out);
/* Checks the slice's span against a slice-data buffer of data_bytes bytes; the
 * data itself is not read. Partial slices are refused. */
enum mpeg2_va029_status mpeg2_va029_slice_check(const uint8_t *in, size_t bytes, size_t data_bytes,
                                                struct mpeg2_va029_slice *out);

struct mpeg2_va029_slice_input {
    const uint8_t *picture, *iq, *slice;
    size_t picture_bytes, iq_bytes, slice_bytes, slice_data_bytes;
    const struct mpeg2_va029_matrices *previous;
    uint32_t max_width, max_height;
    bool progressive;
};
struct mpeg2_va029_intra_slice {
    struct mpeg2_va029_picture_words picture;
    struct mpeg2_va029_matrices matrices; /* updated; the caller decides to keep them */
    struct mpeg2_va029_slice slice;
    uint32_t sr_setup[3];
};
/* One whole intra slice of an I frame picture. Stages in order: arguments,
 * picture words, picture scope, slice check, slice scope, IQ update. */
enum mpeg2_va029_status mpeg2_va029_prepare_intra_slice(const struct mpeg2_va029_slice_input *in,
                                                        struct mpeg2_va029_intra_slice *out);
#endif
