// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Vertex program, vertex PDS and SECONDARY PDS words (sgx535_vertex.h).
 *
 * - The PDS programs equal EMGD's captured ones word for word (docs/re/emgd-linux/vertex-path.md),
 * apart from
 *   data words the programs never read.
 * - Encoded vertex programs run on an independent model of the words (field
 *   layouts as the rev121 decoder reads them, written apart from the encoder)
 *   and must give each output exactly as the IR evaluated directly. Both
 *   sides give the one-source operations the same distinct stand-in
 *   functions: the model checks data flow and encoding, not arithmetic. */
#include "sgx535_vertex_nir.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)

/* ---- PDS programs against EMGD's ---- */

static void pds_equal(const struct sgx535_vertex_pds *p, const uint32_t *data, const int *used,
                      uint32_t data_words, const uint32_t *code, uint32_t code_words)
{
    CHECK(p->data_words == data_words && p->word_count == data_words + code_words);
    for (uint32_t i = 0; i < data_words; i++)
        if (used[i])
            CHECK(p->words[i] == data[i]);
    CHECK(!memcmp(p->words + data_words, code, code_words * 4u));
}

static void test_vertex_pds(void)
{
    /* tri, k6 user draw at f84001c0: pa0 and pa4, 4 dwords each, stride 32. */
    {
        const struct sgx535_vertex_pds_layout in = {2, {{0, 4, 0x20}, {4, 4, 0x20}}, {0xa1a, 0, 0}};
        const uint32_t data[12] = {0, 3, 0, 0x403, 0xa1a, 0, 0, 0, 0x20, 0};
        const int used[12] = {0, 1, 0, 1, 1, 1, 0, 0, 1, 1};
        const uint32_t code[] = {0x67800072, 0x2f032343, 0x2f072343, 0x030803e5, 0xaf000000};
        struct sgx535_vertex_pds p;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_OK);
        pds_equal(&p, data, used, 12, code, 5);
        CHECK(p.source_word[0] == 0 && p.source_word[1] == 2);
    }
    /* vbo/tri frame 1's quad at f84000e0: 3 dwords to pa0, 4 to pa4, stride 28,
     * a phase-1 task. */
    {
        const struct sgx535_vertex_pds_layout in = {2, {{0, 3, 0x1c}, {4, 4, 0x1c}}, {0x61a, 0xf, 0}};
        const uint32_t data[12] = {0, 2, 0, 0x403, 0x61a, 0xf, 0, 0, 0x1c, 0};
        const int used[12] = {0, 1, 0, 1, 1, 1, 0, 0, 1, 1};
        const uint32_t code[] = {0x67800072, 0x2f032343, 0x2f072343, 0x030803e5, 0xaf000000};
        struct sgx535_vertex_pds p;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_OK);
        pds_equal(&p, data, used, 12, code, 5);
    }
    /* ff at f84001c0: three elements, stride 44, 9 temporaries and phase 1. */
    {
        const struct sgx535_vertex_pds_layout in = {
            3, {{0, 4, 0x2c}, {4, 3, 0x2c}, {8, 4, 0x2c}}, {0xa1a, 0x48000017, 0}};
        const uint32_t data[12] = {0, 3, 0, 0x402, 0, 0x803, 0xa1a, 0x48000017, 0x2c, 0};
        const int used[12] = {0, 1, 0, 1, 0, 1, 1, 1, 1, 1};
        const uint32_t code[] = {0x67800072, 0x2f032343, 0x2f072343,
                                 0x2f0b2343, 0x030c03e5, 0xaf000000};
        struct sgx535_vertex_pds p;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_OK);
        pds_equal(&p, data, used, 12, code, 6);
        CHECK(p.source_word[2] == 4);
    }
    /* Two strides and a constant element: one MUL per stride (temporaries 50,
     * 52), MOVS for the constant, DOUTU2 after the strides (ds1[2]). */
    {
        const struct sgx535_vertex_pds_layout in = {
            3, {{0, 3, 12}, {4, 4, 0}, {8, 2, 16}}, {0x11a, 9u << 27, 0}};
        struct sgx535_vertex_pds p;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_OK);
        CHECK(p.data_words == 12 && p.words[8] == 12 && p.words[9] == 16 && p.words[10] == 0);
        CHECK(p.words[1] == 2 && p.words[3] == 0x403 && p.words[5] == 0x801);
        CHECK(p.words[12] == 0x67800072 && p.words[13] == (0x67800040u | 2u << 9 | 52u));
        CHECK(p.words[14] == 0x2f032343 && p.words[15] == 0x07070223);
        CHECK(p.words[16] == (0x2f000343u | 2u << 18 | 26u << 13));
        /* DOUTU2 at ds1[2]: pair 1, low dword (swizzle 2). */
        CHECK(p.words[17] == (0x03000005u | 3u << 18 | 1u << 13 | 0x1au << 5));
        CHECK(p.words[18] == 0xaf000000 && p.word_count == 19);
    }
    /* Eight elements: DS0 runs into its second row (data words 16..). */
    {
        struct sgx535_vertex_pds_layout in = {8, {{0}}, {1, 2, 3}};
        struct sgx535_vertex_pds p;
        for (uint32_t e = 0; e < 8; e++)
            in.element[e] = (struct sgx535_vertex_pds_element){4u * e, 4, 64};
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_OK);
        CHECK(p.source_word[4] == 16 && p.words[17] == (16u << 8 | 3u));
        CHECK(p.words[32] == 1 && p.words[33] == 2 && p.words[8] == 64 && p.words[9] == 3);
        CHECK(p.data_words == 36);
    }
    /* Refusals leave the output alone. */
    {
        struct sgx535_vertex_pds_layout in = {1, {{0, 4, 16}}, {0}};
        struct sgx535_vertex_pds p, before;
        memset(&p, 0x5a, sizeof(p));
        before = p;
        in.element[0].primary = 2;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_RANGE);
        in.element[0] = (struct sgx535_vertex_pds_element){0, 5, 16};
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_RANGE);
        in.element[0] = (struct sgx535_vertex_pds_element){0, 4, 18};
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_RANGE);
        in.element[0] = (struct sgx535_vertex_pds_element){0, 4, 0x10000};
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_RANGE);
        in.element_count = 0;
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_INVALID);
        in.element_count = 5;
        for (uint32_t e = 0; e < 5; e++)
            in.element[e] = (struct sgx535_vertex_pds_element){4u * e, 4, 4u + 4u * e};
        CHECK(sgx535_vertex_pds_build(&in, &p) == SGX535_VERTEX_UNSUPPORTED);
        CHECK(!memcmp(&p, &before, sizeof(p)));
    }
}

static void test_secondary(void)
{
    const uint32_t task[3] = {0x1a, 0, 0};
    const uint32_t code[] = {0x07030223, 0x07040345, 0xaf000000};
    struct sgx535_vertex_secondary p;
    /* tri, f8400300: 24 dwords as 2 lines of 12. */
    CHECK(sgx535_vertex_secondary_build(24, task, &p) == SGX535_VERTEX_OK);
    CHECK(p.data_words == 12 && p.word_count == 15 && p.transfer_count == 1);
    CHECK(p.words[1] == 0x0160001b && p.words[2] == 0x1a && p.words[3] == 0 && p.words[8] == 0);
    CHECK(!memcmp(p.words + 12, code, sizeof(code)) && p.source_word[0] == 0);
    /* ff, f8400420: 96 dwords as 6 lines of 16. */
    CHECK(sgx535_vertex_secondary_build(96, task, &p) == SGX535_VERTEX_OK);
    CHECK(p.words[1] == 0x01e0005f && !memcmp(p.words + 12, code, sizeof(code)));
    /* One line; and a prime count as 16 x 7 plus a line of 15. */
    CHECK(sgx535_vertex_secondary_build(7, task, &p) == SGX535_VERTEX_OK && p.words[1] == 6);
    CHECK(sgx535_vertex_secondary_build(127, task, &p) == SGX535_VERTEX_OK);
    CHECK(p.transfer_count == 2 && p.words[1] == (15u << 21 | 6u << 4 | 15u));
    CHECK(p.words[3] == (112u << 8 | 14u) && p.source_offset[1] == 448 && p.source_word[1] == 2);
    CHECK(p.words[4] == 0x1a && p.words[12] == 0x07030223 && p.words[13] == 0x07070223);
    CHECK(p.words[14] == 0x07080345 && p.words[15] == 0xaf000000 && p.word_count == 16);
    for (uint32_t n = 1; n <= SGX535_VERTEX_CONSTANTS; n++) {
        uint32_t total = 0;
        CHECK(sgx535_vertex_secondary_build(n, task, &p) == SGX535_VERTEX_OK);
        for (uint32_t t = 0; t < p.transfer_count; t++) {
            uint32_t d = p.words[2u * t + 1u], columns = (d & 15u) + 1u;
            uint32_t lines = (d >> 4 & 15u) + 1u;
            CHECK((d >> 8 & 0x7ffu) == total && (lines == 1u || (d >> 21 & 15u) + 1u == columns));
            CHECK(p.source_offset[t] == total * 4u);
            total += lines * columns;
        }
        CHECK(total == n);
    }
    CHECK(sgx535_vertex_secondary_build(0, task, &p) == SGX535_VERTEX_INVALID);
    CHECK(sgx535_vertex_secondary_build(SGX535_VERTEX_CONSTANTS + 1u, task, &p) ==
          SGX535_VERTEX_INVALID);
}

/* ---- an independent model of the emitted words ---- */

/* Stand-ins (no libm): distinct per operation, the same on both sides. */
static float absolute(float v)
{
    return v < 0.0f ? -v : v;
}
static float minimum(float a, float b)
{
    return b < a ? b : a;
}
static float maximum(float a, float b)
{
    return b > a ? b : a;
}
static float unary(unsigned f, float a)
{
    return f == 0 ? 1.0f / a : f == 1 ? a * 0.5f + 3.0f : f == 2 ? a - 1.0f : a * 2.0f;
}

struct machine {
    float r[128], o[128], pa[128], sa[128];
};
static float *bank(struct machine *m, unsigned b)
{
    return b == 0 ? m->r : b == 1 ? m->o : b == 2 ? m->pa : m->sa;
}
static float modify(float v, uint32_t high, unsigned negate_bit)
{
    if (high >> (negate_bit + 1u) & 1u)
        v = absolute(v);
    return high >> negate_bit & 1u ? -v : v;
}
static void run(const struct sgx535_vertex_code *c, struct machine *m)
{
    for (uint32_t i = 0;; i++) {
        uint32_t lo = c->words[2u * i], hi = c->words[2u * i + 1u];
        unsigned dst = lo >> 21 & 127u, op = hi >> 27;
        float *d = hi & 1u ? m->o : m->r;
        float a = modify(bank(m, lo >> 30)[lo >> 7 & 127u], hi, 5);
        float b = modify(bank(m, lo >> 28 & 3u)[lo & 127u], hi, 3);
        CHECK(i < c->instruction_count && !(hi & 2u) && !(hi >> 19 & 1u));
        if (lo == 0xa0200000u && hi == 0xfb275000u) {
            CHECK(i + 1u == c->instruction_count);
            return;
        }
        CHECK(!(hi >> 18 & 1u) && !(hi >> 12 & 15u)); /* no end, no repeat */
        if (op == 8) { /* unpckf32u8.scale: a byte of the source dword / 255 */
            uint32_t bits;
            memcpy(&bits, &bank(m, lo >> 30)[lo >> 7 & 127u], 4);
            CHECK((hi & ~1u) == 0x40a101bcu && (lo & 0x301c007fu) == 0x20040000u);
            d[dst] = (float)(bits >> 8 * (lo >> 16 & 3u) & 255u) / 255.0f;
        } else if (op == 5) { /* mov */
            CHECK((hi & ~1u) == 0x28a10000u && (lo >> 28 & 3u) == 2u && !(lo >> 14 & 127u));
            d[dst] = bank(m, lo >> 30)[lo >> 7 & 127u];
        } else if (op == 0) { /* fmad: src0 a temporary or PA */
            float s0 = modify((hi & 4u ? m->pa : m->r)[lo >> 14 & 127u], hi, 7);
            volatile float product = s0 * a;
            CHECK((hi & 0x07fff600u) == 0x00a00000u);
            d[dst] = product + b;
        } else if (op == 3) { /* fmin/fmax */
            CHECK((hi & 0x07fffc04u) == 0x00a00000u && !(lo >> 14 & 127u));
            d[dst] = hi & 0x200u ? maximum(a, b) : minimum(a, b);
        } else if (op == 1) { /* frcp/frsq/flog/fexp */
            unsigned f = hi >> 9 & 3u;
            CHECK((hi & 0x07fff804u) == 0x00a00000u && !(lo & 0x301fc07fu));
            d[dst] = unary(f, a);
        } else
            CHECK(!"opcode");
    }
}
static float source(const struct sgx535_vertex_source *s, const float *value,
                    const struct machine *m)
{
    float v = s->kind == SGX535_VERTEX_VALUE       ? value[s->index]
              : s->kind == SGX535_VERTEX_ATTRIBUTE ? m->pa[s->index]
                                                   : m->sa[s->index];
    if (s->absolute)
        v = absolute(v);
    return s->negate ? -v : v;
}
static void evaluate(const struct sgx535_vertex_instruction *in, uint32_t count, struct machine *m)
{
    static float value[SGX535_VERTEX_MAX_INSTRUCTIONS];
    for (uint32_t i = 0; i < count; i++) {
        float a = source(&in[i].source[0], value, m), b = source(&in[i].source[1], value, m);
        float c = source(&in[i].source[2], value, m), v;
        switch (in[i].opcode) {
        case SGX535_VERTEX_MOV: v = a; break;
        case SGX535_VERTEX_MAD: {
            volatile float product = a * b;
            v = product + c;
            break;
        }
        case SGX535_VERTEX_MIN: v = minimum(a, b); break;
        case SGX535_VERTEX_MAX: v = maximum(a, b); break;
        default:
            if (in[i].opcode >= SGX535_VERTEX_UNORM8) {
                uint32_t bits;
                memcpy(&bits, &m->pa[in[i].source[0].index], 4);
                v = (float)(bits >> 8u * (in[i].opcode - SGX535_VERTEX_UNORM8) & 255u) / 255.0f;
            } else
                v = unary(in[i].opcode - SGX535_VERTEX_RCP, a);
            break;
        }
        if (in[i].output >= 0)
            m->o[in[i].output] = v;
        else
            value[i] = v;
    }
}

static uint32_t rng_state = 0x535121u;
static uint32_t rnd(uint32_t n)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) % n;
}
static struct sgx535_vertex_source random_source(uint32_t defined, const int *values, bool modifiers)
{
    struct sgx535_vertex_source s = {0};
    uint32_t kind = rnd(defined ? 4 : 2);
    if (kind >= 2) {
        /* One of the last 32 values: at most 33 live, under the 48 temporaries. */
        uint32_t window = defined < 32u ? defined : 32u;
        s.kind = SGX535_VERTEX_VALUE;
        s.index = (uint16_t)values[defined - 1u - rnd(window)];
    } else {
        s.kind = kind ? SGX535_VERTEX_CONSTANT : SGX535_VERTEX_ATTRIBUTE;
        s.index = (uint16_t)rnd(kind ? SGX535_VERTEX_CONSTANTS : SGX535_VERTEX_ATTRIBUTES);
    }
    if (modifiers) {
        s.negate = (uint8_t)rnd(2);
        s.absolute = (uint8_t)(rnd(4) == 0);
    }
    return s;
}

static void test_programs(void)
{
    static struct sgx535_vertex_instruction in[SGX535_VERTEX_MAX_INSTRUCTIONS];
    static struct sgx535_vertex_code code;
    static int values[SGX535_VERTEX_MAX_INSTRUCTIONS];
    for (unsigned trial = 0; trial < 4000; trial++) {
        uint32_t outputs = 1u + rnd(SGX535_VERTEX_OUTPUTS), count = 0, defined = 0, next = 0;
        uint32_t body = rnd(trial < 3000 ? 40 : 200);
        struct machine m, model;
        bool written[SGX535_VERTEX_OUTPUTS] = {0};
        for (uint32_t i = 0; i < body + outputs; i++) {
            struct sgx535_vertex_instruction *x = &in[count];
            bool out = i >= body;
            memset(x, 0, sizeof(*x));
            x->opcode = (uint8_t)rnd(8);
            for (unsigned k = 0; k < 3; k++)
                if (k < (x->opcode == SGX535_VERTEX_MAD   ? 3u
                         : x->opcode <= SGX535_VERTEX_MOV ? 1u
                         : x->opcode <= SGX535_VERTEX_MAX ? 2u
                                                          : 1u))
                    x->source[k] = random_source(defined, values, x->opcode != SGX535_VERTEX_MOV);
            if (out) {
                while (written[next])
                    next++;
                written[next] = true;
                x->output = (int8_t)next;
            } else {
                x->output = -1;
                values[defined++] = (int)count;
            }
            count++;
        }
        CHECK(sgx535_vertex_encode(in, count, outputs, &code) == SGX535_VERTEX_OK);
        CHECK(code.temps <= SGX535_VERTEX_TEMPS);
        memset(&m, 0, sizeof(m));
        for (unsigned r = 0; r < 128; r++) {
            m.pa[r] = (float)((int)rnd(2001) - 1000) / 256.0f + 0.5f;
            m.sa[r] = (float)((int)rnd(2001) - 1000) / 64.0f + 0.25f;
            m.r[r] = m.o[r] = -12345.0f;
        }
        model = m;
        evaluate(in, count, &model);
        run(&code, &m);
        for (uint32_t o = 0; o < outputs; o++)
            CHECK(!memcmp(&m.o[o], &model.o[o], 4) || (m.o[o] != m.o[o] && model.o[o] != model.o[o]));
    }
}

static void test_words(void)
{
    struct sgx535_vertex_code c;
    /* o0 = pa0 * sa1 + sa2: a direct fmad, then the emit (emitvtx.end.freep). */
    struct sgx535_vertex_instruction mad = {
        SGX535_VERTEX_MAD,
        0,
        {{SGX535_VERTEX_ATTRIBUTE, 0, 0, 0}, {SGX535_VERTEX_CONSTANT, 0, 0, 1},
         {SGX535_VERTEX_CONSTANT, 0, 0, 2}}};
    struct sgx535_vertex_instruction swap = mad, move[2] = {{0}};
    uint32_t task[3];
    CHECK(sgx535_vertex_encode(&mad, 1, 1, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 2 && c.temps == 0);
    CHECK(c.words[0] == (0u << 21 | 0u << 14 | 1u << 7 | 3u << 30 | 2u | 3u << 28));
    CHECK(c.words[1] == 0x00a00005u && c.words[2] == 0xa0200000u && c.words[3] == 0xfb275000u);
    /* An SA first factor swaps with a PA second one. */
    swap.source[0] = mad.source[1];
    swap.source[1] = mad.source[0];
    CHECK(sgx535_vertex_encode(&swap, 1, 1, &c) == SGX535_VERTEX_OK && c.words[1] == 0x00a00005u);
    /* Two SA factors: a mov into a temporary first. */
    swap.source[1] = mad.source[1];
    CHECK(sgx535_vertex_encode(&swap, 1, 1, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 3 && c.temps == 1 && c.words[1] == 0x28a10000u);
    CHECK(c.words[0] == (0x20000000u | 1u << 7 | 3u << 30));
    /* mov o0, sa5: EMGD's mov o8, sa23 is the same instruction (decoded alike). */
    move[0] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MOV, 0, {{SGX535_VERTEX_CONSTANT, 0, 0, 5}}};
    CHECK(sgx535_vertex_encode(move, 1, 1, &c) == SGX535_VERTEX_OK);
    CHECK(c.words[0] == (0x20000000u | 5u << 7 | 3u << 30) && c.words[1] == 0x28a10001u);
    /* Refusals: an output twice, a missing output, a forward value, a value
     * read from an output instruction, MOV modifiers; OUT unchanged. */
    {
        struct sgx535_vertex_code before;
        memset(&c, 0x5a, sizeof(c));
        before = c;
        move[1] = move[0];
        CHECK(sgx535_vertex_encode(move, 2, 1, &c) == SGX535_VERTEX_INVALID);
        CHECK(sgx535_vertex_encode(move, 1, 2, &c) == SGX535_VERTEX_INVALID);
        move[0].source[0] = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, 1};
        move[0].output = -1;
        CHECK(sgx535_vertex_encode(move, 2, 1, &c) == SGX535_VERTEX_INVALID);
        move[0] = (struct sgx535_vertex_instruction){SGX535_VERTEX_MOV, 0, {{SGX535_VERTEX_CONSTANT, 0, 0, 5}}};
        move[1].source[0] = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, 0};
        CHECK(sgx535_vertex_encode(move, 2, 2, &c) == SGX535_VERTEX_INVALID);
        move[0].source[0].negate = 1;
        CHECK(sgx535_vertex_encode(move, 1, 1, &c) == SGX535_VERTEX_INVALID);
        move[0].source[0] = (struct sgx535_vertex_source){SGX535_VERTEX_CONSTANT, 0, 0, 128};
        CHECK(sgx535_vertex_encode(move, 1, 1, &c) == SGX535_VERTEX_RANGE);
        CHECK(!memcmp(&c, &before, sizeof(c)));
    }
    /* More live values than temporaries. */
    {
        static struct sgx535_vertex_instruction in[2u * SGX535_VERTEX_TEMPS + 4u];
        uint32_t n = 0, previous = 0;
        for (uint32_t i = 0; i <= SGX535_VERTEX_TEMPS; i++)
            in[n++] = (struct sgx535_vertex_instruction){
                SGX535_VERTEX_MAX, -1,
                {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)i}, {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
        for (uint32_t i = 1; i <= SGX535_VERTEX_TEMPS; i++, n++) {
            in[n] = (struct sgx535_vertex_instruction){
                SGX535_VERTEX_MAD, -1,
                {{SGX535_VERTEX_VALUE, 0, 0, (uint16_t)i},
                 {SGX535_VERTEX_CONSTANT, 0, 0, 1},
                 {SGX535_VERTEX_VALUE, 0, 0, (uint16_t)previous}}};
            previous = n;
        }
        in[n++] = (struct sgx535_vertex_instruction){
            SGX535_VERTEX_MOV, 0, {{SGX535_VERTEX_VALUE, 0, 0, (uint16_t)previous}}};
        CHECK(sgx535_vertex_encode(in, n, 1, &c) == SGX535_VERTEX_RANGE);
        /* An empty program. */
        CHECK(sgx535_vertex_encode(in + 1, 0, 1, &c) == SGX535_VERTEX_INVALID);
    }
    sgx535_vertex_task(0x11a, 37, task);
    CHECK(task[0] == 0x0020011a && task[1] == (37u << 27) && task[2] == 1);
}

/* The link step: element defaults, output placement, the colour clamp, dead
 * code. gl_Position = m * p (m in sa3..sa18); colour = c * k (sa19..22). */
/* COLOR0 (gl_varying_slot 1) as the backend's TC0, all four components. */
static const struct sgx535_vertex_tc color_tc = {1, 4};
static enum sgx535_vertex_status link(const struct sgx535_vertex_nir_program *p,
                                      const uint8_t *layout, bool color,
                                      struct sgx535_vertex_code *c)
{
    return sgx535_vertex_nir_link(p, layout, &color_tc, color ? 1u : 0u, c);
}
/* P's output K at LOCATION, written and clamped as given; its sources kept. */
static struct sgx535_vertex_nir_output *output(struct sgx535_vertex_nir_program *p, uint32_t k,
                                               uint16_t location, uint8_t written, bool clamp)
{
    p->output[k].location = location;
    p->output[k].written = written;
    p->output[k].clamp = clamp;
    if (p->output_count <= k)
        p->output_count = k + 1u;
    return &p->output[k];
}

static void test_link(void)
{
    static struct sgx535_vertex_nir_program p;
    static struct sgx535_vertex_code c;
    const uint8_t full[SGX535_VERTEX_INPUTS] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    uint8_t short_position[SGX535_VERTEX_INPUTS];
    struct machine m, model;
    memset(&p, 0, sizeof(p));
    p.zero = 1;
    p.one = 2;
    p.constant_count = 23;
    for (uint32_t r = 0; r < 4; r++) {
        uint32_t base = p.count;
        for (uint32_t k = 0; k < 4; k++)
            p.instruction[p.count++] = (struct sgx535_vertex_instruction){
                SGX535_VERTEX_MAD, -1,
                {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)k},
                 {SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)(3u + 4u * k + r)},
                 k ? (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, (uint16_t)(base + k - 1u)}
                   : (struct sgx535_vertex_source){SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
        p.position[r] = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, (uint16_t)(base + 3u)};
    }
    output(&p, 0, 1, 15, true);
    for (uint32_t r = 0; r < 4; r++) {
        p.instruction[p.count] = (struct sgx535_vertex_instruction){
            SGX535_VERTEX_MAD, -1,
            {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4u + r)},
             {SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)(19u + r)},
             {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
        p.output[0].source[r] =
            (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, (uint16_t)p.count++};
    }
    p.has_position = true;
    /* Without colour: 16 MADs (the last of each row writes o0..o3), the emit. */
    CHECK(link(&p, full, false, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 17);
    for (uint32_t pass = 0; pass < 3; pass++) {
        memset(&m, 0, sizeof(m));
        for (unsigned r = 0; r < 128; r++) {
            m.pa[r] = (float)(r % 7) - 2.5f;
            m.sa[r] = (float)(r % 5) * 0.75f - 1.0f;
        }
        m.sa[1] = 0.0f;
        m.sa[2] = 1.0f;
        memcpy(short_position, full, sizeof(full));
        if (pass == 1)
            short_position[0] = 3; /* p.w reads 1.0 */
        if (pass == 2)
            short_position[0] = 2; /* p.z reads 0.0, p.w 1.0 */
        CHECK(link(&p, short_position, true, &c) == SGX535_VERTEX_OK);
        /* A defaulted p.w or p.z is an SA factor next to an SA one: a move first. */
        CHECK(c.instruction_count == 16 + 4 + 8 + 1 + 4 * pass);
        model = m;
        if (pass >= 1)
            model.pa[3] = 1.0f;
        if (pass == 2)
            model.pa[2] = 0.0f;
        run(&c, &m);
        for (uint32_t r = 0; r < 4; r++) {
            float v = 0.0f, k;
            for (uint32_t i = 0; i < 4; i++) {
                volatile float product = model.pa[i] * model.sa[3u + 4u * i + r];
                v = product + v;
            }
            CHECK(!memcmp(&m.o[r], &v, 4));
            {
                volatile float product = model.pa[4u + r] * model.sa[19u + r];
                k = product + 0.0f;
            }
            k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
            CHECK(!memcmp(&m.o[4u + r], &k, 4));
        }
    }
    /* A shared position value is moved; a negated one goes through MAX. */
    p.position[1] = p.position[0];
    p.position[2] = p.position[0];
    p.position[2].negate = 1;
    CHECK(link(&p, full, false, &c) == SGX535_VERTEX_OK);
    /* rows 1 and 2 are dead now: 8 MADs, two moves, a MAX, the emit */
    CHECK(c.instruction_count == 8 + 3 + 1);
    /* COLOR0 partly written, or not at all */
    p.output[0].written = 7;
    CHECK(link(&p, full, true, &c) == SGX535_VERTEX_UNSUPPORTED);
    p.output[0].location = 2;
    p.output[0].written = 15;
    CHECK(link(&p, full, true, &c) == SGX535_VERTEX_UNSUPPORTED);
    /* fadd(-fmul(pa0, sa5), fmul(pa1, sa6)) as NIR gives it: the first product
     * fuses into the add (negated), the second stays (the add reads it as its
     * addend). */
    memset(&p, 0, sizeof(p));
    p.zero = 1;
    p.one = 2;
    p.constant_count = 7;
    p.instruction[0] = (struct sgx535_vertex_instruction){
        SGX535_VERTEX_MAD, -1,
        {{SGX535_VERTEX_ATTRIBUTE, 0, 0, 0}, {SGX535_VERTEX_CONSTANT, 0, 0, 5},
         {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
    p.instruction[1] = (struct sgx535_vertex_instruction){
        SGX535_VERTEX_MAD, -1,
        {{SGX535_VERTEX_ATTRIBUTE, 0, 0, 1}, {SGX535_VERTEX_CONSTANT, 0, 0, 6},
         {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
    p.instruction[2] = (struct sgx535_vertex_instruction){
        SGX535_VERTEX_MAD, -1,
        {{SGX535_VERTEX_VALUE, 1, 0, 0}, {SGX535_VERTEX_CONSTANT, 0, 0, 2},
         {SGX535_VERTEX_VALUE, 0, 0, 1}}};
    p.count = 3;
    for (unsigned r = 0; r < 4; r++)
        p.position[r] = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, 2};
    p.has_position = true;
    CHECK(link(&p, full, false, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 2 + 4 + 1);
    memset(&m, 0, sizeof(m));
    m.pa[0] = 1.5f;
    m.pa[1] = -2.25f;
    m.sa[1] = 0.0f;
    m.sa[2] = 1.0f;
    m.sa[5] = 3.0f;
    m.sa[6] = 0.5f;
    run(&c, &m);
    CHECK(m.o[0] == -1.5f * 3.0f + -2.25f * 0.5f && m.o[3] == m.o[0]);
    /* -fmul(pa0, sa5) read twice: the add fuses the other product instead. */
    p.position[3] = (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, 0};
    CHECK(link(&p, full, false, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 2 + 4 + 1);
    run(&c, &m);
    CHECK(m.o[0] == -1.5f * 3.0f + -2.25f * 0.5f && m.o[3] == 1.5f * 3.0f);
    /* A packed UNORM8 colour (BGRA bytes in pa4): position = pa0..pa3 as
     * they are, colour = unpacked bytes x sa3, clamped. */
    memset(&p, 0, sizeof(p));
    p.zero = 1;
    p.one = 2;
    p.constant_count = 4;
    p.input_count = 2;
    p.input_components[0] = p.input_components[1] = 15;
    for (unsigned r = 0; r < 4; r++) {
        p.instruction[r] = (struct sgx535_vertex_instruction){
            SGX535_VERTEX_MAD, -1,
            {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4u + r)}, {SGX535_VERTEX_CONSTANT, 0, 0, 3},
             {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
        output(&p, 0, 1, 15, true)->source[r] =
            (struct sgx535_vertex_source){SGX535_VERTEX_VALUE, 0, 0, (uint16_t)r};
        p.position[r] = (struct sgx535_vertex_source){SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)r};
    }
    p.count = 4;
    p.has_position = true;
    {
        uint8_t layout[SGX535_VERTEX_INPUTS];
        const uint32_t bytes = 0x80ff4010u; /* B 0x10, G 0x40, R 0xff, A 0x80 */
        memcpy(layout, full, sizeof(layout));
        layout[1] = 4 | SGX535_VERTEX_PACKED | SGX535_VERTEX_BGRA;
        CHECK(link(&p, layout, true, &c) == SGX535_VERTEX_OK);
        CHECK(c.instruction_count == 4 + 4 + 4 + 8 + 1); /* unpacks, MADs, moves, clamps */
        memset(&m, 0, sizeof(m));
        memcpy(&m.pa[4], &bytes, 4);
        m.sa[1] = 0.0f;
        m.sa[2] = 1.0f;
        m.sa[3] = 2.0f;
        m.pa[0] = 0.5f;
        run(&c, &m);
        CHECK(m.o[0] == 0.5f && m.o[4] == 1.0f && m.o[6] == 2.0f * (16.0f / 255.0f));
        CHECK(m.o[5] == 2.0f * (64.0f / 255.0f) && m.o[7] == 1.0f);
        layout[1] = SGX535_VERTEX_BGRA | 4;
        CHECK(link(&p, layout, true, &c) == SGX535_VERTEX_INVALID);
    }
}

/* Texture coordinates in the fragment stage's order, of the sizes it reads:
 * COLOR0 clamped, the others as written; one of 1 to 4 components; ten of
 * four fill o43. */
static void test_link_varyings(void)
{
    static struct sgx535_vertex_nir_program p;
    static struct sgx535_vertex_code c;
    const uint8_t full[SGX535_VERTEX_INPUTS] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
    const struct sgx535_vertex_tc both[2] = {{1, 4}, {32, 2}}, swapped[2] = {{32, 2}, {1, 4}};
    struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS + 1u];
    struct machine m;
    memset(&p, 0, sizeof(p));
    p.zero = 1;
    p.one = 2;
    p.constant_count = 13;
    p.has_position = true;
    for (uint16_t r = 0; r < 4; r++) {
        p.position[r] = (struct sgx535_vertex_source){SGX535_VERTEX_ATTRIBUTE, 0, 0, r};
        output(&p, 0, 32, 3, false)->source[r] =
            (struct sgx535_vertex_source){SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4u + r)};
    }
    for (uint16_t r = 0; r < 4; r++) {
        output(&p, 1, 1, 15, true)->source[r] =
            (struct sgx535_vertex_source){SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(8u + r)};
        output(&p, 2, 4, 15, false)->source[r] =
            (struct sgx535_vertex_source){SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(12u + r)};
    }
    memset(&m, 0, sizeof(m));
    for (unsigned r = 0; r < 16; r++)
        m.pa[r] = (float)r * 0.75f - 3.0f;
    m.pa[4] = -2.0f; /* VAR0 out of [0, 1], unclamped */
    m.pa[5] = 3.5f;
    m.pa[8] = 1.5f;
    m.pa[9] = -0.5f;
    m.pa[10] = 0.25f;
    m.sa[1] = 0.0f;
    m.sa[2] = 1.0f;
    /* four position moves, COLOR0's MAX and MIN per component, two moves */
    CHECK(sgx535_vertex_nir_link(&p, full, both, 2, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 4 + 8 + 2 + 1);
    run(&c, &m);
    CHECK(m.o[0] == m.pa[0] && m.o[3] == m.pa[3]);
    CHECK(m.o[4] == 1.0f && m.o[5] == 0.0f && m.o[6] == 0.25f && m.o[7] == 1.0f);
    CHECK(m.o[8] == -2.0f && m.o[9] == 3.5f);
    memset(m.o, 0, sizeof(m.o));
    CHECK(sgx535_vertex_nir_link(&p, full, swapped, 2, &c) == SGX535_VERTEX_OK);
    run(&c, &m);
    CHECK(m.o[4] == m.pa[4] && m.o[5] == m.pa[5] && m.o[6] == 1.0f && m.o[9] == 1.0f);
    /* a component not written, a location not written, sizes and counts out
     * of range */
    tc[0] = (struct sgx535_vertex_tc){32, 3};
    CHECK(sgx535_vertex_nir_link(&p, full, tc, 1, &c) == SGX535_VERTEX_UNSUPPORTED);
    tc[0] = (struct sgx535_vertex_tc){33, 1};
    CHECK(sgx535_vertex_nir_link(&p, full, tc, 1, &c) == SGX535_VERTEX_UNSUPPORTED);
    tc[0] = (struct sgx535_vertex_tc){4, 0};
    CHECK(sgx535_vertex_nir_link(&p, full, tc, 1, &c) == SGX535_VERTEX_INVALID);
    tc[0] = (struct sgx535_vertex_tc){4, 5};
    CHECK(sgx535_vertex_nir_link(&p, full, tc, 1, &c) == SGX535_VERTEX_INVALID);
    CHECK(sgx535_vertex_nir_link(&p, full, NULL, 1, &c) == SGX535_VERTEX_INVALID);
    for (unsigned t = 0; t <= SGX535_VERTEX_TCS; t++)
        tc[t] = (struct sgx535_vertex_tc){4, 4};
    CHECK(sgx535_vertex_nir_link(&p, full, tc, SGX535_VERTEX_TCS + 1u, &c) ==
          SGX535_VERTEX_INVALID);
    /* ten outputs of four constants: o4 to o43 */
    p.output_count = 0;
    for (uint32_t k = 0; k < SGX535_VERTEX_TCS; k++) {
        struct sgx535_vertex_nir_output *o = output(&p, k, (uint16_t)(32u + k), 15, false);
        for (unsigned r = 0; r < 4; r++)
            o->source[r] =
                (struct sgx535_vertex_source){SGX535_VERTEX_CONSTANT, 0, 0, (uint16_t)(3u + k)};
        tc[SGX535_VERTEX_TCS - 1u - k] = (struct sgx535_vertex_tc){(uint16_t)(32u + k), 4};
        m.sa[3u + k] = (float)k + 0.5f;
    }
    CHECK(sgx535_vertex_nir_link(&p, full, tc, SGX535_VERTEX_TCS, &c) == SGX535_VERTEX_OK);
    CHECK(c.instruction_count == 4 + 40 + 1 && SGX535_VERTEX_OUTPUTS == 44);
    memset(m.o, 0, sizeof(m.o));
    run(&c, &m);
    for (uint32_t k = 0; k < SGX535_VERTEX_TCS; k++)
        CHECK(m.o[4u + 4u * k] == m.sa[3u + SGX535_VERTEX_TCS - 1u - k] &&
              m.o[7u + 4u * k] == m.o[4u + 4u * k]);
}

int main(void)
{
    test_vertex_pds();
    test_secondary();
    test_words();
    test_programs();
    test_link();
    test_link_varyings();
    printf("sgx535 vertex: %u checks\n", checks);
    return 0;
}
