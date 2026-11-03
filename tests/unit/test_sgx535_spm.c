// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Partial-render register programs (docs/re/firmware/partial-render.md): exact action lists for a
 * 300x300 scene
 * with depth and stencil, depth only and no planes, first and later partial
 * renders, DPM contexts 0 and 1, and the refusals. CPU construction only. */
#include "sgx535_spm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define W(o, v) {SGX535_ENGINE121_WRITE, (o), (v), 0}
#define WS(o, m) {SGX535_ENGINE121_WAIT_SET, (o), (m), (m)}
#define WC(o, m) {SGX535_ENGINE121_WAIT_CLEAR, (o), 0, (m)}
#define RB(o) {SGX535_ENGINE121_READBACK, (o), 0, 0}
#define SET(o, v) {SGX535_SPM_SET_BITS, (o), (v), 0}
#define CLR(o, v) {SGX535_SPM_CLEAR_BITS, (o), (v), 0}
#define PAUSE {SGX535_SPM_PAUSE, 0xa74, 1, 0}
#define RELOAD {SGX535_SPM_RELOAD, 0x684, 1, 0}
#define ACK(m) WS(0x12c, m), W(0x134, m), WC(0x12c, m)

/* The 300x300 scene at GPU 0x00100000: region array 0 at +0x1000, the state
 * table at +0x2b50 (words[10]). */
#define SCENE UINT32_C(0x00100000)
#define STATE_TABLE (SCENE + 0x2b50u)
static const struct sgx535_engine121_scene_binding scene = {1, SCENE, 0x5000, 300, 300, 0, 0, 0};

/* The job's raster writes as SUBMIT passes them: D24S8 kept after the render
 * (ZLSCTL extent 19, format 1, store bits), the planes in one BO, IPFMISC and
 * the background object. */
#define DEPTH_BO UINT32_C(0x02000000)
static const struct sgx535_engine121_action client[] = {
    W(0x480, 0x004513d0), W(0x484, DEPTH_BO),   W(0x488, DEPTH_BO),   W(0x48c, DEPTH_BO),
    W(0x490, DEPTH_BO),   W(0x414, 0x000f0000), W(0x4c4, 0x08012340),
};
#define CLIENT                                                                                     \
    W(0x480, 0x004513d0), W(0x484, DEPTH_BO), W(0x488, DEPTH_BO), W(0x48c, DEPTH_BO),              \
        W(0x490, DEPTH_BO), W(0x414, 0x000f0000), W(0x4c4, 0x08012340)
#define CLIENT_COUNT (sizeof(client) / sizeof(client[0]))
#define CONTINUATION UINT32_C(0x08045670)

/* The dummy stencil pair 1 MiB in: two 256 KiB buffers. */
#define DUMMY_BASE UINT32_C(0x00f00000)
static const struct sgx535_spm_dummy dummy = {DUMMY_BASE, DUMMY_BASE + 0x40000,
                                              DUMMY_BASE + 0x80000};
#define DUMMY_PAIR W(0xcb0, DUMMY_BASE), W(0x48c, 0x80040000), W(0x490, 0x80080000), W(0x480, 0x82)

/* engine121's raster tail for the scene; a partial or whole-scene render has
 * it without the 0x65c and 0x658 writes. */
#define CACHES                                                                                     \
    W(0xad4, 1), WS(0x138, 0x44), W(0x140, 0x44), WC(0x138, 0x44), W(0xae0, 1), WS(0x138, 1),      \
        W(0x140, 1), WC(0x138, 1), W(0x804, 0x1000ffff), ACK(0x04000000), W(0x43c, 1),             \
        W(0xa08, 1), RB(0xa08), W(0x428, 1)
#define TAIL_SPM W(0x63c, 3), W(0x408, SCENE + 0x1000), CACHES
#define TAIL_FULL W(0x65c, 0), W(0x63c, 3), W(0x658, 0), W(0x408, SCENE + 0x1000), CACHES

static void same(const struct sgx535_spm_program *p, const struct sgx535_engine121_action *e,
                 size_t n, int line)
{
    size_t i;
    ++checks;
    if (p->count != n) {
        fprintf(stderr, "FAIL line %d: %u actions, expected %zu\n", line, p->count, n);
        exit(1);
    }
    for (i = 0; i < n; i++) {
        const struct sgx535_engine121_action *a = &p->actions[i];
        if (a->kind != e[i].kind || a->offset != e[i].offset || a->value != e[i].value ||
            a->mask != e[i].mask) {
            fprintf(stderr,
                    "FAIL line %d: action %zu is %u %03x %08x %08x, expected %u %03x %08x %08x\n",
                    line, i, a->kind, a->offset, a->value, a->mask, e[i].kind, e[i].offset,
                    e[i].value, e[i].mask);
            exit(1);
        }
    }
}
#define SAME(p, e) same(p, e, sizeof(e) / sizeof(e[0]), __LINE__)

static struct sgx535_spm_program program;

static void te_step_and_abort(void)
{
    static const struct sgx535_engine121_action t[] = {
        PAUSE,  W(0x24c, 1), ACK(0x00100000), W(0x624, 7716), SET(0x218, 0x00400000),
        RELOAD, W(0x69c, 1),
    };
    static const struct sgx535_engine121_action a[] = {
        W(0x24c, 1),
        ACK(0x00100000),
        W(0x69c, 4),
        SET(0xa74, 1),
    };
    CHECK(sgx535_spm_te_step_build(7715, 8188, &program) == SGX535_ENGINE121_OK);
    SAME(&program, t);
    CHECK(sgx535_spm_te_step_build(8187, 8188, &program) == SGX535_ENGINE121_OK);
    CHECK(program.actions[5].value == 8188);
    CHECK(sgx535_spm_abort_build(&program) == SGX535_ENGINE121_OK);
    SAME(&program, a);
}

/* B for context 0 (EMGD's 0x630 values 0x2 and 0x3) and 1 (0x5 and 0x4), with
 * PARTIAL_RENDER first, and without it for the whole-scene render. */
static void handoff(void)
{
#define HANDOFF(id)                                                                                \
    W(0x630, id), W(0x62c, STATE_TABLE), CLR(0xc74, 0x200), W(0x694, 1), ACK(0x10), W(0x694, 2),   \
        ACK(0x40), W(0x630, (id) ^ 1u), W(0x694, 4), ACK(0x20), W(0x630, id), W(0x65c, 0)
    static const struct sgx535_engine121_action c0[] = {W(0x658, 1), HANDOFF(0x2u)};
    static const struct sgx535_engine121_action c1[] = {W(0x658, 1), HANDOFF(0x5u)};
    static const struct sgx535_engine121_action whole[] = {HANDOFF(0x5u)};
    /* At the TA: ALLOC and LS on c, DALLOC on whatever the last raster freed. */
    CHECK(sgx535_spm_handoff_build(0x2, 0, STATE_TABLE, 1, &program) == SGX535_ENGINE121_OK);
    SAME(&program, c0);
    CHECK(sgx535_spm_handoff_build(0x0, 0, STATE_TABLE, 1, &program) == SGX535_ENGINE121_OK);
    SAME(&program, c0);
    CHECK(sgx535_spm_handoff_build(0x5, 1, STATE_TABLE, 1, &program) == SGX535_ENGINE121_OK);
    SAME(&program, c1);
    CHECK(sgx535_spm_handoff_build(0x7, 1, STATE_TABLE, 0, &program) == SGX535_ENGINE121_OK);
    SAME(&program, whole);
    CHECK(SGX535_SPM_HANDOFF_DPM_ID(UINT32_C(0x7), 1u) == 0x5u);
#undef HANDOFF
}

static struct sgx535_spm_render render(uint32_t kind, uint32_t planes, uint32_t done)
{
    struct sgx535_spm_render r = {kind,   client, CLIENT_COUNT, &scene,
                                  planes, done,   CONTINUATION, dummy};
    return r;
}

static void build(uint32_t kind, uint32_t planes, uint32_t done)
{
    struct sgx535_spm_render r = render(kind, planes, done);
    CHECK(sgx535_spm_render_build(&r, &program) == SGX535_ENGINE121_OK);
}

/* C: the job's writes, the plane bits (LOADMASK from the second partial render
 * on), PROCESSEMPTY, the continuation from the second on, the tail. */
static void partial(void)
{
    static const struct sgx535_engine121_action ds_first[] = {CLIENT, SET(0x480, 0x004000d6),
                                                              SET(0x414, 0x100), TAIL_SPM};
    static const struct sgx535_engine121_action ds_later[] = {
        CLIENT, SET(0x480, 0x006000d6), SET(0x414, 0x100), W(0x4c4, CONTINUATION), TAIL_SPM};
    static const struct sgx535_engine121_action d_first[] = {CLIENT, SET(0x480, 0x00400054),
                                                             SET(0x414, 0x100), TAIL_SPM};
    static const struct sgx535_engine121_action d_later[] = {
        CLIENT, SET(0x480, 0x00600054), SET(0x414, 0x100), W(0x4c4, CONTINUATION), TAIL_SPM};
    static const struct sgx535_engine121_action none_first[] = {CLIENT, DUMMY_PAIR,
                                                                SET(0x414, 0x100), TAIL_SPM};
    static const struct sgx535_engine121_action none_later[] = {
        CLIENT, DUMMY_PAIR, SET(0x414, 0x100), W(0x4c4, CONTINUATION), TAIL_SPM};
    static const struct sgx535_engine121_action stencil[] = {CLIENT, SET(0x480, 0x82),
                                                             SET(0x414, 0x100), TAIL_SPM};
    const uint32_t ds = SGX535_SPM_DEPTH | SGX535_SPM_STENCIL;
    build(SGX535_SPM_RENDER_PARTIAL, ds, 0);
    SAME(&program, ds_first);
    build(SGX535_SPM_RENDER_PARTIAL, ds, 1);
    SAME(&program, ds_later);
    build(SGX535_SPM_RENDER_PARTIAL, ds, 3);
    SAME(&program, ds_later);
    build(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 0);
    SAME(&program, d_first);
    build(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 1);
    SAME(&program, d_later);
    build(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    SAME(&program, none_first);
    build(SGX535_SPM_RENDER_PARTIAL, 0, 2);
    SAME(&program, none_later);
    build(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_STENCIL, 0);
    SAME(&program, stencil);
    /* E3 changes the planes as a partial render does. */
    build(SGX535_SPM_RENDER_WHOLE, ds, 0);
    SAME(&program, ds_first);
    build(SGX535_SPM_RENDER_WHOLE, ds, 2);
    SAME(&program, ds_later);
}

/* F: load what the partial renders stored, the job's store bits kept. */
static void final(void)
{
    static const struct sgx535_engine121_action ds[] = {
        CLIENT, SET(0x480, 0x00200006), SET(0x414, 0x100), W(0x4c4, CONTINUATION), TAIL_FULL};
    static const struct sgx535_engine121_action d[] = {
        CLIENT, SET(0x480, 0x00200004), SET(0x414, 0x100), W(0x4c4, CONTINUATION), TAIL_FULL};
    static const struct sgx535_engine121_action none[] = {CLIENT, DUMMY_PAIR, SET(0x414, 0x100),
                                                          W(0x4c4, CONTINUATION), TAIL_FULL};
    static const struct sgx535_engine121_action own_background[] = {CLIENT, SET(0x480, 0x00200004),
                                                                    SET(0x414, 0x100), TAIL_FULL};
    struct sgx535_spm_render r = render(SGX535_SPM_RENDER_FINAL, SGX535_SPM_DEPTH, 1);
    build(SGX535_SPM_RENDER_FINAL, SGX535_SPM_DEPTH | SGX535_SPM_STENCIL, 1);
    SAME(&program, ds);
    build(SGX535_SPM_RENDER_FINAL, SGX535_SPM_DEPTH, 4);
    SAME(&program, d);
    build(SGX535_SPM_RENDER_FINAL, 0, 1);
    SAME(&program, none);
    r.continuation = 0;
    CHECK(sgx535_spm_render_build(&r, &program) == SGX535_ENGINE121_OK);
    SAME(&program, own_background);
}

static void end_and_ta_end(void)
{
#define END W(0x698, 2), W(0x658, 0), ACK(0x200), W(0x65c, 0)
    static const struct sgx535_engine121_action restart[] = {CLR(0xa74, 1), END, W(0x69c, 1)};
    static const struct sgx535_engine121_action after_steps[] = {
        W(0x624, 7715), RELOAD, W(0x218, 0x00000003), CLR(0xa74, 1), END, W(0x69c, 1)};
    static const struct sgx535_engine121_action whole[] = {
        CLR(0xa74, 1), END, W(0x694, 2), W(0x698, 2), W(0x24c, 1), ACK(0x00100240)};
    static const struct sgx535_engine121_action ta_end[] = {W(0x624, 7715), RELOAD,
                                                            CLR(0xa74, 0xf)};
    CHECK(sgx535_spm_end_build(SGX535_SPM_END_RESTART, 0, 7715, 3, &program) ==
          SGX535_ENGINE121_OK);
    SAME(&program, restart);
    CHECK(sgx535_spm_end_build(SGX535_SPM_END_RESTART, 2, 7715, 3, &program) ==
          SGX535_ENGINE121_OK);
    SAME(&program, after_steps);
    CHECK(sgx535_spm_end_build(SGX535_SPM_END_WHOLE, 0, 7715, 3, &program) == SGX535_ENGINE121_OK);
    SAME(&program, whole);
    CHECK(sgx535_spm_ta_end_build(1, 7715, &program) == SGX535_ENGINE121_OK);
    SAME(&program, ta_end);
#undef END
}

/* Every refusal leaves the program as it was. */
static void refused(enum sgx535_engine121_status got, enum sgx535_engine121_status want, int line)
{
    static const uint8_t poison = 0xa5;
    size_t i;
    const uint8_t *bytes = (const uint8_t *)&program;
    ++checks;
    if (got != want) {
        fprintf(stderr, "FAIL line %d: status %d, expected %d\n", line, got, want);
        exit(1);
    }
    for (i = 0; i < sizeof(program); i++)
        if (bytes[i] != poison) {
            fprintf(stderr, "FAIL line %d: the refusal wrote the program\n", line);
            exit(1);
        }
}
#define REFUSED(call, want)                                                                        \
    do {                                                                                           \
        memset(&program, 0xa5, sizeof(program));                                                   \
        refused(call, want, __LINE__);                                                             \
    } while (0)

static void refusals(void)
{
    struct sgx535_engine121_action bad[CLIENT_COUNT];
    struct sgx535_engine121_scene_binding small = scene;
    struct sgx535_spm_render r = render(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 0);
    static struct sgx535_engine121_action many[SGX535_SPM_ACTIONS];
    size_t i;
    /* T: the step past the ceiling, a ceiling beyond 16 bits. */
    REFUSED(sgx535_spm_te_step_build(8188, 8188, &program), SGX535_ENGINE121_RANGE);
    REFUSED(sgx535_spm_te_step_build(7715, 0x10000, &program), SGX535_ENGINE121_RANGE);
    CHECK(sgx535_spm_te_step_build(1, 2, NULL) == SGX535_ENGINE121_INVALID);
    CHECK(sgx535_spm_abort_build(NULL) == SGX535_ENGINE121_INVALID);
    /* B: an unaligned state table, c > 1, a shadow whose ALLOC or LS is not c, no
     * table. */
    REFUSED(sgx535_spm_handoff_build(0x2, 0, STATE_TABLE + 8u, 1, &program),
            SGX535_ENGINE121_ALIGNMENT);
    REFUSED(sgx535_spm_handoff_build(0x2, 0, STATE_TABLE + 4u, 0, &program),
            SGX535_ENGINE121_ALIGNMENT);
    REFUSED(sgx535_spm_handoff_build(0x7, 2, STATE_TABLE, 1, &program), SGX535_ENGINE121_INVALID);
    REFUSED(sgx535_spm_handoff_build(0x5, 0, STATE_TABLE, 1, &program), SGX535_ENGINE121_STALE);
    REFUSED(sgx535_spm_handoff_build(0x4, 1, STATE_TABLE, 1, &program), SGX535_ENGINE121_STALE);
    REFUSED(sgx535_spm_handoff_build(0x1, 1, STATE_TABLE, 1, &program), SGX535_ENGINE121_STALE);
    REFUSED(sgx535_spm_handoff_build(0x2, 0, 0, 1, &program), SGX535_ENGINE121_INVALID);
    /* C, E3, F. */
    r.kind = 0;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    r.kind = SGX535_SPM_RENDER_FINAL + 1u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    r = render(SGX535_SPM_RENDER_FINAL, SGX535_SPM_DEPTH, 0); /* F needs a partial render */
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    r = render(SGX535_SPM_RENDER_PARTIAL, 4, 0);
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    r = render(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 0);
    r.client = NULL;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    memcpy(bad, client, sizeof(bad));
    bad[2].kind = SGX535_ENGINE121_WAIT_SET; /* SUBMIT passes writes only */
    r.client = bad;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    memcpy(bad, client, sizeof(bad));
    bad[0].value |= 0x30000u; /* ZLOADFORMAT 3: the DPM's own depth buffer */
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_UNSUPPORTED);
    for (i = 0; i < SGX535_SPM_ACTIONS; i++)
        many[i] = client[1];
    r = render(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 0);
    r.client = many;
    r.client_count = SGX535_SPM_ACTIONS - 6u - SGX535_ENGINE121_RASTER_ACTIONS;
    CHECK(sgx535_spm_render_build(&r, &program) == SGX535_ENGINE121_OK &&
          program.count == SGX535_SPM_ACTIONS - 6u);
    r.client_count++;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_NO_SPACE);
    /* The dummy pair: base 1 MiB aligned, 16-byte offsets above it, inside the
     * 28-bit window, the two buffers apart. */
    r = render(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    r.dummy.base += 0x1000u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r = render(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    r.dummy.load = DUMMY_BASE - 0x40000u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r = render(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    r.dummy.store += 8u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r = render(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    r.dummy.store = r.dummy.load + 0x3fff0u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r = render(SGX535_SPM_RENDER_PARTIAL, 0, 0);
    r.dummy.store = DUMMY_BASE + 0x0ffc0000u + 0x10u;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r.dummy.store = DUMMY_BASE + 0x0ffc0000u; /* the last that fits */
    CHECK(sgx535_spm_render_build(&r, &program) == SGX535_ENGINE121_OK &&
          program.actions[CLIENT_COUNT + 2].value == 0x8ffc0000u);
    /* With planes the pair is not looked at. */
    r = render(SGX535_SPM_RENDER_PARTIAL, SGX535_SPM_DEPTH, 0);
    r.dummy.base = 1;
    CHECK(sgx535_spm_render_build(&r, &program) == SGX535_ENGINE121_OK);
    /* engine121's own refusal of the scene passes through. */
    small.allocation_bytes = 0x1000;
    r.scene = &small;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_RANGE);
    r.scene = NULL;
    REFUSED(sgx535_spm_render_build(&r, &program), SGX535_ENGINE121_INVALID);
    /* D, E1. */
    REFUSED(sgx535_spm_end_build(0, 0, 7715, 3, &program), SGX535_ENGINE121_INVALID);
    REFUSED(sgx535_spm_end_build(SGX535_SPM_END_WHOLE, 1, 7715, 3, &program),
            SGX535_ENGINE121_INVALID);
    REFUSED(sgx535_spm_end_build(SGX535_SPM_END_RESTART, 1, 0x10000, 3, &program),
            SGX535_ENGINE121_RANGE);
    REFUSED(sgx535_spm_ta_end_build(0, 7715, &program), SGX535_ENGINE121_INVALID);
    REFUSED(sgx535_spm_ta_end_build(1, 0x10000, &program), SGX535_ENGINE121_RANGE);
}

int main(void)
{
    te_step_and_abort();
    handoff();
    partial();
    final();
    end_and_ta_end();
    refusals();
    printf("sgx535 spm programs: %lu checks\n", checks);
    return 0;
}
