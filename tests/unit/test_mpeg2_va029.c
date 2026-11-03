// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

/* Every MPEG-2 descriptor vector of tests/vectors/msvdx-video against
 * mpeg2_va029. Descriptors sit at odd addresses; refusals must leave outputs
 * untouched; the IQ update is also run with overlapping input, previous and
 * output. Optional argument: the vector directory. */
#include "mpeg2_va029.h"
#include "msvdx_video_vectors.h"

static const char *const status_names[] = {"ok", "argument", "length", "unsupported", "range"};

static const char *status_name(enum mpeg2_va029_status s)
{
    VECTOR_REQUIRE((unsigned)s < sizeof(status_names) / sizeof(status_names[0]));
    return status_names[s];
}
static int flag(const struct vector_line *l, const char *key)
{
    uint32_t value = vector_u32(l, key);
    VECTOR_REQUIRE(value <= 1);
    return value != 0;
}
static void previous(const struct vector_line *l, struct mpeg2_va029_matrices *m)
{
    VECTOR_REQUIRE(vector_bytes(l, "prev_intra", m->intra, sizeof(m->intra)) == 64);
    VECTOR_REQUIRE(vector_bytes(l, "prev_non_intra", m->non_intra, sizeof(m->non_intra)) == 64);
}

static void emit_picture(struct vector_out *o, const struct mpeg2_va029_picture *p)
{
    emit(o,
         " hsize=%u vsize=%u fwd=0x%08x bwd=0x%08x type=0x%08x fcode=0x%08x ext=0x%08x"
         " dc_precision=%u structure=%u tff=%u fpfd=%u cmv=%u qst=%u ivlc=%u alt=%u rff=%u"
         " progressive=%u first_field=%u",
         (unsigned)p->horizontal_size, (unsigned)p->vertical_size, (unsigned)p->forward_reference,
         (unsigned)p->backward_reference, (unsigned)p->coding_type, (unsigned)p->f_code,
         (unsigned)p->extension, (unsigned)p->intra_dc_precision, (unsigned)p->picture_structure,
         (unsigned)p->top_field_first, (unsigned)p->frame_pred_frame_dct,
         (unsigned)p->concealment_motion_vectors, (unsigned)p->q_scale_type,
         (unsigned)p->intra_vlc_format, (unsigned)p->alternate_scan,
         (unsigned)p->repeat_first_field, (unsigned)p->progressive_frame, (unsigned)p->first_field);
}
static void emit_words(struct vector_out *o, const struct mpeg2_va029_picture_words *w)
{
    emit_picture(o, &w->picture);
    emit(o,
         " fe_pps0=0x%08x fe_pps1=0x%08x be_pps0=0x%08x be_pps1=0x%08x be_pps2=0x%08x"
         " coded_width=%u coded_height=%u mb_width=%u mb_height=%u coded_size=0x%08x"
         " mb_first=0x%04x mb_last=0x%04x",
         (unsigned)w->fe_pps0, (unsigned)w->fe_pps1, (unsigned)w->be_pps0, (unsigned)w->be_pps1,
         (unsigned)w->be_pps2, (unsigned)w->coded_width, (unsigned)w->coded_height,
         (unsigned)w->mb_columns, (unsigned)w->mb_rows, (unsigned)w->coded_picture_size,
         (unsigned)w->first_mb, (unsigned)w->last_mb);
}
static void emit_matrices(struct vector_out *o, const struct mpeg2_va029_matrices *m)
{
    emit_bytes(o, "intra", m->intra, sizeof(m->intra));
    emit_bytes(o, "non_intra", m->non_intra, sizeof(m->non_intra));
}
static void emit_slice(struct vector_out *o, const struct mpeg2_va029_slice *s)
{
    emit(o,
         " size=%u offset=%u flag=0x%08x mb_bit_offset=%u vertical=%u qscale=0x%08x"
         " intra_slice=0x%08x",
         (unsigned)s->data_size, (unsigned)s->data_offset, (unsigned)s->data_flag,
         (unsigned)s->macroblock_offset, (unsigned)s->vertical_position,
         (unsigned)s->quantiser_scale_code, (unsigned)s->intra_slice_flag);
}

static void picture_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    struct mpeg2_va029_picture p, before;
    struct vector_out o;
    enum mpeg2_va029_status s;
    memset(&p, 0xa5, sizeof(p));
    memcpy(&before, &p, sizeof(p));
    s = mpeg2_va029_picture_read(vector_null(l, "input") ? NULL : storage + 1, bytes,
                                 vector_null(l, "output") ? NULL : &p);
    vector_reset(&o);
    emit(&o, "status=%s", status_name(s));
    if (s == MPEG2_VA029_OK)
        emit_picture(&o, &p);
    vector_expect(f, l, &o);
    vector_check(f, s == MPEG2_VA029_OK || !memcmp(&p, &before, sizeof(p)),
                 "picture refusal changed the output");
}

static void normalize_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    struct mpeg2_va029_picture_words w, before;
    struct vector_out o;
    enum mpeg2_va029_status s;
    memset(&w, 0xa5, sizeof(w));
    memcpy(&before, &w, sizeof(w));
    s = mpeg2_va029_picture_words(vector_null(l, "input") ? NULL : storage + 1, bytes,
                                  flag(l, "progressive"), vector_u32(l, "max_width"),
                                  vector_u32(l, "max_height"),
                                  vector_null(l, "output") ? NULL : &w);
    vector_reset(&o);
    emit(&o, "status=%s", status_name(s));
    if (s == MPEG2_VA029_OK)
        emit_words(&o, &w);
    vector_expect(f, l, &o);
    vector_check(f, s == MPEG2_VA029_OK || !memcmp(&w, &before, sizeof(w)),
                 "picture-words refusal changed the output");
}

static void iq_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t storage[1 + 512];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    struct mpeg2_va029_matrices prev, out, before, same;
    union {
        struct mpeg2_va029_matrices m;
        uint8_t b[1 + 512];
    } alias;
    struct vector_out o;
    enum mpeg2_va029_status s;
    previous(l, &prev);
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    s = mpeg2_va029_iq_update(vector_null(l, "input") ? NULL : storage + 1, bytes,
                              vector_null(l, "previous") ? NULL : &prev,
                              vector_null(l, "output") ? NULL : &out);
    vector_reset(&o);
    emit(&o, "status=%s", status_name(s));
    if (s == MPEG2_VA029_OK)
        emit_matrices(&o, &out);
    vector_expect(f, l, &o);
    if (s != MPEG2_VA029_OK) {
        vector_check(f, !memcmp(&out, &before, sizeof(out)), "IQ refusal changed the output");
        return;
    }
    /* Previous and output are one object. */
    memcpy(&same, &prev, sizeof(same));
    s = mpeg2_va029_iq_update(storage + 1, bytes, &same, &same);
    vector_check(f, s == MPEG2_VA029_OK && !memcmp(&same, &out, sizeof(out)),
                 "IQ update differs when previous is the output");
    /* Descriptor at an odd address inside the output object. */
    VECTOR_REQUIRE(bytes < sizeof(alias.b));
    memset(&alias, 0x3c, sizeof(alias));
    memcpy(alias.b + 1, storage + 1, bytes);
    s = mpeg2_va029_iq_update(alias.b + 1, bytes, &prev, &alias.m);
    vector_check(f, s == MPEG2_VA029_OK && !memcmp(&alias.m, &out, sizeof(out)),
                 "IQ update differs when the descriptor overlaps the output");
}

static void slice_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    struct mpeg2_va029_slice out, before;
    struct vector_out o;
    enum mpeg2_va029_status s;
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    s = mpeg2_va029_slice_check(vector_null(l, "input") ? NULL : storage + 1, bytes,
                                (size_t)vector_integer(l, "data"),
                                vector_null(l, "output") ? NULL : &out);
    vector_reset(&o);
    emit(&o, "status=%s", status_name(s));
    if (s == MPEG2_VA029_OK)
        emit_slice(&o, &out);
    vector_expect(f, l, &o);
    vector_check(f, s == MPEG2_VA029_OK || !memcmp(&out, &before, sizeof(out)),
                 "slice refusal changed the output");
}

static void prepare_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t picture[1 + 64], iq[1 + 512], slice[1 + 64];
    struct mpeg2_va029_matrices prev;
    struct mpeg2_va029_slice_input in;
    struct mpeg2_va029_intra_slice out, before;
    struct vector_out o;
    enum mpeg2_va029_status s;
    (void)vector_text(l, "case");
    previous(l, &prev);
    memset(&in, 0, sizeof(in));
    in.picture_bytes = vector_bytes(l, "picture", picture + 1, sizeof(picture) - 1);
    in.iq_bytes = vector_bytes(l, "iq", iq + 1, sizeof(iq) - 1);
    in.slice_bytes = vector_bytes(l, "slice", slice + 1, sizeof(slice) - 1);
    in.picture = vector_null(l, "picture") ? NULL : picture + 1;
    in.iq = vector_null(l, "iq") ? NULL : iq + 1;
    in.slice = vector_null(l, "slice") ? NULL : slice + 1;
    in.previous = vector_null(l, "previous") ? NULL : &prev;
    in.slice_data_bytes = (size_t)vector_integer(l, "data");
    in.max_width = vector_u32(l, "max_width");
    in.max_height = vector_u32(l, "max_height");
    in.progressive = flag(l, "progressive");
    memset(&out, 0xa5, sizeof(out));
    memcpy(&before, &out, sizeof(out));
    s = mpeg2_va029_prepare_intra_slice(&in, vector_null(l, "output") ? NULL : &out);
    vector_reset(&o);
    emit(&o, "status=%s", status_name(s));
    if (s == MPEG2_VA029_OK) {
        emit_words(&o, &out.picture);
        emit_matrices(&o, &out.matrices);
        emit_slice(&o, &out.slice);
        emit(&o, " sr_setup=0x%08x,0x%08x,0x%08x", (unsigned)out.sr_setup[0],
             (unsigned)out.sr_setup[1], (unsigned)out.sr_setup[2]);
    }
    vector_expect(f, l, &o);
    vector_check(f, s == MPEG2_VA029_OK || !memcmp(&out, &before, sizeof(out)),
                 "prepare refusal changed the output");
    if (s == MPEG2_VA029_OK) {
        memcpy(&before, &out, sizeof(out));
        vector_check(f,
                     mpeg2_va029_prepare_intra_slice(NULL, &out) == MPEG2_VA029_ARGUMENT &&
                         !memcmp(&out, &before, sizeof(out)),
                     "prepare accepted an absent input record");
    }
}

static void run(const char *directory, const char *name, unsigned expected)
{
    struct vector_file f;
    struct vector_line l;
    vector_open(&f, directory, name);
    while (vector_next(&f, &l)) {
        ++f.vectors;
        if (!strcmp(l.op, "picture"))
            picture_vector(&f, &l);
        else if (!strcmp(l.op, "normalize"))
            normalize_vector(&f, &l);
        else if (!strcmp(l.op, "iq"))
            iq_vector(&f, &l);
        else if (!strcmp(l.op, "slice"))
            slice_vector(&f, &l);
        else if (!strcmp(l.op, "prepare"))
            prepare_vector(&f, &l);
        else
            vector_check(&f, 0, "unknown operation");
    }
    vector_close(&f, expected);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : VECTOR_DIRECTORY;
    run(directory, "mpeg2-picture.txt", 98);
    run(directory, "mpeg2-normalize.txt", 507);
    run(directory, "mpeg2-iq.txt", 84);
    run(directory, "mpeg2-slice.txt", 122);
    run(directory, "mpeg2-prepare.txt", 46);
    if (vector_failures) {
        printf("FAIL mpeg2_va029: %u of %u vectors or properties failed\n", vector_failures,
               vector_total);
        return 1;
    }
    printf("PASS mpeg2_va029: %u vectors (picture, IQ, slice, prepare); CPU only, no device\n",
           vector_total);
    return 0;
}
