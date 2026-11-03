// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment_normalized.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_fragment_raw_sign_normalized.h"
#include "sgx535_fragment_generic_f32.h"
#include "sgx535_pack.h"
#include "sgx535_usse.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_calloc(size_t, size_t);
void __real_free(void *);
static int fail_after = -1;
static void *allocated[8];
void *__wrap_calloc(size_t n, size_t bytes);
void __wrap_free(void *p);
void *__wrap_calloc(size_t n, size_t bytes)
{
    void *p;
    unsigned i;
    if (fail_after == 0)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    p = __real_calloc(n, bytes);
    if (!p)
        return p;
    for (i = 0; i < 8; i++)
        if (!allocated[i]) {
            allocated[i] = p;
            return p;
        }
    assert(0);
    return NULL;
}
void __wrap_free(void *p)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        if (allocated[i] == p)
            allocated[i] = NULL;
    __real_free(p);
}
static void no_allocations(void)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        assert(!allocated[i]);
}

/* Independent finite-only reference, with no encoder calls, host FPU, shader
 * evaluation or claim about hardware SCALE. Decode the exact binary32 rational,
 * clamp, multiply by255 and round to nearest integer with even ties. */
static int reference(uint32_t word, uint8_t *out)
{
    unsigned exponent = (word >> 23) & 255u, shift;
    uint64_t numerator, quotient, remainder, midpoint;
    uint32_t magnitude = word & 0x7fffffffu;
    if (exponent == 255)
        return -1;
    if (word & 0x80000000u) {
        *out = 0;
        return 0;
    }
    if (magnitude >= 0x3f800000u) {
        *out = 255;
        return 0;
    }
    numerator = (uint64_t)((word & 0x7fffffu) | (exponent ? 0x800000u : 0u)) * 255u;
    shift = exponent ? 150u - exponent : 149u;
    if (shift >= 33) {
        *out = 0;
        return 0;
    }
    quotient = numerator >> shift;
    remainder = numerator & ((UINT64_C(1) << shift) - 1u);
    midpoint = UINT64_C(1) << (shift - 1u);
    if (remainder > midpoint || (remainder == midpoint && (quotient & 1u)))
        quotient++;
    assert(quotient <= 255);
    *out = (uint8_t)quotient;
    return 0;
}
static void reference_boundaries(void)
{
    static const struct {
        uint32_t word;
        int expected;
    } fixtures[] = {{0x3b008080u, 0},
                    {0x3b008081u, 1},
                    {0x3b008082u, 1},
                    {0x3bc0c0c0u, 1},
                    {0x3bc0c0c1u, 2},
                    {0x3bc0c0c2u, 2},
                    {0x3c20a0a0u, 2},
                    {0x3c20a0a1u, 3},
                    {0x3c20a0a2u, 3},
                    {0x3e7efefeu, 63},
                    {0x3e7efeffu, 64},
                    {0x3e7eff00u, 64},
                    {0x3effffffu, 127},
                    {0x3f000000u, 128},
                    {0x3f000001u, 128},
                    {0x3f010100u, 128},
                    {0x3f010101u, 128},
                    {0x3f010102u, 129},
                    {0x3f7e7e7du, 253},
                    {0x3f7e7e7eu, 253},
                    {0x3f7e7e7fu, 254},
                    {0x3f7f7f7eu, 254},
                    {0x3f7f7f7fu, 254},
                    {0x3f7f7f80u, 255},
                    {0, 0},
                    {0x80000000u, 0},
                    {1, 0},
                    {0x007fffffu, 0},
                    {0x00800000u, 0},
                    {0xbf800000u, 0},
                    {0xff7fffffu, 0},
                    {0x3f7fffffu, 255},
                    {0x3f800000u, 255},
                    {0x3f800001u, 255},
                    {0x7f7fffffu, 255},
                    {0x7f800000u, -1},
                    {0xff800000u, -1},
                    {0x7fc00000u, -1},
                    {0x7f800001u, -1}};
    unsigned i;
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        uint8_t byte = 19;
        int status = reference(fixtures[i].word, &byte);
        if (fixtures[i].expected < 0)
            assert(status == -1 && byte == 19);
        else
            assert(status == 0 && byte == fixtures[i].expected);
    }
    {
        const uint32_t rgba[4] = {0x3f800000u, 0x3f000000u, 0xbf800000u, 0x40000000u};
        const unsigned lane[4] = {2, 1, 0, 3};
        uint32_t argb = 0;
        for (i = 0; i < 4; i++) {
            uint8_t byte;
            assert(!reference(rgba[i], &byte));
            argb |= (uint32_t)byte << (lane[i] * 8u);
        }
        assert(argb == 0xffff8000u);
    }
}
static struct sgx535_fragment_linkage binding(void)
{
    struct sgx535_fragment_linkage b = {0};
    b.count = 1;
    b.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    return b;
}
static void passthrough(struct sgx535_fragment_instruction *p)
{
    unsigned i;
    memset(p, 0, 8u * sizeof(*p));
    for (i = 0; i < 4; i++) {
        p[i].opcode = SGX535_FRAGMENT_INPUT;
        p[i].component = i;
        p[i + 4u].opcode = SGX535_FRAGMENT_OUTPUT;
        p[i + 4u].index = i;
        p[i + 4u].source[0] = i;
    }
}
static void unqualified(const struct sgx535_fragment_normalized_candidate *p)
{
    assert(p->missing_contracts == 0x3ffu);
    assert(!p->arithmetic_qualified && !p->numerical_qualified && !p->linkage_qualified);
    assert(!p->output_qualified && !p->hardware_qualified && !p->nonfinite_behavior_qualified);
    assert(p->reference_policy == SGX535_NORMALIZED_FINITE_CLAMP_RTE);
    assert(sgx535_fragment_normalized_native_admission(p) == SGX535_NIR_ERR_UNSUPPORTED);
}
static void refused(const struct sgx535_fragment_instruction *ir, size_t n,
                    const struct sgx535_fragment_linkage *b,
                    struct sgx535_fragment_normalized_candidate *out,
                    enum sgx535_nir_status expected)
{
    struct sgx535_fragment_normalized_candidate *saved = malloc(sizeof(*saved));
    assert(saved);
    *saved = *out;
    assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, n, b, out) == expected);
    assert(!memcmp(saved, out, sizeof(*saved)));
    free(saved);
    no_allocations();
}

static void uniform_passthrough(struct sgx535_fragment_instruction *ir)
{
    passthrough(ir);
    for (unsigned i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_UNIFORM;
        ir[i].index = i;
        ir[i].component = 0;
    }
}

static void no_input_refused(const struct sgx535_fragment_instruction *ir, size_t count,
                             struct sgx535_fragment_normalized_candidate *out,
                             enum sgx535_nir_status expected)
{
    struct sgx535_fragment_normalized_candidate *saved = malloc(sizeof(*saved));
    enum sgx535_nir_status status;
    assert(saved);
    *saved = *out;
    status = sgx535_fragment_compile_no_input_normalized_candidate(ir, count, out);
    if (status != expected)
        fprintf(stderr, "no-input refusal count=%zu fail_after=%d status=%d expected=%d\n", count,
                fail_after, status, expected);
    assert(status == expected);
    assert(!memcmp(saved, out, sizeof(*saved)));
    free(saved);
    no_allocations();
}

static void no_input_cases(void)
{
    struct sgx535_fragment_instruction *ir = malloc(SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    struct sgx535_fragment_normalized_candidate *p = malloc(sizeof(*p));
    struct sgx535_fragment_program *raw = malloc(sizeof(*raw));
    const struct sgx535_fragment_linkage color0 = binding();
    static const uint32_t expected[36] = {
        0xe0000000u, 0x28a10000u, 0xe0200080u, 0x28a10000u, 0xe0400100u, 0x28a10000u,
        0xe0600180u, 0x28a10000u, 0x00800000u, 0xfca00000u, 0x00a00000u, 0xfca0f1c0u,
        0x00c00004u, 0x18a00200u, 0x00000305u, 0x18a00000u, 0x00c00084u, 0x18a00200u,
        0x00200305u, 0x18a00000u, 0x00c00104u, 0x18a00200u, 0x00400305u, 0x18a00000u,
        0x00c00184u, 0x18a00200u, 0x00600305u, 0x18a00000u, 0x20040000u, 0x40a10c11u,
        0x20040080u, 0x40a10c09u, 0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
    assert(ir && p && raw);
    /* This is the scalar form of a vec4 uniform -> complete color output.
     * Literal words independently fix PA0/SA4 and every clamp/PACK control. */
    uniform_passthrough(ir);
    struct sgx535_fragment_instruction original[8];
    memcpy(original, ir, sizeof(original));
    memset(p, 0xa5, sizeof(*p));
    assert(sgx535_fragment_compile_no_input_normalized_candidate(ir, 8, p) == SGX535_NIR_OK);
    assert(!memcmp(ir, original, sizeof(original)));
    assert(!p->logical_components && !p->primary_register_count &&
           p->secondary_register_count == 4);
    assert(p->uniform_words_used[0] == 15 && !p->uniform_words_used[1] &&
           !p->uniform_words_used[2] && !p->uniform_words_used[3]);
    assert(p->source_instruction_count == 8 && p->lowered_instruction_count == 18);
    assert(p->encoding.word_count == 36 && p->encoding.temp_register_count == 7 &&
           p->encoding.peak_live_temps == 7);
    assert(!memcmp(p->encoding.words, expected, sizeof(expected)));
    for (unsigned i = 0; i < 4; i++) {
        struct sgx535_mov move;
        struct sgx535_pack_f32_u8_channel pack;
        const unsigned lane[4] = {2, 1, 0, 3};
        assert(!p->primary_component[i] && p->output_byte_lane[i] == lane[i]);
        assert(sgx535_mov_decode(p->encoding.words + i * 2u, &move) == SGX535_MOV_OK);
        assert(move.source.bank == SGX535_MOV_SECONDARY && move.source.number == i && !move.end);
        assert(move.destination.bank == SGX535_MOV_TEMP && move.destination.number == i);
        assert(sgx535_pack_f32_u8_channel_decode(p->encoding.words + 28u + i * 2u, &pack) ==
               SGX535_PACK_OK);
        assert(pack.destination.bank == SGX535_MOV_OUTPUT && !pack.destination.number &&
               pack.source.bank == SGX535_MOV_TEMP && pack.source.number == i &&
               pack.byte_index == lane[i] && pack.scale == 1 && pack.end == (i == 3));
    }
    for (unsigned i = 36; i < SGX535_NIR_MAX_WORDS; i++)
        assert(!p->encoding.words[i]);
    unqualified(p);
    no_allocations();
    /* The COLOR0 entry still needs its original INPUT; this entry never
     * supplies a dummy attribute to satisfy that distinct contract. */
    refused(ir, 8, &color0, p, SGX535_NIR_ERR_UNSUPPORTED);

    /* A literal and arbitrary existing arithmetic keep their source words;
     * one high uniform reference costs an extent of128, not one word. */
    memset(ir, 0, 7u * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[0].index = 127;
    ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[1].immediate = 0x3f000000u;
    ir[2].opcode = SGX535_FRAGMENT_MAD;
    ir[2].source[0] = ir[2].source[2] = 0;
    ir[2].source[1] = 1;
    for (unsigned i = 0; i < 4; i++) {
        ir[i + 3u].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 3u].index = i;
        ir[i + 3u].source[0] = 2;
    }
    assert(sgx535_fragment_compile(ir, 7, SGX535_FRAGMENT_RAW_F32_WORDS, raw) == SGX535_NIR_OK);
    assert(sgx535_fragment_compile_no_input_normalized_candidate(ir, 7, p) == SGX535_NIR_OK);
    assert(!memcmp(raw->encoding.words, p->encoding.words, 6u * sizeof(uint32_t)));
    assert(!p->primary_register_count && p->secondary_register_count == 128 &&
           p->uniform_words_used[3] == 0x80000000u);
    for (unsigned i = 0; i < 3; i++)
        assert(!p->uniform_words_used[i]);
    unqualified(p);
    no_allocations();

    /* No uniform is required: the grammar also accepts a finite literal
     * without consuming SECONDARY space for either original or tail LIMM. */
    memset(ir, 0, 5u * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[0].immediate = 0x80000000u;
    for (unsigned i = 0; i < 4; i++) {
        ir[i + 1u].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 1u].index = i;
    }
    assert(sgx535_fragment_compile_no_input_normalized_candidate(ir, 5, p) == SGX535_NIR_OK);
    assert(!p->primary_register_count && !p->secondary_register_count);
    for (unsigned i = 0; i < 4; i++)
        assert(!p->uniform_words_used[i]);
    unqualified(p);
    no_allocations();
    for (unsigned i = 0; i < 4; i++) {
        static const uint32_t special[4] = {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u};
        ir[0].immediate = special[i];
        no_input_refused(ir, 5, p, SGX535_NIR_ERR_UNSUPPORTED);
    }

    uniform_passthrough(ir);
    memmove(ir + 1, ir, 8u * sizeof(*ir));
    ir[0] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_INPUT};
    for (unsigned i = 5; i < 9; i++)
        ir[i].source[0]++;
    no_input_refused(ir, 9, p, SGX535_NIR_ERR_UNSUPPORTED);
    for (unsigned i = 0; i < 7; i++) {
        enum sgx535_nir_status status = SGX535_NIR_ERR_UNSUPPORTED;
        uniform_passthrough(ir);
        switch (i) {
        case 0:
            ir[0].index = 128;
            status = SGX535_NIR_ERR_LIMIT;
            break;
        case 1:
            ir[4].source[0] = 4;
            status = SGX535_NIR_ERR_SSA;
            break;
        case 2:
            ir[5].index = 0;
            break;
        case 3:
            ir[0].component = 1;
            break;
        case 4:
            ir[0].source[0] = 1;
            break;
        case 5:
            ir[0].opcode = (enum sgx535_fragment_opcode)99;
            break;
        default:
            ir[0].opcode = SGX535_FRAGMENT_RAW_AND;
            break;
        }
        no_input_refused(ir, 8, p, status);
    }
    uniform_passthrough(ir);
    for (unsigned i = 0; i < 3; i++) {
        fail_after = (int)i;
        no_input_refused(ir, 8, p, SGX535_NIR_ERR_NOMEM);
        fail_after = -1;
    }
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[0].index = 127;
    for (unsigned i = 1; i < 498; i++) {
        ir[i].opcode = SGX535_FRAGMENT_MOV;
        ir[i].source[0] = i - 1u;
    }
    for (unsigned i = 0; i < 4; i++) {
        ir[498u + i].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[498u + i].index = i;
        ir[498u + i].source[0] = 497;
    }
    assert(sgx535_fragment_compile_no_input_normalized_candidate(ir, 502, p) == SGX535_NIR_OK);
    assert(p->encoding.word_count == 1024 && p->lowered_instruction_count == 512 &&
           !p->primary_register_count && p->secondary_register_count == 128 &&
           p->uniform_words_used[3] == 0x80000000u);
    unqualified(p);
    no_allocations();
    no_input_refused(ir, 4, p, SGX535_NIR_ERR_LIMIT);
    no_input_refused(ir, 503, p, SGX535_NIR_ERR_LIMIT);
    no_input_refused(NULL, 8, p, SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_fragment_compile_no_input_normalized_candidate(ir, 8, NULL) ==
           SGX535_NIR_ERR_ARGUMENT);
    {
        union alias {
            struct sgx535_fragment_instruction ir[8];
            struct sgx535_fragment_normalized_candidate out;
        } *u = malloc(sizeof(*u));
        assert(u);
        uniform_passthrough(u->ir);
        assert(sgx535_fragment_compile_no_input_normalized_candidate(u->ir, 8, &u->out) ==
               SGX535_NIR_OK);
        assert(!memcmp(u->out.encoding.words, expected, sizeof(expected)));
        unqualified(&u->out);
        no_allocations();
        free(u);
    }
    puts(
        "PASS no-input normalized candidate: literal PA0/SA4 controls, source prefix and uniform extent, complete refusals and atomic output; native BLOCKED");
    free(raw);
    free(p);
    free(ir);
}

static void raw_shift_cases(void)
{
    static const uint32_t counts[] = {1, 2, 4, 8, 16, 31};
    struct sgx535_fragment_instruction ir[8] = {0}, saved[8];
    struct sgx535_fragment_linkage b = binding();
    struct sgx535_fragment_normalized_candidate *p = malloc(sizeof(*p)),
                                                *before = malloc(sizeof(*p));
    assert(p && before);
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[1].immediate = UINT32_MAX;
    ir[2].opcode = SGX535_FRAGMENT_RAW_USHR_IMM;
    ir[2].source[0] = 1;
    ir[3].opcode = SGX535_FRAGMENT_RAW_AND;
    ir[3].source[1] = 2;
    for (unsigned c = 0; c < 4; c++) {
        ir[4 + c].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[4 + c].index = c;
        ir[4 + c].source[0] = 3;
    }
    for (unsigned k = 0; k < sizeof(counts) / sizeof(*counts); k++) {
        ir[2].immediate = counts[k];
        memcpy(saved, ir, sizeof(ir));
        assert(sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(ir, 8, &b, p) ==
               SGX535_NIR_OK);
        no_allocations();
        assert(!memcmp(saved, ir, sizeof(ir)));
        assert(p->source_instruction_count == 8 && p->lowered_instruction_count == 18 &&
               p->encoding.word_count == 36);
        /* The nonfinite literal's only real edge is unary integer SHR.
         * Its source must be classified; unused source fields are not edges. */
        assert(p->encoding.words[5] == UINT32_C(0x68a10000));
        assert((p->encoding.words[4] & ~UINT32_C(0x0fe03f80)) ==
               (UINT32_C(0x20000000) | counts[k]));
        unqualified(p);
        memset(p, 0x6a, sizeof(*p));
        *before = *p;
        assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 8, &b, p) ==
               SGX535_NIR_ERR_UNSUPPORTED);
        assert(!memcmp(p, before, sizeof(*p)));
        assert(sgx535_fragment_compile_dri_color0_raw_sign_normalized_candidate(ir, 8, &b, p) ==
               SGX535_NIR_ERR_UNSUPPORTED);
        assert(!memcmp(p, before, sizeof(*p)));
        no_allocations();
    }
    for (unsigned field = 0; field < 10; field++) {
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        memcpy(ir, saved, sizeof(ir));
        if (field == 0)
            ir[2].immediate = 0;
        else if (field == 1)
            ir[2].immediate = 3;
        else if (field == 2)
            ir[2].index = 1;
        else if (field == 3)
            ir[2].component = 1;
        else if (field == 4)
            ir[2].source[1] = 1;
        else if (field == 5)
            ir[2].source[2] = 1;
        else if (field == 6) {
            ir[2].source[0] = 2;
            expected = SGX535_NIR_ERR_SSA;
        } else if (field == 7)
            ir[2].source[0] = 0; /* The nonfinite literal is now unused. */
        else if (field == 8) {
            ir[3].opcode = SGX535_FRAGMENT_MOV;
            ir[3].source[0] = 1; /* A second, floating edge cannot hide. */
            ir[3].source[1] = 0;
        } else {
            /* The same literal's valid raw SHR edge cannot authorize a
             * separate floating OUTPUT edge, including a dead scalar use. */
            ir[7].source[0] = 1;
        }
        assert(sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(ir, 8, &b, p) ==
               expected);
        assert(!memcmp(p, before, sizeof(*p)));
        no_allocations();
    }
    memcpy(ir, saved, sizeof(ir));
    for (unsigned i = 0; i < 3; i++) {
        fail_after = (int)i;
        assert(sgx535_fragment_compile_dri_color0_raw_mask_normalized_candidate(ir, 8, &b, p) ==
               SGX535_NIR_ERR_NOMEM);
        fail_after = -1;
        assert(!memcmp(p, before, sizeof(*p)));
        no_allocations();
    }
    free(before);
    free(p);
    puts(
        "PASS raw-mask normalized SHR: real unary raw-word edges, old-profile refusals and atomic errors; native BLOCKED");
}

static void generic_secondary_source(struct sgx535_fragment_instruction *ir, uint32_t index)
{
    memset(ir, 0, 6 * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    ir[1].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[1].index = index;
    for (unsigned i = 0; i < 4; i++)
        ir[i + 2] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i % 2}};
}

static void generic_secondary_cases(void)
{
    struct sgx535_fragment_instruction ir[10], saved[6];
    struct sgx535_fragment_linkage link = binding(), bad;
    struct sgx535_fragment_generic_f32_secondary_candidate *p = malloc(sizeof(*p));
    struct sgx535_fragment_generic_f32_secondary_candidate *before = malloc(sizeof(*before));
    struct sgx535_fragment_normalized_candidate *logical = malloc(sizeof(*logical));
    assert(p && before && logical);
    /* Literal PA0 and SA(i+1) MOV fields are independent of the compiler's
     * own decoder. The complete old clamp/PACK tail must stay byte-identical. */
    for (uint32_t index = 0; index < 127; index++) {
        generic_secondary_source(ir, index);
        memcpy(saved, ir, sizeof(saved));
        assert(sgx535_fragment_compile_generic_f32_normalized_candidate(ir, 6, &link, logical) ==
               SGX535_NIR_OK);
        assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, p) ==
               SGX535_NIR_OK);
        assert(!memcmp(ir, saved, sizeof(saved)));
        assert(p->program.encoding.words[0] == 0xa0000000u &&
               p->program.encoding.words[1] == 0x28a10000u &&
               p->program.encoding.words[2] == (0xe0200000u | ((index + 1u) << 7)) &&
               p->program.encoding.words[3] == 0x28a10000u);
        assert(logical->encoding.words[2] == (0xe0200000u | (index << 7)));
        assert(p->program.encoding.word_count == logical->encoding.word_count);
        for (uint32_t word = 0; word < logical->encoding.word_count; word++)
            if (word != 2)
                assert(p->program.encoding.words[word] == logical->encoding.words[word]);
        for (uint32_t word = 0; word < 4; word++) {
            assert(p->logical_uniform_words_used[word] ==
                   (word == index / 32u ? UINT32_C(1) << (index % 32u) : 0u));
            assert(p->program.uniform_words_used[word] ==
                   (word == (index + 1u) / 32u ? UINT32_C(1) << ((index + 1u) % 32u) : 0u));
        }
        assert(p->logical_uniform_extent == index + 1 && p->uniform_sa_words == index + 1 &&
               p->reserved_sa_words == 1 && p->physical_sa_words == index + 2 &&
               p->program.secondary_register_count == index + 2 && !p->literal_sa_words &&
               !p->sampler_state_sa_words);
        assert(p->physical_sa_words == p->reserved_sa_words + p->uniform_sa_words +
                                           p->literal_sa_words + p->sampler_state_sa_words);
        assert(p->program.primary_register_count == 4 && p->program.primary_component[0] == 0);
        unqualified(&p->program);
        no_allocations();
    }
    /* Removing uniforms yields the exact existing generic-F32 result; no
     * reserved-only SECONDARY allocation is invented. Finite LIMM consumes0. */
    generic_secondary_source(ir, 0);
    ir[1] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                 .immediate = 0x3f000000u};
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, p) ==
           SGX535_NIR_OK);
    assert(sgx535_fragment_compile_generic_f32_normalized_candidate(ir, 6, &link, logical) ==
           SGX535_NIR_OK);
    assert(!memcmp(&p->program, logical, sizeof(*logical)) && !p->physical_sa_words &&
           !p->reserved_sa_words && !p->logical_uniform_extent && !p->literal_sa_words);

    /* Combined occupancy remains128 when finite literals fill the existing
     * instruction budget: those words are LIMM code, not hidden SA data. */
    struct sgx535_fragment_instruction *full = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*full));
    assert(full);
    generic_secondary_source(full, 126);
    for (uint32_t i = 2; i < 498; i++)
        full[i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                       .immediate = 0x3f000000u + i};
    for (uint32_t i = 0; i < 4; i++)
        full[498 + i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = i, .source = {i % 2}};
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(full, 502, &link, p) ==
           SGX535_NIR_OK);
    assert(p->logical_uniform_extent == 127 && p->physical_sa_words == 128 &&
           !p->literal_sa_words && !p->sampler_state_sa_words &&
           p->program.encoding.word_count == SGX535_NIR_MAX_WORDS);
    *before = *p;
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(full, 503, &link, p) ==
           SGX535_NIR_ERR_LIMIT);
    assert(!memcmp(p, before, sizeof(*p)));
    free(full);
    no_allocations();

    /* All logical declarations count, including an unused high load. One
     * scalar at127 cannot hide the combined129-DWORD physical footprint. */
    memset(p, 0xa5, sizeof(*p));
    *before = *p;
    generic_secondary_source(ir, 127);
    ir[3].source[0] = ir[5].source[0] = 0;
    assert(sgx535_fragment_compile_generic_f32_normalized_candidate(ir, 6, &link, logical) ==
               SGX535_NIR_OK &&
           logical->secondary_register_count == 128);
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, p) ==
               SGX535_NIR_ERR_LIMIT &&
           !memcmp(p, before, sizeof(*p)));
    for (unsigned change = 0; change < 9; change++) {
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        generic_secondary_source(ir, 0);
        bad = link;
        if (change == 0)
            ir[1].immediate = 1;
        else if (change == 1)
            ir[1].component = 1;
        else if (change == 2)
            ir[1].source[0] = 1;
        else if (change == 3) {
            ir[1].index = 128;
            expected = SGX535_NIR_ERR_LIMIT;
        } else if (change == 4) {
            ir[1].opcode = SGX535_FRAGMENT_SAMPLE2D_COMPONENT;
        } else if (change == 5) {
            ir[1] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                         .immediate = 0x7fc12345u};
        } else if (change == 6) {
            ir[5].source[0] = 5;
            expected = SGX535_NIR_ERR_SSA;
        } else if (change == 7)
            bad.varying[0].driver_base = 1;
        else
            bad.count = 0;
        assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &bad, p) == expected);
        assert(!memcmp(p, before, sizeof(*p)));
        no_allocations();
    }
    generic_secondary_source(ir, 3);
    for (unsigned failure = 0; failure < 3; failure++) {
        fail_after = (int)failure;
        assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, p) ==
               SGX535_NIR_ERR_NOMEM);
        fail_after = -1;
        assert(!memcmp(p, before, sizeof(*p)));
        no_allocations();
    }
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(NULL, 6, &link, p) ==
           SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, NULL, p) ==
           SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, NULL) ==
           SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 4, &link, p) ==
           SGX535_NIR_ERR_LIMIT);
    assert(!memcmp(p, before, sizeof(*p)));
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(ir, 6, &link, p) ==
           SGX535_NIR_OK);
    union secondary_alias {
        struct sgx535_fragment_generic_f32_secondary_candidate output;
        struct {
            struct sgx535_fragment_instruction ir[6];
            struct sgx535_fragment_linkage link;
        } source;
    } *alias = malloc(sizeof(*alias));
    assert(alias);
    memcpy(alias->source.ir, ir, sizeof(saved));
    alias->source.link = link;
    assert(sgx535_fragment_compile_generic_f32_secondary_candidate(
               alias->source.ir, 6, &alias->source.link, &alias->output) == SGX535_NIR_OK);
    assert(!memcmp(&alias->output, p, sizeof(*p)));
    free(alias);
    free(logical);
    free(before);
    free(p);
    no_allocations();
    puts(
        "PASS generic F32 ordinary SECONDARY: logical/physical ledger, reserved SA0, shared128 budget; native BLOCKED");
}

int main(void)
{
    struct sgx535_fragment_instruction *ir = malloc(SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    struct sgx535_fragment_instruction *copy = malloc(8u * sizeof(*copy));
    struct sgx535_fragment_normalized_candidate *p = malloc(sizeof(*p));
    struct sgx535_fragment_dri_color0_program *old = malloc(sizeof(*old));
    struct sgx535_fragment_linkage b = binding(), original_binding = b, bad_binding;
    static const uint32_t expected[36] = {
        0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u, 0xa0400000u, 0x28a10000u,
        0xa0600180u, 0x28a10000u, 0x00800000u, 0xfca00000u, 0x00a00000u, 0xfca0f1c0u,
        0x00c00004u, 0x18a00200u, 0x00000305u, 0x18a00000u, 0x00c00084u, 0x18a00200u,
        0x00200305u, 0x18a00000u, 0x00c00104u, 0x18a00200u, 0x00400305u, 0x18a00000u,
        0x00c00184u, 0x18a00200u, 0x00600305u, 0x18a00000u, 0x20040000u, 0x40a10c11u,
        0x20040080u, 0x40a10c09u, 0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
    unsigned i, j;
    assert(ir && copy && p && old);
    reference_boundaries();
    no_input_cases();
    raw_shift_cases();
    generic_secondary_cases();
    passthrough(ir);
    memcpy(copy, ir, 8u * sizeof(*copy));
    assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 8, &b, p) == SGX535_NIR_OK);
    no_allocations();
    assert(!memcmp(copy, ir, 8u * sizeof(*copy)) && !memcmp(&b, &original_binding, sizeof(b)));
    assert(p->source_instruction_count == 8 && p->lowered_instruction_count == 18);
    assert(p->encoding.word_count == 36 && p->encoding.temp_register_count == 7 &&
           p->encoding.peak_live_temps == 7);
    assert(!memcmp(p->encoding.words, expected, sizeof(expected)));
    unqualified(p);
    assert(p->primary_register_count == 4 && p->logical_components == 4 &&
           !p->secondary_register_count);
    for (i = 0; i < 4; i++) {
        const unsigned lane[4] = {2, 1, 0, 3};
        struct sgx535_pack_f32_u8_channel pack;
        assert(p->primary_component[i] == lane[i] && p->output_byte_lane[i] == lane[i]);
        assert(sgx535_pack_f32_u8_channel_decode(p->encoding.words + 28u + i * 2u, &pack) ==
               SGX535_PACK_OK);
        assert(pack.byte_index == lane[i] && pack.scale == 1 && pack.end == (i == 3));
        assert(pack.destination.bank == SGX535_MOV_OUTPUT && !pack.destination.number);
        assert(pack.source.bank == SGX535_MOV_TEMP && pack.source.number == i);
    }
    for (i = 36; i < SGX535_NIR_MAX_WORDS; i++)
        assert(!p->encoding.words[i]);
    assert(sgx535_fragment_compile_dri_color0(ir, 8, &b, SGX535_FRAGMENT_U8_WORD_PROJECTION, old) ==
           SGX535_NIR_OK);
    no_allocations();
    assert(old->encoding.word_count == 16);
    assert(sgx535_fragment_compile_dri_color0(ir, 8, &b, SGX535_FRAGMENT_NORMALIZED_U8, old) ==
           SGX535_NIR_ERR_UNSUPPORTED);
    no_allocations();
    /* Preserve an original arithmetic prefix and the exact uniform bitmap;
     * the candidate neither evaluates nor rewrites that source arithmetic. */
    memset(ir, 0, 10u * sizeof(*ir));
    for (i = 0; i < 4; i++) {
        ir[i].opcode = SGX535_FRAGMENT_INPUT;
        ir[i].component = i;
    }
    ir[4].opcode = SGX535_FRAGMENT_UNIFORM;
    ir[4].index = 127;
    ir[5].opcode = SGX535_FRAGMENT_MAD;
    ir[5].source[0] = 0;
    ir[5].source[1] = 4;
    ir[5].source[2] = 1;
    for (i = 0; i < 4; i++) {
        ir[i + 6u].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[i + 6u].index = i;
        ir[i + 6u].source[0] = i ? i : 5;
    }
    assert(sgx535_fragment_compile_dri_color0(ir, 10, &b, SGX535_FRAGMENT_RAW_F32_WORDS, old) ==
           SGX535_NIR_OK);
    assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 10, &b, p) == SGX535_NIR_OK);
    assert(!memcmp(p->encoding.words, old->encoding.words, 12u * sizeof(uint32_t)));
    assert(p->secondary_register_count == 128 && p->uniform_words_used[3] == 0x80000000u);
    for (i = 0; i < 3; i++)
        assert(!p->uniform_words_used[i]);
    no_allocations();
    unqualified(p);
    /* Invalid old OUTPUT sources must not be made valid by the inserted nodes. */
    passthrough(ir);
    ir[4].source[0] = 4;
    refused(ir, 8, &b, p, SGX535_NIR_ERR_SSA);
    for (i = 0; i < 7; i++) {
        passthrough(ir);
        switch (i) {
        case 0:
            ir[0].immediate = 1;
            break;
        case 1:
            ir[3].source[2] = 1;
            break;
        case 2:
            ir[5].index = 0;
            break;
        case 3:
            ir[0].opcode = (enum sgx535_fragment_opcode)99;
            break;
        case 4:
            ir[3].component = 4;
            break;
        case 5:
            ir[3].index = 1;
            break;
        default:
            ir[3].opcode = SGX535_FRAGMENT_OUTPUT;
            break;
        }
        refused(ir, 8, &b, p, i == 4 ? SGX535_NIR_ERR_LIMIT : SGX535_NIR_ERR_UNSUPPORTED);
    }
    for (i = 0; i < 4; i++) {
        static const uint32_t special[4] = {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u};
        passthrough(ir);
        ir[3].opcode = SGX535_FRAGMENT_CONSTANT;
        ir[3].component = 0;
        ir[3].immediate = special[i];
        refused(ir, 8, &b, p, SGX535_NIR_ERR_UNSUPPORTED);
    }
    for (i = 0; i < 3; i++) {
        passthrough(ir);
        fail_after = (int)i;
        refused(ir, 8, &b, p, SGX535_NIR_ERR_NOMEM);
        fail_after = -1;
    }
    passthrough(ir);
    bad_binding = b;
    bad_binding.varying[0].interpolation = SGX535_FRAGMENT_FLAT;
    refused(ir, 8, &bad_binding, p, SGX535_NIR_ERR_UNSUPPORTED);
    bad_binding = b;
    bad_binding.varying[1].components = 1;
    refused(ir, 8, &bad_binding, p, SGX535_NIR_ERR_UNSUPPORTED);
    memset(ir, 0, SGX535_NIR_MAX_INSTRUCTIONS * sizeof(*ir));
    ir[0].opcode = SGX535_FRAGMENT_INPUT;
    for (i = 1; i < 498; i++) {
        ir[i].opcode = SGX535_FRAGMENT_MOV;
        ir[i].source[0] = i - 1u;
    }
    for (i = 0; i < 4; i++) {
        ir[498u + i].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[498u + i].index = i;
        ir[498u + i].source[0] = 497;
    }
    assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 502, &b, p) ==
           SGX535_NIR_OK);
    assert(p->encoding.word_count == 1024 && p->lowered_instruction_count == 512);
    no_allocations();
    unqualified(p);
    refused(ir, 503, &b, p, SGX535_NIR_ERR_LIMIT);
    refused(ir, 4, &b, p, SGX535_NIR_ERR_LIMIT);
    refused(NULL, 8, &b, p, SGX535_NIR_ERR_ARGUMENT);
    refused(ir, 8, NULL, p, SGX535_NIR_ERR_ARGUMENT);
    assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 8, &b, NULL) ==
           SGX535_NIR_ERR_ARGUMENT);
    p->missing_contracts = 0;
    p->arithmetic_qualified = p->numerical_qualified = p->linkage_qualified = 1;
    p->output_qualified = p->hardware_qualified = p->nonfinite_behavior_qualified = 1;
    assert(sgx535_fragment_normalized_native_admission(p) == SGX535_NIR_ERR_UNSUPPORTED);
    assert(sgx535_fragment_normalized_native_admission(NULL) == SGX535_NIR_ERR_ARGUMENT);
    {
        union alias {
            struct sgx535_fragment_instruction ir[8];
            struct sgx535_fragment_linkage b;
            struct sgx535_fragment_normalized_candidate out;
        };
        union alias *u = malloc(sizeof(*u));
        assert(u);
        passthrough(u->ir);
        assert(sgx535_fragment_compile_dri_color0_normalized_candidate(u->ir, 8, &b, &u->out) ==
               SGX535_NIR_OK);
        assert(!memcmp(u->out.encoding.words, expected, sizeof(expected)));
        no_allocations();
        u->b = b;
        passthrough(ir);
        assert(sgx535_fragment_compile_dri_color0_normalized_candidate(ir, 8, &u->b, &u->out) ==
               SGX535_NIR_OK);
        for (j = 0; j < 4; j++)
            assert(u->out.output_byte_lane[j] == (j < 3 ? 2u - j : 3u));
        no_allocations();
        free(u);
    }
    puts(
        "PASS finite normalized COLOR0 candidate: original SSA validated, explicit clamp/SCALE/lane words, independent finite UNORM reference boundaries; all numeric/hardware qualifications BLOCKED");
    free(old);
    free(p);
    free(copy);
    free(ir);
    return 0;
}
