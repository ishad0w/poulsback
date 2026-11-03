// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_iterator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void literal_oracles(void)
{
    static const struct {
        struct sgx535_iterator_f32 input;
        struct sgx535_iterator_words words;
    } cases[] = {{{0, 1, 0}, {UINT32_C(0x0d00000f), UINT32_MAX}},
                 {{0, 4, 0}, {UINT32_C(0x0dc0000f), UINT32_MAX}},
                 {{3, 4, 1}, {UINT32_C(0x0fc0300f), UINT32_MAX}},
                 {{9, 1, 0}, {UINT32_C(0x0d00900f), UINT32_MAX}},
                 {{9, 2, 1}, {UINT32_C(0x0f40900f), UINT32_MAX}}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct sgx535_iterator_words words;
        struct sgx535_iterator_f32 decoded;
        CHECK(sgx535_iterator_f32_encode(&cases[i].input, &words) == SGX535_ITERATOR_OK);
        CHECK(words.issue == cases[i].words.issue && words.tag_layer == cases[i].words.tag_layer);
        CHECK(sgx535_iterator_f32_decode(&cases[i].words, &decoded) == SGX535_ITERATOR_OK);
        CHECK(memcmp(&decoded, &cases[i].input, sizeof(decoded)) == 0);
    }
}

static void fields_and_mutations(void)
{
    uint32_t slot, components, last;
    unsigned bit;
    for (slot = 0; slot <= SGX535_ITERATOR_F32_MAX_SLOT; ++slot) {
        for (components = 1; components <= 4; ++components) {
            for (last = 0; last <= 1; ++last) {
                struct sgx535_iterator_f32 input = {slot, components, last}, decoded;
                struct sgx535_iterator_words words;
                CHECK(sgx535_iterator_f32_encode(&input, &words) == SGX535_ITERATOR_OK);
                CHECK(sgx535_iterator_f32_decode(&words, &decoded) == SGX535_ITERATOR_OK);
                CHECK(memcmp(&decoded, &input, sizeof(input)) == 0);
                for (bit = 0; bit < 64; ++bit) {
                    struct sgx535_iterator_words altered = words, canonical;
                    struct sgx535_iterator_f32 sentinel = {3, 2, 1};
                    if (bit < 32)
                        altered.issue ^= UINT32_C(1) << bit;
                    else
                        altered.tag_layer ^= UINT32_C(1) << (bit - 32);
                    decoded = sentinel;
                    if (sgx535_iterator_f32_decode(&altered, &decoded) == SGX535_ITERATOR_OK) {
                        CHECK(bit < 32 &&
                              ((bit >= 12 && bit <= 15) || bit == 22 || bit == 23 || bit == 25));
                        CHECK(sgx535_iterator_f32_encode(&decoded, &canonical) ==
                              SGX535_ITERATOR_OK);
                        CHECK(memcmp(&canonical, &altered, sizeof(canonical)) == 0);
                    } else {
                        CHECK(memcmp(&decoded, &sentinel, sizeof(sentinel)) == 0);
                    }
                }
            }
        }
    }
}

static void invalid_and_aliases(void)
{
    struct sgx535_iterator_f32 input = {0, 1, 0};
    struct sgx535_iterator_words output = {23, 45}, sentinel = output;
    static const struct sgx535_iterator_f32 bad[] = {
        {10, 1, 0}, {13, 4, 1},         {UINT32_MAX, 1, 0}, {0, 0, 0},
        {0, 5, 0},  {0, UINT32_MAX, 0}, {0, 1, 2},          {0, 1, UINT32_MAX}};
    size_t i;
    CHECK(sgx535_iterator_f32_encode(NULL, &output) == SGX535_ITERATOR_ERR_ARGUMENT);
    CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    CHECK(sgx535_iterator_f32_encode(&input, NULL) == SGX535_ITERATOR_ERR_ARGUMENT);
    CHECK(sgx535_iterator_f32_decode(NULL, &input) == SGX535_ITERATOR_ERR_ARGUMENT);
    CHECK(sgx535_iterator_f32_decode(&output, NULL) == SGX535_ITERATOR_ERR_ARGUMENT);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(sgx535_iterator_f32_encode(&bad[i], &output) == SGX535_ITERATOR_ERR_RANGE);
        CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
    }
    /* These are format changes, not additional F32 inputs. */
    CHECK(sgx535_iterator_f32_encode(&input, &output) == SGX535_ITERATOR_OK);
    output.issue |= UINT32_C(0x20000000);
    CHECK(sgx535_iterator_f32_decode(&output, &input) == SGX535_ITERATOR_ERR_UNSUPPORTED);
    CHECK(input.slot == 0 && input.components == 1 && input.last == 0);
    output.issue ^= UINT32_C(0x30000000);
    CHECK(sgx535_iterator_f32_decode(&output, &input) == SGX535_ITERATOR_ERR_UNSUPPORTED);
    {
        union {
            struct sgx535_iterator_f32 descriptor;
            struct sgx535_iterator_words words;
        } alias;
        alias.descriptor = (struct sgx535_iterator_f32){7, 3, 1};
        CHECK(sgx535_iterator_f32_encode(&alias.descriptor, &alias.words) == SGX535_ITERATOR_OK);
        CHECK(alias.words.issue == UINT32_C(0x0f80700f));
        CHECK(sgx535_iterator_f32_decode(&alias.words, &alias.descriptor) == SGX535_ITERATOR_OK);
        CHECK(alias.descriptor.slot == 7 && alias.descriptor.components == 3 &&
              alias.descriptor.last == 1);
    }
}

int main(void)
{
    literal_oracles();
    fields_and_mutations();
    invalid_and_aliases();
    printf("F32 iterator descriptor CPU checks: %u; PDS launch and hardware: unqualified\n",
           checks);
    return 0;
}
