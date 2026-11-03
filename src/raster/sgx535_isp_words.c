// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_isp_words.h"

#define ISPA_ALWAYS 7u
#define ISPA_NO_TAG_WRITE (1u << 21)
#define ISPA_NO_DEPTH_WRITE (1u << 20)
#define ISPA_TWO_SIDED (1u << 11)
#define ISPA_B_PRESENT (1u << 9)
#define ISPA_C_PRESENT (1u << 8)

static bool stencil_ok(const struct sgx535_isp_stencil *s)
{
    return s->func <= 7u && s->fail <= 7u && s->zfail <= 7u && s->zpass <= 7u;
}

static uint32_t ispc(const struct sgx535_isp_stencil *s)
{
    return (uint32_t)s->func << 25 | (uint32_t)s->fail << 22 | (uint32_t)s->zfail << 19 |
           (uint32_t)s->zpass << 16 | (uint32_t)s->value_mask << 8 | s->write_mask;
}

/* ISPC holds every field of a face but the reference. */
static bool same_stencil(const struct sgx535_isp_stencil *a, const struct sgx535_isp_stencil *b)
{
    return ispc(a) == ispc(b) && a->ref == b->ref;
}

bool sgx535_isp_words(const struct sgx535_isp_state *state, struct sgx535_isp_words *words)
{
    const struct sgx535_isp_stencil *main_face, *other_face;
    struct sgx535_isp_words w = {0};
    uint32_t common, ispb;
    if (!state || !words || state->depth_func > 7u || state->pass_type > 7u ||
        state->cull > SGX535_CULL_BOTH || state->cull == SGX535_CULL_BOTH ||
        (state->stencil_test && (!stencil_ok(&state->front) || !stencil_ok(&state->back))) ||
        (state->offset && (state->offset_factor < -16 || state->offset_factor > 15 ||
                           state->offset_units < -16 || state->offset_units > 15)))
        return false;
    /* The window's y flip turns GL's front faces into the hardware's back
     * faces: ISPA takes GL's back state there, BF-ISPA its front (EMGD, CCW
     * front faces); a bottom-up FBO keeps GL's, and CW front faces swap them
     * again. */
    main_face = (state->bottom_up != 0) == (state->front_ccw != 0) ? &state->front : &state->back;
    other_face = main_face == &state->front ? &state->back : &state->front;
    w.has_ispc = state->stencil_test != 0;
    w.two_sided = w.has_ispc && !same_stencil(&state->front, &state->back);
    common = (uint32_t)(state->pass_type & 7u) << 25 |
             (state->depth_test ? state->depth_func : ISPA_ALWAYS) << 22 |
             (state->no_color ? ISPA_NO_TAG_WRITE : 0u) |
             (state->depth_test && state->depth_write ? 0u : ISPA_NO_DEPTH_WRITE) |
             (w.two_sided ? ISPA_TWO_SIDED : 0u) | ISPA_B_PRESENT |
             (w.has_ispc ? ISPA_C_PRESENT : 0u);
    /* ISPB: polygon offset factor 24:20 and units 19:15 (5-bit two's
     * complement), UPASSCTL 7:4 the colour mask as A 8, R 4, G 2, B 1. */
    ispb = (uint32_t)(state->colormask & 1u) << 6 | (uint32_t)(state->colormask & 2u) << 4 |
           (uint32_t)(state->colormask & 4u) << 2 | (uint32_t)(state->colormask & 8u) << 4;
    if (state->offset)
        ispb |= ((uint32_t)state->offset_factor & 31u) << 20 | ((uint32_t)state->offset_units & 31u)
                                                                   << 15;
    w.ispa = common | (w.has_ispc ? main_face->ref : 0u);
    w.ispb = ispb;
    if (w.has_ispc)
        w.ispc = ispc(main_face);
    if (w.two_sided) {
        /* BF-ISPB without UPASSCTL (EMGD: 0); the offset is the face's too */
        w.bf_ispa = common | other_face->ref;
        w.bf_ispb = ispb & ~UINT32_C(0xff);
        w.bf_ispc = ispc(other_face);
    }
    /* MTE cull (1:0): GL's back with CCW front faces is 1 in the window, its
     * front 2, and the reverse in a bottom-up FBO. */
    if (state->cull != SGX535_CULL_NONE)
        w.cull = ((state->cull == SGX535_CULL_BACK) == (state->front_ccw != 0)) !=
                         (state->bottom_up != 0)
                     ? 1u
                     : 2u;
    *words = w;
    return true;
}
