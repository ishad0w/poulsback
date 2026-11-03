// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

#include "mpeg2_va029.h"
#include <string.h>

/* H.262 zigzag scan rank of each raster position, rows top to bottom. */
static const uint8_t zigzag_rank[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
    41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
    46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint8_t bits(uint32_t word, unsigned low, unsigned width)
{
    return (uint8_t)(word >> low & ((1u << width) - 1u));
}
static int limit_valid(uint32_t limit)
{
    return limit && limit <= MPEG2_VA029_MAX_DIMENSION;
}

enum mpeg2_va029_status mpeg2_va029_picture_read(const uint8_t *in, size_t bytes,
                                                 struct mpeg2_va029_picture *out)
{
    struct mpeg2_va029_picture p;
    if (!in || !out)
        return MPEG2_VA029_ARGUMENT;
    if (bytes != MPEG2_VA029_PICTURE_BYTES)
        return MPEG2_VA029_LENGTH;
    memset(&p, 0, sizeof(p));
    p.horizontal_size = (uint16_t)(in[0] | in[1] << 8);
    p.vertical_size = (uint16_t)(in[2] | in[3] << 8);
    p.forward_reference = le32(in + 4);
    p.backward_reference = le32(in + 8);
    p.coding_type = le32(in + 12);
    p.f_code = le32(in + 16);
    p.extension = le32(in + 20);
    p.intra_dc_precision = bits(p.extension, 0, 2);
    p.picture_structure = bits(p.extension, 2, 2);
    p.top_field_first = bits(p.extension, 4, 1);
    p.frame_pred_frame_dct = bits(p.extension, 5, 1);
    p.concealment_motion_vectors = bits(p.extension, 6, 1);
    p.q_scale_type = bits(p.extension, 7, 1);
    p.intra_vlc_format = bits(p.extension, 8, 1);
    p.alternate_scan = bits(p.extension, 9, 1);
    p.repeat_first_field = bits(p.extension, 10, 1);
    p.progressive_frame = bits(p.extension, 11, 1);
    p.first_field = bits(p.extension, 12, 1);
    memcpy(out, &p, sizeof(p));
    return MPEG2_VA029_OK;
}

enum mpeg2_va029_status mpeg2_va029_picture_words(const uint8_t *in, size_t bytes, bool progressive,
                                                  uint32_t max_width, uint32_t max_height,
                                                  struct mpeg2_va029_picture_words *out)
{
    struct mpeg2_va029_picture_words w;
    const struct mpeg2_va029_picture *p = &w.picture;
    enum mpeg2_va029_status s;
    uint32_t f_code;
    if (!out || !limit_valid(max_width) || !limit_valid(max_height))
        return MPEG2_VA029_ARGUMENT;
    memset(&w, 0, sizeof(w));
    s = mpeg2_va029_picture_read(in, bytes, &w.picture);
    if (s != MPEG2_VA029_OK)
        return s;
    if (p->coding_type < 1 || p->coding_type > 3)
        return MPEG2_VA029_UNSUPPORTED;
    if (!p->horizontal_size || p->horizontal_size > max_width || !p->vertical_size ||
        p->vertical_size > max_height)
        return MPEG2_VA029_RANGE;
    /* Interlaced contexts round the height to a whole macroblock pair. */
    w.mb_columns = (p->horizontal_size + 15u) / 16u;
    w.mb_rows = progressive ? (p->vertical_size + 15u) / 16u : (p->vertical_size + 31u) / 32u * 2u;
    w.coded_width = 16u * w.mb_columns;
    w.coded_height = 16u * w.mb_rows;
    if (w.coded_width > MPEG2_VA029_MAX_DIMENSION || w.coded_height > MPEG2_VA029_MAX_DIMENSION)
        return MPEG2_VA029_RANGE;
    f_code = p->f_code & 0xffffu;
    w.fe_pps0 = (uint32_t)p->alternate_scan << 9 | (uint32_t)p->q_scale_type << 8 |
                (uint32_t)p->intra_dc_precision << 6 | (uint32_t)p->top_field_first << 5 |
                (uint32_t)p->concealment_motion_vectors << 4 |
                (uint32_t)p->frame_pred_frame_dct << 3 | (uint32_t)p->picture_structure << 1 |
                p->intra_vlc_format;
    w.fe_pps1 = p->coding_type << 16 | f_code;
    w.be_pps0 = (uint32_t)p->frame_pred_frame_dct << 9 | (uint32_t)p->q_scale_type << 8 |
                (uint32_t)p->intra_dc_precision << 6 | (uint32_t)p->alternate_scan << 3;
    w.be_pps1 = f_code;
    w.be_pps2 = p->coding_type << 4 | (uint32_t)p->top_field_first << 3 |
                (uint32_t)p->concealment_motion_vectors << 2 | p->picture_structure;
    w.coded_picture_size = (w.coded_height - 1u) << 12 | (w.coded_width - 1u);
    w.first_mb = 0;
    w.last_mb = (uint16_t)((w.mb_rows - 1u) << 8 | (w.mb_columns - 1u));
    memcpy(out, &w, sizeof(w));
    return MPEG2_VA029_OK;
}

enum mpeg2_va029_status mpeg2_va029_iq_update(const uint8_t *in, size_t bytes,
                                              const struct mpeg2_va029_matrices *previous,
                                              struct mpeg2_va029_matrices *out)
{
    struct mpeg2_va029_matrices m;
    unsigned i;
    if (!in || !previous || !out)
        return MPEG2_VA029_ARGUMENT;
    if (bytes != MPEG2_VA029_IQ_BYTES)
        return MPEG2_VA029_LENGTH;
    if (le32(in + 8) || le32(in + 12))
        return MPEG2_VA029_UNSUPPORTED;
    memcpy(&m, previous, sizeof(m));
    if (le32(in))
        for (i = 0; i < 64; ++i)
            m.intra[i] = in[16 + zigzag_rank[i]];
    if (le32(in + 4))
        for (i = 0; i < 64; ++i)
            m.non_intra[i] = in[80 + zigzag_rank[i]];
    memcpy(out, &m, sizeof(m));
    return MPEG2_VA029_OK;
}

enum mpeg2_va029_status mpeg2_va029_slice_check(const uint8_t *in, size_t bytes, size_t data_bytes,
                                                struct mpeg2_va029_slice *out)
{
    struct mpeg2_va029_slice s;
    if (!in || !out)
        return MPEG2_VA029_ARGUMENT;
    if (bytes != MPEG2_VA029_SLICE_BYTES)
        return MPEG2_VA029_LENGTH;
    s.data_size = le32(in);
    s.data_offset = le32(in + 4);
    s.data_flag = le32(in + 8);
    s.macroblock_offset = le32(in + 12);
    s.vertical_position = le32(in + 16);
    s.quantiser_scale_code = le32(in + 20);
    s.intra_slice_flag = le32(in + 24);
    if (s.data_flag)
        return MPEG2_VA029_UNSUPPORTED;
    if (!s.data_size || s.data_offset > data_bytes || s.data_size > data_bytes - s.data_offset ||
        s.macroblock_offset >= (uint64_t)s.data_size * 8u)
        return MPEG2_VA029_RANGE;
    memcpy(out, &s, sizeof(s));
    return MPEG2_VA029_OK;
}

enum mpeg2_va029_status mpeg2_va029_prepare_intra_slice(const struct mpeg2_va029_slice_input *in,
                                                        struct mpeg2_va029_intra_slice *out)
{
    struct mpeg2_va029_intra_slice r;
    enum mpeg2_va029_status s;
    if (!in || !out || !in->picture || !in->iq || !in->slice || !in->previous)
        return MPEG2_VA029_ARGUMENT;
    memset(&r, 0, sizeof(r));
    s = mpeg2_va029_picture_words(in->picture, in->picture_bytes, in->progressive, in->max_width,
                                  in->max_height, &r.picture);
    if (s != MPEG2_VA029_OK)
        return s;
    if (r.picture.picture.coding_type != MPEG2_VA029_I_PICTURE ||
        r.picture.picture.picture_structure != MPEG2_VA029_FRAME_PICTURE)
        return MPEG2_VA029_UNSUPPORTED;
    s = mpeg2_va029_slice_check(in->slice, in->slice_bytes, in->slice_data_bytes, &r.slice);
    if (s != MPEG2_VA029_OK)
        return s;
    if (r.slice.vertical_position >= r.picture.mb_rows || r.slice.vertical_position > 127 ||
        r.slice.quantiser_scale_code < 1 || r.slice.quantiser_scale_code > 31 ||
        r.slice.intra_slice_flag)
        return MPEG2_VA029_RANGE;
    s = mpeg2_va029_iq_update(in->iq, in->iq_bytes, in->previous, &r.matrices);
    if (s != MPEG2_VA029_OK)
        return s;
    r.sr_setup[0] = MPEG2_VA029_SR_SETUP;
    r.sr_setup[1] = r.slice.macroblock_offset;
    r.sr_setup[2] = r.slice.data_size;
    memcpy(out, &r, sizeof(r));
    return MPEG2_VA029_OK;
}
