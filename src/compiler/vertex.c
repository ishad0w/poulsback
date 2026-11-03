// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Vertex program, vertex PDS and SECONDARY PDS words (sgx535_vertex.h). */
#include "sgx535_vertex_nir.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif

/* USE register banks of a source field: temporary, output, PA, SA. */
enum bank { TEMP, OUTPUT, PRIMARY, SECONDARY };
/* The high words of the arithmetic opcodes (with skipinv), and their sources. */
static const struct {
    uint32_t high;
    unsigned sources;
} opcode[] = {
    [SGX535_VERTEX_MAD] = {UINT32_C(0x00a00000), 3}, [SGX535_VERTEX_MIN] = {UINT32_C(0x18a00000), 2},
    [SGX535_VERTEX_MAX] = {UINT32_C(0x18a00200), 2}, [SGX535_VERTEX_RCP] = {UINT32_C(0x08a00000), 1},
    [SGX535_VERTEX_RSQ] = {UINT32_C(0x08a00200), 1}, [SGX535_VERTEX_LOG] = {UINT32_C(0x08a00400), 1},
    [SGX535_VERTEX_EXP] = {UINT32_C(0x08a00600), 1}, [SGX535_VERTEX_MOV] = {0, 1},
    [SGX535_VERTEX_UNORM8] = {0, 1}, [SGX535_VERTEX_UNORM8 + 1] = {0, 1},
    [SGX535_VERTEX_UNORM8 + 2] = {0, 1}, [SGX535_VERTEX_UNORM8 + 3] = {0, 1}};
#define EMIT_LOW UINT32_C(0xa0200000)
#define EMIT_HIGH UINT32_C(0xfb275000) /* emitvtx.end.freep */

struct operand {
    unsigned bank, number, negate, absolute;
};
struct encoder {
    struct sgx535_vertex_code code;
    int8_t temp[SGX535_VERTEX_MAX_INSTRUCTIONS]; /* each value's temporary */
    uint32_t last[SGX535_VERTEX_MAX_INSTRUCTIONS];
    bool busy[SGX535_VERTEX_TEMPS];
};

static int temp_get(struct encoder *e)
{
    for (unsigned t = 0; t < SGX535_VERTEX_TEMPS; t++)
        if (!e->busy[t]) {
            e->busy[t] = true;
            if (t + 1u > e->code.temps)
                e->code.temps = t + 1u;
            return (int)t;
        }
    return -1;
}
static bool put(struct encoder *e, uint32_t low, uint32_t high)
{
    if (e->code.instruction_count == SGX535_VERTEX_MAX_CODE)
        return false;
    e->code.words[2u * e->code.instruction_count] = low;
    e->code.words[2u * e->code.instruction_count + 1u] = high;
    e->code.instruction_count++;
    return true;
}
static struct operand operand(const struct encoder *e, const struct sgx535_vertex_source *s)
{
    struct operand o = {TEMP, s->index, s->negate, s->absolute};
    if (s->kind == SGX535_VERTEX_ATTRIBUTE)
        o.bank = PRIMARY;
    else if (s->kind == SGX535_VERTEX_CONSTANT)
        o.bank = SECONDARY;
    else
        o.number = (unsigned)e->temp[s->index];
    return o;
}
/* mov: any source bank, no modifiers. */
static bool mov(struct encoder *e, unsigned dst_bank, unsigned dst, struct operand s)
{
    return put(e, UINT32_C(0x20000000) | dst << 21 | s.number << 7 | s.bank << 30,
               UINT32_C(0x28a10000) | dst_bank);
}
/* unpckf32u8.scale: byte LANE (17:16) of S (13:7, bank 31:30) as an F32 in
 * [0, 1], all four destination bytes written (5:2); the second source is the
 * immediate 0 (bank 2 with the extension bit 16), as EMGD's unpacks have it.
 * Bit 21 set makes 15:12 a repeat count (zero: once), as in the other
 * instructions; clear, they are a mask of repeats (EMGD's unpacks set 12). */
static bool unorm8(struct encoder *e, unsigned dst_bank, unsigned dst, struct operand s,
                   unsigned lane)
{
    return put(e, dst << 21 | UINT32_C(1) << 18 | lane << 16 | s.number << 7 | s.bank << 30 |
                      UINT32_C(2) << 28,
               UINT32_C(0x40a10000) | 6u << 6 | 15u << 2 | dst_bank);
}
/* One arithmetic instruction. Sources: MAD's at 20:14 (a temporary or PA,
 * modifiers at bits 7/8), 13:7 (bank 31:30, modifiers 5/6) and 6:0 (bank
 * 29:28, modifiers 3/4); two-source operations use the last two fields, a
 * one-source operation the middle one. */
static bool arithmetic(struct encoder *e, unsigned op, unsigned dst_bank, unsigned dst,
                       const struct operand *s)
{
    unsigned n = opcode[op].sources;
    const struct operand *mid = &s[n == 3 ? 1 : 0], *last = &s[n == 3 ? 2 : 1];
    uint32_t low = dst << 21 | mid->number << 7 | mid->bank << 30;
    uint32_t high = opcode[op].high | dst_bank | mid->negate << 5 | mid->absolute << 6;
    if (n == 3) {
        low |= s[0].number << 14;
        high |= (s[0].bank == PRIMARY ? 4u : 0u) | s[0].negate << 7 | s[0].absolute << 8;
    }
    if (n >= 2) {
        low |= last->number | last->bank << 28;
        high |= last->negate << 3 | last->absolute << 4;
    }
    return put(e, low, high);
}

static enum sgx535_vertex_status check(const struct sgx535_vertex_instruction *in, uint32_t count,
                                       uint32_t outputs, uint32_t *last)
{
    uint64_t written = 0;
    if (!in || !count || count > SGX535_VERTEX_MAX_INSTRUCTIONS || !outputs ||
        outputs > SGX535_VERTEX_OUTPUTS)
        return SGX535_VERTEX_INVALID;
    for (uint32_t i = 0; i < count; i++) {
        unsigned n;
        if (in[i].opcode > SGX535_VERTEX_UNORM8 + 3u ||
            (in[i].opcode >= SGX535_VERTEX_UNORM8 &&
             in[i].source[0].kind != SGX535_VERTEX_ATTRIBUTE))
            return SGX535_VERTEX_INVALID;
        n = opcode[in[i].opcode].sources;
        if (in[i].output >= 0) {
            if ((uint32_t)in[i].output >= outputs || (written >> in[i].output & 1u))
                return SGX535_VERTEX_INVALID;
            written |= UINT64_C(1) << in[i].output;
        } else if (in[i].output != -1)
            return SGX535_VERTEX_INVALID;
        last[i] = i;
        for (unsigned k = 0; k < 3; k++) {
            const struct sgx535_vertex_source *s = &in[i].source[k];
            if (k >= n) {
                if (s->kind || s->negate || s->absolute || s->index)
                    return SGX535_VERTEX_INVALID;
                continue;
            }
            if (s->negate > 1 || s->absolute > 1 ||
                ((in[i].opcode == SGX535_VERTEX_MOV || in[i].opcode >= SGX535_VERTEX_UNORM8) &&
                 (s->negate || s->absolute)))
                return SGX535_VERTEX_INVALID;
            if (s->kind == SGX535_VERTEX_VALUE) {
                if (s->index >= i || in[s->index].output >= 0)
                    return SGX535_VERTEX_INVALID;
                last[s->index] = i;
            } else if ((s->kind == SGX535_VERTEX_ATTRIBUTE && s->index >= SGX535_VERTEX_ATTRIBUTES) ||
                       (s->kind == SGX535_VERTEX_CONSTANT && s->index >= SGX535_VERTEX_CONSTANTS) ||
                       s->kind > SGX535_VERTEX_CONSTANT)
                return SGX535_VERTEX_RANGE;
        }
    }
    return written == (UINT64_C(1) << outputs) - 1u ? SGX535_VERTEX_OK : SGX535_VERTEX_INVALID;
}

enum sgx535_vertex_status sgx535_vertex_encode(const struct sgx535_vertex_instruction *in,
                                               uint32_t count, uint32_t outputs,
                                               struct sgx535_vertex_code *out)
{
    static struct encoder encoder; /* single-threaded callers, like the fragment compiler */
    struct encoder *e = &encoder;
    enum sgx535_vertex_status status;
    if (!out)
        return SGX535_VERTEX_INVALID;
    memset(e, 0, sizeof(*e));
    status = check(in, count, outputs, e->last);
    if (status)
        return status;
    for (uint32_t i = 0; i < count; i++) {
        const struct sgx535_vertex_instruction *x = &in[i];
        struct operand s[3] = {{0}};
        unsigned n = opcode[x->opcode].sources, dst_bank = OUTPUT, dst = 0;
        int scratch = -1;
        bool ok;
        for (unsigned k = 0; k < n; k++)
            s[k] = operand(e, &x->source[k]);
        /* MAD's first factor reads a temporary or PA only: swap the factors,
         * else move it into a temporary first. */
        if (x->opcode == SGX535_VERTEX_MAD && s[0].bank != TEMP && s[0].bank != PRIMARY) {
            if (s[1].bank == TEMP || s[1].bank == PRIMARY) {
                struct operand t = s[0];
                s[0] = s[1];
                s[1] = t;
            } else {
                scratch = temp_get(e);
                if (scratch < 0 || !mov(e, TEMP, (unsigned)scratch, s[0]))
                    return SGX535_VERTEX_RANGE;
                s[0].bank = TEMP;
                s[0].number = (unsigned)scratch;
            }
        }
        /* Values whose last use is this instruction free their temporaries
         * before the destination is chosen: a scalar reads before it writes. */
        for (unsigned k = 0; k < n; k++)
            if (x->source[k].kind == SGX535_VERTEX_VALUE && e->last[x->source[k].index] == i)
                e->busy[(unsigned)e->temp[x->source[k].index]] = false;
        if (scratch >= 0)
            e->busy[scratch] = false;
        if (x->output >= 0)
            dst = (unsigned)x->output;
        else {
            int t = temp_get(e);
            if (t < 0)
                return SGX535_VERTEX_RANGE;
            e->temp[i] = (int8_t)t;
            dst_bank = TEMP;
            dst = (unsigned)t;
            if (e->last[i] == i) /* never read */
                e->busy[t] = false;
        }
        ok = x->opcode == SGX535_VERTEX_MOV      ? mov(e, dst_bank, dst, s[0])
             : x->opcode >= SGX535_VERTEX_UNORM8
                 ? unorm8(e, dst_bank, dst, s[0], x->opcode - SGX535_VERTEX_UNORM8)
                                                 : arithmetic(e, x->opcode, dst_bank, dst, s);
        if (!ok)
            return SGX535_VERTEX_RANGE;
    }
    if (!put(e, EMIT_LOW, EMIT_HIGH))
        return SGX535_VERTEX_RANGE;
    memcpy(out, &e->code, sizeof(*out));
    return SGX535_VERTEX_OK;
}

void sgx535_vertex_task(uint32_t link, uint32_t temps, uint32_t task[3])
{
    task[0] = link | UINT32_C(0x00200000);
    task[1] = temps << 27;
    task[2] = temps >> 5;
}

/* PDS data: DS0 and DS1 interleaved by rows of eight dwords. */
static uint32_t ds0(uint32_t d)
{
    return d / 8u * 16u + d % 8u;
}
static uint32_t ds1(uint32_t d)
{
    return d / 8u * 16u + 8u + d % 8u;
}
#define PDS_HALT UINT32_C(0xaf000000)
/* movs doutd, ds0[2e], ds0[2e + 1]: the DMA of data pair E. */
static uint32_t movs_doutd(uint32_t pair)
{
    return UINT32_C(0x07030223) | pair << 18;
}
/* A DMA descriptor: DWORDS into registers from FIRST, in lines of at most
 * sixteen (one line: no line fields, as EMGD writes it). */
static uint32_t descriptor(uint32_t first, uint32_t lines, uint32_t columns)
{
    uint32_t d = first << 8 | (columns - 1u);
    if (lines > 1u)
        d |= (columns - 1u) << 21 | (lines - 1u) << 4;
    return d;
}

enum sgx535_vertex_status sgx535_vertex_pds_build(const struct sgx535_vertex_pds_layout *in,
                                                  struct sgx535_vertex_pds *out)
{
    struct sgx535_vertex_pds p;
    uint32_t stride[SGX535_VERTEX_PDS_STRIDES], slot[SGX535_VERTEX_PDS_ELEMENTS];
    uint32_t strides = 0, e, n, code, top;
    if (!in || !out || !in->element_count || in->element_count > SGX535_VERTEX_PDS_ELEMENTS)
        return SGX535_VERTEX_INVALID;
    n = in->element_count;
    memset(&p, 0, sizeof(p));
    for (e = 0; e < n; e++) {
        const struct sgx535_vertex_pds_element *x = &in->element[e];
        uint32_t k;
        if ((x->primary & 3u) || x->primary + 4u > SGX535_VERTEX_ATTRIBUTES || !x->dwords ||
            x->dwords > 4u || x->stride > 0xffffu || (x->stride & 3u))
            return SGX535_VERTEX_RANGE;
        slot[e] = UINT32_MAX;
        if (!x->stride)
            continue;
        for (k = 0; k < strides && stride[k] != x->stride; k++) {
        }
        if (k == strides) {
            if (strides == SGX535_VERTEX_PDS_STRIDES)
                return SGX535_VERTEX_UNSUPPORTED;
            stride[strides++] = x->stride;
        }
        slot[e] = k;
    }
    /* DS0: each element's source address and descriptor, then DOUTU0/1; DS1:
     * the strides, then DOUTU2. */
    for (e = 0; e < n; e++) {
        p.source_word[e] = ds0(2u * e);
        p.words[ds0(2u * e + 1u)] = descriptor(in->element[e].primary, 1, in->element[e].dwords);
    }
    p.words[ds0(2u * n)] = in->task[0];
    p.words[ds0(2u * n + 1u)] = in->task[1];
    for (e = 0; e < strides; e++)
        p.words[ds1(e)] = stride[e];
    p.words[ds1(strides)] = in->task[2];
    top = ds0(2u * n + 1u) > ds1(strides) ? ds0(2u * n + 1u) : ds1(strides);
    p.data_words = (top + 4u) & ~3u;
    code = p.data_words;
    /* mul ds1[50 + 2k], ir0.l, ds1[k].l: the vertex's offset at stride k. */
    for (e = 0; e < strides; e++)
        p.words[code++] = UINT32_C(0x67800040) | (2u * e) << 9 | (50u + 2u * e);
    /* movsa doutd, ds0[2e], ds0[2e + 1], ds1[50 + 2k]: address + offset. */
    for (e = 0; e < n; e++)
        p.words[code++] = slot[e] == UINT32_MAX
                              ? movs_doutd(e)
                              : UINT32_C(0x2f000343) | e << 18 | (25u + slot[e]) << 13;
    /* (if0) movs doutu, ds0[2n], ds0[2n + 1], ds1[strides]. */
    {
        uint32_t x = 2u + (strides & 1u);
        p.words[code++] = UINT32_C(0x03000005) | n << 18 | (strides >> 1) << 13 |
                          (UINT32_C(0x10) | x << 2 | x) << 5;
    }
    p.words[code++] = PDS_HALT;
    p.word_count = code;
    *out = p;
    return SGX535_VERTEX_OK;
}

enum sgx535_vertex_status sgx535_vertex_secondary_build(uint32_t dwords, const uint32_t task[3],
                                                        struct sgx535_vertex_secondary *out)
{
    struct sgx535_vertex_secondary p;
    uint32_t columns, code;
    if (!task || !out || !dwords || dwords > SGX535_VERTEX_CONSTANTS)
        return SGX535_VERTEX_INVALID;
    memset(&p, 0, sizeof(p));
    /* Lines of at most sixteen: the largest such divisor with at most sixteen
     * lines (EMGD: 24 as 2 x 12, 96 as 6 x 16); else whole lines of sixteen
     * and a second transfer for the rest. */
    for (columns = dwords < 16u ? dwords : 16u; columns > 1u; columns--)
        if (!(dwords % columns) && dwords / columns <= 16u)
            break;
    if (dwords <= 16u || (columns > 1u && !(dwords % columns))) {
        p.transfer_count = 1;
        p.words[ds0(1)] = descriptor(0, dwords / columns, columns);
    } else {
        uint32_t lines = dwords / 16u, rest = dwords % 16u;
        p.transfer_count = 2;
        p.words[ds0(1)] = descriptor(0, lines, 16);
        p.words[ds0(3)] = descriptor(lines * 16u, 1, rest);
        p.source_offset[1] = lines * 64u;
    }
    p.source_word[0] = ds0(0);
    p.source_word[1] = ds0(2);
    p.words[ds0(2u * p.transfer_count)] = task[0];
    p.words[ds0(2u * p.transfer_count + 1u)] = task[1];
    p.words[ds1(0)] = task[2];
    p.data_words = (ds1(0) + 4u) & ~3u;
    code = p.data_words;
    for (uint32_t t = 0; t < p.transfer_count; t++)
        p.words[code++] = movs_doutd(t);
    /* movs doutu, ds0[2t], ds0[2t + 1], ds1[0]. */
    p.words[code++] = UINT32_C(0x07000345) | p.transfer_count << 18;
    p.words[code++] = PDS_HALT;
    p.word_count = code;
    *out = p;
    return SGX535_VERTEX_OK;
}

/* ---- one draw's program from a NIR vertex shader (sgx535_vertex_nir.h) ---- */

/* A source of the draw's program: values move past the K unpacks first; an
 * attribute component the element lacks reads GL's default (0, 0, 0, 1); a
 * UNORM8 element's component reads its unpack (UNPACKED[input][component]). */
static struct sgx535_vertex_source substituted(const struct sgx535_vertex_nir_program *p,
                                               const uint8_t components[SGX535_VERTEX_INPUTS],
                                               uint16_t (*unpacked)[4], uint32_t k,
                                               struct sgx535_vertex_source s)
{
    uint32_t input = s.index / 4u, c = s.index % 4u;
    if (s.kind == SGX535_VERTEX_VALUE)
        s.index = (uint16_t)(s.index + k);
    else if (s.kind == SGX535_VERTEX_ATTRIBUTE && c >= (components[input] & 7u)) {
        s.kind = SGX535_VERTEX_CONSTANT;
        s.index = (uint16_t)(c == 3u ? p->one : p->zero);
    } else if (s.kind == SGX535_VERTEX_ATTRIBUTE && (components[input] & SGX535_VERTEX_PACKED)) {
        s.kind = SGX535_VERTEX_VALUE;
        s.index = unpacked[input][c];
    }
    return s;
}
static bool plain(const struct sgx535_vertex_source *s)
{
    return !s->negate && !s->absolute;
}
static bool is_constant(const struct sgx535_vertex_source *s, uint32_t index)
{
    return s->kind == SGX535_VERTEX_CONSTANT && s->index == index && plain(s);
}
/* NIR's fadd of an fmul, as the adapter writes them: MAD(x, 1, y) or
 * MAD(y, 1, x) with x = MAD(a, b, 0) read only there, becomes MAD(+-a, b, y);
 * the product is then dead. USES counts each value's readers. */
static void fuse(const struct sgx535_vertex_nir_program *p, struct sgx535_vertex_instruction *in,
                 uint32_t n, const uint32_t *uses)
{
    for (uint32_t i = 0; i < n; i++) {
        struct sgx535_vertex_instruction *x = &in[i];
        if (x->opcode != SGX535_VERTEX_MAD)
            continue;
        for (unsigned k = 0; k < 2; k++) {
            struct sgx535_vertex_source product = x->source[k == 0 ? 0 : 2],
                                        other = x->source[k == 0 ? 2 : 0];
            const struct sgx535_vertex_instruction *m;
            if (!is_constant(&x->source[1], p->one) || product.kind != SGX535_VERTEX_VALUE ||
                product.absolute || product.index >= i || uses[product.index] != 1u)
                continue;
            m = &in[product.index];
            if (m->opcode != SGX535_VERTEX_MAD || m->output >= 0 ||
                !is_constant(&m->source[2], p->zero))
                continue;
            x->source[0] = m->source[0];
            x->source[0].negate ^= product.negate;
            x->source[1] = m->source[1];
            x->source[2] = other;
            break;
        }
    }
}

/* The output of P at LOCATION, or NULL. */
static const struct sgx535_vertex_nir_output *output_at(const struct sgx535_vertex_nir_program *p,
                                                        uint16_t location)
{
    for (uint32_t k = 0; k < p->output_count && k < SGX535_VERTEX_NIR_OUTPUTS; k++)
        if (p->output[k].location == location)
            return &p->output[k];
    return NULL;
}

enum sgx535_vertex_status sgx535_vertex_nir_link(const struct sgx535_vertex_nir_program *p,
                                                 const uint8_t components[SGX535_VERTEX_INPUTS],
                                                 const struct sgx535_vertex_tc *tc,
                                                 uint32_t tc_count, struct sgx535_vertex_code *out)
{
    /* Single-threaded callers, as for the encoder. */
    static struct sgx535_vertex_instruction in[SGX535_VERTEX_MAX_INSTRUCTIONS],
        work[SGX535_VERTEX_MAX_INSTRUCTIONS];
    static uint32_t map[SGX535_VERTEX_MAX_INSTRUCTIONS], uses[SGX535_VERTEX_MAX_INSTRUCTIONS];
    static bool live[SGX535_VERTEX_MAX_INSTRUCTIONS];
    static uint16_t unpacked[SGX535_VERTEX_INPUTS][4];
    struct sgx535_vertex_source result[SGX535_VERTEX_OUTPUTS];
    bool clamp[SGX535_VERTEX_OUTPUTS] = {false};
    uint32_t outputs = 4, n = 0, k = 0, total;
    if (!p || !components || !out || (tc_count && !tc) || tc_count > SGX535_VERTEX_TCS ||
        p->count > SGX535_VERTEX_MAX_INSTRUCTIONS || p->zero >= SGX535_VERTEX_CONSTANTS ||
        p->one >= SGX535_VERTEX_CONSTANTS || p->output_count > SGX535_VERTEX_NIR_OUTPUTS)
        return SGX535_VERTEX_INVALID;
    for (uint32_t t = 0; t < tc_count; t++)
        if (!tc[t].components || tc[t].components > 4u)
            return SGX535_VERTEX_INVALID;
    if (!p->has_position)
        return SGX535_VERTEX_UNSUPPORTED;
    for (uint32_t t = 0; t < tc_count; t++) {
        const struct sgx535_vertex_nir_output *o = output_at(p, tc[t].location);
        if (!o || (o->written & ((1u << tc[t].components) - 1u)) != (1u << tc[t].components) - 1u)
            return SGX535_VERTEX_UNSUPPORTED;
    }
    for (uint32_t i = 0; i < SGX535_VERTEX_INPUTS; i++)
        if (!(components[i] & 7u) || (components[i] & 7u) > 4u ||
            (components[i] & ~(7u | SGX535_VERTEX_PACKED | SGX535_VERTEX_BGRA)) ||
            ((components[i] & SGX535_VERTEX_BGRA) && !(components[i] & SGX535_VERTEX_PACKED)))
            return SGX535_VERTEX_INVALID;
    /* A packed UNORM8 element's components the program reads: unpacked once,
     * first (BGRA: lanes 2, 1, 0, 3). */
    for (uint32_t i = 0; i < p->input_count && i < SGX535_VERTEX_INPUTS; i++)
        for (uint32_t c = 0; (components[i] & SGX535_VERTEX_PACKED) && c < (components[i] & 7u);
             c++)
            if (p->input_components[i] >> c & 1u) {
                if (k == SGX535_VERTEX_MAX_INSTRUCTIONS)
                    return SGX535_VERTEX_RANGE;
                work[k] = (struct sgx535_vertex_instruction){
                    (uint8_t)(SGX535_VERTEX_UNORM8 +
                              ((components[i] & SGX535_VERTEX_BGRA) && c < 3u ? 2u - c : c)),
                    -1,
                    {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4u * i)}}};
                unpacked[i][c] = (uint16_t)k++;
            }
    total = k + p->count;
    if (total > SGX535_VERTEX_MAX_INSTRUCTIONS)
        return SGX535_VERTEX_RANGE;
    for (uint32_t o = 0; o < 4u; o++)
        result[o] = substituted(p, components, unpacked, k, p->position[o]);
    for (uint32_t t = 0; t < tc_count; t++) {
        const struct sgx535_vertex_nir_output *o = output_at(p, tc[t].location);
        for (uint32_t c = 0; c < tc[t].components; c++, outputs++) {
            result[outputs] = substituted(p, components, unpacked, k, o->source[c]);
            clamp[outputs] = o->clamp;
        }
    }
    /* Substituted sources, then fused products (readers counted over the whole
     * program: a product feeding a dropped output too stays). */
    memset(uses, 0, sizeof(uses));
    for (uint32_t i = k; i < total; i++) {
        work[i] = p->instruction[i - k];
        for (unsigned j = 0; j < 3; j++) {
            if (work[i].source[j].kind == SGX535_VERTEX_VALUE && work[i].source[j].index >= i - k)
                return SGX535_VERTEX_INVALID;
            work[i].source[j] = substituted(p, components, unpacked, k, work[i].source[j]);
            if (work[i].source[j].kind == SGX535_VERTEX_VALUE)
                uses[work[i].source[j].index]++;
        }
    }
    for (uint32_t o = 0; o < 4u + 4u * p->output_count; o++) {
        const struct sgx535_vertex_nir_output *x = o < 4u ? NULL : &p->output[o / 4u - 1u];
        struct sgx535_vertex_source s;
        if (x && !(x->written >> o % 4u & 1u))
            continue;
        s = substituted(p, components, unpacked, k, x ? x->source[o % 4u] : p->position[o]);
        if (s.kind == SGX535_VERTEX_VALUE && s.index < total)
            uses[s.index]++;
    }
    fuse(p, work, total, uses);
    /* The instructions the outputs need, renumbered. */
    memset(live, 0, sizeof(live));
    for (uint32_t o = 0; o < outputs; o++)
        if (result[o].kind == SGX535_VERTEX_VALUE) {
            if (result[o].index >= total)
                return SGX535_VERTEX_INVALID;
            live[result[o].index] = true;
        }
    for (uint32_t i = total; i-- > 0;)
        for (unsigned j = 0; live[i] && j < 3; j++)
            if (work[i].source[j].kind == SGX535_VERTEX_VALUE && work[i].source[j].index < i)
                live[work[i].source[j].index] = true;
    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < total; i++) {
        if (!live[i])
            continue;
        in[n] = work[i];
        for (unsigned j = 0; j < 3; j++) {
            struct sgx535_vertex_source *s = &in[n].source[j];
            if (s->kind == SGX535_VERTEX_VALUE && s->index < i && live[s->index]) {
                s->index = (uint16_t)map[s->index];
                uses[s->index]++;
            }
        }
        map[i] = n++;
    }
    for (uint32_t o = 0; o < outputs; o++)
        if (result[o].kind == SGX535_VERTEX_VALUE) {
            result[o].index = (uint16_t)map[result[o].index];
            uses[result[o].index]++;
        }
    /* An output the defining instruction writes when nothing else reads the
     * value; else a move (a MAX of the value with itself when it carries
     * modifiers). COLOR0: clamped to [0, 1]. */
    for (uint32_t o = 0; o < outputs; o++) {
        struct sgx535_vertex_source s = result[o];
        if (n + 2u > SGX535_VERTEX_MAX_INSTRUCTIONS)
            return SGX535_VERTEX_RANGE;
        if (clamp[o]) {
            const struct sgx535_vertex_source zero = {SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)p->zero},
                                              one = {SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)p->one},
                                              t = {SGX535_VERTEX_VALUE, 0, 0, (uint16_t)n};
            in[n++] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MAX, -1, {s, zero}};
            in[n++] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MIN, (int8_t)o, {t, one}};
        } else if (s.kind == SGX535_VERTEX_VALUE && plain(&s) && uses[s.index] == 1u &&
                   in[s.index].output < 0)
            in[s.index].output = (int8_t)o;
        else if (plain(&s))
            in[n++] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MOV, (int8_t)o, {s}};
        else
            in[n++] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MAX, (int8_t)o, {s, s}};
    }
    return sgx535_vertex_encode(in, n, outputs, out);
}
