// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Independent word/source-flow checks. No floating evaluator or renderer. */
#include "sgx535_fragment.h"
#include "sgx535_fragment_linkage.h"
#include "sgx535_texture2d.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long checks, programs;
static bool fail_allocation;
void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_calloc(size_t n, size_t s)
{
    return fail_allocation ? NULL : __real_calloc(n, s);
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL%d %s\n", __LINE__, #x);                                          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static uint32_t random_state = 0x5a93f27du;
static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static unsigned sources(enum sgx535_fragment_opcode op)
{
    if (op == SGX535_FRAGMENT_MAD)
        return 3;
    if (op == SGX535_FRAGMENT_MIN || op == SGX535_FRAGMENT_MAX)
        return 2;
    if (op == SGX535_FRAGMENT_MOV || op == SGX535_FRAGMENT_RCP || op == SGX535_FRAGMENT_RSQ ||
        op == SGX535_FRAGMENT_FRACT || op == SGX535_FRAGMENT_OUTPUT)
        return 1;
    return 0;
}
static void trace(const struct sgx535_fragment_instruction *p, unsigned n,
                  const struct sgx535_fragment_program *r)
{
    unsigned reg_value[124], components[10] = {0}, primary[10] = {0};
    unsigned i, j, base = 0, entries = 0, highest = 0;
    uint32_t uniform[4] = {0}, high_uniform = 0;
    for (i = 0; i < 124; i++)
        reg_value[i] = UINT32_MAX;
    for (i = 0; i < n; i++) {
        if (p[i].opcode == SGX535_FRAGMENT_INPUT && p[i].component + 1 > components[p[i].index])
            components[p[i].index] = p[i].component + 1;
        if (p[i].opcode == SGX535_FRAGMENT_UNIFORM) {
            uniform[p[i].index / 32] |= 1u << (p[i].index % 32);
            if (p[i].index + 1 > high_uniform)
                high_uniform = p[i].index + 1;
        }
    }
    for (i = 0; i < 10; i++)
        if (components[i]) {
            CHECK(r->input[entries].slot == i && r->input[entries].components == components[i]);
            CHECK(r->input[entries].primary_base == base);
            primary[i] = base;
            base += components[i];
            entries++;
        }
    for (i = 0; i < entries; i++) {
        const struct sgx535_fragment_input *in = &r->input[i];
        CHECK(in->iterator.issue == (0x0d00000fu | (in->slot << 12) | ((in->components - 1) << 22) |
                                     ((i == entries - 1) ? 0x02000000u : 0)));
        CHECK(in->iterator.tag_layer == 0xffffffffu);
    }
    CHECK(r->input_count == entries && r->primary_register_count == base);
    CHECK(!memcmp(r->uniform_words_used, uniform, sizeof(uniform)));
    CHECK(r->secondary_register_count == high_uniform);
    CHECK(r->arithmetic_qualified == 0 && r->linkage_qualified == 0 && r->output_qualified == 0);
    CHECK((r->output_profile == SGX535_FRAGMENT_RAW_F32_WORDS ||
           r->output_profile == SGX535_FRAGMENT_U8_WORD_PROJECTION) &&
          r->encoding.word_count == n * 2);
    CHECK(sgx535_fragment_native_admission(r) == SGX535_NIR_ERR_UNSUPPORTED);
    for (i = 0; i < n; i++) {
        uint32_t low = r->encoding.words[i * 2], high = r->encoding.words[i * 2 + 1];
        unsigned dst = (low >> 21) & 127u, source[3] = {0}, count = sources(p[i].opcode);
        if (p[i].opcode == SGX535_FRAGMENT_CONSTANT) {
            uint32_t value =
                (low & 0x1fffffu) | ((high & 0x1f0u) << 17) | (((high >> 12) & 63u) << 26);
            CHECK(value == p[i].immediate && (high & 0xfffc0e0fu) == 0xfca00000u);
        } else if (p[i].opcode == SGX535_FRAGMENT_INPUT || p[i].opcode == SGX535_FRAGMENT_UNIFORM) {
            unsigned bank = p[i].opcode == SGX535_FRAGMENT_INPUT ? 2 : 3;
            unsigned number = bank == 2 ? primary[p[i].index] + p[i].component : p[i].index;
            CHECK(high == 0x28a10000u &&
                  low == (0x20000000u | (bank << 30) | (dst << 21) | (number << 7)));
        } else {
            uint32_t expected;
            switch (p[i].opcode) {
            case SGX535_FRAGMENT_MOV:
                expected = 0x28a10000u;
                source[0] = (low >> 7) & 127u;
                break;
            case SGX535_FRAGMENT_OUTPUT:
                source[0] = (low >> 7) & 127u;
                if (r->output_profile == SGX535_FRAGMENT_U8_WORD_PROJECTION) {
                    static const unsigned mask[] = {4, 2, 1, 8};
                    expected = 0x40a10c01u | (mask[p[i].index] << 2) | (i == n - 1 ? 0x40000u : 0);
                    CHECK(dst == 0 && low == (0x20040000u | (source[0] << 7)));
                } else {
                    expected = 0x28a10001u | (i == n - 1 ? 0x40000u : 0);
                    CHECK(dst == p[i].index &&
                          low == (0x20000000u | (dst << 21) | (source[0] << 7)));
                }
                break;
            case SGX535_FRAGMENT_MAD:
                expected = 0x00a00000u;
                source[0] = (low >> 14) & 127u;
                source[1] = (low >> 7) & 127u;
                source[2] = low & 127u;
                break;
            case SGX535_FRAGMENT_MIN:
            case SGX535_FRAGMENT_MAX:
                expected = p[i].opcode == SGX535_FRAGMENT_MIN ? 0x18a00000u : 0x18a00200u;
                source[0] = (low >> 7) & 127u;
                source[1] = low & 127u;
                break;
            case SGX535_FRAGMENT_RCP:
                expected = 0x08a00000u;
                source[0] = (low >> 7) & 127u;
                break;
            case SGX535_FRAGMENT_RSQ:
                expected = 0x08a00200u;
                source[0] = (low >> 7) & 127u;
                break;
            case SGX535_FRAGMENT_FRACT:
                expected = 0x00a00600u;
                source[0] = (low >> 7) & 127u;
                CHECK((low & 127u) == source[0]);
                break;
            default:
                CHECK(false);
                return;
            }
            CHECK(high == expected);
            for (j = 0; j < count; j++) {
                CHECK(source[j] < 124 && reg_value[source[j]] == p[i].source[j]);
                if (p[i].opcode != SGX535_FRAGMENT_OUTPUT)
                    CHECK(source[j] != dst);
            }
            if (p[i].opcode != SGX535_FRAGMENT_MOV && p[i].opcode != SGX535_FRAGMENT_OUTPUT)
                CHECK((low & 0xf0000000u) == 0);
        }
        if (p[i].opcode != SGX535_FRAGMENT_OUTPUT) {
            CHECK(dst < 124);
            reg_value[dst] = i;
            if (dst + 1 > highest)
                highest = dst + 1;
        }
    }
    CHECK(r->encoding.temp_register_count == highest && r->encoding.peak_live_temps > 0 &&
          r->encoding.peak_live_temps <= highest);
    programs++;
}
static void positive(struct sgx535_fragment_instruction *p, unsigned n)
{
    struct sgx535_fragment_program result;
    CHECK(sgx535_fragment_compile(p, n, SGX535_FRAGMENT_RAW_F32_WORDS, &result) == SGX535_NIR_OK);
    trace(p, n, &result);
    CHECK(sgx535_fragment_compile(p, n, SGX535_FRAGMENT_U8_WORD_PROJECTION, &result) ==
          SGX535_NIR_OK);
    trace(p, n, &result);
}
static void failure(struct sgx535_fragment_instruction *p, unsigned n,
                    enum sgx535_fragment_output_profile profile, enum sgx535_nir_status expected)
{
    struct sgx535_fragment_program result, before;
    memset(&result, 0xa5, sizeof(result));
    before = result;
    CHECK(sgx535_fragment_compile(p, n, profile, &result) == expected);
    CHECK(!memcmp(&result, &before, sizeof(result)));
}
static void outputs(struct sgx535_fragment_instruction *p, unsigned n, unsigned value)
{
    unsigned c;
    for (c = 0; c < 4; c++) {
        p[n + c] = (struct sgx535_fragment_instruction){0};
        p[n + c].opcode = SGX535_FRAGMENT_OUTPUT;
        p[n + c].index = c;
        p[n + c].source[0] = value;
    }
}
static struct sgx535_fragment_linkage color0_binding(void)
{
    struct sgx535_fragment_linkage binding = {0};
    binding.count = 1;
    binding.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    return binding;
}
static void dri_failure(const struct sgx535_fragment_instruction *p, unsigned n,
                        const struct sgx535_fragment_linkage *binding,
                        enum sgx535_fragment_output_profile profile, enum sgx535_nir_status status)
{
    struct sgx535_fragment_dri_color0_program out, saved;
    memset(&out, 0xa5, sizeof(out));
    saved = out;
    CHECK(sgx535_fragment_compile_dri_color0(p, n, binding, profile, &out) == status);
    CHECK(!memcmp(&out, &saved, sizeof(out)));
}
static void dri_positive(const struct sgx535_fragment_instruction *p, unsigned n,
                         const struct sgx535_fragment_linkage *binding,
                         enum sgx535_fragment_output_profile profile)
{
    static const uint32_t primary[4] = {2, 1, 0, 3};
    struct sgx535_fragment_dri_color0_program out;
    struct sgx535_fragment_program legacy;
    unsigned i, component_prefix = 0;
    CHECK(sgx535_fragment_compile(p, n, profile, &legacy) == SGX535_NIR_OK);
    trace(p, n, &legacy); /* Existing independent SSA/register/word oracle. */
    CHECK(sgx535_fragment_compile_dri_color0(p, n, binding, profile, &out) == SGX535_NIR_OK);
    CHECK(out.primary_register_count == 4 && out.encoding.word_count == n * 2);
    CHECK(!memcmp(out.primary_component, primary, sizeof(primary)));
    CHECK(out.encoding.peak_live_temps == legacy.encoding.peak_live_temps &&
          out.encoding.temp_register_count == legacy.encoding.temp_register_count);
    CHECK(
        out.secondary_register_count == legacy.secondary_register_count &&
        !memcmp(out.uniform_words_used, legacy.uniform_words_used, sizeof(out.uniform_words_used)));
    for (i = 0; i < n; i++) {
        uint32_t expected = legacy.encoding.words[i * 2];
        if (p[i].opcode == SGX535_FRAGMENT_INPUT) {
            CHECK(!p[i].index);
            expected = (expected & ~(127u << 7)) | (primary[p[i].component] << 7);
            if (p[i].component + 1 > component_prefix)
                component_prefix = p[i].component + 1;
        }
        CHECK(out.encoding.words[i * 2] == expected);
        CHECK(out.encoding.words[i * 2 + 1] == legacy.encoding.words[i * 2 + 1]);
    }
    for (i = n * 2; i < SGX535_NIR_MAX_WORDS; i++)
        CHECK(!out.encoding.words[i]);
    CHECK(out.logical_components == component_prefix && out.output_profile == profile);
    CHECK(!out.arithmetic_qualified && !out.linkage_qualified && !out.output_qualified);
    out.arithmetic_qualified = out.linkage_qualified = out.output_qualified = 1;
    CHECK(sgx535_fragment_dri_color0_native_admission(&out) == SGX535_NIR_ERR_UNSUPPORTED);
}
static void dri_color0_contract(void)
{
    /* Same full 16-word SOURCE oracle retained by the independent packet5
     * fixture; no execution or numerical agreement is inferred from it. */
    static const uint32_t literal[16] = {0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u,
                                         0xa0400000u, 0x28a10000u, 0xa0600180u, 0x28a10000u,
                                         0x20040000u, 0x40a10c11u, 0x20040080u, 0x40a10c09u,
                                         0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
    struct sgx535_fragment_instruction p[16] = {0}, saved[16];
    struct sgx535_fragment_linkage binding = color0_binding(), bad;
    struct sgx535_fragment_dri_color0_program out, expected;
    unsigned i, c, run;
    for (i = 0; i < 4; i++) {
        p[i] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_INPUT, 0, 0, i, {0}};
        p[4 + i] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_OUTPUT, 0, i, 0, {i, 0, 0}};
    }
    memcpy(saved, p, sizeof(saved));
    CHECK(sgx535_fragment_compile_dri_color0(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                             &out) == SGX535_NIR_OK);
    CHECK(out.encoding.word_count == 16 && !memcmp(out.encoding.words, literal, sizeof(literal)));
    CHECK(!memcmp(saved, p, sizeof(saved)));
    expected = out;
    dri_positive(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION);
    for (c = 0; c < 4; c++) {
        p[0] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_INPUT, 0, 0, c, {0}};
        outputs(p, 1, 0);
        dri_positive(p, 5, &binding, SGX535_FRAGMENT_RAW_F32_WORDS);
        dri_positive(p, 5, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION);
    }
    for (run = 0; run < 40; run++) {
        memset(p, 0, sizeof(p));
        for (i = 0; i < 4; i++)
            p[i] = (struct sgx535_fragment_instruction){
                SGX535_FRAGMENT_INPUT, 0, 0, (i + run) % 4, {0}};
        p[4] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_UNIFORM, 0, run % 2 ? 127 : 0, 0, {0}};
        p[5] = (struct sgx535_fragment_instruction){
            SGX535_FRAGMENT_CONSTANT, random_word(), 0, 0, {0}};
        p[6] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_MAD, 0, 0, 0, {run % 4, 4, 5}};
        p[7] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_RCP, 0, 0, 0, {6, 0, 0}};
        for (i = 0; i < 4; i++)
            p[8 + i] = (struct sgx535_fragment_instruction){
                SGX535_FRAGMENT_OUTPUT, 0, i, 0, {(i + run) % 8, 0, 0}};
        dri_positive(p, 12, &binding, SGX535_FRAGMENT_RAW_F32_WORDS);
        dri_positive(p, 12, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION);
    }
    memcpy(p, saved, sizeof(p));
    binding = color0_binding();
    for (i = 0; i < 15; i++) {
        bad = binding;
        switch (i) {
        case 0:
            bad.count = 0;
            break;
        case 1:
            bad.count = 2;
            break;
        case 2:
            bad.varying[0].semantic = SGX535_FRAGMENT_GENERIC;
            break;
        case 3:
            bad.varying[0].index = 1;
            break;
        case 4:
            bad.varying[0].driver_base = 10;
            break;
        case 5:
            bad.varying[0].components = 0;
            break;
        case 6:
            bad.varying[0].components = 3;
            break;
        case 7:
            bad.varying[0].components = 5;
            break;
        case 8:
            bad.varying[0].interpolation = SGX535_FRAGMENT_FLAT;
            break;
        case 9:
            bad.varying[0].interpolation = SGX535_FRAGMENT_COLOR_INTERPOLATION;
            break;
        case 10:
            bad.varying[0].interpolation = (enum sgx535_fragment_interpolation)99;
            break;
        case 11:
            bad.varying[1].semantic = SGX535_FRAGMENT_GENERIC;
            break;
        case 12:
            bad.varying[1].driver_base = 1;
            break;
        case 13:
            bad.varying[9].components = 4;
            break;
        default:
            bad.varying[0].driver_base = 1;
            break;
        }
        dri_failure(p, 8, &bad, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_UNSUPPORTED);
    }
    dri_failure(p, 8, &binding, SGX535_FRAGMENT_NORMALIZED_U8, SGX535_NIR_ERR_UNSUPPORTED);
    p[0].index = 1;
    dri_failure(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_UNSUPPORTED);
    p[0].index = 0;
    p[0].component = 4;
    dri_failure(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_LIMIT);
    memcpy(p, saved, sizeof(p));
    p[4].source[0] = 4;
    dri_failure(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_SSA);
    memcpy(p, saved, sizeof(p));
    fail_allocation = true;
    dri_failure(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_NOMEM);
    fail_allocation = false;
    p[0] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_CONSTANT, 0, 0, 0, {0}};
    outputs(p, 1, 0);
    dri_failure(p, 5, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_UNSUPPORTED);
    memcpy(p, saved, sizeof(p));
    {
        union {
            struct sgx535_fragment_instruction ir[512];
            struct sgx535_fragment_dri_color0_program result;
        } alias;
        union {
            struct sgx535_fragment_linkage linkage;
            struct sgx535_fragment_dri_color0_program result;
        } shared;
        memcpy(alias.ir, p, 8 * sizeof(*p));
        shared.linkage = binding;
        CHECK(sgx535_fragment_compile_dri_color0(alias.ir, 8, &binding,
                                                 SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                                 &alias.result) == SGX535_NIR_OK);
        CHECK(!memcmp(&alias.result, &expected, sizeof(expected)));
        CHECK(sgx535_fragment_compile_dri_color0(p, 8, &shared.linkage,
                                                 SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                                 &shared.result) == SGX535_NIR_OK);
        CHECK(!memcmp(&shared.result, &expected, sizeof(expected)));
    }
    dri_failure(NULL, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_ARGUMENT);
    dri_failure(p, 8, NULL, SGX535_FRAGMENT_U8_WORD_PROJECTION, SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_compile_dri_color0(p, 8, &binding, SGX535_FRAGMENT_U8_WORD_PROJECTION,
                                             NULL) == SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_dri_color0_native_admission(NULL) == SGX535_NIR_ERR_ARGUMENT);
}

static void fractional_sources(void)
{
    static const uint32_t values[] = {0,          0x80000000, 0x3e800000, 0xbe800000,
                                      0xbfa00000, 0xbf800000, 0x4b000000, 1,
                                      0x80000001, 0x7f800000, 0x7fc12345};
    struct sgx535_fragment_instruction p[7] = {0}, saved[7];
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]) + 2; i++) {
        memset(p, 0, sizeof(p));
        p[0].opcode = i < sizeof(values) / sizeof(values[0])    ? SGX535_FRAGMENT_CONSTANT
                      : i == sizeof(values) / sizeof(values[0]) ? SGX535_FRAGMENT_INPUT
                                                                : SGX535_FRAGMENT_UNIFORM;
        if (p[0].opcode == SGX535_FRAGMENT_CONSTANT)
            p[0].immediate = values[i];
        p[1].opcode = p[2].opcode = SGX535_FRAGMENT_FRACT;
        p[2].source[0] = 1;
        outputs(p, 3, 2);
        memcpy(saved, p, sizeof(p));
        positive(p, 7);
        CHECK(!memcmp(p, saved, sizeof(p)));
    }
    /* A source-flow check, including nonfinite literal bits above, never
     * evaluates native FRC or upgrades arithmetic qualification. */
    p[1].source[1] = 1;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[1].source[1] = 0;
    p[1].source[0] = 1;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_SSA);
    p[1].source[0] = 0;
    p[1].index = 1;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[1].index = 0;
    p[1].immediate = 1;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[1].immediate = 0;
    p[1].component = 1;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[1].component = 0;
    fail_allocation = true;
    failure(p, 7, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_NOMEM);
    fail_allocation = false;
}

static struct sgx535_texture2d_primary_plan sampled_primary(unsigned first)
{
    /* Independent literal UNPACK profile: PA0, RGBA byte lanes2/1/0/3.
     * Only destination TEMP bits vary; no production constructor is the oracle. */
    const uint32_t low[] = {0xa0068000u, 0xa0254000u, 0xa0440000u, 0xa067c000u};
    struct sgx535_texture2d_primary_plan p = {.result_ssa = 9,
                                              .result_dwords = 1,
                                              .primary_dwords = 1,
                                              .packed_source = {SGX535_MOV_PRIMARY, 0}};
    for (unsigned c = 0; c < 4; c++) {
        p.unpack[c * 2] = low[c] + (first << 21);
        p.unpack[c * 2 + 1] = 0x40a001bcu;
    }
    return p;
}
static void sampled_input(struct sgx535_fragment_instruction p[10])
{
    memset(p, 0, 10 * sizeof(*p));
    for (unsigned i = 0; i < 2; i++)
        p[i] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_INPUT, .index = 7, .component = i};
    for (unsigned c = 0; c < 4; c++) {
        p[2 + c] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_SAMPLE2D_COMPONENT, .component = c, .source = {0, 1, 0}};
        p[6 + c] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {2 + c, 0, 0}};
    }
}
static void sampled_contract(void)
{
    struct sgx535_fragment_instruction p[512] = {{0}};
    const struct sgx535_fragment_sampled_input sample = {.texture_slot = 3,
                                                         .sampler_slot = 5,
                                                         .coordinate_slot = 7,
                                                         .coordinate_components = 2,
                                                         .interpolation = SGX535_FRAGMENT_SMOOTH,
                                                         .result_format =
                                                             SGX535_FRAGMENT_SAMPLED_BGRA8_U8,
                                                         .result_dwords = 1};
    struct sgx535_texture2d_primary_plan primary;
    struct sgx535_fragment_program result, before;
    const unsigned placements[] = {0, 11, 120};
    sampled_input(p);
    for (unsigned which = 0; which < 3; which++) {
        unsigned first = placements[which];
        primary = sampled_primary(first);
        CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &sample, &primary, &result) ==
              SGX535_NIR_OK);
        CHECK(result.primary_register_count == 1 && !result.input_count &&
              !result.secondary_register_count &&
              result.output_profile == SGX535_FRAGMENT_U8_WORD_PROJECTION &&
              result.encoding.word_count == 16 && result.encoding.peak_live_temps == 4 &&
              result.encoding.temp_register_count == first + 4 && !result.arithmetic_qualified &&
              !result.linkage_qualified && !result.output_qualified);
        CHECK(!memcmp(result.encoding.words, primary.unpack, sizeof(primary.unpack)));
        const uint32_t high[] = {0x40a10c11u, 0x40a10c09u, 0x40a10c05u, 0x40a50c21u};
        for (unsigned c = 0; c < 4; c++) {
            CHECK(result.encoding.words[8 + c * 2] == (0x20040000u | ((first + c) << 7)));
            CHECK(result.encoding.words[9 + c * 2] == high[c]);
        }
        CHECK(sgx535_fragment_native_admission(&result) == SGX535_NIR_ERR_UNSUPPORTED);
        programs++;
    }
    /* The prefix wrote all4TEMP values already. A constant/MAD between the
     * first and later symbolic sample defs must not overwrite those future defs. */
    struct sgx535_fragment_instruction interleaved[12] = {{0}};
    interleaved[0] = p[0];
    interleaved[1] = p[1];
    interleaved[2] = p[2];
    interleaved[3] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                          .immediate = 0x3f800000u};
    interleaved[4] =
        (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAD, .source = {2, 3, 3}};
    for (unsigned c = 1; c < 4; c++)
        interleaved[4 + c] = p[2 + c];
    for (unsigned c = 0; c < 4; c++)
        interleaved[8 + c] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {c ? 4 + c : 4, 0, 0}};
    primary = sampled_primary(0);
    CHECK(sgx535_fragment_compile_sampled_candidate(interleaved, 12, &sample, &primary, &result) ==
          SGX535_NIR_OK);
    CHECK(result.encoding.word_count == 20 && result.encoding.peak_live_temps == 6 &&
          result.encoding.temp_register_count == 6);
    CHECK(((result.encoding.words[8] >> 21) & 127u) == 4);
    CHECK(result.encoding.words[10] == ((5u << 21) | (4u << 7) | 4u) &&
          result.encoding.words[11] == 0x00a00000u);
    CHECK(result.encoding.words[12] == (0x20040000u | (5u << 7)));
    for (unsigned c = 1; c < 4; c++)
        CHECK(result.encoding.words[12 + c * 2] == (0x20040000u | (c << 7)));
    programs++;

    /* Only one component remains live; the other prefix destinations release
     * when their symbolic defs are visited, without clobbering the repeated one. */
    sampled_input(p);
    for (unsigned c = 0; c < 4; c++)
        p[6 + c].source[0] = 2;
    primary = sampled_primary(0);
    CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &sample, &primary, &result) ==
          SGX535_NIR_OK);
    CHECK(result.encoding.word_count == 16 && result.encoding.peak_live_temps == 4 &&
          result.encoding.temp_register_count == 4);
    for (unsigned c = 0; c < 4; c++)
        CHECK(result.encoding.words[8 + c * 2] == 0x20040000u);
    programs++;
    /* All4sample defs are dead. The later constant can reuse TEMP0, but the
     * resource extent/peak still includes the actual four UNPACK writes. */
    sampled_input(p);
    p[6] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                .immediate = 0x3f800000u};
    for (unsigned c = 0; c < 4; c++)
        p[7 + c] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {6, 0, 0}};
    CHECK(sgx535_fragment_compile_sampled_candidate(p, 11, &sample, &primary, &result) ==
          SGX535_NIR_OK);
    CHECK(result.encoding.word_count == 18 && result.encoding.peak_live_temps == 4 &&
          result.encoding.temp_register_count == 4);
    uint32_t low = result.encoding.words[8], high = result.encoding.words[9];
    CHECK(((low >> 21) & 127u) == 0 && (high & 0xfffc0e0fu) == 0xfca00000u);
    CHECK(((low & 0x1fffffu) | ((high & 0x1f0u) << 17) | (((high >> 12) & 63u) << 26)) ==
          0x3f800000u);
    for (unsigned c = 0; c < 4; c++)
        CHECK(result.encoding.words[10 + c * 2] == 0x20040000u);
    programs++;

    for (unsigned mode = 0; mode < 16; mode++) {
        sampled_input(p);
        struct sgx535_fragment_sampled_input selected = sample;
        primary = sampled_primary(0);
        enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED;
        switch (mode) {
        case 0:
            primary.result_dwords = 2;
            break;
        case 1:
            primary.primary_dwords = 2;
            break;
        case 2:
            primary.record_index = 1;
            break;
        case 3:
            primary.packed_source.bank = SGX535_MOV_TEMP;
            break;
        case 4:
            primary.packed_source.number = 1;
            break;
        case 5:
            primary.unpack[0] ^= 1;
            break;
        case 6:
            primary.unpack[1] |= 0x40000u;
            break;
        case 7:
            primary.unpack[2] = primary.unpack[0];
            break;
        case 8:
            primary = sampled_primary(121);
            expected = SGX535_NIR_ERR_LIMIT;
            break;
        case 9:
            selected.result_format = 0;
            break;
        case 10:
            selected.coordinate_slot = 10;
            expected = SGX535_NIR_ERR_ARGUMENT;
            break;
        case 11:
            p[2].source[0] = 2;
            expected = SGX535_NIR_ERR_SSA;
            break;
        case 12:
            p[3].component = 0;
            expected = SGX535_NIR_ERR_ARGUMENT;
            break;
        case 13:
            p[6].source[0] = 0;
            break;
        case 14:
            fail_allocation = true;
            expected = SGX535_NIR_ERR_NOMEM;
            break;
        default:
            selected.reserved = 1;
            expected = SGX535_NIR_ERR_ARGUMENT;
            break;
        }
        memset(&result, 0xa5, sizeof(result));
        before = result;
        CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &selected, &primary, &result) ==
              expected);
        fail_allocation = false;
        CHECK(!memcmp(&result, &before, sizeof(result)));
    }
    /* All4prefix values stay live while120 constants fill the remaining
     * registers. The next real MIN needs a distinct destination before release. */
    memset(p, 0, sizeof(p));
    sampled_input(p);
    for (unsigned i = 0; i < 120; i++)
        p[6 + i] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_CONSTANT,
                                                        .immediate = 0x3f800000u + i};
    unsigned cursor = 126, accumulator = 6;
    for (unsigned i = 1; i < 120; i++) {
        p[cursor] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MIN,
                                                         .source = {accumulator, 6 + i, 0}};
        accumulator = cursor++;
    }
    p[cursor] = (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_MAD,
                                                     .source = {accumulator, 2, 2}};
    accumulator = cursor++;
    for (unsigned c = 0; c < 4; c++)
        p[cursor++] =
            (struct sgx535_fragment_instruction){.opcode = SGX535_FRAGMENT_OUTPUT,
                                                 .index = c,
                                                 .source = {c ? 2 + c : accumulator, 0, 0}};
    primary = sampled_primary(0);
    memset(&result, 0xa5, sizeof(result));
    before = result;
    CHECK(sgx535_fragment_compile_sampled_candidate(p, cursor, &sample, &primary, &result) ==
          SGX535_NIR_ERR_PRESSURE);
    CHECK(!memcmp(&result, &before, sizeof(result)));
    sampled_input(p);
    primary = sampled_primary(0);
    union {
        struct sgx535_fragment_instruction ir[512];
        struct sgx535_texture2d_primary_plan primary;
        struct sgx535_fragment_sampled_input sample;
        struct sgx535_fragment_program result;
    } alias;
    memcpy(alias.ir, p, 10 * sizeof(*p));
    CHECK(sgx535_fragment_compile_sampled_candidate(alias.ir, 10, &sample, &primary,
                                                    &alias.result) == SGX535_NIR_OK);
    alias.primary = primary;
    CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &sample, &alias.primary,
                                                    &alias.result) == SGX535_NIR_OK);
    alias.sample = sample;
    CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &alias.sample, &primary,
                                                    &alias.result) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_compile_sampled_candidate(NULL, 10, &sample, &primary, &result) ==
          SGX535_NIR_ERR_ARGUMENT);
    CHECK(sgx535_fragment_compile_sampled_candidate(p, 10, &sample, &primary, NULL) ==
          SGX535_NIR_ERR_ARGUMENT);
    failure(p, 10, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    puts(
        "PASS conditional sampled USE: exact UNPACK/PACK words, early precolor liveness,deaddefs,pressure,aliases,16refusals; arrival/native admission unqualified");
}
int main(void)
{
    struct sgx535_fragment_instruction p[SGX535_NIR_MAX_INSTRUCTIONS] = {0};
    unsigned i, c, run;
    fractional_sources();
    p[0].opcode = SGX535_FRAGMENT_CONSTANT;
    p[0].immediate = 0x3f800000;
    outputs(p, 1, 0);
    positive(p, 5);
    for (i = 0; i < 10; i++)
        for (c = 0; c < 4; c++) {
            p[0] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_INPUT, 0, i, c, {0}};
            outputs(p, 1, 0);
            positive(p, 5);
        }
    for (i = 0; i < 128; i++) {
        p[0] = (struct sgx535_fragment_instruction){SGX535_FRAGMENT_UNIFORM, 0, i, 0, {0}};
        outputs(p, 1, 0);
        positive(p, 5);
    }
    for (run = 0; run < 1000; run++) {
        unsigned n = 8 + random_word() % 100;
        memset(p, 0, sizeof(p));
        for (i = 0; i < n; i++) {
            p[i].opcode =
                i < 3 ? (enum sgx535_fragment_opcode)i
                      : (enum sgx535_fragment_opcode)(random_word() % SGX535_FRAGMENT_OUTPUT);
            if (i >= 3 && random_word() % 9 == 0)
                p[i].opcode = SGX535_FRAGMENT_RSQ;
            if (p[i].opcode == SGX535_FRAGMENT_CONSTANT)
                p[i].immediate = random_word();
            if (p[i].opcode == SGX535_FRAGMENT_INPUT) {
                p[i].index = random_word() % 10;
                p[i].component = random_word() % 4;
            }
            if (p[i].opcode == SGX535_FRAGMENT_UNIFORM)
                p[i].index = random_word() % 128;
            for (c = 0; c < sources(p[i].opcode); c++)
                p[i].source[c] = random_word() % i;
        }
        outputs(p, n, n - 1);
        positive(p, n + 4);
    }
    memset(p, 0, sizeof(p));
    for (i = 0; i < 124; i++) {
        p[i].opcode = SGX535_FRAGMENT_CONSTANT;
        p[i].immediate = i;
    }
    for (i = 0; i < 124; i++) {
        p[124 + i].opcode = SGX535_FRAGMENT_MOV;
        p[124 + i].source[0] = i;
    }
    outputs(p, 248, 247);
    failure(p, 252, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_PRESSURE);
    memset(p, 0, sizeof(p));
    p[0].opcode = SGX535_FRAGMENT_INPUT;
    outputs(p, 1, 0);
    failure(p, 5, SGX535_FRAGMENT_NORMALIZED_U8, SGX535_NIR_ERR_UNSUPPORTED);
    p[0].index = 10;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_LIMIT);
    p[0].index = 0;
    p[0].component = 4;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_LIMIT);
    p[0].component = 0;
    p[0].opcode = SGX535_FRAGMENT_UNIFORM;
    p[0].index = 128;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_LIMIT);
    p[0].index = 0;
    p[0].opcode = (enum sgx535_fragment_opcode)99;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[0].opcode = SGX535_FRAGMENT_CONSTANT;
    p[1].source[0] = 1;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_SSA);
    p[1].source[0] = 0;
    p[3].index = 3;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[3].index = 2;
    p[0].source[2] = 1;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_UNSUPPORTED);
    p[0].source[2] = 0;
    fail_allocation = true;
    failure(p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_NOMEM);
    fail_allocation = false;
    {
        union {
            struct sgx535_fragment_instruction p[512];
            struct sgx535_fragment_program r;
        } alias;
        memcpy(alias.p, p, 5 * sizeof(p[0]));
        CHECK(sgx535_fragment_compile(alias.p, 5, SGX535_FRAGMENT_RAW_F32_WORDS, &alias.r) ==
              SGX535_NIR_OK);
        trace(p, 5, &alias.r);
        alias.r.arithmetic_qualified = alias.r.linkage_qualified = alias.r.output_qualified = 1;
        CHECK(sgx535_fragment_native_admission(&alias.r) == SGX535_NIR_ERR_UNSUPPORTED);
    }
    CHECK(sgx535_fragment_native_admission(NULL) == SGX535_NIR_ERR_ARGUMENT);
    failure(p, 0, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_LIMIT);
    failure(p, 513, SGX535_FRAGMENT_RAW_F32_WORDS, SGX535_NIR_ERR_LIMIT);
    dri_color0_contract();
    sampled_contract();
    printf("PASS fragment raw-word compiler: %lu checks, %lu programs; native admission refused\n",
           checks, programs);
    return 0;
}
