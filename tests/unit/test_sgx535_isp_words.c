// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The ISP words (src/raster/sgx535_isp_words.c) against EMGD-form vectors
 * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues), bottom-up
 * orientation, and the refusals. CPU only. */
#include "sgx535_isp_words.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

enum { NEVER, LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS };
enum { KEEP, ZERO, REPLACE, INCR, DECR, INVERT, INCR_WRAP, DECR_WRAP };

struct expected {
    const char *name;
    struct sgx535_isp_state state;
    struct sgx535_isp_words words;
};

#define FACE(func, ref, vmask, wmask, fail, zfail, zpass)                                          \
    {func, ref, vmask, wmask, fail, zfail, zpass}
/* depth test, function, write; stencil test and GL's front, back */
#define DEPTH(func, write) .depth_test = 1, .depth_func = func, .depth_write = write
#define STENCIL(f, b) .stencil_test = 1, .front = f, .back = b

static const struct expected cases[] = {
    {"depth never", {DEPTH(NEVER, 1), .colormask = 15}, {.ispa = 0x00000200, .ispb = 0xf0}},
    {"depth greater", {DEPTH(GREATER, 1), .colormask = 15}, {.ispa = 0x01000200, .ispb = 0xf0}},
    {"depth mask off", {DEPTH(LESS, 0), .colormask = 15}, {.ispa = 0x00500200, .ispb = 0xf0}},
    {"depth test off",
     {.depth_func = LESS, .depth_write = 1, .colormask = 15},
     {.ispa = 0x01d00200, .ispb = 0xf0}},
    {"offset -1 -2",
     {DEPTH(LESS, 1), .offset = 1, .offset_factor = -1, .offset_units = -2, .colormask = 15},
     {.ispa = 0x00400200, .ispb = 0x01ff00f0}},
    {"offset 1 2",
     {DEPTH(LEQUAL, 1), .offset = 1, .offset_factor = 1, .offset_units = 2, .colormask = 15},
     {.ispa = 0x00c00200, .ispb = 0x001100f0}},
    {"stencil less replace",
     {DEPTH(LESS, 1),
      STENCIL(FACE(LESS, 0x40, 0xff, 0xff, KEEP, KEEP, REPLACE),
              FACE(LESS, 0x40, 0xff, 0xff, KEEP, KEEP, REPLACE)),
      .colormask = 15},
     {.ispa = 0x00400340, .ispb = 0xf0, .ispc = 0x0202ffff, .has_ispc = 1}},
    {"stencil masks",
     {DEPTH(LESS, 1),
      STENCIL(FACE(EQUAL, 0x5f, 0xf0, 0x0f, KEEP, KEEP, REPLACE),
              FACE(EQUAL, 0x5f, 0xf0, 0x0f, KEEP, KEEP, REPLACE)),
      .colormask = 15},
     {.ispa = 0x0040035f, .ispb = 0xf0, .ispc = 0x0402f00f, .has_ispc = 1}},
    {"stencil zfail replace",
     {DEPTH(LESS, 1),
      STENCIL(FACE(ALWAYS, 0x33, 0xff, 0xff, KEEP, REPLACE, KEEP),
              FACE(ALWAYS, 0x33, 0xff, 0xff, KEEP, REPLACE, KEEP)),
      .colormask = 15},
     {.ispa = 0x00400333, .ispb = 0xf0, .ispc = 0x0e10ffff, .has_ispc = 1}},
    /* the window: GL's back state in ISPA/ISPC, its front in BF-* */
    {"two-sided refs",
     {DEPTH(LESS, 1),
      STENCIL(FACE(ALWAYS, 0x11, 0xff, 0xff, KEEP, KEEP, REPLACE),
              FACE(ALWAYS, 0x22, 0xff, 0xff, KEEP, KEEP, REPLACE)),
      .front_ccw = 1, .colormask = 15},
     {.ispa = 0x00400b22,
      .ispb = 0xf0,
      .ispc = 0x0e02ffff,
      .bf_ispa = 0x00400b11,
      .bf_ispc = 0x0e02ffff,
      .has_ispc = 1,
      .two_sided = 1}},
    {"two-sided functions",
     {DEPTH(LESS, 1),
      STENCIL(FACE(EQUAL, 0x55, 0xff, 0xff, KEEP, KEEP, KEEP),
              FACE(NOTEQUAL, 0x55, 0xff, 0xff, KEEP, KEEP, KEEP)),
      .front_ccw = 1, .colormask = 15},
     {.ispa = 0x00400b55,
      .ispb = 0xf0,
      .ispc = 0x0a00ffff,
      .bf_ispa = 0x00400b55,
      .bf_ispc = 0x0400ffff,
      .has_ispc = 1,
      .two_sided = 1}},
    {"two-sided write masks",
     {DEPTH(LESS, 1),
      STENCIL(FACE(ALWAYS, 0xaa, 0xff, 0x0f, KEEP, KEEP, REPLACE),
              FACE(ALWAYS, 0xaa, 0xff, 0xf0, KEEP, KEEP, REPLACE)),
      .front_ccw = 1, .colormask = 15},
     {.ispa = 0x00400baa,
      .ispb = 0xf0,
      .ispc = 0x0e02fff0,
      .bf_ispa = 0x00400baa,
      .bf_ispc = 0x0e02ff0f,
      .has_ispc = 1,
      .two_sided = 1}},
    {"cull back",
     {DEPTH(LESS, 1), .cull = SGX535_CULL_BACK, .front_ccw = 1, .colormask = 15},
     {.ispa = 0x00400200, .ispb = 0xf0, .cull = 1}},
    {"cull front",
     {DEPTH(LESS, 1), .cull = SGX535_CULL_FRONT, .front_ccw = 1, .colormask = 15},
     {.ispa = 0x00400200, .ispb = 0xf0, .cull = 2}},
    {"cull back cw front",
     {DEPTH(LESS, 1), .cull = SGX535_CULL_BACK, .colormask = 15},
     {.ispa = 0x00400200, .ispb = 0xf0, .cull = 2}},
    /* pass types and UPASSCTL */
    {"blend",
     {.depth_func = LESS, .depth_write = 1, .pass_type = 1, .colormask = 15},
     {.ispa = 0x03d00200, .ispb = 0xf0}},
    {"mask r",
     {.depth_func = LESS, .depth_write = 1, .colormask = 1},
     {.ispa = 0x01d00200, .ispb = 0x40}},
    {"mask gb",
     {.depth_func = LESS, .depth_write = 1, .colormask = 6},
     {.ispa = 0x01d00200, .ispb = 0x30}},
    {"mask a",
     {.depth_func = LESS, .depth_write = 1, .colormask = 8},
     {.ispa = 0x01d00200, .ispb = 0x80}},
    {"alpha test",
     {DEPTH(LESS, 1), .pass_type = 4, .colormask = 15},
     {.ispa = 0x08400200, .ispb = 0xf0}},
    {"frag depth",
     {DEPTH(LESS, 1), .pass_type = 5, .colormask = 15},
     {.ispa = 0x0a400200, .ispb = 0xf0}},
    {"alpha test blend",
     {DEPTH(LESS, 1), .pass_type = 2, .colormask = 15},
     {.ispa = 0x04400200, .ispb = 0xf0}},
    {"alpha never",
     {DEPTH(LESS, 0), .colormask = 15, .no_color = 1},
     {.ispa = 0x00700200, .ispb = 0xf0}},
};

static bool same(const struct sgx535_isp_words *a, const struct sgx535_isp_words *b)
{
    return a->ispa == b->ispa && a->ispb == b->ispb && a->has_ispc == b->has_ispc &&
           (!a->has_ispc || a->ispc == b->ispc) && a->two_sided == b->two_sided &&
           (!a->two_sided ||
            (a->bf_ispa == b->bf_ispa && a->bf_ispb == b->bf_ispb && a->bf_ispc == b->bf_ispc)) &&
           a->cull == b->cull;
}

static void emgd_words(void)
{
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct sgx535_isp_words w;
        CHECK(sgx535_isp_words(&cases[i].state, &w));
        if (!same(&w, &cases[i].words))
            fprintf(stderr, "%s: %08x %08x %08x %08x %08x %08x cull %u\n", cases[i].name, w.ispa,
                    w.ispb, w.ispc, w.bf_ispa, w.bf_ispb, w.bf_ispc, w.cull);
        CHECK(same(&w, &cases[i].words));
    }
}

/* A bottom-up FBO undoes the window's swap: GL's front in ISPA, the cull
 * values reversed (docs/native-3d.md#pixel-pds-and-isp-words; no capture). CW front faces swap the faces
 * again, in either orientation. */
static void bottom_up(void)
{
    struct sgx535_isp_state s = cases[9].state;
    struct sgx535_isp_words w;
    s.bottom_up = 1;
    CHECK(sgx535_isp_words(&s, &w) && w.ispa == 0x00400b11 && w.bf_ispa == 0x00400b22);
    s.front_ccw = 0;
    CHECK(sgx535_isp_words(&s, &w) && w.ispa == 0x00400b22 && w.bf_ispa == 0x00400b11);
    s.bottom_up = 0;
    CHECK(sgx535_isp_words(&s, &w) && w.ispa == 0x00400b11 && w.bf_ispa == 0x00400b22);
    s = cases[12].state, s.bottom_up = 1;
    CHECK(sgx535_isp_words(&s, &w) && w.cull == 2);
    s = cases[13].state, s.bottom_up = 1;
    CHECK(sgx535_isp_words(&s, &w) && w.cull == 1);
}

static void refusals(void)
{
    struct sgx535_isp_state s = {DEPTH(LESS, 1), .colormask = 15};
    struct sgx535_isp_words w;
    s.offset = 1, s.offset_factor = 16;
    CHECK(!sgx535_isp_words(&s, &w));
    s.offset_factor = 0, s.offset_units = -17;
    CHECK(!sgx535_isp_words(&s, &w));
    s.offset = 0, s.cull = SGX535_CULL_BOTH;
    CHECK(!sgx535_isp_words(&s, &w));
    s.cull = 0, s.depth_func = 8;
    CHECK(!sgx535_isp_words(&s, &w));
    s.depth_func = LESS, s.stencil_test = 1, s.back.zpass = 8;
    CHECK(!sgx535_isp_words(&s, &w));
    CHECK(!sgx535_isp_words(NULL, &w) && !sgx535_isp_words(&s, NULL));
}

int main(void)
{
    emgd_words();
    bottom_up();
    refusals();
    printf("PASS: sgx535 ISP words (%lu checks, %zu EMGD draws)\n", checks,
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
