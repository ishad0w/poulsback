// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_fragment.h"
#include "sgx535_pds_linear.h"
#include "sgx535_texture2d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint32_t movs_source_data_dword(uint32_t instruction, uint32_t lane, uint32_t data_words)
{
    static const uint32_t data_dword_by_bank_word[2][8] = {{0, 1, 2, 3, 4, 5, 6, 7},
                                                           {8, 9, 10, 11, 12, 13, 14, 15}};
    static const uint32_t word_by_pair_lane[4][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
    static const uint32_t swizzle_shift[4] = {11, 9, 7, 5};
    uint32_t selector, bank, pair, bank_word, data_dword;

    CHECK(lane < 4U);
    selector = (instruction >> swizzle_shift[lane]) & 3U;
    bank = selector >> 1;
    pair = (instruction >> (bank ? 13U : 18U)) & 31U;
    CHECK(pair < 4U);
    bank_word = word_by_pair_lane[pair][selector & 1U];
    data_dword = data_dword_by_bank_word[bank][bank_word];
    CHECK(data_dword < data_words);
    return data_dword;
}

static void round_trip(uint32_t count, uint32_t rotation, uint32_t components)
{
    /* Literal oracle vectors from the complete producer contract, independent
     * of the encoder's allocator and MOVS formulas. */
    static const uint32_t offsets[13] = {0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6};
    static const uint32_t movs[11] = {0x07000365U, 0x070418a2U, 0x07042362U, 0x07042de2U,
                                      0x07083622U, 0x070838a2U, 0x07084362U, 0x07084de2U,
                                      0x070c5622U, 0x070c58a2U, 0x070c6362U};
    static const uint32_t totals[11] = {14, 15, 16, 17, 18, 19, 20, 21, 26, 27, 28};
    struct sgx535_pds_fragment input = {{0x12345678U, 0x9abcdef0U, 42}, 0, {{0}}};
    struct sgx535_pds_fragment output, sentinel;
    struct sgx535_pds_fragment_program words, canonical, mutation;
    union {
        struct sgx535_pds_fragment task;
        struct sgx535_pds_fragment_program program;
    } alias;
    uint32_t i, bit, value, instruction, data_dword;
    input.iterator_count = count;
    for (i = 0; i < count; i++)
        input.iterator[i] = (struct sgx535_iterator_f32){(i + rotation) % 10U, components,
                                                         i + 1U == count ? 1U : 0U};
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_OK);
    CHECK(words.total_words == totals[count]);
    CHECK(words.data_words == (count < 8U ? 12U : 16U));
    instruction = words.words[words.data_words];
    CHECK((instruction & 31U) == 5U);
    for (i = 0; i < 3U; i++) {
        CHECK(words.words[offsets[i]] == input.task_control[i]);
        data_dword = movs_source_data_dword(instruction, i, words.data_words);
        CHECK(data_dword == offsets[i]);
        CHECK(words.words[data_dword] == input.task_control[i]);
    }
    for (i = 0; i < count; i++) {
        value = 0x0d00000fU | (input.iterator[i].slot << 12) | ((components - 1U) << 22) |
                (i + 1U == count ? 0x02080000U : 0U);
        CHECK(words.words[offsets[i + 3U]] == value);
        instruction = words.words[words.data_words + 1U + i];
        CHECK((instruction & 31U) == 2U);
        data_dword = movs_source_data_dword(instruction, 0U, words.data_words);
        CHECK(data_dword == offsets[i + 3U]);
        CHECK(words.words[data_dword] == value);
    }
    for (i = 0; i < count + 1U; i++)
        CHECK(words.words[words.data_words + i] == movs[i]);
    CHECK(words.words[words.total_words - 1U] == 0xaf000000U);
    CHECK(sgx535_pds_fragment_decode(&words, &output) == SGX535_PDS_FRAGMENT_OK);
    CHECK(memcmp(&input, &output, sizeof(input)) == 0);
    canonical = words;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    /* Controls are deliberately opaque. Every other bit must be canonical. */
    for (i = 0; i < SGX535_PDS_FRAGMENT_MAX_WORDS; i++) {
        if (i == offsets[0] || i == offsets[1] || i == offsets[2])
            continue;
        for (bit = 0; bit < 32U; bit++) {
            mutation = canonical;
            mutation.words[i] ^= 1U << bit;
            output = sentinel;
            if (sgx535_pds_fragment_decode(&mutation, &output) == SGX535_PDS_FRAGMENT_OK) {
                CHECK(sgx535_pds_fragment_encode(&output, &words) == SGX535_PDS_FRAGMENT_OK);
                CHECK(memcmp(&mutation, &words, sizeof(words)) == 0);
            } else {
                CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
            }
        }
    }
    alias.task = input;
    CHECK(sgx535_pds_fragment_encode(&alias.task, &alias.program) == SGX535_PDS_FRAGMENT_OK);
    CHECK(memcmp(&canonical, &alias.program, sizeof(canonical)) == 0);
    CHECK(sgx535_pds_fragment_decode(&alias.program, &alias.task) == SGX535_PDS_FRAGMENT_OK);
    CHECK(memcmp(&input, &alias.task, sizeof(input)) == 0);
}

static void sampled_input(void)
{
    struct sgx535_texture2d_nearest nearest = {
        .sampler_controls = 0x03fe0090,
        .width = 17,
        .height = 19,
        .stride_bytes = 128,
        .format = SGX535_TEXTURE2D_BGRA8_UNORM,
        .span_bytes = 2432,
        .allocation_bytes = 4096,
        .result = {SGX535_TEXTURE2D_RESULT_BGRA8_U8, 1, {2, 1, 0, 3}}};
    const struct sgx535_texture2d_cpu_binding binding = {0x00500000, 4096};
    struct sgx535_pds_fragment_sample task = {{0x00180000, 0x12345678, 0x9abcdef0}, 0};
    struct sgx535_pds_fragment_sample_program program, before;
    /* Independent literal allocator/port oracle. Ordinals0..6 occupy
     * offsets0,1,8,9,2,3,10; only data9 is an iterator, followed by DOUTT. */
    static const uint32_t expected[] = {0x00100000, 0x12345678, 0x03fe0090, 0x6c010012,
                                        0,          0,          0,          0,
                                        0x9abcdef0, 0x0c00f900, 0x00500000, 0,
                                        0x07000365, 0x070418a2, 0x07042364, 0xaf000000};
    for (uint32_t slot = 0; slot <= 9; slot++) {
        task.coordinate_slot = slot;
        CHECK(sgx535_pds_fragment_sample_encode(&task, &nearest, &binding, &program) ==
              SGX535_PDS_FRAGMENT_OK);
        CHECK(program.primary_dwords == 1 && program.data_words == 12 && program.total_words == 16);
        for (uint32_t i = 0; i < 16; i++)
            CHECK(program.words[i] == (i == 9 ? expected[i] | slot : expected[i]));
        CHECK((program.words[0] & UINT32_C(0x00180000)) == UINT32_C(0x00100000));
        CHECK(!(program.words[9] & UINT32_C(0x02000000)) && (program.words[9] & UINT32_C(0x800)));
        CHECK(movs_source_data_dword(program.words[14], 0, 12) == 2);
        CHECK(movs_source_data_dword(program.words[14], 1, 12) == 3);
        CHECK(movs_source_data_dword(program.words[14], 2, 12) == 10);
    }
    memcpy(&before, &program, sizeof(before));
    task.coordinate_slot = 10;
    CHECK(sgx535_pds_fragment_sample_encode(&task, &nearest, &binding, &program) ==
          SGX535_PDS_FRAGMENT_ERR_RANGE);
    CHECK(!memcmp(&program, &before, sizeof(program)));
    task.coordinate_slot = 0;
    nearest.sampler_controls |= UINT32_C(0x08000000);
    CHECK(sgx535_pds_fragment_sample_encode(&task, &nearest, &binding, &program) ==
          SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED);
    CHECK(!memcmp(&program, &before, sizeof(program)));
    CHECK(sgx535_pds_fragment_sample_encode(NULL, &nearest, &binding, &program) ==
          SGX535_PDS_FRAGMENT_ERR_ARGUMENT);
}

static void sampled_task(void)
{
    const struct sgx535_pds_fragment_sample_task selected = {
        .code = {0x02012000, 4096, 0x02012010, 5, 1},
        .word_count = 16,
        .temp_register_count = 15,
        .phase_count = 1,
        .coordinate_slot = 7,
        .texture_issue_count = 1,
    };
    const struct sgx535_pds_fragment_sample expected = {{0x00120125, 0x78000000, 0x20}, 7};
    struct sgx535_pds_fragment_sample output, before;
    struct sgx535_pds_fragment_sample_task input;
    union {
        struct sgx535_pds_fragment_sample_task input;
        struct sgx535_pds_fragment_sample output;
    } alias;
    CHECK(sgx535_pds_fragment_sample_task_project(&selected, &output) == SGX535_PDS_FRAGMENT_OK);
    CHECK(!memcmp(&output, &expected, sizeof(expected)));
    /* Carry across the two TRC words is independent of instruction encoding. */
    const uint32_t counts[] = {4, 15, 31, 32, 124};
    const uint32_t word1[] = {0x20000000, 0x78000000, 0xf8000000, 0, 0xe0000000};
    const uint32_t word2[] = {0x20, 0x20, 0x20, 0x21, 0x23};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        input = selected;
        input.temp_register_count = counts[i];
        CHECK(sgx535_pds_fragment_sample_task_project(&input, &output) == SGX535_PDS_FRAGMENT_OK);
        CHECK(output.task_control[0] == 0x00120125 && output.task_control[1] == word1[i] &&
              output.task_control[2] == word2[i] && output.coordinate_slot == 7);
    }
    /* A body may end exactly at allocation end; the entire allocation itself
     * retains the code-bank codec's conservative bank-end restriction. */
    input = selected;
    input.code.code_size_bytes = 80;
    CHECK(sgx535_pds_fragment_sample_task_project(&input, &output) == SGX535_PDS_FRAGMENT_OK);
    CHECK(!memcmp(&output, &expected, sizeof(expected)));
    alias.input = selected;
    CHECK(sgx535_pds_fragment_sample_task_project(&alias.input, &alias.output) ==
          SGX535_PDS_FRAGMENT_OK);
    CHECK(!memcmp(&alias.output, &expected, sizeof(expected)));

    memset(&before, 0x5a, sizeof(before));
    for (unsigned mode = 0; mode < 27; mode++) {
        input = selected;
        switch (mode) {
        case 0:
            input.word_count = 0;
            break;
        case 1:
            input.word_count = 3;
            break;
        case 2:
            input.word_count = 1026;
            break;
        case 3:
            input.temp_register_count = 0;
            break;
        case 4:
            input.temp_register_count = 125;
            break;
        case 5:
            input.temp_register_count = UINT32_MAX;
            break;
        case 6:
            input.phase_count = 0;
            break;
        case 7:
            input.phase_count = 2;
            break;
        case 8:
            input.texture_issue_count = 0;
            break;
        case 9:
            input.texture_issue_count = 2;
            break;
        case 10:
            input.use_issue_count = 1;
            break;
        case 11:
            input.last_texture_issue = 1;
            break;
        case 12:
            input.coordinate_slot = 10;
            break;
        case 13:
            input.code.code_size_bytes = 72;
            break;
        case 14:
            input.code.code_size_bytes = 0;
            break;
        case 15:
            input.code.code_size_bytes = 79;
            break;
        case 16:
            input.code.entry_va_bytes += 8;
            break;
        case 17:
            input.code.entry_va_bytes = input.code.code_va_bytes - 16;
            break;
        case 18:
            input.code.entry_va_bytes = input.code.code_va_bytes + 4096;
            break;
        case 19:
            input.code.bank_index = 16;
            break;
        case 20:
            input.code.data_master = 0;
            break;
        case 21:
            input.code.data_master = 2;
            break;
        case 22:
            input.code = (struct sgx535_use_code_bank_input){0xfffffff0, 32, 0xfffffff0, 5, 1};
            break;
        case 23:
            input.code.code_size_bytes = 0x6e008; /* One instruction past the bank end. */
            break;
        case 24:
            input.code = (struct sgx535_use_code_bank_input){0x02017ff8, 4096, 0x02018000, 5, 1};
            break;
        case 25:
            input.code.code_va_bytes++;
            break;
        case 26:
            input.temp_register_count = 3;
            break;
        }
        output = before;
        CHECK(sgx535_pds_fragment_sample_task_project(&input, &output) < 0);
        CHECK(!memcmp(&output, &before, sizeof(output)));
    }
    CHECK(sgx535_pds_fragment_sample_task_project(NULL, &output) ==
          SGX535_PDS_FRAGMENT_ERR_ARGUMENT);
    CHECK(sgx535_pds_fragment_sample_task_project(&selected, NULL) ==
          SGX535_PDS_FRAGMENT_ERR_ARGUMENT);
    CHECK(!memcmp(&output, &before, sizeof(output)));
}

int main(void)
{
    struct sgx535_pds_fragment input = {0}, output;
    struct sgx535_pds_fragment_program words, sentinel;
    struct sgx535_pds_linear old = {{0x12345678U, 0x9abcdef0U, 42}, 0};
    uint32_t legacy[SGX535_PDS_LINEAR_WORDS], count, rotation, components;
    for (count = 0; count <= 10U; count++)
        for (rotation = 0; rotation < 10U; rotation++)
            for (components = 1; components <= 4U; components++)
                round_trip(count, rotation, components);
    memcpy(input.task_control, old.task_control, sizeof(old.task_control));
    CHECK(sgx535_pds_linear_encode(&old, legacy, SGX535_PDS_LINEAR_WORDS) == SGX535_PDS_LINEAR_OK);
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_OK);
    CHECK(memcmp(legacy, words.words, sizeof(legacy)) == 0);
    memset(&sentinel, 0xa5, sizeof(sentinel));
    words = sentinel;
    input.iterator_count = 11;
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_ERR_RANGE);
    CHECK(memcmp(&words, &sentinel, sizeof(words)) == 0);
    input.iterator_count = 1;
    input.iterator[0] = (struct sgx535_iterator_f32){0, 4, 0};
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_ERR_RANGE);
    input.iterator[0].last = 1;
    input.iterator[1].slot = 1;
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED);
    input.iterator_count = 2;
    input.iterator[1] = input.iterator[0];
    input.iterator[0].last = 0;
    CHECK(sgx535_pds_fragment_encode(&input, &words) == SGX535_PDS_FRAGMENT_ERR_UNSUPPORTED);
    CHECK(sgx535_pds_fragment_encode(NULL, &words) == SGX535_PDS_FRAGMENT_ERR_ARGUMENT);
    CHECK(sgx535_pds_fragment_decode(NULL, &output) == SGX535_PDS_FRAGMENT_ERR_ARGUMENT);
    sampled_input();
    sampled_task();
    printf("PASS PDS fragment: %u canonical/literal/mutation/alias checks\n", checks);
    return 0;
}
