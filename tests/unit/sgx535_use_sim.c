// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* USSE model for host tests (sgx535_use_sim.h). Field positions follow
 * docs/re/sgx535/usse-instruction-contracts.md. */
#include "sgx535_use_sim.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BITS(w, hi, lo) (((w) >> (lo)) & ((1u << ((hi) - (lo) + 1u)) - 1u))

float sgx535_sim_f32(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
uint32_t sgx535_sim_bits(float value)
{
    uint32_t u;
    memcpy(&u, &value, sizeof(u));
    return u;
}

enum kind { K_R, K_O, K_PA, K_SA, K_SPECIAL, K_IMM, K_I, K_INDEX, K_NONE };
struct operand {
    enum kind kind;
    unsigned number, mod;
};

static struct operand src12(unsigned bank, unsigned ext, unsigned number)
{
    static const enum kind standard[] = {K_R, K_O, K_PA, K_SA};
    static const enum kind extended[] = {K_INDEX, K_SPECIAL, K_IMM, K_I};
    struct operand o = {ext ? extended[bank] : standard[bank], number, 0};
    if (o.kind == K_R && number >= 124)
        o = (struct operand){K_I, number - 124, 0};
    return o;
}
static struct operand src1(uint32_t w0, uint32_t w1)
{
    return src12(BITS(w0, 31, 30), BITS(w1, 17, 17), BITS(w0, 13, 7));
}
static struct operand src2(uint32_t w0, uint32_t w1)
{
    return src12(BITS(w0, 29, 28), BITS(w1, 16, 16), BITS(w0, 6, 0));
}
static struct operand src0(uint32_t w0, uint32_t w1, unsigned ext_bit)
{
    static const enum kind standard[] = {K_R, K_PA}, extended[] = {K_O, K_SA};
    unsigned bank = BITS(w1, 2, 2), ext = ext_bit < 32 ? (w1 >> ext_bit) & 1u : 0;
    struct operand o = {ext ? extended[bank] : standard[bank], BITS(w0, 20, 14), 0};
    if (o.kind == K_R && o.number >= 124)
        o = (struct operand){K_I, o.number - 124, 0};
    return o;
}
static struct operand dst(uint32_t w0, uint32_t w1, bool has_ext)
{
    static const enum kind standard[] = {K_R, K_O, K_PA, K_INDEX};
    static const enum kind extended[] = {K_SA, K_SPECIAL, K_INDEX, K_I};
    unsigned bank = BITS(w1, 1, 0), ext = has_ext ? BITS(w1, 19, 19) : 0;
    struct operand o = {ext ? extended[bank] : standard[bank], BITS(w0, 27, 21), 0};
    if (o.kind == K_R && o.number >= 124)
        o = (struct operand){K_I, o.number - 124, 0};
    return o;
}

/* ---- text ------------------------------------------------------------------ */

struct buffer {
    char *text;
    size_t size, used;
};
static void out(struct buffer *b, const char *format, ...)
{
    va_list args;
    int n;
    if (b->used >= b->size)
        return;
    va_start(args, format);
    n = vsnprintf(b->text + b->used, b->size - b->used, format, args);
    va_end(args);
    if (n > 0)
        b->used += (size_t)n;
}
static void reg_text(struct buffer *b, struct operand o)
{
    switch (o.kind) {
    case K_R:
        out(b, "r%u", o.number);
        break;
    case K_O:
        out(b, "o%u", o.number);
        break;
    case K_PA:
        out(b, "pa%u", o.number);
        break;
    case K_SA:
        out(b, "sa%u", o.number);
        break;
    case K_SPECIAL:
        if (o.number & 0x40)
            out(b, "g%u", o.number & 0x3f);
        else
            out(b, "c%u", o.number);
        break;
    case K_IMM:
        out(b, "#%u", o.number);
        break;
    case K_I:
        out(b, "i%u", o.number);
        break;
    default:
        out(b, "?");
        break;
    }
    if (o.mod & 1)
        out(b, ".neg");
    if (o.mod & 2)
        out(b, ".abs");
}
static const char *const epred[] = {"", "p0 ", "p1 ", "p2 ", "p3 ", "!p0 ", "!p1 ", "Pn "};
static const char *const spred[] = {"", "p0 ", "p1 ", "!p0 "};

static void flags(struct buffer *b, uint32_t w1, bool skipinv, bool syncs, bool end)
{
    if (skipinv && (w1 >> 23 & 1))
        out(b, ".skipinv");
    if (syncs && (w1 >> 20 & 1))
        out(b, ".syncs");
    if (end && (w1 >> 18 & 1))
        out(b, ".end");
}
static void repeat(struct buffer *b, uint32_t w1)
{
    if ((w1 >> 21 & 1) && BITS(w1, 15, 12))
        out(b, ".repeat%u", BITS(w1, 15, 12) + 1u);
}

static bool text_float(struct buffer *b, uint32_t w0, uint32_t w1)
{
    static const char *const scalar[] = {"frcp", "frsq", "flog", "fexp"};
    unsigned op = w1 >> 27, op2 = BITS(w1, 10, 9);
    struct operand d = dst(w0, w1, op != 2), a, s1 = src1(w0, w1), s2 = src2(w0, w1);
    const char *name;
    s1.mod = BITS(w1, 6, 5), s2.mod = BITS(w1, 4, 3);
    if (op == 0)
        name = op2 == 0 ? "fmad" : op2 == 3 ? "fsubflr" : NULL;
    else if (op == 1)
        name = scalar[op2];
    else if (op == 2)
        name = op2 == 0 ? "fdp" : NULL;
    else if (op == 3)
        name = op2 == 0 ? "fmin" : op2 == 1 ? "fmax" : NULL;
    else
        name = op2 == 0 ? "fdsx" : op2 == 1 ? "fdsy" : NULL;
    if (!name || !(w1 >> 21 & 1))
        return false;
    out(b, "%s%s", epred[BITS(w1, 26, 24)], name);
    flags(b, w1, true, true, true);
    repeat(b, w1);
    out(b, " ");
    reg_text(b, d);
    if (op == 0 && op2 == 0) {
        a = src0(w0, w1, 99);
        a.mod = BITS(w1, 8, 7);
        out(b, ", ");
        reg_text(b, a);
    }
    out(b, ", ");
    reg_text(b, s1);
    if (op != 1) {
        out(b, ", ");
        reg_text(b, s2);
    }
    return true;
}

static bool text_mov(struct buffer *b, uint32_t w0, uint32_t w1)
{
    if (BITS(w1, 10, 8) || (!(w1 >> 21 & 1) && BITS(w1, 15, 12) != 1))
        return false;
    out(b, "%smov", epred[BITS(w1, 26, 24)]);
    flags(b, w1, true, false, true);
    repeat(b, w1);
    out(b, " ");
    reg_text(b, dst(w0, w1, true));
    out(b, ", ");
    reg_text(b, src1(w0, w1));
    return true;
}

static bool text_test(struct buffer *b, uint32_t w0, uint32_t w1)
{
    static const char *const f32[16] = {
        [0] = "fadd", [9] = "fmin", [10] = "fmax", [13] = "fmul", [14] = "fsub"};
    static const char *const bitwise[8] = {"and", "or", "xor"};
    const char *alu = BITS(w0, 19, 18) == 0 ? f32[BITS(w0, 17, 14)]
                      : BITS(w0, 19, 18) == 3
                          ? (BITS(w0, 17, 14) < 8 ? bitwise[BITS(w0, 17, 14)] : NULL)
                          : NULL;
    static const char sign[] = {'t', 'n', 'p'}, *zero[] = {"t", "z", "nz"};
    if (!alu || BITS(w1, 11, 10) > 2 || BITS(w1, 9, 8) > 2 || BITS(w1, 6, 4))
        return false;
    out(b, "%s%s", epred[BITS(w1, 26, 24)], alu);
    flags(b, w1, true, false, false);
    out(b, ".test%c%c%s.chan0 ", sign[BITS(w1, 11, 10)], (w1 >> 7 & 1) ? 'a' : 'o',
        zero[BITS(w1, 9, 8)]);
    if (w0 >> 20 & 1)
        reg_text(b, dst(w0, w1, true));
    else
        out(b, "!c%u", BITS(w0, 27, 21));
    out(b, ", p%u, ", BITS(w1, 3, 2));
    reg_text(b, src1(w0, w1));
    out(b, ", ");
    reg_text(b, src2(w0, w1));
    return true;
}

static bool text_pack(struct buffer *b, uint32_t w0, uint32_t w1)
{
    unsigned srcf = BITS(w1, 11, 9), dstf = BITS(w1, 8, 6), mask = BITS(w1, 5, 2);
    struct operand s1 = src1(w0, w1), s2 = src2(w0, w1);
    if (srcf == 6 && dstf == 0) {
        out(b, "%spcku8f32", epred[BITS(w1, 26, 24)]);
        flags(b, w1, true, false, true);
        if (w0 >> 18 & 1)
            out(b, ".scale");
        out(b, " ");
        reg_text(b, dst(w0, w1, true));
        out(b, ".bytemask%u%u%u%u, ", mask >> 3 & 1, mask >> 2 & 1, mask >> 1 & 1, mask & 1);
    } else if ((srcf == 0 || srcf == 3 || srcf == 5) && dstf == 6 && mask == 15) {
        out(b, "%sunpckf32%s", epred[BITS(w1, 26, 24)],
            srcf == 0   ? "u8"
            : srcf == 3 ? "u16"
                        : "f16");
        flags(b, w1, true, false, true);
        if (w0 >> 18 & 1)
            out(b, ".scale");
        out(b, " ");
        reg_text(b, dst(w0, w1, true));
        out(b, ", ");
    } else
        return false;
    reg_text(b, s1);
    out(b, ".%u, ", BITS(w0, 17, 16));
    if (s2.kind == K_IMM)
        out(b, "#%u.%u", s2.number, BITS(w0, 15, 14));
    else {
        reg_text(b, s2);
        out(b, ".%u", BITS(w0, 15, 14));
    }
    out(b, ", nearest");
    return true;
}

static bool text_smp(struct buffer *b, uint32_t w0, uint32_t w1)
{
    static const char *const lodm[] = {"", "bias", "replace", "grad"};
    unsigned cdim = BITS(w1, 11, 10), mode = BITS(w1, 9, 8);
    if (cdim > 2 || BITS(w1, 15, 12) != 1 || (w1 >> 21 & 1) || BITS(w1, 4, 3))
        return false;
    out(b, "%ssmp%ud%s", epred[BITS(w1, 26, 24)], cdim + 1u, lodm[mode]);
    flags(b, w1, true, true, false);
    out(b, " %s%u, ", (w1 >> 7 & 1) ? "pa" : "r", BITS(w0, 27, 21));
    reg_text(b, src0(w0, w1, 18));
    out(b, ", ");
    reg_text(b, src1(w0, w1));
    if (mode) {
        out(b, ", ");
        reg_text(b, src2(w0, w1));
    }
    out(b, ", drc%u", BITS(w1, 1, 0));
    return true;
}

static bool text_bitwise(struct buffer *b, uint32_t w0, uint32_t w1)
{
    unsigned op = w1 >> 27;
    const char *name = op == 11 ? "xor" : (w1 >> 3 & 1) ? "or" : "and";
    if (BITS(w1, 10, 4) || BITS(w1, 2, 2) || (op == 11 && (w1 >> 3 & 1)))
        return false;
    out(b, "%s%s", spred[BITS(w1, 26, 25)], name);
    flags(b, w1, true, false, true);
    repeat(b, w1);
    out(b, " ");
    reg_text(b, dst(w0, w1, true));
    out(b, ", ");
    reg_text(b, src1(w0, w1));
    out(b, ", %s", (w1 >> 11 & 1) ? "~" : "");
    reg_text(b, src2(w0, w1));
    return true;
}

/* SOP2/SOPWM registers: 6 bits, bit 6 the C10 format. */
static void sop_reg(struct buffer *b, struct operand o)
{
    bool c10 =
        (o.kind == K_R || o.kind == K_O || o.kind == K_PA || o.kind == K_SA) && (o.number & 0x40);
    if (c10)
        o.number &= 0x3f;
    if (o.kind == K_R && o.number >= 60 && o.number < 64)
        out(b, "i%u", o.number - 60);
    else
        reg_text(b, o);
    if (c10)
        out(b, ".c10");
}
static const char *const sop_op[] = {"add", "sub", "min", "max"};

static bool text_sop2(struct buffer *b, uint32_t w0, uint32_t w1)
{
    static const char *const csel[8] = {"zero", "s1", "s2", "s1a", "s2a", "asat"};
    static const char *const asel[4] = {"zero", "s1a", "s2a"};
    struct operand d = dst(w0, w1, true), s1 = src1(w0, w1), s2 = src2(w0, w1);
    if (BITS(w1, 8, 6) > 5 || BITS(w1, 5, 3) > 5 || BITS(w1, 21, 20) > 2 || BITS(w1, 10, 9) > 2)
        return false;
    if (d.kind == K_I)
        d = (struct operand){K_R, d.number + 124, 0};
    out(b, "%ssop2", spred[BITS(w1, 26, 25)]);
    flags(b, w1, true, false, false);
    if (w1 >> 22 & 1)
        out(b, ".nosched");
    if (w1 >> 18 & 1)
        out(b, ".end");
    if (BITS(w1, 14, 12))
        out(b, ".repeat%u", BITS(w1, 14, 12) + 1u);
    out(b, " ");
    sop_reg(b, d);
    out(b, ", ");
    sop_reg(b, s1);
    out(b, ", ");
    sop_reg(b, s2);
    out(b, ", %s%s%s, %s%s, %s+asop2 %s%s%s, %s%s, %s%s", (w0 >> 20 & 1) ? "comp, " : "",
        csel[BITS(w1, 8, 6)], (w1 >> 24 & 1) ? ".comp" : "", csel[BITS(w1, 5, 3)],
        (w1 >> 15 & 1) ? ".comp" : "", sop_op[BITS(w0, 19, 18)], (w0 >> 15 & 1) ? "comp, " : "",
        asel[BITS(w1, 21, 20)], (w1 >> 11 & 1) ? ".comp" : "", asel[BITS(w1, 10, 9)],
        (w1 >> 2 & 1) ? ".comp" : "", sop_op[BITS(w0, 17, 16)], (w0 >> 14 & 1) ? ", neg" : "");
    return true;
}

static bool text_sopwm(struct buffer *b, uint32_t w0, uint32_t w1)
{
    static const char *const sel[8] = {"zero", "asat", "s1", "s1a", NULL, NULL, "s2", "s2a"};
    unsigned m = BITS(w1, 14, 11);
    struct operand d = dst(w0, w1, true);
    if (!sel[BITS(w1, 8, 6)] || !sel[BITS(w1, 5, 3)] || BITS(w0, 20, 14) || (w1 >> 2 & 1))
        return false;
    out(b, "%ssop2wm", spred[BITS(w1, 26, 25)]);
    flags(b, w1, true, false, false);
    if (w1 >> 22 & 1)
        out(b, ".nosched");
    if (w1 >> 18 & 1)
        out(b, ".end");
    out(b, " ");
    sop_reg(b, d);
    out(b, ".bytemask%u%u%u%u, ", m & 1, m >> 3 & 1, m >> 2 & 1, m >> 1 & 1);
    sop_reg(b, src1(w0, w1));
    out(b, ", ");
    sop_reg(b, src2(w0, w1));
    out(b, ", %s%s, %s%s, %s, %s", sel[BITS(w1, 8, 6)], (w1 >> 24 & 1) ? ".comp" : "",
        sel[BITS(w1, 5, 3)], (w1 >> 15 & 1) ? ".comp" : "", sop_op[BITS(w1, 21, 20)],
        sop_op[BITS(w1, 10, 9)]);
    return true;
}

static bool text_special(struct buffer *b, uint32_t w0, uint32_t w1)
{
    unsigned opcat = BITS(w1, 21, 20);
    if (opcat == 0) { /* flow control */
        unsigned op2 = BITS(w1, 8, 6);
        if (op2 == 5) {
            out(b, "%snop%s", epred[BITS(w1, 26, 24)], (w1 >> 18 & 1) ? ".end" : "");
            return true;
        }
        if (op2 > 1)
            return false;
        out(b, "%s%s%s", epred[BITS(w1, 26, 24)], op2 ? "br" : "ba",
            (w1 >> 23 & 1) ? ".synce" : "");
        if (op2 && (w0 & 0x800))
            out(b, " -#0x%08X", 0x1000u - (w0 & 0xfffu));
        else
            out(b, " #0x%08X", w0 & 0xfffu);
        return true;
    }
    if (opcat == 2) {
        if (BITS(w1, 26, 24) == 1) {
            out(b, "wdf drc%u", BITS(w1, 1, 0));
            return true;
        }
        if (BITS(w1, 26, 24) == 4) { /* LIMM */
            uint32_t value = (w0 & 0x1fffffu) | BITS(w1, 8, 4) << 21 | BITS(w1, 17, 12) << 26;
            out(b, "mov");
            flags(b, w1, true, false, true);
            out(b, " ");
            reg_text(b, dst(w0, w1, false));
            out(b, ", #0x%08X", value);
            return true;
        }
        return false;
    }
    if (opcat == 3) { /* visibility test */
        unsigned op2 = BITS(w1, 26, 24);
        const char *name = op2 == 0 ? "pcoeff" : op2 == 1 ? "atst8" : op2 == 3 ? "depthf" : NULL;
        if (!name)
            return false;
        out(b, "%s%s", op2 ? spred[BITS(w1, 10, 9)] : "", name);
        if (op2) {
            if (w1 >> 23 & 1)
                out(b, ".syncs");
            if (op2 == 1 && (w1 >> 22 & 1))
                out(b, ".c10");
            if (w1 >> 11 & 1)
                out(b, ".nosched");
        }
        if (w1 >> 18 & 1)
            out(b, ".end");
        out(b, " ");
        if (op2 == 1) {
            reg_text(b, dst(w0, w1, false));
            out(b, ", %sp%u, ", (w1 >> 6 & 1) ? "" : "!", BITS(w1, 8, 7));
        }
        reg_text(b, src0(w0, w1, 19));
        out(b, ", ");
        reg_text(b, src1(w0, w1));
        out(b, ", ");
        reg_text(b, src2(w0, w1));
        if (op2) {
            if (w1 >> 5 & 1)
                out(b, ", twosided");
            out(b, ", %s", (w1 >> 4 & 1) ? "optdwd" : (w1 >> 3 & 1) ? "nfb" : "fb");
        }
        return true;
    }
    return false;
}

bool sgx535_sim_text(uint32_t w0, uint32_t w1, char *text, size_t size)
{
    struct buffer b = {text, size, 0};
    bool ok;
    if (!text || !size)
        return false;
    text[0] = 0;
    switch (w1 >> 27) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
        ok = text_float(&b, w0, w1);
        break;
    case 5:
        ok = text_mov(&b, w0, w1);
        break;
    case 8:
        ok = text_pack(&b, w0, w1);
        break;
    case 9:
        ok = text_test(&b, w0, w1);
        break;
    case 10:
    case 11:
        ok = text_bitwise(&b, w0, w1);
        break;
    case 16:
        ok = text_sop2(&b, w0, w1);
        break;
    case 18:
        ok = text_sopwm(&b, w0, w1);
        break;
    case 28:
        ok = text_smp(&b, w0, w1);
        break;
    case 31:
        ok = text_special(&b, w0, w1);
        break;
    default:
        ok = false;
    }
    return ok && b.used < size;
}

/* ---- integer blend: SOP2 and SOPWM on B8G8R8A8 bytes ----------------------------- */

static float byte_of(uint32_t w, unsigned b)
{
    return (float)(w >> (8u * b) & 0xffu) / 255.0f;
}
static uint32_t to_byte(float x)
{
    x = x != x ? 0.0f : x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return (uint32_t)lrintf(x * 255.0f);
}
/* SOP2's colour selectors zero s1 s2 s1a s2a asat; alpha zero s1a s2a. */
static float sop2_factor(unsigned sel, bool alpha, uint32_t a, uint32_t b, unsigned byte)
{
    if (alpha)
        return sel == 1 ? byte_of(a, 3) : sel == 2 ? byte_of(b, 3) : 0.0f;
    switch (sel) {
    case 1:
        return byte_of(a, byte);
    case 2:
        return byte_of(b, byte);
    case 3:
        return byte_of(a, 3);
    case 4:
        return byte_of(b, 3);
    case 5:
        return fminf(byte_of(a, 3), 1.0f - byte_of(b, 3));
    default:
        return 0.0f;
    }
}
/* SOPWM's selectors zero 0, asat 1, s1 2, s1a 3, s2 6, s2a 7. */
static float sopwm_factor(unsigned sel, uint32_t a, uint32_t b, unsigned byte)
{
    switch (sel) {
    case 1:
        return fminf(byte_of(a, 3), 1.0f - byte_of(b, 3));
    case 2:
        return byte_of(a, byte);
    case 3:
        return byte_of(a, 3);
    case 6:
        return byte_of(b, byte);
    case 7:
        return byte_of(b, 3);
    default:
        return 0.0f;
    }
}
/* min and max take no factors. */
static float blend_op(unsigned op, float x, float fx, float y, float fy)
{
    return op == 0   ? x * fx + y * fy
           : op == 1 ? x * fx - y * fy
           : op == 2 ? fminf(x, y)
                     : fmaxf(x, y);
}

/* ---- execution ---------------------------------------------------------------- */

struct machine {
    struct sgx535_sim *sim;
    bool ok;
};

static void error(struct sgx535_sim *sim, const char *format, ...)
{
    va_list args;
    if (sim->error[0])
        return;
    va_start(args, format);
    vsnprintf(sim->error, sizeof(sim->error), format, args);
    va_end(args);
}

static uint32_t *location(struct sgx535_sim *sim, unsigned pixel, struct operand o)
{
    struct sgx535_sim_pixel *p = &sim->pixel[pixel];
    switch (o.kind) {
    case K_R:
        return o.number < 128 ? &p->r[o.number] : NULL;
    case K_I:
        return &p->r[124 + o.number];
    case K_O:
        return &p->o[o.number];
    case K_PA:
        return &p->pa[o.number];
    case K_SA:
        return &sim->sa[o.number];
    default:
        return NULL;
    }
}

static uint32_t read(struct sgx535_sim *sim, unsigned pixel, struct operand o)
{
    uint32_t *at, v;
    if (o.kind == K_IMM)
        v = o.number;
    else if (o.kind == K_SPECIAL) {
        if (o.number == (0x40u | 16u))
            v = sim->pixel[pixel].g16;
        else if (o.number == 48)
            v = 0;
        else if (o.number == 52)
            v = 0x3f800000u;
        else if (o.number == 15)
            v = 0x3e800000u; /* 0.25 */
        else if (o.number == 56)
            v = 0xffffffffu; /* all ones (EMGD's COPY_INVERTED) */
        else {
            error(sim, "special register %u not modelled", o.number);
            v = 0;
        }
    } else if ((at = location(sim, pixel, o)))
        v = *at;
    else {
        error(sim, "operand bank not modelled");
        v = 0;
    }
    if (o.mod & 2)
        v &= 0x7fffffffu;
    if (o.mod & 1)
        v ^= 0x80000000u;
    return v;
}
static float readf(struct sgx535_sim *sim, unsigned pixel, struct operand o)
{
    return sgx535_sim_f32(read(sim, pixel, o));
}
static void write(struct sgx535_sim *sim, unsigned pixel, struct operand o, uint32_t v)
{
    uint32_t *at = location(sim, pixel, o);
    if (!at) {
        error(sim, "destination bank not modelled");
        return;
    }
    *at = v;
}
static void writef(struct sgx535_sim *sim, unsigned pixel, struct operand o, float v)
{
    write(sim, pixel, o, sgx535_sim_bits(v));
}

static bool predicate(struct sgx535_sim *sim, unsigned pixel, unsigned pred, bool short_form)
{
    const bool *p = sim->pixel[pixel].p;
    if (short_form) {
        static const int map[] = {-1, 0, 1, -2};
        int m = map[pred];
        return m == -1 ? true : m == -2 ? !p[0] : p[m];
    }
    switch (pred) {
    case 0:
        return true;
    case 1:
    case 2:
    case 3:
    case 4:
        return p[pred - 1];
    case 5:
        return !p[0];
    case 6:
        return !p[1];
    default:
        error(sim, "predicate Pn not modelled");
        return false;
    }
}

static struct operand step(struct operand o, unsigned k)
{
    if (o.kind != K_SPECIAL && o.kind != K_IMM)
        o.number += k;
    return o;
}

static bool test_result(uint32_t result, unsigned stst, unsigned ztst, bool and_)
{
    bool sign = result >> 31 & 1, zero = (result & 0x7fffffffu) == 0 && true;
    bool s = stst == 0 ? and_ : stst == 1 ? sign : !sign;
    bool z = ztst == 0 ? and_ : ztst == 1 ? zero : !zero;
    return and_ ? (s && z) : (s || z);
}

/* One quad-wide gradient: x pairs (0,1) (2,3), y pairs (0,2) (1,3). */
static float gradient(struct sgx535_sim *sim, unsigned pixel, struct operand o, bool y)
{
    unsigned a = y ? pixel & 1u : pixel & 2u, b = y ? a + 2u : a + 1u;
    return readf(sim, b, o) - readf(sim, a, o);
}

/* An IEEE half's value (F1616 planes). */
static float half_float(uint32_t h)
{
    unsigned exponent = h >> 10 & 31u, mantissa = h & 1023u;
    float v = exponent == 0    ? ldexpf((float)mantissa, -24)
              : exponent == 31 ? (mantissa ? NAN : INFINITY)
                               : ldexpf((float)(mantissa | 1024u), (int)exponent - 25);
    return h & 0x8000u ? -v : v;
}

static void execute(struct sgx535_sim *sim, uint32_t w0, uint32_t w1, int32_t *branch)
{
    unsigned op = w1 >> 27, pixel;
    uint32_t staged[4][16];
    bool active[4];
    *branch = 0;
    switch (op) {
    case 0:
    case 1:
    case 3:
    case 4: { /* float: compute all pixels, then write (gradients read neighbours) */
        unsigned op2 = BITS(w1, 10, 9), count = BITS(w1, 15, 12) + 1u;
        struct operand d = dst(w0, w1, true), a = src0(w0, w1, 99), b = src1(w0, w1),
                       c = src2(w0, w1);
        a.mod = BITS(w1, 8, 7), b.mod = BITS(w1, 6, 5), c.mod = BITS(w1, 4, 3);
        if (!(w1 >> 21 & 1)) {
            error(sim, "write-mask form not modelled");
            return;
        }
        for (pixel = 0; pixel < 4; pixel++) {
            active[pixel] = predicate(sim, pixel, BITS(w1, 26, 24), false);
            for (unsigned k = 0; k < count; k++) {
                float x = readf(sim, pixel, step(b, k)), y = readf(sim, pixel, step(c, k)), r;
                if (op == 0 && op2 == 0)
                    r = readf(sim, pixel, step(a, k)) * x + y;
                else if (op == 0 && op2 == 3)
                    r = x - floorf(y);
                else if (op == 1)
                    r = op2 == 0   ? 1.0f / x
                        : op2 == 1 ? 1.0f / sqrtf(x)
                        : op2 == 2 ? log2f(x)
                                   : exp2f(x);
                else if (op == 3)
                    r = op2 ? fmaxf(x, y) : fminf(x, y);
                else
                    r = gradient(sim, pixel, step(b, k), op2 == 1);
                staged[pixel][k] = sgx535_sim_bits(r);
            }
        }
        for (pixel = 0; pixel < 4; pixel++)
            for (unsigned k = 0; k < count && active[pixel]; k++)
                write(sim, pixel, step(d, k), staged[pixel][k]);
        return;
    }
    case 5: /* mov */
        for (pixel = 0; pixel < 4; pixel++)
            staged[pixel][0] = read(sim, pixel, src1(w0, w1));
        for (pixel = 0; pixel < 4; pixel++)
            if (predicate(sim, pixel, BITS(w1, 26, 24), false))
                write(sim, pixel, dst(w0, w1, true), staged[pixel][0]);
        return;
    case 8: { /* pck / unpck */
        unsigned srcf = BITS(w1, 11, 9), mask = BITS(w1, 5, 2), lane = BITS(w0, 17, 16);
        for (pixel = 0; pixel < 4; pixel++) {
            struct operand d = dst(w0, w1, true);
            uint32_t v;
            if (!predicate(sim, pixel, BITS(w1, 26, 24), false))
                continue;
            if (srcf == 0) { /* U8 lane -> F32 [0, 1] */
                v = read(sim, pixel, src1(w0, w1)) >> (8u * lane) & 0xffu;
                writef(sim, pixel, d, (float)v / 255.0f);
                continue;
            }
            if (srcf == 3 || srcf == 5) { /* the 16 bits from byte LANE: U16 [0, 1] or F16 */
                v = read(sim, pixel, src1(w0, w1)) >> (8u * lane) & 0xffffu;
                writef(sim, pixel, d, srcf == 3 ? (float)v / 65535.0f : half_float(v));
                continue;
            }
            /* One or two bytes: src1 into the lower, src2 into the higher. */
            {
                uint32_t *at = location(sim, pixel, d);
                unsigned n = 0;
                if (!at || mask == 0 || mask == 7 || mask == 11 || mask == 13 || mask == 14 ||
                    mask == 15) {
                    error(sim, "pack mask %u not modelled", mask);
                    return;
                }
                for (unsigned byte = 0; byte < 4; byte++) {
                    float f, c;
                    if (!(mask >> byte & 1u))
                        continue;
                    f = readf(sim, pixel, n++ ? src2(w0, w1) : src1(w0, w1));
                    c = f != f ? 0.0f : f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
                    v = (uint32_t)lrintf(c * 255.0f);
                    *at = (*at & ~(0xffu << (8u * byte))) | v << (8u * byte);
                }
            }
        }
        return;
    }
    case 9: { /* test */
        unsigned sel = BITS(w0, 19, 18), alu = BITS(w0, 17, 14);
        for (pixel = 0; pixel < 4; pixel++) {
            uint32_t x = read(sim, pixel, src1(w0, w1)), y = read(sim, pixel, src2(w0, w1)), r;
            if (!predicate(sim, pixel, BITS(w1, 26, 24), false))
                continue;
            if (sel == 0) {
                float a = sgx535_sim_f32(x), b = sgx535_sim_f32(y);
                r = sgx535_sim_bits(alu == 0    ? a + b
                                    : alu == 14 ? a - b
                                    : alu == 13 ? a * b
                                    : alu == 9  ? fminf(a, b)
                                                : fmaxf(a, b));
            } else
                r = alu == 0 ? x & y : alu == 1 ? x | y : x ^ y;
            sim->pixel[pixel].p[BITS(w1, 3, 2)] =
                test_result(r, BITS(w1, 11, 10), BITS(w1, 9, 8), w1 >> 7 & 1);
            if (w0 >> 20 & 1)
                write(sim, pixel, dst(w0, w1, true), r);
        }
        return;
    }
    case 10:
    case 11: /* and, or, xor; SRC2INV */
        for (pixel = 0; pixel < 4; pixel++) {
            uint32_t x = read(sim, pixel, src1(w0, w1)), y = read(sim, pixel, src2(w0, w1));
            if (w1 >> 11 & 1)
                y = ~y;
            if (predicate(sim, pixel, BITS(w1, 26, 25), true))
                write(sim, pixel, dst(w0, w1, true),
                      op == 11        ? x ^ y
                      : (w1 >> 3 & 1) ? x | y
                                      : x & y);
        }
        return;
    case 16:
    case 18: { /* sop2 (colour bytes by CSEL, alpha by ASEL), sopwm (written bytes) */
        for (pixel = 0; pixel < 4; pixel++) {
            uint32_t a = read(sim, pixel, src1(w0, w1)), b = read(sim, pixel, src2(w0, w1));
            uint32_t *at = location(sim, pixel, dst(w0, w1, true)), r;
            if (!predicate(sim, pixel, BITS(w1, 26, 25), true))
                continue;
            if (!at) {
                error(sim, "sop2 destination");
                return;
            }
            if (op == 16 && BITS(w0, 20, 20) | BITS(w0, 15, 14)) {
                error(sim, "sop2 source modifiers not modelled");
                return;
            }
            r = *at;
            for (unsigned byte = 0; byte < 4; byte++) {
                bool alpha = byte == 3;
                float f1, f2, v;
                unsigned cop;
                if (op == 16) {
                    unsigned s1sel = alpha ? BITS(w1, 21, 20) : BITS(w1, 8, 6);
                    unsigned s2sel = alpha ? BITS(w1, 10, 9) : BITS(w1, 5, 3);
                    unsigned m1 = alpha ? BITS(w1, 11, 11) : BITS(w1, 24, 24);
                    unsigned m2 = alpha ? BITS(w1, 2, 2) : BITS(w1, 15, 15);
                    f1 = sop2_factor(s1sel, alpha, a, b, byte);
                    f2 = sop2_factor(s2sel, alpha, a, b, byte);
                    f1 = m1 ? 1.0f - f1 : f1;
                    f2 = m2 ? 1.0f - f2 : f2;
                    cop = alpha ? BITS(w0, 17, 16) : BITS(w0, 19, 18);
                } else {
                    /* WRITEMASK 14:11: A 11, B 12, G 13, R 14 */
                    static const unsigned bit[4] = {12, 13, 14, 11};
                    if (!(w1 >> bit[byte] & 1u))
                        continue;
                    f1 = sopwm_factor(BITS(w1, 8, 6), a, b, byte);
                    f2 = sopwm_factor(BITS(w1, 5, 3), a, b, byte);
                    f1 = (w1 >> 24 & 1) ? 1.0f - f1 : f1;
                    f2 = (w1 >> 15 & 1) ? 1.0f - f2 : f2;
                    cop = alpha ? BITS(w1, 10, 9) : BITS(w1, 21, 20);
                }
                v = blend_op(cop, byte_of(a, byte), f1, byte_of(b, byte), f2);
                r = (r & ~(0xffu << (8u * byte))) | to_byte(v) << (8u * byte);
            }
            *at = r;
        }
        return;
    }
    case 28: { /* smp */
        unsigned dim = BITS(w1, 11, 10) + 1u;
        struct operand coord = src0(w0, w1, 18), state = src1(w0, w1);
        for (pixel = 0; pixel < 4; pixel++) {
            float c[3] = {0, 0, 0};
            uint32_t word;
            if (!predicate(sim, pixel, BITS(w1, 26, 24), false))
                continue;
            for (unsigned k = 0; k < dim; k++)
                c[k] = readf(sim, pixel, step(coord, k));
            /* The host tests mark texture state words as 0x7e000000 | sampler << 4 |
             * plane << 2. */
            word = read(sim, pixel, state);
            if ((word & 0xff000000u) != 0x7e000000u) {
                error(sim, "smp state is not a test texture");
                return;
            }
            write(sim, pixel, (struct operand){(w1 >> 7 & 1) ? K_PA : K_R, BITS(w0, 27, 21), 0},
                  sim->sample(sim->sample_data, (word >> 4) & 0x3fu, (word >> 2) & 3u, c, dim));
        }
        return;
    }
    case 31: {
        unsigned opcat = BITS(w1, 21, 20);
        if (opcat == 0) {
            unsigned op2 = BITS(w1, 8, 6), pred = BITS(w1, 26, 24);
            bool taken[4];
            if (op2 == 5)
                return;
            for (pixel = 0; pixel < 4; pixel++)
                taken[pixel] = predicate(sim, pixel, pred, false);
            if (taken[0] != taken[1] || taken[0] != taken[2] || taken[0] != taken[3]) {
                error(sim, "branch predicate differs between pixels");
                return;
            }
            if (op2 == 1 && taken[0])
                *branch = (int32_t)((w0 & 0x800u) ? (int32_t)(w0 & 0xfffu) - 0x1000
                                                  : (int32_t)(w0 & 0xfffu));
            else if (op2 != 1) {
                error(sim, "flow control not modelled");
            }
            return;
        }
        if (opcat == 2 && BITS(w1, 26, 24) == 1)
            return;       /* wdf: samples complete at once here */
        if (opcat == 3) { /* pcoeff: nothing modelled; atst8: alpha test; depthf: depth */
            unsigned op2 = BITS(w1, 26, 24);
            for (pixel = 0; pixel < 4 && op2; pixel++) {
                struct sgx535_sim_pixel *px = &sim->pixel[pixel];
                uint32_t colour = read(sim, pixel, src0(w0, w1, 19));
                uint32_t state1 = read(sim, pixel, src2(w0, w1));
                if (!predicate(sim, pixel, BITS(w1, 10, 9), true)) {
                    px->discarded = true; /* a punch-through pixel without its test */
                    continue;
                }
                if (op2 == 3) {
                    px->depth = sgx535_sim_f32(colour);
                    continue;
                }
                {
                    unsigned alpha = colour >> 24, ref = state1 >> 24, func = BITS(state1, 15, 13);
                    bool pass = func == 0   ? false
                                : func == 1 ? alpha < ref
                                : func == 2 ? alpha == ref
                                : func == 3 ? alpha <= ref
                                : func == 4 ? alpha > ref
                                : func == 5 ? alpha != ref
                                : func == 6 ? alpha >= ref
                                            : true;
                    px->discarded |= !pass;
                    write(sim, pixel, dst(w0, w1, false), colour);
                }
            }
            return;
        }
        if (opcat == 2 && BITS(w1, 26, 24) == 4) {
            uint32_t value = (w0 & 0x1fffffu) | BITS(w1, 8, 4) << 21 | BITS(w1, 17, 12) << 26;
            for (pixel = 0; pixel < 4; pixel++)
                write(sim, pixel, dst(w0, w1, false), value);
            return;
        }
        error(sim, "special %08x %08x not modelled", w0, w1);
        return;
    }
    default:
        error(sim, "opcode %u not modelled", op);
    }
}

bool sgx535_sim_run(struct sgx535_sim *sim, const uint32_t *words, uint32_t count)
{
    uint32_t pc = 0;
    unsigned limit = sim->max_steps ? sim->max_steps : 100000u;
    sim->error[0] = 0;
    sim->steps = 0;
    while (pc < count) {
        uint32_t w0 = words[2 * pc], w1 = words[2 * pc + 1];
        int32_t branch;
        bool end;
        char text[160];
        if (++sim->steps > limit) {
            error(sim, "more than %u steps", limit);
            return false;
        }
        if (!sgx535_sim_text(w0, w1, text, sizeof(text))) {
            error(sim, "word %u (%08x %08x) not decoded", pc, w0, w1);
            return false;
        }
        execute(sim, w0, w1, &branch);
        if (sim->error[0])
            return false;
        /* END: bit 18 where the family has it (not smp, test or vistest src0). */
        end = (w1 >> 18 & 1) && (w1 >> 27) != 28 && (w1 >> 27) != 9 &&
              !((w1 >> 27) == 31 && BITS(w1, 21, 20) == 0 && BITS(w1, 8, 6) != 5);
        if (end)
            return true;
        pc = branch ? (uint32_t)((int32_t)pc + branch) : pc + 1u;
    }
    error(sim, "ran off the end at %u", pc);
    return false;
}
