// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The pixel epilogue (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues): COLOR0 packed
 * into B8G8R8A8 bytes by two packs, then
 * one sop2/asop2 for blending (constant factors premultiplied by sop2wm),
 * sop2wm for colour masks, and/or/xor for logic ops; atst8 for the alpha test
 * and discard (pass type 4, or 2 when the colour is blended; pcoeff first),
 * depthf for gl_FragDepth (5). Those run in two phases: the visibility test,
 * then the colour write from pa0 at an even instruction (DOUTU1 EXE1). */
#include "sgx535_fs.h"
#include <stdbool.h>
#include <string.h>

#define NO_REG 0xffu
/* SOP2 colour selectors, alpha selectors, SOPWM's. */
enum { C_ZERO, C_S1, C_S2, C_S1A, C_S2A, C_ASAT };
enum { A_ZERO, A_S1A, A_S2A };
enum { W_ZERO = 0, W_S1 = 2, W_S1A = 3, W_S2 = 6, W_S2A = 7 };
enum { OP_ADD, OP_SUB, OP_MIN, OP_MAX };
/* GL's logic ops. */
enum {
    CLEAR,
    AND,
    AND_REVERSE,
    COPY,
    AND_INVERTED,
    NOOP,
    XOR,
    OR,
    NOR,
    EQUIV,
    INVERT,
    OR_REVERSE,
    COPY_INVERTED,
    OR_INVERTED,
    NAND,
    SET
};

struct linker {
    struct sgx535_fs_variant *out;
    const struct sgx535_fs_program *body;
    bool busy[SGX535_FS_TEMPS];
    bool in_pa0; /* the colour phase: phase 1 left COLOR0 packed in pa0 */
};

static const struct sgx535_use_operand none = {SGX535_USE_R, 0, 0};
static struct sgx535_use_operand reg(uint8_t bank, uint32_t number)
{
    return (struct sgx535_use_operand){bank, (uint8_t)number, 0};
}
#define PA0 reg(SGX535_USE_PA, 0)
#define O0 reg(SGX535_USE_O, 0)
#define ONES reg(SGX535_USE_C, 56) /* all bits set: EMGD's COPY_INVERTED, c56 & ~src */

static enum sgx535_fs_status refuse(struct linker *l, const char *reason)
{
    l->out->reason = reason;
    return SGX535_FS_ERR_UNSUPPORTED;
}

static bool put(struct linker *l, struct sgx535_use_instruction in)
{
    return sgx535_use_append(l->out->words, &l->out->instruction_count,
                             sizeof(l->out->words) / sizeof(l->out->words[0]) / 2u,
                             &in) == SGX535_USE_OK;
}

/* The epilogue's SA word for WHAT, appended after the body's. */
static int constant(struct linker *l, uint32_t what)
{
    return sgx535_fs_constant(l->out->constant, &l->out->constant_count,
                              SGX535_FS_CONSTANT_EPILOGUE, what);
}

/* A temporary the body's results do not occupy, lowest first. */
static int scratch(struct linker *l)
{
    return sgx535_fs_temps(l->busy, SGX535_FS_TEMPS, &l->out->temps, 1);
}

/* Packs take no modifiers: a modified source goes through a temporary. */
static bool unmodified(struct linker *l, struct sgx535_use_operand *o)
{
    int t;
    if (!o->mod)
        return true;
    t = scratch(l);
    if (t < 0 || !put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_FMAX,
                                                         .skipinv = 1,
                                                         .dst = reg(SGX535_USE_R, (uint32_t)t),
                                                         .src = {none, *o, *o}}))
        return false;
    *o = reg(SGX535_USE_R, (uint32_t)t);
    return true;
}

/* COLOR0 into DST's bytes B, G, R, A: G and R by mask 0110 (the first source
 * to the lower byte), B and A by mask 1001; a packed colour is moved whole. */
static bool pack(struct linker *l, struct sgx535_use_operand dst, bool end)
{
    const struct sgx535_fs_program *p = l->body;
    struct sgx535_use_operand c[4];
    /* the pixel task's primary attributes hold a PA destination: a body
     * without iterated inputs has none, so the source of a blend or masked
     * write would read as zero */
    if (dst.bank == SGX535_USE_PA && l->out->primary <= dst.number)
        l->out->primary = dst.number + 1u;
    if (p->color_packed || l->in_pa0) {
        struct sgx535_use_operand from = l->in_pa0 ? PA0 : reg(SGX535_USE_PA, p->color_pa);
        if (dst.bank == from.bank && dst.number == from.number && !end)
            return true;
        return put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_MOV,
                                                      .skipinv = 1,
                                                      .end = end,
                                                      .dst = dst,
                                                      .src = {none, from, none}});
    }
    memcpy(c, p->color, sizeof(c));
    for (unsigned k = 0; k < 4; k++)
        if (!unmodified(l, &c[k]))
            return false;
    return put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_PCKU8F32,
                                                  .skipinv = 1,
                                                  .scale = 1,
                                                  .writemask = 1,
                                                  .mask = 6,
                                                  .dst = dst,
                                                  .src = {none, c[1], c[0]}}) &&
           put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_PCKU8F32,
                                                  .skipinv = 1,
                                                  .scale = 1,
                                                  .writemask = 1,
                                                  .end = end,
                                                  .mask = 9,
                                                  .dst = dst,
                                                  .src = {none, c[2], c[3]}});
}

/* A masked write of pa0 into o0 (EMGD: max of pa0 with itself, factors unused). */
static bool masked(struct linker *l, uint8_t colormask)
{
    /* SOPWM's write mask: A 0, B 1, G 2, R 3. */
    uint8_t mask = (uint8_t)((colormask & 1u) << 3 | (colormask & 2u) << 1 | (colormask & 4u) >> 1 |
                             (colormask & 8u) >> 3);
    return put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_SOPWM,
                                                  .skipinv = 1,
                                                  .end = 1,
                                                  .mask = mask,
                                                  .dst = O0,
                                                  .src = {none, PA0, PA0},
                                                  .cop = OP_MAX,
                                                  .aop = OP_MAX});
}

/* ---- blending -------------------------------------------------------------------- */

static bool constant_factor(uint8_t f)
{
    return f >= SGX535_FS_CONSTANT_COLOR && f <= SGX535_FS_ONE_MINUS_CONSTANT_ALPHA;
}
/* GL's factor F as a SOP2 colour or alpha selector and its complement, with
 * the source in s1 (SWAP: in s2). */
static void selector(uint8_t f, bool alpha, bool swap, uint8_t *sel, uint8_t *comp)
{
    uint8_t src = alpha ? (swap ? A_S2A : A_S1A) : (swap ? C_S2 : C_S1);
    uint8_t dst = alpha ? (swap ? A_S1A : A_S2A) : (swap ? C_S1 : C_S2);
    uint8_t src_alpha = alpha ? src : (swap ? C_S2A : C_S1A);
    uint8_t dst_alpha = alpha ? dst : (swap ? C_S1A : C_S2A);
    *comp = 0;
    switch (f) {
    case SGX535_FS_ONE:
        *comp = 1;
        /* fallthrough */
    case SGX535_FS_ZERO:
        *sel = alpha ? A_ZERO : C_ZERO;
        return;
    case SGX535_FS_ONE_MINUS_SRC_COLOR:
        *comp = 1;
        /* fallthrough */
    case SGX535_FS_SRC_COLOR:
        *sel = src;
        return;
    case SGX535_FS_ONE_MINUS_DST_COLOR:
        *comp = 1;
        /* fallthrough */
    case SGX535_FS_DST_COLOR:
        *sel = dst;
        return;
    case SGX535_FS_ONE_MINUS_SRC_ALPHA:
        *comp = 1;
        /* fallthrough */
    case SGX535_FS_SRC_ALPHA:
        *sel = src_alpha;
        return;
    case SGX535_FS_ONE_MINUS_DST_ALPHA:
        *comp = 1;
        /* fallthrough */
    case SGX535_FS_DST_ALPHA:
        *sel = dst_alpha;
        return;
    default: /* SRC_ALPHA_SATURATE: min(As, 1 - Ad) for colour, one for alpha */
        *sel = alpha ? A_ZERO : C_ASAT;
        *comp = alpha;
        return;
    }
}

/* A target without alpha reads destination alpha as one (docs/compiler.md#fragment-backend). */
static uint8_t substitute(uint8_t f, uint8_t target)
{
    if (target != SGX535_FS_TARGET_RGB8)
        return f;
    if (f == SGX535_FS_DST_ALPHA)
        return SGX535_FS_ONE;
    if (f == SGX535_FS_ONE_MINUS_DST_ALPHA || f == SGX535_FS_SRC_ALPHA_SATURATE)
        return SGX535_FS_ZERO;
    return f;
}

/* One side times its factor by sop2wm, colour bytes then alpha (EMGD's
 * premultiply): the target side as sop2wm T, X, o0, zero, F; the source side
 * as sop2wm pa0, pa0, X, F, zero. X is the operand the factor reads (the blend
 * colour, the source or the target); F names it as s1 or s2 by its place. */
static bool premultiply(struct linker *l, struct sgx535_use_operand dst, bool target_side,
                        const uint8_t factor[2], struct sgx535_use_operand blend_color)
{
    for (unsigned a = 0; a < 2; a++) {
        /* SRC_ALPHA_SATURATE's alpha factor is one */
        uint8_t f = a && factor[a] == SGX535_FS_SRC_ALPHA_SATURATE ? SGX535_FS_ONE : factor[a];
        uint8_t sel = W_ZERO, comp = 0;
        struct sgx535_use_operand x = blend_color;
        bool reads_x = true, alpha;
        switch (f) {
        case SGX535_FS_ZERO:
        case SGX535_FS_ONE:
            comp = f == SGX535_FS_ONE;
            reads_x = false;
            break;
        case SGX535_FS_SRC_ALPHA_SATURATE: /* the source side: min(As, 1 - Ad) */
            x = O0, sel = 1;
            reads_x = false;
            break;
        default:
            comp = f == SGX535_FS_ONE_MINUS_CONSTANT_COLOR ||
                   f == SGX535_FS_ONE_MINUS_CONSTANT_ALPHA || f == SGX535_FS_ONE_MINUS_SRC_COLOR ||
                   f == SGX535_FS_ONE_MINUS_SRC_ALPHA || f == SGX535_FS_ONE_MINUS_DST_COLOR ||
                   f == SGX535_FS_ONE_MINUS_DST_ALPHA;
            alpha = f >= SGX535_FS_CONSTANT_ALPHA ||
                    (f >= SGX535_FS_SRC_ALPHA && f <= SGX535_FS_ONE_MINUS_DST_ALPHA);
            if (f >= SGX535_FS_SRC_COLOR && f <= SGX535_FS_ONE_MINUS_SRC_ALPHA &&
                f != SGX535_FS_DST_COLOR && f != SGX535_FS_ONE_MINUS_DST_COLOR) {
                /* the source: X on the target side, the value itself on the source side */
                x = PA0, reads_x = target_side;
            } else if (f >= SGX535_FS_DST_COLOR && f <= SGX535_FS_ONE_MINUS_DST_ALPHA &&
                       f != SGX535_FS_SRC_ALPHA && f != SGX535_FS_ONE_MINUS_SRC_ALPHA) {
                x = O0, reads_x = !target_side;
            }
            /* s1 is X on the target side and the value on the source side */
            sel = (reads_x == target_side) ? (alpha ? W_S1A : W_S1) : (alpha ? W_S2A : W_S2);
            break;
        }
        if (!put(l, (struct sgx535_use_instruction){
                        .opcode = SGX535_USE_SOPWM,
                        .skipinv = 1,
                        .mask = a ? 1u : 14u,
                        .dst = dst,
                        .src = {none, target_side ? x : PA0, target_side ? O0 : x},
                        .csel1 = target_side ? W_ZERO : sel,
                        .csel2 = target_side ? sel : W_ZERO,
                        .cmod1 = target_side ? 0 : comp,
                        .cmod2 = target_side ? comp : 0}))
            return false;
    }
    return true;
}

/* Subtract in one half and reverse subtract in the other: the halves take
 * opposite operand orders, so the colour bytes and the alpha byte are two
 * sop2wm, each with its own order (sop2's selectors in sop2wm's codes). */
static bool split(struct linker *l, const uint8_t f[4], const uint8_t eq[2], const uint8_t op[2],
                  struct sgx535_use_operand dst_value, bool partial)
{
    static const uint8_t colour[] = {[C_ZERO] = W_ZERO, [C_S1] = W_S1,   [C_S2] = W_S2,
                                     [C_S1A] = W_S1A,   [C_S2A] = W_S2A, [C_ASAT] = 1};
    static const uint8_t alpha[] = {[A_ZERO] = W_ZERO, [A_S1A] = W_S1A, [A_S2A] = W_S2A};
    for (unsigned a = 0; a < 2; a++) {
        bool swap = eq[a] == SGX535_FS_EQ_REVERSE_SUBTRACT;
        uint8_t sel[2], comp[2];
        selector(f[2u * a + swap], a, swap, &sel[0], &comp[0]);
        selector(f[2u * a + !swap], a, swap, &sel[1], &comp[1]);
        if (!put(l, (struct sgx535_use_instruction){
                        .opcode = SGX535_USE_SOPWM,
                        .skipinv = 1,
                        .end = a && !partial,
                        .mask = a ? 1u : 14u,
                        .dst = partial ? PA0 : O0,
                        .src = {none, swap ? dst_value : PA0, swap ? PA0 : dst_value},
                        .csel1 = a ? alpha[sel[0]] : colour[sel[0]],
                        .csel2 = a ? alpha[sel[1]] : colour[sel[1]],
                        .cmod1 = comp[0],
                        .cmod2 = comp[1],
                        .cop = op[a],
                        .aop = op[a]}))
            return false;
    }
    return true;
}

static enum sgx535_fs_status blend(struct linker *l, const struct sgx535_fs_epilogue_key *k)
{
    uint8_t f[4] = {substitute(k->rgb_src, k->target), substitute(k->rgb_dst, k->target),
                    substitute(k->alpha_src, k->target), substitute(k->alpha_dst, k->target)};
    uint8_t eq[2] = {k->rgb_eq, k->alpha_eq}, op[2], sel[4], comp[4];
    bool swap = eq[0] == SGX535_FS_EQ_REVERSE_SUBTRACT || eq[1] == SGX535_FS_EQ_REVERSE_SUBTRACT;
    bool partial = (k->colormask & 15u) != 15u;
    bool src_constant = constant_factor(f[0]) || constant_factor(f[2]);
    bool dst_constant = constant_factor(f[1]) || constant_factor(f[3]);
    struct sgx535_use_operand dst_value = O0;
    int sa = -1;
    for (unsigned c = 0; c < 2; c++) {
        if (eq[c] > SGX535_FS_EQ_MAX)
            return refuse(l, "blend equation");
        /* min and max take no factors; sop2's are ONE then (EMGD). */
        if (eq[c] >= SGX535_FS_EQ_MIN)
            f[2u * c] = f[2u * c + 1u] = SGX535_FS_ONE;
        op[c] = eq[c] == SGX535_FS_EQ_ADD                ? OP_ADD
                : eq[c] <= SGX535_FS_EQ_REVERSE_SUBTRACT ? OP_SUB
                : eq[c] == SGX535_FS_EQ_MIN              ? OP_MIN
                                                         : OP_MAX;
    }
    /* Reverse subtract swaps the sources of both halves, unless the other
     * half subtracts: then each half has its own order (split). */
    bool halves = swap && (eq[0] == SGX535_FS_EQ_SUBTRACT || eq[1] == SGX535_FS_EQ_SUBTRACT);
    for (unsigned i = 0; i < 4; i++)
        if (f[i] > SGX535_FS_SRC_ALPHA_SATURATE)
            return refuse(l, "blend factor");
    /* GL 2.1 takes SRC_ALPHA_SATURATE as a source factor only. */
    if (f[1] == SGX535_FS_SRC_ALPHA_SATURATE || f[3] == SGX535_FS_SRC_ALPHA_SATURATE)
        return refuse(l, "SRC_ALPHA_SATURATE as a destination factor (GL 2.1: source only)");
    /* asat is min(s1a, 1 - s2a), the source in s1: where the colour half's
     * operands are swapped, the source is premultiplied by it first. */
    if (src_constant || dst_constant ||
        (f[0] == SGX535_FS_SRC_ALPHA_SATURATE &&
         (halves ? eq[0] == SGX535_FS_EQ_REVERSE_SUBTRACT : swap))) {
        /* Both sides times their factors, then ONE and ONE: the target side
         * into a temporary first (it may read the unmultiplied source), then
         * the source in pa0 (EMGD's constant-colour form). */
        const uint8_t fd[2] = {f[1], f[3]}, fsrc[2] = {f[0], f[2]};
        int t = scratch(l);
        sa = constant(l, SGX535_FS_BLEND_COLOR);
        if (sa < 0 || t < 0 || !pack(l, PA0, false))
            return SGX535_FS_ERR_LIMIT;
        dst_value = reg(SGX535_USE_R, (uint32_t)t);
        if (!premultiply(l, dst_value, true, fd, reg(SGX535_USE_SA, (uint32_t)sa)) ||
            !premultiply(l, PA0, false, fsrc, reg(SGX535_USE_SA, (uint32_t)sa)))
            return SGX535_FS_ERR_LIMIT;
        for (unsigned i = 0; i < 4; i++)
            f[i] = SGX535_FS_ONE;
    } else if (!pack(l, PA0, false))
        return SGX535_FS_ERR_LIMIT;
    if (halves)
        return split(l, f, eq, op, dst_value, partial) && (!partial || masked(l, k->colormask))
                   ? SGX535_FS_OK
                   : SGX535_FS_ERR_LIMIT;
    /* sop2 RESULT, src1, src2: source then destination, swapped for reverse
     * subtract; the selector for src1's factor is CSEL1. */
    selector(f[swap ? 1 : 0], false, swap, &sel[0], &comp[0]);
    selector(f[swap ? 0 : 1], false, swap, &sel[1], &comp[1]);
    selector(f[swap ? 3 : 2], true, swap, &sel[2], &comp[2]);
    selector(f[swap ? 2 : 3], true, swap, &sel[3], &comp[3]);
    if (!put(l, (struct sgx535_use_instruction){
                    .opcode = SGX535_USE_SOP2,
                    .skipinv = 1,
                    .end = !partial,
                    .dst = partial ? PA0 : O0,
                    .src = {none, swap ? dst_value : PA0, swap ? PA0 : dst_value},
                    .csel1 = sel[0],
                    .cmod1 = comp[0],
                    .csel2 = sel[1],
                    .cmod2 = comp[1],
                    .asel1 = sel[2],
                    .amod1 = comp[2],
                    .asel2 = sel[3],
                    .amod2 = comp[3],
                    .cop = op[0],
                    .aop = op[1]}))
        return SGX535_FS_ERR_LIMIT;
    return !partial || masked(l, k->colormask) ? SGX535_FS_OK : SGX535_FS_ERR_LIMIT;
}

/* ---- logic ops ------------------------------------------------------------------------- */

static enum sgx535_fs_status logic(struct linker *l, const struct sgx535_fs_epilogue_key *k)
{
    /* RESULT = src1 op src2 per GL op, CLEAR to SET: the op (and, or, xor);
     * the sources (s: the fragment pa0 then the target o0, d: the target
     * then the fragment, 1: c56's ones then the target, z: zero then the
     * fragment); src2 inverted (1), or the fragment inverted against the
     * ones (2); then the result inverted by a second step (1). COPY and NOOP
     * never get here. */
    static const char form[16][5] = {"az00", "as00", "as10", "",     "ad10", "",
                                     "xs00", "os00", "os01", "xs01", "a110", "os10",
                                     "a120", "od10", "as01", "o100"};
    static const uint8_t opcode[] = {
        ['a'] = SGX535_USE_AND, ['o'] = SGX535_USE_OR, ['x'] = SGX535_USE_XOR};
    bool partial = (k->colormask & 15u) != 15u;
    struct sgx535_use_operand src1, src2;
    const char *f;
    if (k->logic_op > SET || !form[k->logic_op][0])
        return refuse(l, "logic op");
    f = form[k->logic_op];
    if (!pack(l, PA0, false))
        return SGX535_FS_ERR_LIMIT;
    switch (f[1]) {
    case 's':
        src1 = PA0, src2 = O0;
        break;
    case 'd':
        src1 = O0, src2 = PA0;
        break;
    case '1':
        src1 = ONES, src2 = f[2] == '2' ? PA0 : O0;
        break;
    default:
        src1 = reg(SGX535_USE_C, SGX535_USE_C_ZERO), src2 = PA0;
        break;
    }
    if (f[3] == '1') {
        /* ~(s op d): op into pa0, then c56 & ~pa0 */
        if (!put(l, (struct sgx535_use_instruction){.opcode = opcode[(unsigned char)f[0]],
                                                    .skipinv = 1,
                                                    .dst = PA0,
                                                    .src = {none, src1, src2}}))
            return SGX535_FS_ERR_LIMIT;
        src1 = ONES, src2 = PA0;
        if (!put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_AND,
                                                    .skipinv = 1,
                                                    .end = !partial,
                                                    .invert = 1,
                                                    .dst = partial ? PA0 : O0,
                                                    .src = {none, src1, src2}}))
            return SGX535_FS_ERR_LIMIT;
    } else if (!put(l, (struct sgx535_use_instruction){.opcode = opcode[(unsigned char)f[0]],
                                                       .skipinv = 1,
                                                       .end = !partial,
                                                       .invert = f[2] != '0',
                                                       .dst = partial ? PA0 : O0,
                                                       .src = {none, src1, src2}}))
        return SGX535_FS_ERR_LIMIT;
    return !partial || masked(l, k->colormask) ? SGX535_FS_OK : SGX535_FS_ERR_LIMIT;
}

/* ---- the colour write ------------------------------------------------------------------- */

/* Pass types 1 and 2 (translucent) are for blending and logic ops; a colour
 * mask alone stays opaque, its sop2wm.end beside ISPB UPASSCTL
 * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues). */
static bool translucent(const struct sgx535_fs_epilogue_key *k)
{
    return k->blend || k->logic;
}

/* Blend, logic op or mask into o0, or the plain store. */
static enum sgx535_fs_status colour(struct linker *l, const struct sgx535_fs_epilogue_key *k)
{
    if (k->logic)
        return logic(l, k);
    if (k->blend)
        return blend(l, k);
    if ((k->colormask & 15u) != 15u)
        return pack(l, PA0, false) && masked(l, k->colormask) ? SGX535_FS_OK : SGX535_FS_ERR_LIMIT;
    return pack(l, O0, true) ? SGX535_FS_OK : SGX535_FS_ERR_LIMIT;
}

/* ---- punch-through and depth output ------------------------------------------------- */

/* Phase 2 at the next even instruction (a nop between): the colour, which
 * phase 1 left packed in pa0 (EMGD's mov.end o0, pa0, or its sop2.end). */
static enum sgx535_fs_status second_phase(struct linker *l, const struct sgx535_fs_epilogue_key *k)
{
    if ((l->out->instruction_count & 1u) &&
        !put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_NOP}))
        return SGX535_FS_ERR_LIMIT;
    l->out->phase2 = l->out->instruction_count;
    l->in_pa0 = true;
    return colour(l, k);
}

static enum sgx535_fs_status visibility(struct linker *l, const struct sgx535_fs_epilogue_key *k)
{
    const struct sgx535_fs_program *p = l->body;
    struct sgx535_fs_variant *v = l->out;
    int f0 = constant(l, SGX535_FS_FEEDBACK0), f1 = constant(l, SGX535_FS_FEEDBACK1);
    uint8_t pred = 0;
    if (f0 < 0 || f1 < 0)
        return SGX535_FS_ERR_LIMIT;
    if (p->writes_depth) {
        /* depthf.end DEPTH, STATE0, STATE1; DEPTH a temporary or PA (EMGD: mov r0, c15) */
        struct sgx535_use_operand depth = p->depth;
        if (p->kill != NO_REG || p->kill_pred || k->alpha_test)
            return refuse(l, "gl_FragDepth with discard or alpha test (no captured form)");
        if (translucent(k))
            return refuse(l, "gl_FragDepth with blending or logic ops (no translucent "
                             "depth-feedback pass type)");
        if (!pack(l, PA0, false))
            return SGX535_FS_ERR_LIMIT;
        if (depth.mod || (depth.bank != SGX535_USE_R && depth.bank != SGX535_USE_PA)) {
            int t = scratch(l);
            struct sgx535_use_instruction copy = {.opcode =
                                                      depth.mod ? SGX535_USE_FMAX : SGX535_USE_MOV,
                                                  .skipinv = 1,
                                                  .writemask = depth.mod ? 0 : 1,
                                                  .dst = reg(SGX535_USE_R, (uint32_t)t),
                                                  .src = {none, depth, depth.mod ? depth : none}};
            if (t < 0 || !put(l, copy))
                return SGX535_FS_ERR_LIMIT;
            depth = reg(SGX535_USE_R, (uint32_t)t);
        }
        v->pass_type = 5;
        if (!put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_DEPTHF,
                                                    .end = 1,
                                                    .src = {depth, reg(SGX535_USE_SA, (uint32_t)f0),
                                                            reg(SGX535_USE_SA, (uint32_t)f1)}}))
            return SGX535_FS_ERR_LIMIT;
        return second_phase(l, k);
    }
    /* Pass type 4: ZABS iterated last, pcoeff first in the program. */
    if (v->iteration_count == SGX535_FS_MAX_ITERATIONS + 1u)
        return SGX535_FS_ERR_LIMIT;
    {
        uint32_t zabs = (v->primary + 1u) & ~1u;
        v->iteration[v->iteration_count++] = sgx535_fs_use_issue(14, 3, zabs);
        v->primary = zabs + 4u;
        memmove(&v->words[2], &v->words[0], 2u * v->instruction_count * sizeof(uint32_t));
        v->instruction_count++;
        if (sgx535_use_encode(
                &(struct sgx535_use_instruction){
                    .opcode = SGX535_USE_PCOEFF,
                    .src = {PA0, reg(SGX535_USE_PA, zabs), reg(SGX535_USE_SA, (uint32_t)f1)}},
                v->words) != SGX535_USE_OK)
            return SGX535_FS_ERR_LIMIT;
    }
    /* discard: p1 = (kill == 0), and atst8 runs only there (EMGD's p1 atst8);
     * the body may have tested p1 itself */
    if (p->kill_pred)
        pred = 2;
    if (p->kill != NO_REG) {
        if (!put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_TEST,
                                                    .skipinv = 1,
                                                    .alu = SGX535_USE_ALU_FSUB,
                                                    .test = SGX535_USE_TEST_EQ,
                                                    .pdst = 1,
                                                    .src = {none, reg(SGX535_USE_R, p->kill),
                                                            reg(SGX535_USE_C, SGX535_USE_C_ZERO)}}))
            return SGX535_FS_ERR_LIMIT;
        pred = 2; /* p1 in the short predicate field */
    }
    if (!pack(l, PA0, false))
        return SGX535_FS_ERR_LIMIT;
    /* pcoeff and atst8 are never adjacent in EMGD's words: a move between */
    if (v->instruction_count == 1 &&
        !put(l, (struct sgx535_use_instruction){
                    .opcode = SGX535_USE_MOV, .skipinv = 1, .dst = PA0, .src = {none, PA0, none}}))
        return SGX535_FS_ERR_LIMIT;
    v->pass_type = translucent(k) ? 2 : 4;
    if (!put(l, (struct sgx535_use_instruction){.opcode = SGX535_USE_ATST8,
                                                .pred = pred,
                                                .end = 1,
                                                .dst = PA0,
                                                .src = {PA0, reg(SGX535_USE_SA, (uint32_t)f0),
                                                        reg(SGX535_USE_SA, (uint32_t)f1)}}))
        return SGX535_FS_ERR_LIMIT;
    return second_phase(l, k);
}

static bool no_color(const struct sgx535_fs_epilogue_key *k)
{
    bool zero_one = k->blend && k->rgb_src == SGX535_FS_ZERO && k->rgb_dst == SGX535_FS_ONE &&
                    k->alpha_src == SGX535_FS_ZERO && k->alpha_dst == SGX535_FS_ONE &&
                    k->rgb_eq == SGX535_FS_EQ_ADD && k->alpha_eq == SGX535_FS_EQ_ADD;
    return !(k->colormask & 15u) || (k->logic && k->logic_op == NOOP) || zero_one;
}

static bool opaque_blend(const struct sgx535_fs_epilogue_key *k)
{
    return k->rgb_src == SGX535_FS_ONE && k->rgb_dst == SGX535_FS_ZERO &&
           k->alpha_src == SGX535_FS_ONE && k->alpha_dst == SGX535_FS_ZERO &&
           k->rgb_eq == SGX535_FS_EQ_ADD && k->alpha_eq == SGX535_FS_EQ_ADD;
}

enum sgx535_fs_status sgx535_fs_link(const struct sgx535_fs_program *body,
                                     const struct sgx535_fs_epilogue_key *key,
                                     struct sgx535_fs_variant *out)
{
    struct linker l;
    struct sgx535_fs_epilogue_key k;
    enum sgx535_fs_status status;
    bool visible;
    if (!body || !key || !out || body->instruction_count > SGX535_FS_MAX_WORDS / 2u)
        return SGX535_FS_ERR_ARGUMENT;
    k = *key;
    if (k.target > SGX535_FS_TARGET_RGB8 || k.colormask > 15u || k.blend > 1 || k.logic > 1 ||
        k.alpha_test > 1)
        return SGX535_FS_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    memcpy(out->words, body->words, 2u * body->instruction_count * sizeof(uint32_t));
    out->instruction_count = body->instruction_count;
    out->temps = body->temps;
    out->primary = body->primary;
    out->iteration_count = body->iteration_count;
    memcpy(out->iteration, body->iteration, body->iteration_count * sizeof(body->iteration[0]));
    out->constant_count = body->constant_count;
    memcpy(out->constant, body->constant, body->constant_count * sizeof(body->constant[0]));
    l = (struct linker){out, body, {false}, false};
    for (unsigned c = 0; c < 4; c++)
        if (body->color[c].bank == SGX535_USE_R)
            l.busy[body->color[c].number] = true;
    if (body->writes_depth && body->depth.bank == SGX535_USE_R)
        l.busy[body->depth.number] = true;
    if (body->kill != NO_REG)
        l.busy[body->kill] = true;
    if (k.logic && k.logic_op == COPY)
        k.logic = 0;
    if (k.blend && opaque_blend(&k))
        k.blend = 0;
    visible = k.alpha_test || body->kill != NO_REG || body->kill_pred || body->writes_depth;
    if (no_color(&k)) {
        if (visible)
            return refuse(&l, "no colour write with discard, alpha test or depth output");
        out->no_color = 1;
        out->instruction_count = 0;
        return SGX535_FS_OK;
    }
    if (visible)
        status = visibility(&l, &k);
    else {
        out->pass_type = translucent(&k) ? 1 : 0;
        status = colour(&l, &k);
    }
    out->punchthrough = out->phase2 != 0;
    return status;
}

void sgx535_fs_feedback_words(const struct sgx535_fs_feedback *f, uint32_t w[2])
{
    /* The fields, low bits first, at bit AT of STATE1:STATE0, BITS wide. */
    const uint8_t value[] = {
        f->depth_func,   !f->depth_write, f->stencil_zpass, f->stencil_zfail,     f->stencil_fail,
        f->stencil_func, f->stencil_ref,  f->stencil_mask,  f->back_zpass,        f->back_zfail,
        f->back_fail,    f->back_func,    f->alpha_func,    f->stencil_writemask, f->alpha_ref};
    static const uint8_t at[] = {0, 3, 4, 7, 10, 13, 16, 24, 32, 35, 38, 41, 45, 48, 56};
    static const uint8_t bits[] = {3, 1, 3, 3, 3, 3, 8, 8, 3, 3, 3, 3, 3, 8, 8};
    uint64_t both = 0;
    for (unsigned i = 0; i < sizeof(value); i++)
        both |= (uint64_t)(value[i] & ((1u << bits[i]) - 1u)) << at[i];
    w[0] = (uint32_t)both, w[1] = (uint32_t)(both >> 32);
}
