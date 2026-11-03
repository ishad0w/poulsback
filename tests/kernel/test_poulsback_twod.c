// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* SUBMIT_2D's planner: an accept/refuse row per field and encoder status, and
 * a fuzzer whose accepted plans an independent decoder keeps inside their
 * surfaces. Literals and the decoder's own clipping, not the planner's. */
#include "../../kernel/engine/poulsback_twod.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "twod check %u line %d: %s\n", checks, __LINE__, #x);                  \
        }                                                                                          \
    } while (0)

#define BASE 0xD0000000ULL

struct job {
    struct poulsback_twod_bo bos[POULSBACK_2D_MAX_BOS + 1];
    struct poulsback_2d_surface surfaces[POULSBACK_2D_MAX_SURFACES + 1];
    struct poulsback_2d_op ops[POULSBACK_2D_MAX_OPS + 1];
    uint32_t bo_count, surface_count, op_count;
};

static uint32_t words[POULSBACK_TWOD_MAX_WORDS];

static enum poulsback_twod_status plan(const struct job *j, struct poulsback_twod_plan *p)
{
    return poulsback_twod_plan(j->bos, j->bo_count, j->surfaces, j->surface_count, j->ops,
                               j->op_count, words, POULSBACK_TWOD_MAX_WORDS, p);
}

/* Two 1 MiB BOs; A 256x128 ARGB in BO 0, B 64x64 RGB565 at 4 KiB into BO 1;
 * a fill of A and a copy A -> A. */
static void base_job(struct job *j)
{
    memset(j, 0, sizeof(*j));
    j->bo_count = 2;
    j->bos[0] = (struct poulsback_twod_bo){BASE + 0x100000, 0x100000};
    j->bos[1] = (struct poulsback_twod_bo){BASE + 0x400000, 0x100000};
    j->surface_count = 2;
    j->surfaces[0] = (struct poulsback_2d_surface){0, 0, 1024, 256, 128, POULSBACK_2D_ARGB8888, {0, 0}};
    j->surfaces[1] = (struct poulsback_2d_surface){1, 4096, 128, 64, 64, POULSBACK_2D_RGB565, {0, 0}};
    j->op_count = 2;
    j->ops[0] = (struct poulsback_2d_op){.kind = POULSBACK_2D_FILL, .rop = 3, .pixel = 0x11223344,
                                         .rect = {10, 20, 30, 40}};
    j->ops[1] = (struct poulsback_2d_op){.kind = POULSBACK_2D_COPY, .rop = 3, .rect = {50, 60, 30, 40},
                                         .src_x = 10, .src_y = 20};
}

static enum poulsback_twod_status variant(void (*edit)(struct job *))
{
    struct poulsback_twod_plan p = {7, 7, 7};
    struct job j;
    enum poulsback_twod_status s;
    base_job(&j);
    edit(&j);
    s = plan(&j, &p);
    if (s)
        CHECK(p.count == 7 && p.expected == 7 && p.writes == 7); /* untouched */
    return s;
}

/* One edit per row. */
#define EDIT(name, body)                                                                           \
    static void name(struct job *j)                                                                \
    {                                                                                              \
        body;                                                                                      \
    }
EDIT(none, (void)j)
EDIT(no_bos, j->bo_count = 0)
EDIT(many_bos, j->bo_count = POULSBACK_2D_MAX_BOS + 1)
EDIT(no_surfaces, j->surface_count = 0)
EDIT(many_surfaces, j->surface_count = POULSBACK_2D_MAX_SURFACES + 1)
EDIT(no_ops, j->op_count = 0)
EDIT(many_ops, j->op_count = POULSBACK_2D_MAX_OPS + 1)
EDIT(bo_below_class, j->bos[0].va = BASE - 0x100000)
EDIT(bo_past_class, j->bos[1].va = 0xE0000000ULL - 0x80000)
EDIT(bo_empty, j->bos[1].size = 0)
EDIT(bo_last_in_class, j->bos[1].va = 0xE0000000ULL - 0x100000)
EDIT(surface_bo, j->surfaces[1].bo = 2)
EDIT(surface_offset_past_bo, j->surfaces[1].offset = 0x100000)
EDIT(surface_offset_odd, j->surfaces[1].offset = 4100)
EDIT(surface_offset_8, j->surfaces[1].offset = 4104)
EDIT(surface_rows_past_bo, j->surfaces[1].offset = 0x100000 - 63 * 128)
EDIT(surface_rows_end_at_bo, j->surfaces[1].offset = 0x100000 - 64 * 128)
EDIT(surface_last_row_unpadded, (j->surfaces[0].width = 200,
                                 j->surfaces[0].offset = 0x100000 - (127 * 1024 + 800)))
EDIT(surface_last_row_short, (j->surfaces[0].width = 200,
                              j->surfaces[0].offset = 0x100000 - (127 * 1024 + 800) + 8))
EDIT(surface_stride_short, j->surfaces[0].stride = 896)
EDIT(surface_stride_odd, j->surfaces[0].stride = 1088)
EDIT(surface_stride_max, (j->surfaces[1].stride = 16256, j->surfaces[1].height = 30))
EDIT(surface_stride_over, (j->surfaces[1].stride = 16384, j->surfaces[1].height = 30))
EDIT(surface_width_zero, j->surfaces[1].width = 0)
EDIT(surface_width_max, (j->surfaces[1].width = 4095, j->surfaces[1].stride = 8192,
                         j->surfaces[1].height = 4))
EDIT(surface_width_over, (j->surfaces[1].width = 4096, j->surfaces[1].stride = 8192,
                          j->surfaces[1].height = 4))
EDIT(surface_height_zero, j->surfaces[1].height = 0)
EDIT(surface_height_over, j->surfaces[1].height = 4096)
EDIT(surface_format_zero, j->surfaces[1].format = 0)
EDIT(surface_format_xrgb, j->surfaces[0].format = POULSBACK_2D_XRGB8888)
EDIT(surface_format_over, j->surfaces[1].format = 4)
EDIT(surface_reserved0, j->surfaces[1].reserved[0] = 1)
EDIT(surface_reserved1, j->surfaces[1].reserved[1] = 1)
EDIT(op_kind_zero, j->ops[0].kind = 0)
EDIT(op_kind_over, j->ops[0].kind = 3)
EDIT(op_flags, j->ops[0].flags = 2)
EDIT(op_clip, (j->ops[0].flags = POULSBACK_2D_CLIP, j->ops[0].clip = (struct poulsback_2d_rect){0, 0, 5, 5}))
EDIT(op_clip_unflagged, j->ops[0].clip.height = 1)
EDIT(op_rop_max, j->ops[0].rop = 15)
EDIT(op_rop_over, j->ops[0].rop = 16)
EDIT(op_dst, j->ops[0].dst = 2)
EDIT(fill_src, j->ops[0].src = 1)
EDIT(fill_src_x, j->ops[0].src_x = 1)
EDIT(fill_src_y, j->ops[0].src_y = -1)
EDIT(fill_565_pixel, (j->ops[0].dst = 1, j->ops[0].pixel = 0xffff))
EDIT(fill_565_pixel_over, (j->ops[0].dst = 1, j->ops[0].pixel = 0x10000))
EDIT(fill_8_wide, j->ops[0].rect.width = 8)
EDIT(fill_outside, j->ops[0].rect.x = 256)
EDIT(copy_pixel, j->ops[1].pixel = 1)
EDIT(copy_src, j->ops[1].src = 2)
EDIT(copy_formats, j->ops[1].src = 1)
EDIT(copy_8_wide, j->ops[1].rect.width = 8)
EDIT(copy_8_high, j->ops[1].rect.height = 8)
EDIT(copy_8_clipped, j->ops[1].rect.x = 256 - 8)
EDIT(copy_8_unclipped, (j->ops[1].rect.x = 256 - 9, j->ops[1].rect.width = 9))
EDIT(copy_views, (j->surfaces[1] = j->surfaces[0], j->surfaces[1].offset = 1024,
                  j->surfaces[1].height = 64, j->ops[1].src = 1))
EDIT(copy_same_view, (j->surfaces[1] = j->surfaces[0], j->ops[1].src = 1))
EDIT(copy_other_bo, (j->surfaces[1] = j->surfaces[0], j->surfaces[1].bo = 1, j->ops[1].src = 1))
/* Two whole 4095x4095 fills stay under 2^25 pixels; a third goes over. */
EDIT(pixels_at_cap, (j->bo_count = 3, j->bos[2] = (struct poulsback_twod_bo){BASE + 0x1000000, 0x4000000},
                     j->surface_count = 3,
                     j->surfaces[2] = (struct poulsback_2d_surface){2, 0, 8192, 4095, 4095,
                                                                    POULSBACK_2D_RGB565, {0, 0}},
                     j->ops[0] = (struct poulsback_2d_op){.kind = POULSBACK_2D_FILL, .rop = 3, .dst = 2,
                                                          .rect = {0, 0, 4095, 4095}},
                     j->ops[1] = j->ops[0]))
EDIT(pixels_over_cap, (pixels_at_cap(j), j->op_count = 3, j->ops[2] = j->ops[0]))

static const struct row {
    const char *name;
    void (*edit)(struct job *);
    enum poulsback_twod_status want;
} rows[] = {
#define OK POULSBACK_TWOD_OK
#define INVALID POULSBACK_TWOD_INVALID
#define UNSUPPORTED POULSBACK_TWOD_UNSUPPORTED
    {"base", none, OK},
    {"bo_count 0", no_bos, INVALID},
    {"bo_count 17", many_bos, INVALID},
    {"surface_count 0", no_surfaces, INVALID},
    {"surface_count 17", many_surfaces, INVALID},
    {"op_count 0", no_ops, INVALID},
    {"op_count 257", many_ops, INVALID},
    {"BO below the 2D class", bo_below_class, INVALID},
    {"BO past the 2D class", bo_past_class, INVALID},
    {"BO of size 0", bo_empty, INVALID},
    {"BO at the class end", bo_last_in_class, OK},
    {"surface bo index", surface_bo, INVALID},
    {"surface offset at the BO end", surface_offset_past_bo, INVALID},
    {"surface offset not x8", surface_offset_odd, INVALID},
    {"surface offset x8", surface_offset_8, OK},
    {"surface rows past the BO", surface_rows_past_bo, INVALID},
    {"surface rows end at the BO end", surface_rows_end_at_bo, OK},
    {"surface last row unpadded", surface_last_row_unpadded, OK},
    {"surface last row past the BO", surface_last_row_short, INVALID},
    {"surface stride below a row", surface_stride_short, INVALID},
    {"surface stride not x128", surface_stride_odd, INVALID},
    {"surface stride 16256", surface_stride_max, OK},
    {"surface stride 16384", surface_stride_over, INVALID},
    {"surface width 0", surface_width_zero, INVALID},
    {"surface width 4095", surface_width_max, OK},
    {"surface width 4096", surface_width_over, INVALID},
    {"surface height 0", surface_height_zero, INVALID},
    {"surface height 4096", surface_height_over, INVALID},
    {"surface format 0", surface_format_zero, INVALID},
    {"surface format XRGB", surface_format_xrgb, OK},
    {"surface format 4", surface_format_over, INVALID},
    {"surface reserved[0]", surface_reserved0, INVALID},
    {"surface reserved[1]", surface_reserved1, INVALID},
    {"op kind 0", op_kind_zero, INVALID},
    {"op kind 3", op_kind_over, INVALID},
    {"op flags 2", op_flags, INVALID},
    {"op CLIP", op_clip, OK},
    {"op clip without CLIP", op_clip_unflagged, INVALID},
    {"op rop 15", op_rop_max, OK},
    {"op rop 16", op_rop_over, INVALID},
    {"op dst index", op_dst, INVALID},
    {"fill src", fill_src, INVALID},
    {"fill src_x", fill_src_x, INVALID},
    {"fill src_y", fill_src_y, INVALID},
    {"fill RGB565 pixel 0xffff", fill_565_pixel, OK},
    {"fill RGB565 pixel 0x10000", fill_565_pixel_over, INVALID},
    {"fill 8 wide", fill_8_wide, OK},
    {"fill clipped away", fill_outside, OK},
    {"copy pixel", copy_pixel, INVALID},
    {"copy src index", copy_src, INVALID},
    {"copy between formats", copy_formats, UNSUPPORTED},
    {"copy 8 wide", copy_8_wide, OK},
    {"copy 8 high", copy_8_high, OK},
    {"copy clipped to 8", copy_8_clipped, OK},
    {"copy clipped to 9", copy_8_unclipped, OK},
    {"copy between views of a BO", copy_views, INVALID},
    {"copy between equal views", copy_same_view, OK},
    {"copy between BOs", copy_other_bo, OK},
    {"pixels 2 x 4095^2", pixels_at_cap, OK},
    {"pixels 3 x 4095^2", pixels_over_cap, INVALID},
};

/* Literal words of the base job: FENCE, DST (ARGB, stride 1024, BO 0 at
 * 1 MiB), BLIT PATCOPY + colour, XY, size, FLUSH; then the copy A -> A to the
 * lower right: reverse X and Y (order 0x00800000), corners at the far end. */
static void base_words(void)
{
    static const uint32_t want[] = {
        0x70000000, 0xa0060400, 0x00100000,
        0x8000f0f0, 0x11223344, (10u << 12) | 20, (30u << 12) | 40, 0xf0000000,
        0x70000000, 0xa0060400, 0x00100000, 0x90060400, 0x00100000,
        0x30000000 | (39u << 12) | 59, 0x8080cccc, 0xffffffff, (79u << 12) | 99,
        (30u << 12) | 40, 0xf0000000,
        /* The 1x1 XRGB fill of the scratch page at TWOD_REQ_BASE (HW-2D-004). */
        0x70000000, 0xa0058080, 0x00000000, 0x8000f0f0, 0xff000000, 0, (1u << 12) | 1, 0xf0000000,
    };
    struct poulsback_twod_plan p;
    struct job j;
    base_job(&j);
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK);
    CHECK(p.count == 27 && p.expected == 6 && p.writes == 1);
    CHECK(!memcmp(words, want, sizeof(want)));
    j.ops[0].dst = 1;
    j.ops[0].pixel = 0x1234;
    j.ops[1].rect.x = 0;
    j.ops[1].rect.y = 0;
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK && p.writes == 3 && p.count == 27);
    CHECK(words[1] == (0xa0050000u | 128) && words[2] == 0x00401000);
    CHECK(words[14] == 0x8000cccc); /* to the upper left: forward */
    j.ops[1].rect.x = 300;
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK && p.count == 16 && p.expected == 4);
    j.ops[0].rect.x = 300;
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK && p.count == 0 && p.expected == 0); /* no scratch */
}

/* The fuzzer: a small deterministic generator, mostly valid requests. */
static uint64_t seed = 0x2d2d2d2dULL;
static uint32_t rnd(uint32_t n)
{
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(seed >> 33) % n;
}
/* Mostly near a surface of `extent` pixels, sometimes anywhere. */
static int32_t coord(uint32_t extent)
{
    return rnd(8) ? (int32_t)rnd(extent + 32) - 16 : (int32_t)rnd(720) - 360;
}

static uint32_t size(uint32_t extent)
{
    if (!rnd(5))
        return 7 + rnd(3);
    return 1 + rnd(rnd(4) ? extent + 1 : 400);
}

static uint32_t bpp(uint32_t format)
{
    return format == POULSBACK_2D_RGB565 ? 2 : 4;
}

static void random_job(struct job *j)
{
    uint32_t i;
    memset(j, 0, sizeof(*j));
    j->bo_count = 1 + rnd(3);
    for (i = 0; i < j->bo_count; i++)
        j->bos[i] = (struct poulsback_twod_bo){BASE + i * 0x1000000ULL + rnd(4) * 0x1000ULL,
                                               (1 + rnd(8)) * 0x80000ULL};
    if (!rnd(200))
        j->bos[rnd(j->bo_count)].va = 0xE0000000ULL - 0x1000;
    j->surface_count = 1 + rnd(4);
    for (i = 0; i < j->surface_count; i++) {
        struct poulsback_2d_surface *s = &j->surfaces[i];
        s->bo = rnd(j->bo_count + !rnd(100));
        s->format = rnd(50) ? 1 + rnd(3) : rnd(5);
        s->width = 1 + rnd(320);
        s->height = 1 + rnd(240);
        s->stride = (s->width * 4 + 127) / 128 * 128 + 128 * rnd(3);
        s->offset = 8 * rnd(rnd(4) ? 1 : 8192);
        if (!rnd(50))
            s->stride += 64;
        if (!rnd(50))
            s->offset += 4;
        if (i && !rnd(4))
            *s = j->surfaces[rnd(i)];
    }
    j->op_count = 1 + rnd(8);
    for (i = 0; i < j->op_count; i++) {
        struct poulsback_2d_op *op = &j->ops[i];
        op->kind = rnd(60) ? 1 + rnd(2) : rnd(4);
        op->rop = rnd(60) ? rnd(16) : 16 + rnd(4);
        const struct poulsback_2d_surface *d, *s;
        op->dst = rnd(j->surface_count + !rnd(100));
        d = &j->surfaces[op->dst];
        op->rect = (struct poulsback_2d_rect){coord(d->width), coord(d->height), size(d->width),
                                              size(d->height)};
        if (rnd(3) == 0) {
            op->flags = POULSBACK_2D_CLIP;
            op->clip = (struct poulsback_2d_rect){coord(d->width), coord(d->height),
                                                  rnd(d->width + 16), rnd(d->height + 16)};
        }
        if (op->kind == POULSBACK_2D_FILL) {
            op->pixel = rnd(4) ? rnd(0x10000) : (uint32_t)rnd(1u << 31) * 2;
        } else {
            op->src = rnd(4) ? rnd(j->surface_count) : op->dst;
            s = &j->surfaces[op->src];
            /* Inside one surface, often a short scroll. */
            op->src_x = op->src == op->dst && rnd(2) ? op->rect.x + (int32_t)rnd(9) - 4
                                                     : coord(s->width);
            op->src_y = op->src == op->dst && rnd(2) ? op->rect.y + (int32_t)rnd(9) - 4
                                                     : coord(s->height);
        }
    }
}

static void clip_axis(int64_t *lo, int64_t *hi, int64_t a, int64_t b)
{
    if (a > *lo)
        *lo = a;
    if (b < *hi)
        *hi = b;
}

/* One encoded command of op that covers [x0, x1) x [y0, y1) of its destination
 * exactly, in the order a copy inside one surface needs; 1 when it does, with
 * its length in *used. */
static int command(const struct job *j, const struct poulsback_2d_op *op, const uint32_t *w,
                   int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t *used)
{
    const struct poulsback_2d_surface *d = &j->surfaces[op->dst], *s = &j->surfaces[op->src];
    int copy = op->kind == POULSBACK_2D_COPY, rx = 0, ry = 0;
    int64_t dx = copy ? (int64_t)op->src_x - op->rect.x : 0;
    int64_t dy = copy ? (int64_t)op->src_y - op->rect.y : 0;
    uint64_t daddr = j->bos[d->bo].va - BASE + d->offset, saddr;
    uint32_t i = 0, blit, x, y, wd, ht, offset = 0, fmt[] = {0, 0x60000, 0x58000, 0x50000};
    if (w[i++] != 0x70000000 || w[i++] != (0xa0000000 | fmt[d->format] | d->stride) ||
        w[i++] != daddr)
        return 0;
    if (copy) {
        saddr = j->bos[s->bo].va - BASE + s->offset;
        if (w[i++] != (0x90000000 | fmt[s->format] | s->stride) || w[i++] != saddr ||
            w[i] >> 28 != 3)
            return 0;
        offset = w[i++];
    }
    blit = w[i++];
    if (blit >> 28 != 8 || (blit & 0xffff) >> 8 != (blit & 0xff))
        return 0;
    if (copy) {
        int same = op->src == op->dst || !memcmp(s, d, sizeof(*s));
        uint32_t order = blit >> 23 & 3;
        if (blit & 0x0e7f0000 || w[i++] != 0xffffffff)
            return 0;
        rx = order == 1 || order == 2;
        ry = order == 1 || order == 3;
        /* Reverse only inside one surface, and only toward the source. */
        if (!same ? order != 0 : (rx != (dx < 0) || ry != (dy < 0)))
            return 0;
    } else if (blit & 0x0fff0000) {
        return 0;
    } else {
        i++; /* colour */
    }
    x = w[i] >> 12 & 0xfff;
    y = w[i++] & 0xfff;
    wd = w[i] >> 12 & 0xfff;
    ht = w[i++] & 0xfff;
    if (w[i - 1] >> 24 || w[i - 2] >> 24 || w[i++] != 0xf0000000)
        return 0;
    if (rx)
        x -= wd - 1;
    if (ry)
        y -= ht - 1;
    if (x != x0 || y != y0 || wd != x1 - x0 || ht != y1 - y0)
        return 0;
    if (copy && offset != (0x30000000 | (uint32_t)(x0 + dx + (rx ? wd - 1 : 0)) << 12 |
                           (uint32_t)(y0 + dy + (ry ? ht - 1 : 0))))
        return 0;
    /* Inside the BOs: the last pixel of the last row. */
    if (d->offset + (uint64_t)(y0 + ht - 1) * d->stride + (uint64_t)(x0 + wd) * bpp(d->format) >
            j->bos[d->bo].size ||
        (copy && s->offset + (uint64_t)(y0 + dy + ht - 1) * s->stride +
                         (uint64_t)(x0 + dx + wd) * bpp(s->format) >
                     j->bos[s->bo].size))
        return 0;
    *used = i;
    return 1;
}

/* The decoder's view of one encoded operation: nothing when it is clipped
 * away, else one command, or two when its destination starts off a 64-byte
 * boundary and it keeps at least 192 bytes of 8 rows (HW-2D-006): a head up to
 * the boundary and the aligned body, the body first for a copy toward larger x
 * inside one surface. 1 when the words match op exactly; *split tells which. */
static int decode(const struct job *j, const struct poulsback_2d_op *op, const uint32_t *w,
                  uint32_t *used, int *split)
{
    const struct poulsback_2d_surface *d = &j->surfaces[op->dst], *s = &j->surfaces[op->src];
    int copy = op->kind == POULSBACK_2D_COPY;
    int64_t x0 = op->rect.x, y0 = op->rect.y, x1 = x0 + op->rect.width, y1 = y0 + op->rect.height;
    int64_t dx = copy ? (int64_t)op->src_x - op->rect.x : 0;
    int64_t dy = copy ? (int64_t)op->src_y - op->rect.y : 0;
    uint64_t phase;
    uint32_t head, first = 0, second = 0;
    clip_axis(&x0, &x1, 0, d->width);
    clip_axis(&y0, &y1, 0, d->height);
    if (copy) {
        clip_axis(&x0, &x1, -dx, (int64_t)s->width - dx);
        clip_axis(&y0, &y1, -dy, (int64_t)s->height - dy);
    }
    if (op->flags) {
        clip_axis(&x0, &x1, op->clip.x, (int64_t)op->clip.x + op->clip.width);
        clip_axis(&y0, &y1, op->clip.y, (int64_t)op->clip.y + op->clip.height);
    }
    *split = 0;
    if (x0 >= x1 || y0 >= y1) {
        *used = 0;
        return 1;
    }
    phase = (j->bos[d->bo].va - BASE + d->offset + (uint64_t)x0 * bpp(d->format)) % 64;
    head = phase ? (uint32_t)(64 - phase) / bpp(d->format) : 0;
    if (!head || (x1 - x0) * bpp(d->format) < 192 || y1 - y0 < 8)
        return command(j, op, w, x0, y0, x1, y1, used);
    *split = 1;
    if (copy && (op->src == op->dst || !memcmp(s, d, sizeof(*s))) && dx < 0) {
        *split = 2;
        if (!command(j, op, w, x0 + head, y0, x1, y1, &first) ||
            !command(j, op, w + first, x0, y0, x0 + head, y1, &second))
            return 0;
    } else if (!command(j, op, w, x0, y0, x0 + head, y1, &first) ||
               !command(j, op, w + first, x0 + head, y0, x1, y1, &second)) {
        return 0;
    }
    *used = first + second;
    return 1;
}

/* Front-like layouts for the split (HW-2D-006): A and B 1280x800 ARGB at
 * stride 5,120 in BOs 0 and 1, C 1280x100 RGB565 at offset 8 in BO 2, D as A
 * at offset 16 in BO 2. */
static void split_job(struct job *j, struct poulsback_2d_op op)
{
    memset(j, 0, sizeof(*j));
    j->bo_count = 3;
    j->bos[0] = (struct poulsback_twod_bo){BASE + 0x1000000, 0x400000};
    j->bos[1] = (struct poulsback_twod_bo){BASE + 0x2000000, 0x400000};
    j->bos[2] = (struct poulsback_twod_bo){BASE + 0x3000000, 0x400000};
    j->surface_count = 4;
    j->surfaces[0] = (struct poulsback_2d_surface){0, 0, 5120, 1280, 800, POULSBACK_2D_ARGB8888, {0, 0}};
    j->surfaces[1] = (struct poulsback_2d_surface){1, 0, 5120, 1280, 800, POULSBACK_2D_ARGB8888, {0, 0}};
    j->surfaces[2] = (struct poulsback_2d_surface){2, 8, 2560, 1280, 100, POULSBACK_2D_RGB565, {0, 0}};
    j->surfaces[3] = (struct poulsback_2d_surface){2, 16, 5120, 1280, 700, POULSBACK_2D_ARGB8888, {0, 0}};
    j->op_count = 1;
    j->ops[0] = op;
}

static struct poulsback_2d_op split_copy(uint32_t dst, int32_t x, int32_t y, uint32_t w, uint32_t h,
                                         uint32_t src, int32_t sx, int32_t sy)
{
    return (struct poulsback_2d_op){.kind = POULSBACK_2D_COPY, .rop = 3, .dst = dst, .src = src,
                                    .rect = {x, y, w, h}, .src_x = sx, .src_y = sy};
}

static struct poulsback_2d_op split_fill(uint32_t dst, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    return (struct poulsback_2d_op){.kind = POULSBACK_2D_FILL, .rop = 3, .dst = dst,
                                    .rect = {x, y, w, h}};
}

/* One operation's commands: destination columns {x, width} in order, all of
 * rows [y0, y1); then the scratch fill. */
static void split_case(const char *name, struct poulsback_2d_op op, int64_t y0, int64_t y1,
                       const int32_t (*want)[2], uint32_t n)
{
    struct poulsback_twod_plan p = {0};
    uint32_t i, at = 0, used = 0;
    struct job j;
    int ok;
    split_job(&j, op);
    ok = plan(&j, &p) == POULSBACK_TWOD_OK && p.expected == 2 * n + (n ? 2 : 0);
    for (i = 0; ok && i < n; i++) {
        ok = command(&j, &j.ops[0], words + at, want[i][0], y0, want[i][0] + want[i][1], y1, &used);
        at += used;
    }
    ok = ok && p.count == at + (n ? SGX535_2D_RASTER_FILL_WORDS : 0);
    if (!ok)
        fprintf(stderr, "twod split \"%s\": not as wanted\n", name);
    CHECK(ok);
}

static void splits(void)
{
    static const struct poulsback_2d_op big = {.kind = POULSBACK_2D_COPY, .rop = 3, .dst = 0, .src = 1,
                                               .rect = {700, 100, 300, 300}};
    struct poulsback_2d_op op;
    struct poulsback_twod_plan p;
    struct job j;
    uint32_t i;
#define CASE(name, o, y0, y1, ...)                                                                 \
    do {                                                                                           \
        static const int32_t want[][2] = {__VA_ARGS__};                                            \
        split_case(name, o, y0, y1, want, sizeof(want) / sizeof(want[0]));                         \
    } while (0)
    /* A Present copy: x 700 is 48 mod 64 bytes; 704 is aligned. */
    CASE("copy at 48", split_copy(0, 700, 100, 300, 300, 1, 0, 0), 100, 400, {700, 4}, {704, 296});
    CASE("copy at 16", split_copy(0, 708, 100, 300, 300, 1, 0, 0), 100, 400, {708, 12}, {720, 288});
    CASE("copy aligned", split_copy(0, 704, 100, 300, 300, 1, 0, 0), 100, 400, {704, 300});
    CASE("fill at 48", split_fill(0, 700, 100, 300, 300), 100, 400, {700, 4}, {704, 296});
    /* Inside one surface toward larger x the body goes first (its source lies
     * right of the head's); otherwise the head. */
    CASE("scroll right", split_copy(0, 102, 0, 300, 300, 0, 100, 2), 0, 300, {112, 290}, {102, 10});
    CASE("scroll left", split_copy(0, 102, 0, 300, 300, 0, 104, 2), 0, 300, {102, 10}, {112, 290});
    CASE("scroll down", split_copy(0, 102, 10, 300, 300, 0, 102, 0), 10, 310, {102, 10}, {112, 290});
    CASE("scroll up", split_copy(0, 102, 0, 300, 300, 0, 102, 10), 0, 300, {102, 10}, {112, 290});
    /* The start the encoder's clipping keeps decides: the clip, the source's
     * left edge, the destination's left edge. */
    op = split_copy(0, 690, 100, 300, 300, 1, 0, 0);
    op.flags = POULSBACK_2D_CLIP;
    op.clip = (struct poulsback_2d_rect){700, 0, 1280, 800};
    CASE("clipped start", op, 100, 400, {700, 4}, {704, 286});
    CASE("source edge", split_copy(0, 696, 100, 300, 300, 1, -4, 0), 100, 400, {700, 4}, {704, 292});
    CASE("destination edge", split_fill(0, -3, 0, 300, 300), 0, 300, {0, 297});
    /* The phase counts the surface offset and the pixel size. */
    CASE("RGB565", split_fill(2, 3, 0, 300, 100), 0, 100, {3, 25}, {28, 275});
    CASE("offset 16", split_fill(3, 0, 0, 300, 300), 0, 300, {0, 12}, {12, 288});
    /* Below the alignment policy's 192-byte width or eight-row threshold,
     * clipping leaves one command (docs/hardware.md, HW-2D-006). */
    CASE("47 wide", split_copy(0, 700, 100, 47, 300, 1, 0, 0), 100, 400, {700, 47});
    CASE("48 wide", split_copy(0, 700, 100, 48, 300, 1, 0, 0), 100, 400, {700, 4}, {704, 44});
    CASE("7 rows", split_fill(0, 700, 100, 300, 7), 100, 107, {700, 300});
    CASE("8 rows", split_fill(0, 700, 100, 300, 8), 100, 108, {700, 4}, {704, 296});
    CASE("RGB565 95 wide", split_fill(2, 3, 0, 95, 100), 0, 100, {3, 95});
    CASE("RGB565 96 wide", split_fill(2, 3, 0, 96, 100), 0, 100, {3, 25}, {28, 71});
    op = split_fill(0, 700, 100, 300, 300);
    op.flags = POULSBACK_2D_CLIP;
    op.clip = (struct poulsback_2d_rect){0, 395, 1280, 100};
    CASE("clipped to 5 rows", op, 395, 400, {700, 300});
    CASE("bottom edge", split_fill(0, 700, 795, 300, 300), 795, 800, {700, 300});
    CASE("source bottom", split_copy(0, 700, 100, 300, 300, 1, 0, 795), 100, 105, {700, 300});
    op.clip = (struct poulsback_2d_rect){0, 0, 747, 800};
    CASE("clipped to 47 wide", op, 100, 400, {700, 47});
    split_job(&j, split_fill(0, 1280, 0, 300, 300));
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK && p.count == 0 && p.expected == 0); /* clipped away */
    /* Capacity: 256 split copies and the scratch fill fill it exactly. */
    split_job(&j, big);
    j.op_count = POULSBACK_2D_MAX_OPS;
    for (i = 0; i < j.op_count; i++)
        j.ops[i] = big;
    CHECK(plan(&j, &p) == POULSBACK_TWOD_OK && p.count == POULSBACK_TWOD_MAX_WORDS &&
          p.expected == 2 * (2 * POULSBACK_2D_MAX_OPS + 1));
    CHECK(poulsback_twod_plan(j.bos, j.bo_count, j.surfaces, j.surface_count, j.ops, j.op_count,
                              words, POULSBACK_TWOD_MAX_WORDS - 1, &p) == POULSBACK_TWOD_INVALID);
#undef CASE
}

static void fuzz(unsigned jobs)
{
    unsigned accepted = 0, invalid = 0, unsupported = 0, empty = 0, ordered = 0, bad = 0;
    unsigned splits = 0, body_first = 0;
    unsigned n;
    for (n = 0; n < jobs; n++) {
        struct poulsback_twod_plan p = {0};
        struct job j;
        uint32_t i, at = 0, writes = 0, commands = 0;
        enum poulsback_twod_status s;
        random_job(&j);
        s = plan(&j, &p);
        if (s == POULSBACK_TWOD_INVALID) {
            invalid++;
            continue;
        }
        if (s == POULSBACK_TWOD_UNSUPPORTED) {
            unsupported++;
            continue;
        }
        accepted++;
        for (i = 0; i < j.op_count; i++) {
            uint32_t used = 0;
            int split;
            if (!decode(&j, &j.ops[i], words + at, &used, &split)) {
                if (bad++ < 5)
                    fprintf(stderr, "twod fuzz job %u op %u: plan outside its op\n", n, i);
                break;
            }
            if (!used)
                empty++;
            else if (words[at + (j.ops[i].kind == POULSBACK_2D_COPY ? 6 : 3)] >> 23 & 3)
                ordered++;
            splits += split != 0;
            body_first += split == 2;
            commands += used ? (split ? 2u : 1u) : 0u;
            at += used;
            writes |= 1u << j.surfaces[j.ops[i].dst].bo;
        }
        /* A job that encodes anything ends with the scratch fill. */
        if (i == j.op_count)
            CHECK(at + (commands ? SGX535_2D_RASTER_FILL_WORDS : 0) == p.count &&
                  p.expected == 2 * commands + (commands ? 2 : 0) && p.writes == writes);
    }
    CHECK(bad == 0);
    /* The generator reaches every outcome. */
    CHECK(accepted > jobs / 10 && invalid > jobs / 20 && unsupported > jobs / 100);
    CHECK(empty > 0 && ordered > 0 && splits > 0 && body_first > 0);
    printf("twod fuzz: %u jobs, %u accepted (%u empty ops, %u ordered copies, %u split, %u body "
           "first), %u invalid, %u unsupported\n",
           jobs, accepted, empty, ordered, splits, body_first, invalid, unsupported);
}

int main(void)
{
    size_t i;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        enum poulsback_twod_status s = variant(rows[i].edit);
        if (s != rows[i].want)
            fprintf(stderr, "twod row \"%s\": status %d, want %d\n", rows[i].name, s,
                    rows[i].want);
        CHECK(s == rows[i].want);
    }
    base_words();
    splits();
    fuzz(100000);
    printf("twod-plan: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
