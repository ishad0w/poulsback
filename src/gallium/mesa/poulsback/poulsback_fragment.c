// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_fragment.h"
#include "compiler/shader_enums.h"
#include "pipe/p_state.h"
#include "sgx535_fs_nir.h"
#include "util/u_math.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SGX535_FS_MAX_VARYINGS == SGX535_VERTEX_TCS &&
                   SGX535_VERTEX_TCS == POULSBACK_FRAME_TCS,
               "a varying per texture coordinate");
#define USEISSUE_TC0 0u
#define USECOLFLOAT (UINT32_C(1) << 20)

/* A body per colour interpolation and form of the sampler reads, the least
 * recently used of BODIES replaced; a refused compile is kept with its
 * reason. */
#define BODIES 4u
struct body {
    uint64_t id, used; /* id 0: empty */
    bool flat;
    struct sgx535_fs_sampler sampler[SGX535_FS_MAX_SAMPLERS]; /* the units read */
    const char *refusal;
    struct sgx535_fs_program program;
};
/* A body linked with an epilogue key, the least recently used of VARIANTS
 * replaced (16: gltest --blend's nine keys cycled through eight, relinking
 * every one each frame). */
#define VARIANTS 16u
struct variant {
    uint64_t body;
    struct sgx535_fs_epilogue_key key;
    uint64_t id, used;
    const char *refusal;
    struct sgx535_fs_variant linked;
    /* the ISP words of the last ISP state drawn with (ISP_VALID) */
    bool isp_valid, isp_ok;
    struct sgx535_isp_state isp_state;
    uint32_t isp[6], isp_groups;
};
struct poulsback_fragment {
    struct sgx535_fs_nir ir;
    int color;           /* the TC slot the colour iterates in, or -1 */
    uint32_t tcs;        /* the TC slots in use */
    bool flat;           /* a generic varying is flat */
    struct body body[BODIES];
    struct variant *variant[VARIANTS];
    uint64_t uses;
    uint32_t sa[SGX535_FS_MAX_CONSTANTS];
    struct poulsback_frame_fragment frame;
};

/* Program identities apart from the vertex programs' (poulsback_vertex.c). */
static uint64_t next_id(void)
{
    static uint64_t id;
    return UINT64_C(1) << 63 | __atomic_add_fetch(&id, 1, __ATOMIC_RELAXED);
}

static uint32_t bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* F's TC slots: the colour, and generic varyings the vertex program writes
 * as they are, smooth or flat (iterated from a triangle's last vertex); the
 * refusal, or NULL. */
static const char *slots(struct poulsback_fragment *f)
{
    f->color = -1;
    for (uint32_t v = 0; v < SGX535_FS_MAX_VARYINGS; v++) {
        const unsigned location = f->ir.varying_location[v];
        if (!f->ir.key.varying[v].components)
            continue;
        f->tcs = v + 1u;
        if (location == VARYING_SLOT_COL0)
            f->color = (int)v;
        else if (location != VARYING_SLOT_FOGC && location < VARYING_SLOT_VAR0 &&
                 (location < VARYING_SLOT_TEX0 || location > VARYING_SLOT_TEX7))
            return "an input other than the colour and generic varyings";
        else if (f->ir.key.varying[v].interp != SGX535_FS_SMOOTH)
            f->flat = true;
    }
    return NULL;
}

struct poulsback_fragment *poulsback_fragment_create(const struct nir_shader *nir,
                                                     bool packed_uniforms, const char **refusal)
{
    /* the adapter's reason outlives the program it came with */
    static _Thread_local char reason[sizeof(((struct sgx535_fs_nir *)0)->reason)];
    struct poulsback_fragment *f = calloc(1, sizeof(*f));
    enum sgx535_fs_uniforms layout =
        packed_uniforms ? SGX535_FS_UNIFORMS_DWORDS : SGX535_FS_UNIFORMS_VEC4;
    *refusal = "memory";
    if (f && sgx535_fs_from_nir(nir, layout, &f->ir) == SGX535_FS_OK) {
        *refusal = slots(f);
        /* fdsx and fdsy carry SYNCSTART, which hung the GPU on an smp: not
         * until a derivative has run */
        for (uint32_t i = 0; !*refusal && i < f->ir.count; i++)
            if (f->ir.ir[i].op == SGX535_FS_DDX || f->ir.ir[i].op == SGX535_FS_DDY)
                *refusal = "derivatives";
    } else if (f) {
        memcpy(reason, f->ir.reason, sizeof(reason));
        reason[sizeof(reason) - 1u] = 0;
        *refusal = reason[0] ? reason : "the backend";
    }
    if (*refusal) {
        free(f);
        return NULL;
    }
    return f;
}

struct poulsback_fragment *poulsback_fragment_create_load(void)
{
    struct poulsback_fragment *f = calloc(1, sizeof(*f));
    struct sgx535_fs_instruction *in;
    if (!f)
        return NULL;
    in = f->ir.ir;
    /* TC0's s and t, unit 0 read there, its four channels the colour */
    in[0] = (struct sgx535_fs_instruction){.op = SGX535_FS_INPUT, .comp = 0};
    in[1] = (struct sgx535_fs_instruction){.op = SGX535_FS_INPUT, .comp = 1};
    in[2] = (struct sgx535_fs_instruction){
        .op = SGX535_FS_TEX, .comp = SGX535_FS_TEX_IMPLICIT, .dim = 2, .src = {0, 1}};
    for (uint16_t c = 0; c < 4; c++) {
        in[3 + c] =
            (struct sgx535_fs_instruction){.op = SGX535_FS_TEXC, .comp = (uint8_t)c, .src = {2}};
        in[7 + c] = (struct sgx535_fs_instruction){.op = SGX535_FS_OUTPUT,
                                                   .index = (uint16_t)(SGX535_FS_COLOR_R + c),
                                                   .src = {(uint16_t)(3 + c)}};
    }
    f->ir.count = 11;
    f->ir.key.varying[0] = (struct sgx535_fs_varying){SGX535_FS_SMOOTH, 2, 0};
    f->ir.varying_location[0] = VARYING_SLOT_VAR0;
    f->ir.sampler_mask = 1;
    slots(f);
    return f;
}

void poulsback_fragment_destroy(struct poulsback_fragment *f)
{
    for (uint32_t i = 0; f && i < VARIANTS; i++)
        free(f->variant[i]);
    free(f);
}

bool poulsback_fragment_reads_color(const struct poulsback_fragment *f)
{
    return f->color >= 0;
}


uint32_t poulsback_fragment_samplers(const struct poulsback_fragment *f)
{
    return f->ir.sampler_mask;
}

uint32_t poulsback_fragment_tcs(const struct poulsback_fragment *f,
                                struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS])
{
    for (uint32_t v = 0; v < f->tcs; v++)
        tc[v] = (struct sgx535_vertex_tc){
            f->ir.varying_location[v], (int)v == f->color ? 4u : f->ir.key.varying[v].components};
    return f->tcs;
}

/* The body for a FLAT or smooth colour and the sampler forms SAMPLER (the
 * units read), from the cache or compiled into its least recently used
 * slot. */
static struct body *body(struct poulsback_fragment *f, bool flat,
                         const struct sgx535_fs_sampler sampler[SGX535_FS_MAX_SAMPLERS])
{
    struct sgx535_fs_key key;
    struct body *b = &f->body[0];
    for (uint32_t i = 0; i < BODIES; i++) {
        struct body *x = &f->body[i];
        if (x->id && x->flat == flat &&
            !memcmp(x->sampler, sampler, SGX535_FS_MAX_SAMPLERS * sizeof(*sampler))) {
            x->used = ++f->uses;
            return x;
        }
        if (!x->id ? b->id != 0 : b->id && x->used < b->used)
            b = x;
    }
    b->id = next_id(), b->used = ++f->uses, b->flat = flat, b->refusal = NULL;
    memcpy(b->sampler, sampler, SGX535_FS_MAX_SAMPLERS * sizeof(*sampler));
    key = f->ir.key;
    if (f->color >= 0) {
        key.varying[f->color].interp = flat ? SGX535_FS_FLAT : SGX535_FS_SMOOTH;
        key.varying[f->color].packed = 0;
    }
    memcpy(key.sampler, sampler, sizeof(key.sampler));
    if (sgx535_fs_compile(f->ir.ir, f->ir.count, &key, &b->program) != SGX535_FS_OK)
        b->refusal = b->program.reason ? b->program.reason : "the backend";
    return b;
}

/* The body linked for KEY, from the cache or linked into its least recently
 * used slot; NULL without memory. A refused link is cached with its reason. */
static struct variant *variant(struct poulsback_fragment *f, const struct body *b,
                               const struct sgx535_fs_epilogue_key *key)
{
    struct variant *v;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < VARIANTS; i++) {
        v = f->variant[i];
        if (v && v->body == b->id && !memcmp(&v->key, key, sizeof(*key))) {
            v->used = ++f->uses;
            return v;
        }
        if (!v || (f->variant[slot] && v->used < f->variant[slot]->used))
            slot = i;
    }
    if (!f->variant[slot] && !(f->variant[slot] = malloc(sizeof(*v))))
        return NULL;
    v = f->variant[slot];
    *v = (struct variant){b->id, *key, next_id(), ++f->uses, NULL, {{0}}};
    if (sgx535_fs_link(&b->program, key, &v->linked) != SGX535_FS_OK)
        v->refusal = v->linked.reason ? v->linked.reason : "the epilogue";
    else if (v->linked.iteration_count > POULSBACK_FRAME_FRAGMENT_ITERATIONS)
        v->refusal = "more iterations than the frame's pixel PDS takes";
    return v;
}

/* The SECONDARY word of constant K for S, false when a draw cannot supply it. */
static bool constant(const struct sgx535_fs_constant *k, const struct poulsback_fragment_state *s,
                     uint32_t *word)
{
    const uint8_t *cb0 = s->cb0;
    const uint32_t bytes = cb0 ? s->bytes : 0, height = s->height;
    const struct poulsback_fragment_texture *t;
    uint32_t u = 0, feedback[2];
    if ((k->kind == SGX535_FS_CONSTANT_UNIFORM || k->kind == SGX535_FS_CONSTANT_UNIFORM_INT ||
         k->kind == SGX535_FS_CONSTANT_UNIFORM_BOOL)) {
        if (k->value >= bytes / 4u)
            return false;
        memcpy(&u, cb0 + 4u * k->value, 4);
    }
    switch (k->kind) {
    case SGX535_FS_CONSTANT_NONE:
        *word = 0;
        return true;
    case SGX535_FS_CONSTANT_UNIFORM:
        *word = u;
        return true;
    case SGX535_FS_CONSTANT_UNIFORM_INT:
        *word = bits((float)(int32_t)u);
        return true;
    case SGX535_FS_CONSTANT_UNIFORM_BOOL:
        *word = bits(u ? 1.0f : 0.0f);
        return true;
    case SGX535_FS_CONSTANT_LITERAL:
        *word = k->value;
        return true;
    case SGX535_FS_CONSTANT_FRAG_Y: /* the window: y = height - y */
        *word = bits((float)height);
        return true;
    case SGX535_FS_CONSTANT_FRAG_YSIGN:
        *word = bits(-1.0f);
        return true;
    case SGX535_FS_CONSTANT_FACING: /* the window: g16 bit 0 is clear for CW */
        *word = s->isp.front_ccw;
        return true;
    case SGX535_FS_CONSTANT_EPILOGUE:
        sgx535_fs_feedback_words(&s->feedback, feedback);
        *word = k->value == SGX535_FS_BLEND_COLOR ? s->blend_color
                : k->value == SGX535_FS_FEEDBACK0 ? feedback[0]
                                                  : feedback[1];
        return k->value <= SGX535_FS_FEEDBACK1;
    case SGX535_FS_CONSTANT_TEXTURE: /* sampler << 4 | plane << 2 | word: one plane */
        t = s->texture[k->value >> 4 & 15u];
        if (!t || k->value >> 2 & 3u || (k->value & 3u) > 2u)
            return false;
        *word = t->words[k->value & 3u];
        return true;
    case SGX535_FS_CONSTANT_TEXSIZE: /* sampler << 2 | dimension: 2D, a depth of 1 */
        t = s->texture[k->value >> 2 & 15u];
        if (!t)
            return false;
        *word = bits((float)((k->value & 3u) == 0   ? t->width
                             : (k->value & 3u) == 1 ? t->height
                                                    : 1u));
        return true;
    default:
        return false;
    }
}

/* A Gallium stencil face as the ISP's: Gallium's ops INCR_WRAP, DECR_WRAP and
 * INVERT are 5, 6 and 7, GL's (the ISP's) INVERT, INCR_WRAP and DECR_WRAP. */
static struct sgx535_isp_stencil isp_stencil(const struct pipe_stencil_state *s, uint8_t ref)
{
    static const uint8_t op[8] = {0, 1, 2, 3, 4, 6, 7, 5};
    return (struct sgx535_isp_stencil){(uint8_t)s->func,      ref,
                                       (uint8_t)s->valuemask, (uint8_t)s->writemask,
                                       op[s->fail_op],        op[s->zfail_op],
                                       op[s->zpass_op]};
}

/* Gallium's blend factor F as the epilogue's, plus 1; 0: none (SRC1). ONE ..
 * CONST_ALPHA are 1 .. 8, PIPE_BLENDFACTOR_INVERT_BIT their inverses (ZERO the
 * inverted ONE). */
static uint8_t blend_factor(unsigned f)
{
    static const uint8_t plain[9] = {0, 2, 3, 7, 9, 5, 15, 11, 13};
    static const uint8_t inverted[9] = {0, 1, 4, 8, 10, 6, 0, 12, 14};
    unsigned base = f & ~(unsigned)PIPE_BLENDFACTOR_INVERT_BIT;
    return base < 9u ? (f & PIPE_BLENDFACTOR_INVERT_BIT ? inverted : plain)[base] : 0;
}

/* The ISP state in the window's orientation (GL's compare functions and
 * colour mask bits are Gallium's), the epilogue's key, the
 * blend colour and the ISP state a visibility test feeds back, its faces as
 * sgx535_isp_words takes them (the window's GL back face first with CCW front
 * faces). */
bool poulsback_fragment_state(const struct pipe_rasterizer_state *rs,
                              const struct pipe_depth_stencil_alpha_state *z,
                              const struct pipe_blend_state *b,
                              const struct pipe_blend_color *color,
                              const struct pipe_stencil_ref *ref,
                              const struct pipe_framebuffer_state *fb, bool flat,
                              struct poulsback_fragment_state *s)
{
    /* Gallium's logic ops (by truth table) in GL's order */
    static const uint8_t logic[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    const struct pipe_rt_blend_state *rt = &b->rt[0];
    const uint8_t f[4] = {blend_factor(rt->rgb_src_factor), blend_factor(rt->rgb_dst_factor),
                          blend_factor(rt->alpha_src_factor), blend_factor(rt->alpha_dst_factor)};
    struct sgx535_fs_feedback *k = &s->feedback;
    const struct sgx535_isp_stencil *main_face, *other;
    *s = (struct poulsback_fragment_state){
        .flat = flat,
        .first_vertex = rs->flatshade_first,
        .height = fb->height,
        .isp = {.depth_test = z->depth_enabled,
                .depth_func = (uint8_t)z->depth_func,
                .depth_write = z->depth_writemask,
                .stencil_test = z->stencil[0].enabled,
                .front = isp_stencil(&z->stencil[0], ref->ref_value[0]),
                .back = z->stencil[1].enabled ? isp_stencil(&z->stencil[1], ref->ref_value[1])
                                              : isp_stencil(&z->stencil[0], ref->ref_value[0]),
                .offset = rs->offset_tri,
                .offset_factor = (int8_t)rs->offset_scale,
                .offset_units = (int8_t)rs->offset_units,
                .cull = (uint8_t)rs->cull_face,
                .front_ccw = rs->front_ccw,
                .colormask = (uint8_t)rt->colormask},
        .key = {.target = fb->cbufs[0].format == PIPE_FORMAT_B8G8R8X8_UNORM
                              ? SGX535_FS_TARGET_RGB8
                              : SGX535_FS_TARGET_B8G8R8A8,
                .colormask = (uint8_t)rt->colormask}};
    /* a logic op replaces blending */
    if (b->logicop_enable) {
        s->key.logic = 1;
        s->key.logic_op = logic[b->logicop_func & 15u];
    } else if (rt->blend_enable) {
        if (!f[0] || !f[1] || !f[2] || !f[3])
            return false;
        s->key.blend = 1;
        s->key.rgb_src = f[0] - 1u, s->key.rgb_dst = f[1] - 1u;
        s->key.alpha_src = f[2] - 1u, s->key.alpha_dst = f[3] - 1u;
        s->key.rgb_eq = (uint8_t)rt->rgb_func, s->key.alpha_eq = (uint8_t)rt->alpha_func;
    }
    s->blend_color = (uint32_t)float_to_ubyte(color->color[2]) |
                     (uint32_t)float_to_ubyte(color->color[1]) << 8 |
                     (uint32_t)float_to_ubyte(color->color[0]) << 16 |
                     (uint32_t)float_to_ubyte(color->color[3]) << 24;
    main_face = rs->front_ccw ? &s->isp.back : &s->isp.front;
    other = main_face == &s->isp.back ? &s->isp.front : &s->isp.back;
    k->depth_func = z->depth_enabled ? (uint8_t)z->depth_func : PIPE_FUNC_ALWAYS;
    k->depth_write = z->depth_enabled && z->depth_writemask;
    k->stencil_func = k->back_func = k->alpha_func = PIPE_FUNC_ALWAYS;
    k->stencil_mask = k->stencil_writemask = 0xff;
    if (s->isp.stencil_test) {
        k->stencil_func = main_face->func, k->stencil_ref = main_face->ref;
        k->stencil_mask = main_face->value_mask, k->stencil_writemask = main_face->write_mask;
        k->stencil_fail = main_face->fail, k->stencil_zfail = main_face->zfail;
        k->stencil_zpass = main_face->zpass;
        k->back_func = other->func, k->back_fail = other->fail;
        k->back_zfail = other->zfail, k->back_zpass = other->zpass;
    }
    if (z->alpha_enabled) {
        s->key.alpha_test = 1;
        k->alpha_func = (uint8_t)z->alpha_func;
        k->alpha_ref = float_to_ubyte(z->alpha_ref_value);
    }
    return true;
}

/* A filled triangle's polygon offset: none, or for BACKEND draws the one
 * ISPB holds, a whole factor and units in -16..15 without a clamp (EMGD
 * writes GL's integers). Values without offset_tri do nothing. */
bool poulsback_fragment_offset_supported(const struct pipe_rasterizer_state *r, bool backend)
{
    const float v[2] = {r->offset_scale, r->offset_units};
    bool whole = true;
    for (unsigned i = 0; i < 2; i++)
        whole &= v[i] >= -16.0f && v[i] <= 15.0f && v[i] == (float)(int)v[i];
    if (!r->offset_tri)
        return backend || (r->offset_units == 0 && r->offset_scale == 0 && r->offset_clamp == 0);
    return backend && whole && r->offset_clamp == 0;
}
/* Whether S writes the stencil. */
bool poulsback_fragment_writes_stencil(const struct sgx535_isp_state *s)
{
    const struct sgx535_isp_stencil *f[2] = {&s->front, &s->back};
    bool writes = false;
    for (unsigned i = 0; i < 2 && s->stencil_test; i++)
        writes |= f[i]->write_mask && (f[i]->fail || f[i]->zfail || f[i]->zpass);
    return writes;
}

/* ISP's words for the program's pass type and colour write into OUT's
 * groups: ISPA and ISPB, ISPC with a stencil test, the back faces' when the
 * two faces' stencil differs. */
static bool isp_groups(struct variant *x, const struct sgx535_isp_state *isp,
                       struct poulsback_frame_fragment *out)
{
    struct sgx535_isp_state s = *isp;
    struct sgx535_isp_words w = {0};
    s.pass_type = x->linked.pass_type;
    s.no_color = x->linked.no_color;
    /* the words again only for an ISP state other than the last */
    if (!x->isp_valid || memcmp(&x->isp_state, &s, sizeof(s))) {
        x->isp_valid = true;
        x->isp_state = s;
        x->isp_ok = sgx535_isp_words(&s, &w);
        x->isp[0] = w.ispa, x->isp[1] = w.ispb, x->isp[2] = w.ispc;
        x->isp[3] = w.bf_ispa, x->isp[4] = w.bf_ispb, x->isp[5] = w.bf_ispc;
        x->isp_groups = 3u | (w.has_ispc ? 4u : 0) | (w.two_sided ? 0x38u : 0);
    }
    memcpy(out->isp, x->isp, sizeof(out->isp));
    out->isp_groups = x->isp_groups;
    return x->isp_ok;
}

const struct poulsback_frame_fragment *
poulsback_fragment_draw(struct poulsback_fragment *f, const struct poulsback_fragment_state *s,
                        const char **refusal)
{
    struct sgx535_fs_sampler forms[SGX535_FS_MAX_SAMPLERS] = {{0}};
    /* the draw's words built aside, padding zeroed, so they compare whole */
    struct poulsback_frame_fragment next, *out = &next;
    uint32_t sa[SGX535_FS_MAX_CONSTANTS];
    const struct sgx535_fs_variant *v;
    struct variant *x;
    struct body *b;
    for (uint32_t u = 0; u < SGX535_FS_MAX_SAMPLERS; u++)
        if (f->ir.sampler_mask >> u & 1u) {
            if (!s->texture[u]) {
                *refusal = "a sampler unit without a texture";
                return NULL;
            }
            forms[u] = s->texture[u]->form;
        }
    /* flat varyings iterate a triangle's last vertex */
    if (f->flat && s->first_vertex) {
        *refusal = "flat varyings from the first vertex";
        return NULL;
    }
    b = body(f, s->flat, forms);
    *refusal = b->refusal;
    if (b->refusal)
        return NULL;
    x = variant(f, b, &s->key);
    *refusal = x ? x->refusal : "memory";
    if (!x || x->refusal)
        return NULL;
    v = &x->linked;
    /* a visibility test feeds back one stencil reference and mask pair */
    if (v->phase2 && s->isp.stencil_test &&
        (s->isp.front.ref != s->isp.back.ref || s->isp.front.value_mask != s->isp.back.value_mask ||
         s->isp.front.write_mask != s->isp.back.write_mask)) {
        *refusal = "a visibility test with two stencil references or masks";
        return NULL;
    }
    /* a variant that writes no colour runs no pixel program (no tag) */
    memset(out, 0, sizeof(*out));
    out->id = x->id, out->words = v->words, out->instruction_count = v->instruction_count;
    out->temps = v->temps, out->phase2 = v->phase2, out->primary = v->primary;
    out->iteration_count = v->no_color ? 0 : v->iteration_count;
    out->constants = f->sa, out->constant_count = v->no_color ? 0 : v->constant_count;
    for (uint32_t i = 0; i < out->iteration_count; i++) {
        uint32_t douti = v->iteration[i].douti;
        /* a non-dependent texture read: its unit's words in a DOUTT */
        if (v->iteration[i].texture) {
            out->texture_mask |= 1u << i;
            memcpy(out->texture[i], s->texture[v->iteration[i].sampler]->words, 12);
        }
        /* The GPU vertex program's colour, as the F32 family the writer puts
         * a backend colour draw in delivers it: TC0, F32 (the legacy's F32
         * iteration 0x0fd0000f). V0 delivers it with R and B exchanged (runs
         * 095 and 097). */
        if (f->color >= 0 && !v->iteration[i].texture && (douti >> 12 & 15u) == (uint32_t)f->color)
            douti = (douti & ~(UINT32_C(15) << 12)) | USEISSUE_TC0 << 12 | USECOLFLOAT;
        out->iteration[i] = douti;
    }
    /* the BOs sampled, once each */
    for (uint32_t u = 0; u < SGX535_FS_MAX_SAMPLERS; u++) {
        uint32_t k = 0;
        if (!(f->ir.sampler_mask >> u & 1u))
            continue;
        while (k < out->texture_bo_count && out->texture_bo[k] != s->texture[u]->handle)
            k++;
        if (k == POULSBACK_FRAME_FRAGMENT_TEXTURES) {
            *refusal = "more textures than a draw's BO list takes";
            return NULL;
        }
        if (k == out->texture_bo_count)
            out->texture_bo[out->texture_bo_count++] = s->texture[u]->handle;
    }
    for (uint32_t c = 0; c < out->constant_count; c++)
        if (!constant(&v->constant[c], s, &sa[c])) {
            *refusal = "a constant a draw cannot supply";
            return NULL;
        }
    if (!isp_groups(x, &s->isp, out)) {
        *refusal = "ISP state without a form";
        return NULL;
    }
    if (s->viewport) {
        out->has_viewport = true;
        memcpy(out->viewport, s->viewport, sizeof(out->viewport));
    }
    /* a new generation only when the words or the constants changed */
    if (memcmp(&f->frame, out, offsetof(struct poulsback_frame_fragment, generation)) ||
        memcmp(f->sa, sa, out->constant_count * 4u)) {
        out->generation = f->frame.generation + 1u;
        memcpy(f->sa, sa, out->constant_count * 4u);
        memcpy(&f->frame, out, sizeof(*out));
    }
    return &f->frame;
}
