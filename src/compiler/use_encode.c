// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* USSE instruction words (sgx535_use_encode.h). */
#include "sgx535_use_encode.h"
#include "sgx535_limm_fields.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif

/* Opcodes (word 1 31:27) and second-level operations. */
enum { OP_FARITH = 0, OP_FSCALAR = 1, OP_FDP = 2, OP_FMINMAX = 3, OP_FGRADIENT = 4, OP_MOVC = 5 };
enum { OP_PCK = 8, OP_TEST = 9, OP_ANDOR = 10, OP_XOR = 11, OP_SOP2 = 16, OP_SOPWM = 18 };
enum { OP_SMP = 28, OP_SPECIAL = 31 };
/* An instruction field at its shift. */
#define F(field, shift) ((uint32_t)in->field << (shift))
#define W1_OP(op) ((uint32_t)(op) << 27)
#define W1_SKIPINV (UINT32_C(1) << 23)
#define W1_RCNTSEL (UINT32_C(1) << 21)
#define W1_SYNCSTART (UINT32_C(1) << 20)
#define W1_DBEXT (UINT32_C(1) << 19)
#define W1_END (UINT32_C(1) << 18)
#define W1_S1BEXT (UINT32_C(1) << 17)
#define W1_S2BEXT (UINT32_C(1) << 16)
#define W1_NOSCHED (UINT32_C(1) << 11)

/* A register field: number, bank and its extension bit. */
struct field {
    uint32_t number, bank, ext;
};

static bool reg_number(const struct sgx535_use_operand *o, uint32_t *number)
{
    switch (o->bank) {
    case SGX535_USE_R:
        *number = o->number;
        return o->number < SGX535_USE_TEMPS;
    case SGX535_USE_O:
    case SGX535_USE_PA:
    case SGX535_USE_SA:
    case SGX535_USE_IMM:
        *number = o->number;
        return o->number < 128u;
    case SGX535_USE_C:
        *number = o->number;
        return o->number < 64u;
    case SGX535_USE_G:
        *number = 0x40u | o->number;
        return o->number < 64u;
    case SGX535_USE_I:
        *number = o->number;
        return o->number < 4u;
    }
    return false;
}

/* src1 and src2 (USE0 S1BANK 31:30, S2BANK 29:28 with S1BEXT, S2BEXT). */
static bool source(const struct sgx535_use_operand *o, struct field *f)
{
    static const uint8_t bank[] = {[SGX535_USE_R] = 0,   [SGX535_USE_O] = 1, [SGX535_USE_PA] = 2,
                                   [SGX535_USE_SA] = 3,  [SGX535_USE_C] = 1, [SGX535_USE_G] = 1,
                                   [SGX535_USE_IMM] = 2, [SGX535_USE_I] = 3};
    if (o->bank > SGX535_USE_I || !reg_number(o, &f->number))
        return false;
    f->bank = bank[o->bank];
    f->ext = o->bank >= SGX535_USE_C;
    return true;
}

/* Destinations (D1BANK 1:0 with DBEXT): r, o, pa standard; sa, special, i extended. */
static bool destination(const struct sgx535_use_operand *o, struct field *f)
{
    if (o->mod || !reg_number(o, &f->number))
        return false;
    switch (o->bank) {
    case SGX535_USE_R:
    case SGX535_USE_O:
    case SGX535_USE_PA:
        f->bank = o->bank, f->ext = 0;
        return true;
    case SGX535_USE_SA:
        f->bank = 0, f->ext = 1;
        return true;
    case SGX535_USE_C:
    case SGX535_USE_G:
        f->bank = 1, f->ext = 1;
        return true;
    case SGX535_USE_I:
        f->bank = 3, f->ext = 1;
        return true;
    }
    return false;
}

/* src0 (S0BANK 2): r or pa; with its extension, o or sa. */
static bool source0(const struct sgx535_use_operand *o, struct field *f, bool extended)
{
    if (!reg_number(o, &f->number))
        return false;
    switch (o->bank) {
    case SGX535_USE_R:
        f->bank = 0, f->ext = 0;
        return true;
    case SGX535_USE_PA:
        f->bank = 1, f->ext = 0;
        return true;
    case SGX535_USE_O:
        f->bank = 0, f->ext = 1;
        return extended;
    case SGX535_USE_SA:
        f->bank = 1, f->ext = 1;
        return extended;
    }
    return false;
}

static uint32_t low(const struct field *d, const struct field *s1, const struct field *s2)
{
    return s1->bank << 30 | s2->bank << 28 | d->number << 21 | s1->number << 7 | s2->number;
}
static uint32_t high(const struct field *d, const struct field *s1, const struct field *s2)
{
    return d->bank | d->ext << 19 | s1->ext << 17 | s2->ext << 16;
}

static bool flags_ok(const struct sgx535_use_instruction *in, unsigned pred_max)
{
    return in->pred <= pred_max && in->skipinv <= 1 && in->syncs <= 1 && in->nosched <= 1 &&
           in->end <= 1 && in->repeat <= 16 && in->scale <= 1 && in->src[0].mod <= 3 &&
           in->src[1].mod <= 3 && in->src[2].mod <= 3 && in->writemask <= 15 &&
           !(in->writemask && in->repeat > 1);
}
/* RMSKCNT 15:12: a repeat count (RCNTSEL set) or, given one, a write mask. */
static uint32_t repeat_count(const struct sgx535_use_instruction *in)
{
    if (in->writemask)
        return (uint32_t)in->writemask << 12;
    return W1_RCNTSEL | (uint32_t)(in->repeat ? in->repeat - 1u : 0u) << 12;
}
static uint32_t common(const struct sgx535_use_instruction *in)
{
    return F(pred, 24) | F(skipinv, 23) | F(syncs, 20) | F(end, 18) | F(nosched, 11) |
           repeat_count(in);
}

static const struct sgx535_use_operand none = {SGX535_USE_R, 0, 0};
static bool unused(const struct sgx535_use_operand *o)
{
    return o->bank == none.bank && !o->number && !o->mod;
}

/* FARITH (fmad, ffrc), FMINMAX, FSCALAR, FGRADIENT, FDOTPRODUCT. */
static enum sgx535_use_status farith(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d, s0 = {0, 0, 0}, s1, s2 = {0, 0, 0};
    unsigned op, op2, sources;
    switch (in->opcode) {
    case SGX535_USE_FMAD:
        op = OP_FARITH, op2 = 0, sources = 3;
        break;
    case SGX535_USE_FFRC:
        op = OP_FARITH, op2 = 3, sources = 2;
        break;
    case SGX535_USE_FMIN:
    case SGX535_USE_FMAX:
        op = OP_FMINMAX, op2 = in->opcode == SGX535_USE_FMAX, sources = 2;
        break;
    case SGX535_USE_FDSX:
    case SGX535_USE_FDSY:
        op = OP_FGRADIENT, op2 = in->opcode == SGX535_USE_FDSY, sources = 2;
        break;
    case SGX535_USE_FDP:
        op = OP_FDP, op2 = 0, sources = 2;
        break;
    default: /* FRCP, FRSQ, FLOG, FEXP */
        op = OP_FSCALAR, op2 = (unsigned)(in->opcode - SGX535_USE_FRCP), sources = 1;
        break;
    }
    if (!flags_ok(in, 6) || !destination(&in->dst, &d) || !source(&in->src[1], &s1))
        return SGX535_USE_ERR_RANGE;
    if (sources == 3) {
        /* Word 1 bit 18 is END here, so src0 has no bank extension. */
        if (!source0(&in->src[0], &s0, false))
            return SGX535_USE_ERR_RANGE;
    } else if (!unused(&in->src[0]))
        return SGX535_USE_ERR_UNSUPPORTED;
    if (sources >= 2) {
        if (!source(&in->src[2], &s2))
            return SGX535_USE_ERR_RANGE;
    } else if (!unused(&in->src[2]))
        return SGX535_USE_ERR_UNSUPPORTED;
    /* FDOTPRODUCT's word 1 bit 19 is UPDATEI1, not DBEXT. */
    if (op == OP_FDP && d.ext)
        return SGX535_USE_ERR_UNSUPPORTED;
    if (op == OP_FGRADIENT &&
        (in->src[1].bank != in->src[2].bank || in->src[1].number != in->src[2].number ||
         (in->src[1].bank != SGX535_USE_R && in->src[1].bank != SGX535_USE_PA)))
        return SGX535_USE_ERR_UNSUPPORTED;
    w[0] = low(&d, &s1, &s2) | s0.number << 14;
    w[1] = W1_OP(op) | common(in) | high(&d, &s1, &s2) | (uint32_t)op2 << 9 |
           (uint32_t)in->src[1].mod << 5 | (uint32_t)in->src[2].mod << 3;
    if (sources == 3)
        w[1] |= (uint32_t)in->src[0].mod << 7 | s0.bank << 2;
    return SGX535_USE_OK;
}

/* MOVC unconditional: src1 moved, src2 the immediate #0. */
static enum sgx535_use_status mov(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d, s1, s2 = {0, 2, 1};
    if (!flags_ok(in, 6) || in->syncs || in->src[1].mod || !destination(&in->dst, &d) ||
        !source(&in->src[1], &s1))
        return SGX535_USE_ERR_RANGE;
    if (!unused(&in->src[0]) || !unused(&in->src[2]))
        return SGX535_USE_ERR_UNSUPPORTED;
    w[0] = low(&d, &s1, &s2);
    w[1] = W1_OP(OP_MOVC) | common(in) | high(&d, &s1, &s2);
    return SGX535_USE_OK;
}

static enum sgx535_use_status limm(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    if (in->pred || in->end > 1 || (in->dst.bank != SGX535_USE_R && in->dst.bank != SGX535_USE_O) ||
        in->dst.mod || in->dst.number >= (in->dst.bank == SGX535_USE_R ? SGX535_USE_TEMPS : 128u))
        return SGX535_USE_ERR_RANGE;
    sgx535_limm_compose(in->dst.bank, in->dst.number, in->immediate, in->end, w);
    return SGX535_USE_OK;
}

/* TEST: alu(src1, src2), the result's sign and zero tested into p[pdst];
 * with writeback the result also goes to dst, else dst is the c0 sink. */
static enum sgx535_use_status test(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    static const uint8_t alu[][2] = {
        [SGX535_USE_ALU_FADD] = {0, 0},  [SGX535_USE_ALU_FSUB] = {0, 14},
        [SGX535_USE_ALU_FMUL] = {0, 13}, [SGX535_USE_ALU_FMIN] = {0, 9},
        [SGX535_USE_ALU_FMAX] = {0, 10}, [SGX535_USE_ALU_AND] = {3, 0},
        [SGX535_USE_ALU_OR] = {3, 1},    [SGX535_USE_ALU_XOR] = {3, 2}};
    /* STST 11:10, ZTST 9:8, CRCOMB_AND 7 */
    static const uint16_t cond[] = {[SGX535_USE_TEST_GT] = 2u << 10 | 2u << 8 | 1u << 7,
                                    [SGX535_USE_TEST_GE] = 2u << 10 | 1u << 8,
                                    [SGX535_USE_TEST_EQ] = 0u << 10 | 1u << 8 | 1u << 7,
                                    [SGX535_USE_TEST_LT] = 1u << 10 | 2u << 8 | 1u << 7,
                                    [SGX535_USE_TEST_LE] = 1u << 10 | 1u << 8,
                                    [SGX535_USE_TEST_NE] = 0u << 10 | 2u << 8 | 1u << 7};
    struct field d = {0, 1, 1}, s1, s2;
    if (!flags_ok(in, 6) || in->end || in->syncs || in->repeat > 1 || in->test > 5 ||
        in->alu > SGX535_USE_ALU_XOR || in->pdst > 3 || in->writeback > 1 || in->src[1].mod ||
        in->src[2].mod || !source(&in->src[1], &s1) || !source(&in->src[2], &s2) ||
        !unused(&in->src[0]))
        return SGX535_USE_ERR_RANGE;
    if (in->writeback ? !destination(&in->dst, &d) : !unused(&in->dst))
        return SGX535_USE_ERR_RANGE;
    w[0] = low(&d, &s1, &s2) | F(writeback, 20) | (uint32_t)alu[in->alu][0] << 18 |
           (uint32_t)alu[in->alu][1] << 14;
    w[1] = W1_OP(OP_TEST) | F(pred, 24) | F(skipinv, 23) | F(nosched, 22) | high(&d, &s1, &s2) |
           UINT32_C(1) << 12 | cond[in->test] | F(pdst, 2);
    return SGX535_USE_OK;
}

/* PCKUNPCK: F32 to U8 (bytes of the mask from src1 then src2), or one U8 lane
 * (mask) of src1 to an F32. */
static enum sgx535_use_status pack(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d, s1, s2 = {0, 2, 1};
    bool unpack = in->opcode == SGX535_USE_UNPCKF32U8;
    if (!flags_ok(in, 6) || in->syncs || in->src[1].mod || in->src[2].mod ||
        !destination(&in->dst, &d) || !source(&in->src[1], &s1) || !unused(&in->src[0]))
        return SGX535_USE_ERR_RANGE;
    if (unpack) {
        if (in->mask > 3 || !unused(&in->src[2]) ||
            (in->format != 0 && in->format != 3 && in->format != 5))
            return SGX535_USE_ERR_RANGE;
        w[0] = low(&d, &s1, &s2) | F(scale, 18) | F(mask, 16);
        w[1] = W1_OP(OP_PCK) | common(in) | high(&d, &s1, &s2) | F(format, 9) | 6u << 6 | 15u << 2;
        return SGX535_USE_OK;
    }
    /* Three-byte masks have no established meaning. */
    if (!in->mask || in->mask > 15 || in->mask == 7 || in->mask == 11 || in->mask == 13 ||
        in->mask == 14 || !source(&in->src[2], &s2))
        return SGX535_USE_ERR_RANGE;
    w[0] = low(&d, &s1, &s2) | F(scale, 18);
    w[1] = W1_OP(OP_PCK) | common(in) | high(&d, &s1, &s2) | 6u << 9 | 0u << 6 | F(mask, 2);
    return SGX535_USE_OK;
}

/* SMP: write mask 1 (one destination run), RCNTSEL clear; dst r or pa. */
static enum sgx535_use_status smp(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field s0, s1, s2;
    if (!flags_ok(in, 6) || in->end || in->repeat > 1 || in->cdim > 2 || in->lodm > 3 ||
        in->drc > 3 || in->src[1].mod || in->src[2].mod || in->dst.mod ||
        (in->dst.bank != SGX535_USE_R && in->dst.bank != SGX535_USE_PA) ||
        in->dst.number >= (in->dst.bank == SGX535_USE_R ? SGX535_USE_TEMPS : 128u) ||
        in->src[0].mod || !source0(&in->src[0], &s0, true) || !source(&in->src[1], &s1) ||
        (in->lodm ? !source(&in->src[2], &s2) : !unused(&in->src[2])))
        return SGX535_USE_ERR_RANGE;
    if (!in->lodm)
        s2 = (struct field){0, 0, 0};
    w[0] = s1.bank << 30 | s2.bank << 28 | (uint32_t)in->dst.number << 21 | s0.number << 14 |
           s1.number << 7 | s2.number;
    w[1] = W1_OP(OP_SMP) | F(pred, 24) | F(skipinv, 23) | F(syncs, 20) | s0.ext << 18 |
           s1.ext << 17 | s2.ext << 16 | UINT32_C(1) << 12 | F(cdim, 10) | F(lodm, 8) |
           (uint32_t)(in->dst.bank == SGX535_USE_PA) << 7 | s0.bank << 2 | in->drc;
    return SGX535_USE_OK;
}

/* SPECIAL: flow control (OPCAT 0), wdf (OPCAT 2). */
static enum sgx535_use_status special(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    if (!unused(&in->dst) || !unused(&in->src[0]) || !unused(&in->src[1]) || !unused(&in->src[2]))
        return SGX535_USE_ERR_UNSUPPORTED;
    switch (in->opcode) {
    case SGX535_USE_WDF:
        if (in->drc > 3 || in->pred || in->end)
            return SGX535_USE_ERR_RANGE;
        w[0] = 0;
        w[1] = W1_OP(OP_SPECIAL) | UINT32_C(1) << 24 | UINT32_C(2) << 20 | in->drc;
        return SGX535_USE_OK;
    case SGX535_USE_NOP:
        if (in->pred > 6 || in->end > 1)
            return SGX535_USE_ERR_RANGE;
        w[0] = 0;
        w[1] = W1_OP(OP_SPECIAL) | F(pred, 24) | F(end, 18) | 5u << 6;
        return SGX535_USE_OK;
    default: /* BR relative (-2048..2047), BA absolute (0..4095); syncs is SYNCEND */
        if (in->pred > 6 || in->end || in->syncs > 1 || in->immediate > 0xfffu)
            return SGX535_USE_ERR_RANGE;
        w[0] = in->immediate;
        w[1] = W1_OP(OP_SPECIAL) | F(pred, 24) | F(syncs, 23) |
               (uint32_t)(in->opcode == SGX535_USE_BR) << 6;
        return SGX535_USE_OK;
    }
}

/* Registers 0-63 only in SOP2/SOPWM: bit 6 selects the C10 format. */
static bool sop_field(const struct sgx535_use_operand *o, struct field *f, bool dst)
{
    if (o->mod || (dst ? !destination(o, f) : !source(o, f)))
        return false;
    return o->bank == SGX535_USE_C || o->bank == SGX535_USE_G || o->bank == SGX535_USE_IMM ||
           o->bank == SGX535_USE_I || f->number < 64u;
}

/* Range checks below OR fields together: an OR of values is at most 2^k - 1
 * exactly when each value is. */
static enum sgx535_use_status sop(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d, s1, s2;
    if ((in->skipinv | in->nosched | in->end | in->cmod1 | in->cmod2) > 1 ||
        (in->pred | in->cop | in->aop) > 3 || in->repeat > 8 || !unused(&in->src[0]) ||
        !sop_field(&in->dst, &d, true) || !sop_field(&in->src[1], &s1, false) ||
        !sop_field(&in->src[2], &s2, false))
        return SGX535_USE_ERR_RANGE;
    w[0] = low(&d, &s1, &s2);
    w[1] = high(&d, &s1, &s2) | F(pred, 25) | F(skipinv, 23) | F(nosched, 22) | F(end, 18);
    w[1] |= (uint32_t)(in->repeat ? in->repeat - 1u : 0u) << 12 | F(cmod1, 24) | F(cmod2, 15);
    if (in->opcode == SGX535_USE_SOP2) {
        /* colour selectors zero s1 s2 s1a s2a asat, alpha zero s1a s2a */
        if (in->csel1 > 5 || in->csel2 > 5 || in->asel1 > 2 || in->asel2 > 2 ||
            (in->amod1 | in->amod2 | in->src1mod | in->asrc1mod | in->adstmod) > 1)
            return SGX535_USE_ERR_RANGE;
        w[0] |= F(src1mod, 20) | F(cop, 18) | F(aop, 16);
        w[0] |= F(asrc1mod, 15) | F(adstmod, 14);
        w[1] |= W1_OP(OP_SOP2) | F(asel1, 20) | F(amod1, 11) | F(asel2, 9);
        w[1] |= F(csel1, 6) | F(csel2, 3) | F(amod2, 2);
        return SGX535_USE_OK;
    }
    /* SOPWM selectors: zero 0, asat 1, s1 2, s1a 3, s2 6, s2a 7. */
    if (in->repeat > 1 || (in->csel1 | in->csel2) > 7 || in->mask > 15 || (in->csel1 & 6) == 4 ||
        (in->csel2 & 6) == 4)
        return SGX535_USE_ERR_RANGE;
    w[1] |= W1_OP(OP_SOPWM) | F(cop, 20) | F(mask, 11) | F(aop, 9) | F(csel1, 6) | F(csel2, 3);
    return SGX535_USE_OK;
}

/* SPECIAL OPCAT VISTEST: pcoeff (0), atst8 (1), depthf (3). src0 the colour or
 * depth, src1 and src2 the ISP feedback words; src0's extension is bit 19. */
static enum sgx535_use_status vistest(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d = {0, 0, 0}, s0, s1, s2;
    unsigned op2 = in->opcode == SGX535_USE_PCOEFF ? 0u : in->opcode == SGX535_USE_ATST8 ? 1u : 3u;
    uint8_t flags = in->syncs | in->nosched | in->twosided | in->optdwd | in->nofeedback;
    if ((flags | in->end | in->pdsten | in->repeat) > 1 || (in->pred | in->pdst) > 3 ||
        in->skipinv || (in->src[0].mod | in->src[1].mod | in->src[2].mod) ||
        !source0(&in->src[0], &s0, true) || !source(&in->src[1], &s1) || !source(&in->src[2], &s2))
        return SGX535_USE_ERR_RANGE;
    if (op2 == 1 ? !destination(&in->dst, &d) || d.ext : !unused(&in->dst))
        return SGX535_USE_ERR_RANGE;
    /* A predicate written with the result is atst8's; pcoeff has no options. */
    if ((op2 != 1 && (in->pdst | in->pdsten)) || (op2 == 0 && (flags | in->pred)))
        return SGX535_USE_ERR_UNSUPPORTED;
    /* the test's options: written predicate, two-sided, feedback */
    uint32_t options = F(pdst, 7) | F(pdsten, 6) | F(twosided, 5) | F(optdwd, 4) | F(nofeedback, 3);
    w[0] = low(&d, &s1, &s2) | s0.number << 14;
    w[1] = W1_OP(OP_SPECIAL) | (uint32_t)op2 << 24 | UINT32_C(3) << 20 | s0.bank << 2 | d.bank;
    w[1] |= s0.ext << 19 | s1.ext << 17 | s2.ext << 16 | F(end, 18) | F(syncs, 23);
    w[1] |= options | F(nosched, 11) | F(pred, 9);
    return SGX535_USE_OK;
}

/* ANDOR (OP2 bit 3: and 0, or 1) and XOR: SPRED, plain sources. */
static enum sgx535_use_status bitwise(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    struct field d, s1, s2;
    if (in->pred > 3 || (in->skipinv | in->end | in->invert) > 1 || in->repeat > 16 || in->syncs ||
        in->nosched || in->src[1].mod || in->src[2].mod || !unused(&in->src[0]) ||
        !destination(&in->dst, &d) || !source(&in->src[1], &s1) || !source(&in->src[2], &s2))
        return SGX535_USE_ERR_RANGE;
    w[0] = low(&d, &s1, &s2);
    w[1] = W1_OP(in->opcode == SGX535_USE_XOR ? OP_XOR : OP_ANDOR) | F(pred, 25) | F(skipinv, 23) |
           F(end, 18) | repeat_count(in) | high(&d, &s1, &s2) | F(invert, 11) |
           (uint32_t)(in->opcode == SGX535_USE_OR) << 3;
    return SGX535_USE_OK;
}

enum sgx535_use_status sgx535_use_encode(const struct sgx535_use_instruction *in, uint32_t w[2])
{
    uint32_t out[2];
    enum sgx535_use_status status;
    if (!in || !w)
        return SGX535_USE_ERR_ARGUMENT;
    switch (in->opcode) {
    case SGX535_USE_FMAD:
    case SGX535_USE_FFRC:
    case SGX535_USE_FMIN:
    case SGX535_USE_FMAX:
    case SGX535_USE_FRCP:
    case SGX535_USE_FRSQ:
    case SGX535_USE_FLOG:
    case SGX535_USE_FEXP:
    case SGX535_USE_FDSX:
    case SGX535_USE_FDSY:
    case SGX535_USE_FDP:
        status = farith(in, out);
        break;
    case SGX535_USE_MOV:
        status = mov(in, out);
        break;
    case SGX535_USE_LIMM:
        status = limm(in, out);
        break;
    case SGX535_USE_TEST:
        status = test(in, out);
        break;
    case SGX535_USE_PCKU8F32:
    case SGX535_USE_UNPCKF32U8:
        status = pack(in, out);
        break;
    case SGX535_USE_SMP:
        status = smp(in, out);
        break;
    case SGX535_USE_WDF:
    case SGX535_USE_BR:
    case SGX535_USE_BA:
    case SGX535_USE_NOP:
        status = special(in, out);
        break;
    case SGX535_USE_SOP2:
    case SGX535_USE_SOPWM:
        status = sop(in, out);
        break;
    case SGX535_USE_ATST8:
    case SGX535_USE_DEPTHF:
    case SGX535_USE_PCOEFF:
        status = vistest(in, out);
        break;
    case SGX535_USE_AND:
    case SGX535_USE_OR:
    case SGX535_USE_XOR:
        status = bitwise(in, out);
        break;
    default:
        return SGX535_USE_ERR_UNSUPPORTED;
    }
    if (status == SGX535_USE_OK)
        w[0] = out[0], w[1] = out[1];
    return status;
}

enum sgx535_use_status sgx535_use_append(uint32_t *words, uint32_t *count, uint32_t capacity,
                                         const struct sgx535_use_instruction *in)
{
    enum sgx535_use_status status;
    if (!words || !count)
        return SGX535_USE_ERR_ARGUMENT;
    if (*count >= capacity)
        return SGX535_USE_ERR_RANGE;
    status = sgx535_use_encode(in, &words[2u * *count]);
    if (status == SGX535_USE_OK)
        (*count)++;
    return status;
}
