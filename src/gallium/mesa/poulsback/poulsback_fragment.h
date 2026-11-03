// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_FRAGMENT_H
#define POULSBACK_FRAGMENT_H

#include "poulsback_frame.h"
#include "sgx535_fs.h"
#include "sgx535_isp_words.h"
#include "sgx535_vertex_nir.h"
#include <stdbool.h>
#include <stdint.h>

/* GPU-shaded draws' fragment programs from the backend
 * (docs/compiler.md#window-integration): the CSO's NIR into the backend's IR
 * once, compiled once per colour interpolation and linked per epilogue key
 * (blend, colour mask, logic op, alpha test, target), and per draw the
 * SECONDARY block from the fragment constants and the epilogue's words, and
 * per sampler read form a body of its own. The window integration takes
 * programs whose inputs are the colour and smooth generic varyings, and their
 * 2D texture reads. */

struct nir_shader;
struct poulsback_fragment;

/* NULL with REFUSAL named: the backend or the window integration cannot take
 * the program. */
struct poulsback_fragment *poulsback_fragment_create(const struct nir_shader *nir,
                                                     bool packed_uniforms, const char **refusal);
/* The load draw's program: unit 0 read at TC0 (two components), the result
 * the colour. NULL without memory. */
struct poulsback_fragment *poulsback_fragment_create_load(void);
void poulsback_fragment_destroy(struct poulsback_fragment *f);
/* Whether the program reads gl_Color (the vertex program then outputs it). */
bool poulsback_fragment_reads_color(const struct poulsback_fragment *f);
/* The texture coordinates the program iterates, in order, as the vertex
 * program outputs them (the colour all four components): their count. */
uint32_t poulsback_fragment_tcs(const struct poulsback_fragment *f,
                                struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS]);
/* The sampler units the program reads: a mask. */
uint32_t poulsback_fragment_samplers(const struct poulsback_fragment *f);
/* A unit a draw samples: the backend's form of its reads (result class, the
 * view's swizzle, a stride texture's REPEAT in USE), its texture words
 * (DOUTT0-2), its level's size and its BO. */
struct poulsback_fragment_texture {
    struct sgx535_fs_sampler form;
    uint32_t words[3];
    uint32_t width, height;
    uint32_t handle;
};
/* A draw's state beyond its program: a FLAT or smooth colour, the fragment
 * constant buffer CB0 (BYTES), a window target of HEIGHT rows, the ISP state
 * (the pass type and colour write are the program's), the epilogue's key
 * (blend, colour mask, logic op, alpha test, target) and its words: the
 * blend colour and the ISP state a visibility test feeds back; the textures
 * of the units the program reads. */
struct poulsback_fragment_state {
    bool flat;
    bool first_vertex; /* flat values from a triangle's first vertex (GL's other convention) */
    const void *cb0;
    uint32_t bytes, height;
    struct sgx535_isp_state isp;
    struct sgx535_fs_epilogue_key key;
    uint32_t blend_color; /* B8G8R8A8 */
    struct sgx535_fs_feedback feedback;
    const struct poulsback_fragment_texture *texture[SGX535_FS_MAX_SAMPLERS];
    /* the draw's viewport as the MTE's words (Tx, Sx, Ty, Sy, Tz, Sz), NULL:
     * the window's */
    const uint32_t *viewport;
};
struct pipe_rasterizer_state;
struct pipe_depth_stencil_alpha_state;
struct pipe_blend_state;
struct pipe_blend_color;
struct pipe_stencil_ref;
struct pipe_framebuffer_state;
/* S from Gallium's bound state for a FLAT or smooth colour, its constant
 * buffer left to the caller. false: a blend factor without a form
 * (dual-source). */
bool poulsback_fragment_state(const struct pipe_rasterizer_state *rs,
                              const struct pipe_depth_stencil_alpha_state *z,
                              const struct pipe_blend_state *b,
                              const struct pipe_blend_color *color,
                              const struct pipe_stencil_ref *ref,
                              const struct pipe_framebuffer_state *fb, bool flat,
                              struct poulsback_fragment_state *s);
/* Whether a filled triangle's polygon offset has a form: none, or for
 * BACKEND draws ISPB's. */
bool poulsback_fragment_offset_supported(const struct pipe_rasterizer_state *r, bool backend);
/* Whether ISP's stencil state writes the stencil. */
bool poulsback_fragment_writes_stencil(const struct sgx535_isp_state *s);
/* The draw's program for S, and its ISP words. NULL with REFUSAL named: the
 * draw stays on the legacy program. The result lives until the next call. */
const struct poulsback_frame_fragment *
poulsback_fragment_draw(struct poulsback_fragment *f, const struct poulsback_fragment_state *s,
                        const char **refusal);

#endif
