// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#define main old_normalized_main
#include "test_fragment_normalized.c"
#undef main
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_generic_f32.h"
#include <stdbool.h>

static unsigned raw_checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        raw_checks++;                                                                              \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void sign_stream(struct sgx535_fragment_instruction ir[14])
{
    unsigned i;
    memset(ir, 0, 14u * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
    }
    ir[4] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                 .immediate = 0x80000000u};
    ir[5] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_RAW_XOR,
                                                 .source = {0, 4, 0}};
    ir[6] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                 .immediate = 0x7fffffffu};
    ir[7] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_RAW_AND,
                                                 .source = {1, 6, 0}};
    ir[8] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_RAW_AND,
                                                 .source = {2, 6, 0}};
    ir[9] =
        (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MOV, .source = {3, 0, 0}};
    for (i = 0; i < 4; i++) {
        static const unsigned source[4] = {5, 7, 8, 9};
        ir[10u + i].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[10u + i].index = i;
        ir[10u + i].source[0] = source[i];
    }
}

/* Independent literal instruction/register schedule: no production encoder
 * or decoder builds this table. The final four writes target distinct bytes
 * of OUTPUT0 and preserve the other accumulated lanes. */
static const uint32_t sign_words[48] = {
    0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u, 0xa0400000u, 0x28a10000u, 0xa0600180u,
    0x28a10000u, 0x00800000u, 0xfca20000u, 0x00a00004u, 0x58a00000u, 0x001fffffu, 0xfca1f1f0u,
    0x00800080u, 0x50a00000u, 0x00200100u, 0x50a00000u, 0x20000180u, 0x28a10000u, 0x00400000u,
    0xfca00000u, 0x00600000u, 0xfca0f1c0u, 0x00c00282u, 0x18a00200u, 0x00a00303u, 0x18a00000u,
    0x00c00202u, 0x18a00200u, 0x00800303u, 0x18a00000u, 0x00c00082u, 0x18a00200u, 0x00200303u,
    0x18a00000u, 0x00c00002u, 0x18a00200u, 0x00000303u, 0x18a00000u, 0x20040280u, 0x40a10c11u,
    0x20040200u, 0x40a10c09u, 0x20040080u, 0x40a10c05u, 0x20040000u, 0x40a50c21u};

/* Unit-only raw prefix and finite reference replay. It interprets no general
 * F32 arithmetic: only sign bits, the explicit clamp-to0/1 pattern, and the
 * independent exact rational byte reference above. Neither those MIN/MAX
 * choices nor numeric SCALE are asserted as measured SGX behavior. */
static int finite_replay(const struct sgx535_nir_program *program, const uint32_t primary[4],
                         uint32_t *packed, uint32_t sign_value[4])
{
    uint32_t reg[124] = {0}, valid[124] = {0}, value = 0xa5a5a5a5u;
    unsigned i, written = 0, clamp = 0;
    for (i = 0; i < program->word_count; i += 2) {
        uint32_t lo = program->words[i], hi = program->words[i + 1];
        unsigned dst = (lo >> 21) & 127u, a = (lo >> 7) & 127u, b = lo & 127u;
        CHECK((i + 2 == program->word_count) == !!(hi & 0x40000u));
        if ((hi & 0xfff80000u) == 0xfca00000u) {
            CHECK(dst < 124 && !(hi & 3u));
            reg[dst] = (lo & 0x1fffffu) | (((hi >> 4) & 31u) << 21) | (((hi >> 12) & 63u) << 26);
            valid[dst] = 1;
        } else if (hi == 0x28a10000u) {
            unsigned bank = lo >> 30;
            CHECK(dst < 124);
            if (bank == 2) {
                CHECK(a < 4);
                reg[dst] = primary[a];
            } else {
                CHECK(bank == 0 && a < 124 && valid[a]);
                reg[dst] = reg[a];
            }
            valid[dst] = 1;
        } else if (hi == 0x50a00000u || hi == 0x58a00000u) {
            CHECK(dst < 124 && a < 124 && b < 124 && valid[a] && valid[b]);
            CHECK(lo == (dst << 21 | a << 7 | b));
            reg[dst] = hi == 0x50a00000u ? reg[a] & reg[b] : reg[a] ^ reg[b];
            valid[dst] = 1;
        } else if (hi == 0x18a00200u || hi == 0x18a00000u) {
            CHECK(dst < 124 && a < 124 && b < 124 && valid[a] && valid[b]);
            CHECK(lo == (dst << 21 | a << 7 | b));
            if (hi == 0x18a00200u) {
                CHECK(clamp < 4 && reg[b] == 0);
                sign_value[clamp++] = reg[a];
                if ((reg[a] & 0x7f800000u) == 0x7f800000u)
                    return -1;
                reg[dst] = reg[a] & 0x80000000u ? 0 : reg[a];
            } else {
                CHECK(reg[b] == 0x3f800000u);
                reg[dst] = reg[a] > reg[b] ? reg[b] : reg[a];
            }
            valid[dst] = 1;
        } else {
            unsigned mask = (hi >> 2) & 15u, lane;
            uint8_t byte;
            CHECK((hi & 0xfffbffc3u) == 0x40a10c01u);
            CHECK(lo == (0x20040000u | a << 7) && a < 124 && valid[a]);
            CHECK(mask && !(mask & (mask - 1u)) && !(written & mask));
            for (lane = 0; lane < 4; lane++)
                if (mask == (1u << lane))
                    break;
            CHECK(lane < 4 && reference(reg[a], &byte) == 0);
            value = (value & ~(0xffu << (lane * 8u))) | (uint32_t)byte << (lane * 8u);
            written |= mask;
        }
    }
    CHECK(written == 15 && clamp == 4);
    *packed = value;
    return 0;
}

static void raw_refused(const struct sgx535_fragment_instruction *ir, size_t count,
                        const struct sgx535_fragment_linkage *binding,
                        struct sgx535_fragment_normalized_candidate *out,
                        enum sgx535_nir_status expected)
{
    struct sgx535_fragment_normalized_candidate *before = malloc(sizeof(*before));
    CHECK(before != NULL);
    *before = *out;
    CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, count, binding,
                                                                           out) == expected);
    CHECK(!memcmp(out, before, sizeof(*out)));
    free(before);
    no_allocations();
}

static void fractional_finite_sources(struct sgx535_fragment_normalized_candidate *p,
                                      const struct sgx535_fragment_linkage *b)
{
    static const uint32_t finite[] = {0,          0x80000000, 0x3e800000, 0xbe800000, 0xbfa00000,
                                      0xbf800000, 0x4b000000, 1,          0x80000001, 0x7f7fffff};
    struct sgx535_fragment_instruction ir[10] = {0}, saved[10];
    for (unsigned value = 0; value <= sizeof(finite) / sizeof(finite[0]); value++) {
        bool constant = value < sizeof(finite) / sizeof(finite[0]);
        unsigned total = constant ? 10 : 9, node = total - 5;
        memset(ir, 0, sizeof(ir));
        for (unsigned i = 0; i < 4; i++)
            ir[i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_INPUT,
                                                         .component = i};
        if (constant)
            ir[4] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                         .immediate = finite[value]};
        ir[node] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_FRACT,
                                                        .source = {constant ? 4 : 3}};
        for (unsigned i = 0; i < 4; i++)
            ir[node + 1 + i] = (struct sgx535_fragment_instruction){
                .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {node}};
        memcpy(saved, ir, sizeof(ir));
        for (unsigned generic = 0; generic < 2; generic++) {
            enum sgx535_nir_status ret =
                generic ? sgx535_fragment_compile_generic_f32_normalized_candidate(ir, total, b, p)
                        : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                              ir, total, b, p);
            CHECK(ret == SGX535_NIR_OK);
            CHECK(!memcmp(saved, ir, sizeof(ir)) && p->source_instruction_count == total);
            unsigned frc = 0;
            uint32_t loaded[124] = {0};
            bool literal[124] = {0};
            for (unsigned word = 0; word < p->encoding.word_count; word += 2) {
                uint32_t lo = p->encoding.words[word], hi = p->encoding.words[word + 1];
                unsigned dst = (lo >> 21) & 127u, source = (lo >> 7) & 127u;
                if ((hi & 0xfff80000u) == 0xfca00000u) {
                    CHECK(dst < 124);
                    loaded[dst] =
                        (lo & 0x1fffffu) | ((hi & 0x1f0u) << 17) | (((hi >> 12) & 63u) << 26);
                    literal[dst] = true;
                } else if (hi == 0x28a10000u) {
                    CHECK(dst < 124);
                    literal[dst] = false;
                } else if (hi == 0x00a00600u) {
                    CHECK(dst < 124 && source < 124 && (lo & 127u) == source);
                    CHECK(constant ? literal[source] && loaded[source] == finite[value]
                                   : !literal[source]);
                    frc++;
                }
            }
            CHECK(frc == 1);
            unqualified(p);
            no_allocations();
        }
    }
}

static void fractional_mask_uses(struct sgx535_fragment_normalized_candidate *p,
                                 const struct sgx535_fragment_linkage *b)
{
    struct sgx535_fragment_instruction saved[8],
        ir[8] = {
            {.opcode = SGX535_FRAGMENT_INPUT},
            {.opcode = SGX535_FRAGMENT_CONSTANT, .immediate = 0x7fffffffu},
            {.opcode = SGX535_FRAGMENT_RAW_AND, .source = {0, 1}},
            {.opcode = SGX535_FRAGMENT_FRACT, .source = {1}},
        };
    struct sgx535_fragment_normalized_candidate *before = malloc(sizeof(*before));
    CHECK(before != NULL);
    for (unsigned live = 0; live < 2; live++) {
        for (unsigned i = 0; i < 4; i++)
            ir[4 + i] = (struct sgx535_fragment_instruction){
                .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {live ? 3 : 2}};
        memcpy(saved, ir, sizeof(ir));
        for (unsigned generic = 0; generic < 2; generic++) {
            memset(p, 0xa5, sizeof(*p));
            *before = *p;
            enum sgx535_nir_status ret =
                generic
                    ? sgx535_fragment_compile_generic_f32_normalized_candidate(ir, 8, b, p)
                    : sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 8, b, p);
            if (ret != SGX535_NIR_ERR_UNSUPPORTED)
                fprintf(stderr, "shared abs-mask/FRACT generic=%u live=%u: actual=%d expected=%d\n",
                        generic, live, ret, SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(ret == SGX535_NIR_ERR_UNSUPPORTED);
            CHECK(!memcmp(saved, ir, sizeof(ir)));
            CHECK(!memcmp(p, before, sizeof(*p)));
            no_allocations();
        }
    }
    free(before);
}

int main(void)
{
    struct sgx535_fragment_instruction *ir = malloc(SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    struct sgx535_fragment_instruction saved[14];
    struct sgx535_fragment_linkage b = binding(), bad;
    struct sgx535_fragment_normalized_candidate *p = malloc(sizeof(*p));
    struct sgx535_fragment_normalized_candidate *old = malloc(sizeof(*old));
    static const uint32_t word[] = {0,           0x80000000u, 1,           0x80000001u, 0x007fffffu,
                                    0x807fffffu, 0x3b008080u, 0x3b008081u, 0x3effffffu, 0x3f000000u,
                                    0x3f000001u, 0x3f7fffffu, 0x3f800000u, 0x3f800001u, 0xbf000000u,
                                    0xbf800000u, 0x7f7fffffu, 0xff7fffffu};
    unsigned i, j;
    CHECK(ir && p && old);
    CHECK(old_normalized_main() == 0);
    fractional_mask_uses(p, &b);
    fractional_finite_sources(p, &b);
    sign_stream(ir);
    memcpy(saved, ir, sizeof(saved));
    CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 14, &b, p) ==
          SGX535_NIR_OK);
    CHECK(!memcmp(saved, ir, sizeof(saved)) && p->source_instruction_count == 14 &&
          p->lowered_instruction_count == 24);
    CHECK(p->encoding.word_count == 48 && p->encoding.temp_register_count == 7 &&
          p->encoding.peak_live_temps == 7);
    CHECK(!memcmp(p->encoding.words, sign_words, sizeof(sign_words)));
    unqualified(p);
    for (i = 48; i < SGX535_NIR_MAX_WORDS; i++)
        CHECK(p->encoding.words[i] == 0);
    CHECK(p->primary_register_count == 4 && p->logical_components == 4 &&
          !p->secondary_register_count);
    CHECK(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 14, &b, old) ==
          SGX535_NIR_ERR_UNSUPPORTED);
    for (i = 0; i < sizeof(word) / sizeof(word[0]); i++) {
        uint32_t input[4], actual[4] = {0}, expected[4], packed = 0, oracle = 0;
        for (j = 0; j < 4; j++)
            input[j] = word[(i + j) % (sizeof(word) / sizeof(word[0]))];
        expected[0] = input[2] ^ 0x80000000u;
        expected[1] = input[1] & 0x7fffffffu;
        expected[2] = input[0] & 0x7fffffffu;
        expected[3] = input[3];
        for (j = 0; j < 4; j++) {
            static const unsigned lane[4] = {2, 1, 0, 3};
            uint8_t byte;
            CHECK(reference(expected[j], &byte) == 0);
            oracle |= (uint32_t)byte << (lane[j] * 8u);
        }
        CHECK(finite_replay(&p->encoding, input, &packed, actual) == 0);
        CHECK(packed == oracle && !memcmp(actual, expected, sizeof(actual)));
    }
    /* The prefix exactly preserves/toggles Inf/NaN payload bits. The finite
     * normalized reference deliberately provides no nonfinite byte result. */
    for (i = 0; i < 8; i++) {
        static const uint32_t special[8] = {0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc12345u,
                                            0x7f812345u, 0xff812345u, 0x7fffffffu, 0xffffffffu};
        uint32_t input[4] = {0, 0, special[i], 0}, actual[4] = {0}, packed = 0x12345678u;
        CHECK(finite_replay(&p->encoding, input, &packed, actual) == -1);
        CHECK(actual[0] == (special[i] ^ 0x80000000u) && packed == 0x12345678u);
        input[2] = 0;
        input[1] = special[i];
        CHECK(finite_replay(&p->encoding, input, &packed, actual) == -1);
        CHECK(actual[0] == 0x80000000u && actual[1] == (special[i] & 0x7fffffffu));
        CHECK(packed == 0x12345678u);
    }
    for (i = 0; i < 13; i++) {
        sign_stream(ir);
        switch (i) {
        case 0:
            ir[6].immediate = 0x7fc12345u;
            break;
        case 1:
            ir[6].immediate = 0x7f800000u;
            break;
        case 2:
            ir[4].immediate = 0;
            break;
        case 3:
            ir[7].source[0] = 6;
            break;
        case 4:
            ir[9].source[0] = 6;
            break;
        case 5:
            ir[10].source[0] = 6;
            break;
        case 6:
            ir[7].opcode = SGX535_FRAGMENT_MAX;
            break;
        case 7:
            ir[7].source[1] = 1;
            break;
        case 8:
            ir[7].source[0] = 7;
            break;
        case 9:
            ir[8].source[2] = 1;
            break;
        case 10:
            ir[9].opcode = (enum sgx535_fragment_opcode)99;
            break;
        case 11:
            ir[8].source[1] = 4;
            break;
        default:
            ir[7].opcode = SGX535_FRAGMENT_MOV;
            ir[7].source[1] = 0;
            ir[8].opcode = SGX535_FRAGMENT_MOV;
            ir[8].source[1] = 0;
            break;
        }
        raw_refused(ir, 14, &b, p, i == 8 ? SGX535_NIR_ERR_SSA : SGX535_NIR_ERR_UNSUPPORTED);
    }
    /* A mask at node0 must not gain a fictitious use from unused zero slots. */
    passthrough(ir);
    ir[0].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[0].immediate = 0x7fffffffu;
    ir[4].source[0] = 1;
    raw_refused(ir, 8, &b, p, SGX535_NIR_ERR_UNSUPPORTED);
    for (i = 0; i < 3; i++) {
        sign_stream(ir);
        fail_after = (int)i;
        raw_refused(ir, 14, &b, p, SGX535_NIR_ERR_NOMEM);
        fail_after = -1;
    }
    sign_stream(ir);
    bad = b;
    bad.varying[0].driver_base = 1;
    raw_refused(ir, 14, &bad, p, SGX535_NIR_ERR_UNSUPPORTED);
    raw_refused(NULL, 14, &b, p, SGX535_NIR_ERR_ARGUMENT);
    raw_refused(ir, 14, NULL, p, SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 14, &b, NULL) ==
          SGX535_NIR_ERR_ARGUMENT);
    raw_refused(ir, 503, &b, p, SGX535_NIR_ERR_LIMIT);
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[1].immediate = 0x7fffffffu;
    ir[2].opcode = SGX535_FRAGMENT_RAW_AND;
    ir[2].source[1] = 1;
    for (i = 3; i < 498; i++) {
        ir[i].opcode = SGX535_FRAGMENT_MOV;
        ir[i].source[0] = i - 1u;
    }
    for (i = 0; i < 4; i++) {
        ir[498u + i].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[498u + i].index = i;
        ir[498u + i].source[0] = 497;
    }
    CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 502, &b, p) ==
          SGX535_NIR_OK);
    CHECK(p->encoding.word_count == 1024 && p->lowered_instruction_count == 512);
    unqualified(p);
    {
        union alias {
            struct sgx535_fragment_instruction ir[14];
            struct sgx535_fragment_linkage b;
            struct sgx535_fragment_normalized_candidate out;
        };
        union alias *a = malloc(sizeof(*a));
        CHECK(a != NULL);
        sign_stream(a->ir);
        CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                  a->ir, 14, &b, &a->out) == SGX535_NIR_OK);
        CHECK(!memcmp(a->out.encoding.words, sign_words, sizeof(sign_words)));
        a->b = b;
        sign_stream(ir);
        CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(
                  ir, 14, &a->b, &a->out) == SGX535_NIR_OK);
        CHECK(!memcmp(a->out.encoding.words, sign_words, sizeof(sign_words)));
        free(a);
    }
    /* Every old no-raw program retains identical complete result bytes. */
    for (i = 0; i < 18; i++) {
        passthrough(ir);
        ir[3].opcode = SGX535_FRAGMENT_CONSTANT;
        ir[3].component = 0;
        ir[3].immediate = word[i];
        CHECK(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 8, &b, old) ==
              SGX535_NIR_OK);
        CHECK(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 8, &b, p) ==
              SGX535_NIR_OK);
        CHECK(!memcmp(old, p, sizeof(*p)));
        unqualified(p);
    }
    no_allocations();
    free(old);
    free(p);
    free(ir);
    printf("raw-sign normalized complete program: %u checks PASS (CPU reference only)\n",
           raw_checks);
    return 0;
}
