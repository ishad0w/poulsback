// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* What poulsback_context.c (frames, draws, flush) and poulsback_context_state.c
 * (bound state, shaders, samplers, the resource and blit hooks) share. */
#ifndef POULSBACK_CONTEXT_PRIVATE_H
#define POULSBACK_CONTEXT_PRIVATE_H
#include "poulsback_context.h"
#include "draw/draw_context.h"
#include "draw/draw_fs.h"
#include "draw/draw_pipe.h"
#include "draw/draw_vs.h"
#include "nir.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "poulsback_draw.h"
#include "poulsback_fence.h"
#include "poulsback_fragment.h"
#include "poulsback_image.h"
#include "poulsback_lost.h"
#include "poulsback_nir.h"
#include "poulsback_resource.h"
#include "poulsback_sampler.h"
#include "poulsback_scene_color.h"
#include "poulsback_scene_samples.h"
#include "util/u_math.h"
#include "poulsback_selfcheck.h"
#include "poulsback_surface.h"
#include "poulsback_vertex.h"
#include "indices/u_indices.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_bool32.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "tgsi/tgsi_scan.h"
#include "util/format/u_format.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/os_time.h"
#include "poulsback_refuse.h"
#include "sgx535_draw_mask.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PB_VERTEX_BUFFERS 16u
#define PB_CONSTANT_BYTES 65536u
#define PB_DRAWS 64u
/* CSO objects have one caller reference and optional bound reference. */
enum state_kind { RASTER, BLEND, DEPTH, ELEMENTS, VS, FS, STATE_KINDS };
struct state {
    struct state *next;
    enum state_kind kind;
    unsigned refs;
    bool caller;
    union {
        struct pipe_rasterizer_state raster;
        struct pipe_blend_state blend;
        struct pipe_depth_stencil_alpha_state depth;
        struct {
            unsigned count;
            struct pipe_vertex_element element[PIPE_MAX_ATTRIBS];
        } ve;
        struct {
            struct draw_vertex_shader *draw;
            struct poulsback_vertex_shader *gpu; /* NULL: the CPU (draw module) only */
        } vs;
        struct {
            struct draw_fragment_shader *draw;
            nir_shader *nir;
            struct sgx535_fragment_instruction *ir;
            struct sgx535_fragment_program *program;
            size_t count;
            uint32_t scene_profile;
            /* nir compiled for the scene, [0] smooth and [1] flat, on first use. */
            struct sgx535_fragment_dri_nir_ir *scene_program[2];
            bool has_uniform;
            bool solid;
            struct sgx535_fragment_linkage linkage;
            /* GPU-shaded draws' program from the backend, or NULL;
             * BACKEND_ONLY: no legacy program (discard, gl_FragCoord, depth
             * output, generic varyings, texture reads), so no CPU-path or
             * legacy draw */
            struct poulsback_fragment *backend;
            bool backend_only;
            /* REFUSED: no path draws the program (REFUSAL why); SKIPPED: the
             * reason a skipped draw's line last named */
            bool refused;
            char refusal[64];
            const char *skipped;
        } fs;
    } u;
};
#define POULSBACK_CONTEXT_FRAME_TEXTURES 32u
struct poulsback_context {
    struct pipe_context base;
    /* POULSBACK_TRACE per-frame figures: the clock at the frame's first draw
     * (ns; read only while tracing: on HPET each read is a syscall), and its
     * draws and vertices. */
    uint64_t t_frame_begin;
    unsigned t_draws, t_vertices;
    const char *refusal; /* last refused draw-state block */
    struct draw_context *draw;
    /* The vertex shader the draw module holds: bound only when a draw takes
     * the CPU path, as binding parses its TGSI again. */
    struct draw_vertex_shader *draw_vs;
    bool draw_inputs; /* the draw module holds this draw's buffers */
    struct draw_stage *stage;
    struct poulsback_ws_render *render;
    struct poulsback_sampler_bank *samplers;
    struct state *states, *bound[STATE_KINDS];
    struct pipe_vertex_buffer vb[PB_VERTEX_BUFFERS];
    unsigned vb_count;
    struct pipe_framebuffer_state fb;
    struct pipe_viewport_state viewport;
    /* Engine mode's draw module clips, culls and transforms nothing (the MTE
     * does): it gets the bound rasterizer with cull NONE and an
     * identity viewport. */
    struct pipe_rasterizer_state draw_raster;
    struct pipe_scissor_state scissor;
    struct pipe_blend_color blend_color;
    struct pipe_stencil_ref stencil_ref;
    unsigned sample_mask, min_samples;
    /* bumped by every bind and by the setters of the state above: a backend
     * draw's packed fragment state and frame shape hold while it does */
    uint32_t state_serial;
    nir_shader *scene_fragment;
    uint32_t scene_fragment_profile;
    bool scene_fragment_owned;
    /* An owned scene_fragment compiled for the current draw. */
    struct sgx535_fragment_dri_nir_ir *scene_program;
    struct poulsback_scene_sample_input scene_samples;
    bool scene_samples_ready;
    struct poulsback_scene_color_state scene_color;
    uintptr_t scene_color_cso;
    struct pipe_resource *scene_target;
    struct pipe_resource *scene_depth_target;
    /* The frame the writer records, open from its clear or first draw until
     * the flush that submits it, and its clear. */
    bool frame, frame_cleared;
    uint32_t frame_clear;
    struct poulsback_color0_draw frame_draw;
    /* The resource layer's CPU access, reached through scene_access(). */
    void *(*resource_map)(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                          const struct pipe_box *, struct pipe_transfer **);
    void (*resource_subdata)(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                             const struct pipe_box *, const void *, unsigned, uintptr_t);
    struct {
        uint8_t *data;
        struct pipe_resource *resource;
        unsigned bytes, offset;
    } constant[2];
    struct pipe_fence_handle *last_fence, *failed_fence;
    bool have_viewport, have_scissor, scene_depth_ready;
    /* The stencil's value since its last clear, unless a draw has written it
     * since (DIRTY); READY: the open frame has set it. */
    uint8_t stencil_value;
    bool stencil_dirty, stencil_ready;
    /* The ISP's draw mask in the open render: set over the
     * target at its start, else (CUT) over MASK alone */
    bool mask_cut;
    struct sgx535_rect mask;
    /* The load draw's programs (poulsback_context_load). */
    struct poulsback_fragment *load_fragment;
    struct sgx535_vertex_code load_code;
    struct poulsback_frame_gpu_program load_program;
    /* The images the open frame samples, held until it is submitted. */
    uint32_t frame_texture_count;
    struct pipe_resource *frame_texture[POULSBACK_CONTEXT_FRAME_TEXTURES];
    struct poulsback_ws_image *frame_image[POULSBACK_CONTEXT_FRAME_TEXTURES];
    /* The last backend draw's fragment state (its colour FLAT; OK: it has a
     * form) and frame shape (SOLID, FLAT, the frame's CLEAR, a DEPTH target,
     * its STATUS) at state SERIAL. */
    struct poulsback_packed_state {
        bool valid, ok, flat;
        uint32_t serial;
        struct poulsback_fragment_state state;
    } packed_state;
    struct poulsback_packed_shape {
        bool valid, solid, flat, depth;
        uint32_t serial, clear;
        const nir_shader *fragment;
        int status;
        poulsback_frame_shape shape;
    } packed_shape;
    int error;
    /* The GPU's loss as this context reports it (poulsback_lost.h). */
    struct poulsback_lost_view lost;
};
/* A clock read for POULSBACK_TRACE timing only. */
static inline uint64_t trace_clock(void)
{
    return poulsback_trace_on() ? os_time_get_nano() : 0;
}
static inline struct poulsback_context *ctx(struct pipe_context *p)
{
    return (struct poulsback_context *)p;
}
/* The first error stops the context; the message names where it was raised. */
void poulsback_context_fail_at(struct poulsback_context *, int e, const char *function, int line);
#define fail(c, e) poulsback_context_fail_at((c), (e), __func__, __LINE__)
/* fail() at this line, returning the error for `e = refuse(c, -E...)`. */
#define refuse(c, e) ((e) ? (fail((c), (e)), (e)) : 0)

/* poulsback_context_state.c */
void poulsback_context_state_init(struct poulsback_context *);
/* Binds the caller's CSO object P of kind K (NULL unbinds). */
void poulsback_context_bind_state(struct poulsback_context *, void *p, enum state_kind k);
/* Drops a CSO object's reference; the last one frees it. */
void poulsback_context_release_state(struct poulsback_context *, struct state *);
/* The bound constant buffers' bytes for this draw. */
int poulsback_context_refresh_constants(struct poulsback_context *);
/* A draw the CPU path runs: the draw module gets the bound vertex shader,
 * the borrowed vertex buffers and indices; the end flushes and drops them. */
void poulsback_context_cpu_draw_begin(struct poulsback_context *,
                                      const struct pipe_vertex_buffer *vb,
                                      const void *const *buffers, const size_t *sizes,
                                      const void *indices, unsigned index_size,
                                      unsigned index_bytes);
void poulsback_context_cpu_draw_end(struct poulsback_context *);
/* The bound fragment shader's scene program for this draw. */
int poulsback_context_snapshot_fragment(struct poulsback_context *);
/* poulsback_context.c: submits the open frame, if any. */
int poulsback_context_flush_frame(struct poulsback_context *);
/* poulsback_context.c: a draw's state as the frame writer's shape (BACKEND:
 * a backend program's group; OBJECT: a frame object's, from the window and
 * not the bound state). */
int poulsback_context_frame_shape(struct poulsback_context *c,
                                  const struct sgx535_fragment_dri_nir_ir *program, bool solid,
                                  bool flat, const uint32_t *uniforms, uint32_t uniform_count,
                                  uint32_t profile, bool backend, bool object,
                                  poulsback_frame_shape *shape);
/* poulsback_context_objects.c: the solid template and
 * the colour pass-through a backend draw's group is built from, with the
 * template's default colour; the draw mask to the bound scissor (SCISSOR) or
 * the whole target; the stencil's clear object; the load draw, which starts
 * an open frame no full clear began; a clear that does not start the frame
 * over; an object that writes nothing, for a frame of clears alone. */
extern const struct sgx535_fragment_dri_nir_ir poulsback_template_program[2];
extern const uint32_t poulsback_template_uniforms[4];
int poulsback_context_draw_mask(struct poulsback_context *c, bool scissor);
const struct poulsback_fragment_state *poulsback_context_fragment_state(struct poulsback_context *,
                                                                        bool flat);
int poulsback_context_draw_shape(struct poulsback_context *, bool solid, bool flat,
                                 const poulsback_frame_shape **);
int poulsback_context_stencil_object(struct poulsback_context *c);
int poulsback_context_load(struct poulsback_context *c);
int poulsback_context_clear_object(struct poulsback_context *c, unsigned buffers, uint32_t colors,
                                   uint8_t stencil_mask, const struct pipe_scissor_state *s,
                                   const union pipe_color_union *value, double depth,
                                   unsigned stencil);
int poulsback_context_inert_object(struct poulsback_context *c);
/* poulsback_context_textures.c: the textures of the fragment units in MASK
 * as a backend draw samples them (T, indexed by unit), the open frame holding
 * their images; REFUSAL names what has no form. Whether the open frame
 * samples R. The frame's images let go once it is submitted. */
int poulsback_context_textures(struct poulsback_context *c, uint32_t mask,
                               struct poulsback_fragment_texture t[SGX535_FS_MAX_SAMPLERS],
                               const char **refusal);
bool poulsback_context_samples(const struct poulsback_context *c, const struct pipe_resource *r);
int poulsback_context_target_texture(struct poulsback_context *c,
                                     struct poulsback_fragment_texture *t, const char **refusal);
int poulsback_context_textures_release(struct poulsback_context *c);
#endif
