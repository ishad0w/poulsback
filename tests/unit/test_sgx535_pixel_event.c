// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pixel_event.h"
#include "sgx535_pixel_event_reference.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                       \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void write_word(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i)
        bytes[i] = (uint8_t)(value >> (8u * i));
}

static void serialize(const struct sgx535_pixel_event_program *program, uint8_t pds[148],
                      uint8_t use[32])
{
    for (uint32_t i = 0; i < 37; ++i)
        write_word(pds + i * 4u, program->pds_words[i]);
    for (uint32_t i = 0; i < 8; ++i)
        write_word(use + i * 4u, program->use_words[i]);
}

static void variant(enum sgx535_pixel_event_variant selection)
{
    struct {
        uint32_t before;
        struct sgx535_pixel_event_program program;
        uint32_t after;
    } guarded;
    uint8_t pds[148], use[32], actual[256], expected[256];
    uint32_t initial = selection == SGX535_PIXEL_EVENT_PTOFF_PBE;
    uint32_t pds_count = initial ? 37u : 32u;
    uint32_t use_count = initial ? 8u : 4u;
    uint32_t state_index = initial ? 2u : 0u, pbe_index = initial ? 4u : 2u;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.before = 0x56341200u;
    guarded.after = 0x89abcdefu;
    CHECK(sgx535_pixel_event_build(selection, &guarded.program) == 0);
    CHECK(guarded.before == 0x56341200u && guarded.after == 0x89abcdefu);
    CHECK(guarded.program.pds_data_words == 16);
    CHECK(guarded.program.pds_word_count == pds_count);
    CHECK(guarded.program.use_word_count == use_count);
    for (uint32_t i = pds_count; i < 37; ++i)
        CHECK(!guarded.program.pds_words[i]);
    for (uint32_t i = use_count; i < 8; ++i)
        CHECK(!guarded.program.use_words[i]);
    serialize(&guarded.program, pds, use);
    CHECK(sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u, (int)initial,
                                             0, 0, 1u << 21));

    /* Coverage: all16 data words, every instruction/fixed/unused field, all
     * emitted USE bits and padding. No single serialized-bit change is ignored. */
    for (uint32_t byte = 0; byte < pds_count * 4u; ++byte)
        for (uint32_t bit = 0; bit < 8; ++bit) {
            int valid;
            pds[byte] ^= (uint8_t)(1u << bit);
            valid = sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u,
                                                       (int)initial, 0, 0, 1u << 21);
            if (valid)
                fprintf(stderr, "unobserved PDS bit: variant%u byte%u bit%u\n", initial, byte, bit);
            CHECK(!valid);
            pds[byte] ^= (uint8_t)(1u << bit);
        }
    for (uint32_t byte = 0; byte < use_count * 4u; ++byte)
        for (uint32_t bit = 0; bit < 8; ++bit) {
            use[byte] ^= (uint8_t)(1u << bit);
            CHECK(!sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u,
                                                      (int)initial, 0, 0, 1u << 21));
            use[byte] ^= (uint8_t)(1u << bit);
        }
    CHECK(!sgx535_pixel_event_reference_check(NULL, pds_count * 4u, use, use_count * 4u,
                                              (int)initial, 0, 0, 1u << 21));
    CHECK(!sgx535_pixel_event_reference_check(pds, pds_count * 4u, NULL, use_count * 4u,
                                              (int)initial, 0, 0, 1u << 21));
    CHECK(!sgx535_pixel_event_reference_check(pds, pds_count * 4u - 1u, use, use_count * 4u,
                                              (int)initial, 0, 0, 1u << 21));
    CHECK(!sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u - 1u,
                                              (int)initial, 0, 0, 1u << 21));
    CHECK(!sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u, 2, 0, 0,
                                              1u << 21));

    /* Relocated links are explicit independent fixture inputs, never inferred
     * from actual bytes. They do not change instruction or data-hole checks. */
    write_word(pds + state_index * 4u, 0x34560000u);
    write_word(pds + pbe_index * 4u, 0x0025a683u);
    if (initial)
        write_word(pds, 0x0001b492u);
    CHECK(sgx535_pixel_event_reference_check(pds, pds_count * 4u, use, use_count * 4u, (int)initial,
                                             0x34560000u, initial ? 0x0001b492u : 0u, 0x0025a683u));
    memset(actual, 0x5a, sizeof(actual));
    memset(expected, 0x5a, sizeof(expected));
    memcpy(actual + 16, pds, pds_count * 4u);
    memcpy(actual + 176, use, use_count * 4u);
    CHECK(sgx535_pixel_event_reference_packet(actual, expected, sizeof(actual), 16, 176,
                                              (int)initial, 0x34560000u, initial ? 0x0001b492u : 0u,
                                              0x0025a683u));
    for (uint32_t byte = 0; byte < sizeof(actual); ++byte) {
        actual[byte] ^= 1;
        CHECK(!sgx535_pixel_event_reference_packet(actual, expected, sizeof(actual), 16, 176,
                                                   (int)initial, 0x34560000u,
                                                   initial ? 0x0001b492u : 0u, 0x0025a683u));
        actual[byte] ^= 1;
    }
    CHECK(!sgx535_pixel_event_reference_packet(actual, expected, sizeof(actual), 16, 16,
                                               (int)initial, 0x34560000u,
                                               initial ? 0x0001b492u : 0u, 0x0025a683u));
    CHECK(!sgx535_pixel_event_reference_packet(actual, expected, sizeof(actual), UINT32_MAX, 176,
                                               (int)initial, 0x34560000u,
                                               initial ? 0x0001b492u : 0u, 0x0025a683u));
}

int main(void)
{
    struct sgx535_pixel_event_program output, before;
    struct pixel_event_ref_op loop[2] = {{0}};
    struct pixel_event_ref_trace trace;
    uint32_t data[16] = {0};
    variant(SGX535_PIXEL_EVENT_PBE_ONLY);
    variant(SGX535_PIXEL_EVENT_PTOFF_PBE);
    memset(&output, 0xa5, sizeof(output));
    before = output;
    CHECK(sgx535_pixel_event_build((enum sgx535_pixel_event_variant) - 1, &output) == -1);
    CHECK(!memcmp(&output, &before, sizeof(output)));
    CHECK(sgx535_pixel_event_build((enum sgx535_pixel_event_variant)2, &output) == -1);
    CHECK(!memcmp(&output, &before, sizeof(output)));
    CHECK(sgx535_pixel_event_build(SGX535_PIXEL_EVENT_PBE_ONLY, NULL) == -1);
    CHECK(sgx535_pixel_event_build(SGX535_PIXEL_EVENT_PTOFF_PBE, NULL) == -1);
    /* The model itself remains bounded on malformed code, independently of the
     * selected-profile structural rejection performed by the public helper. */
    loop[0].group = 2;
    loop[0].type = 0;
    loop[1].group = 2;
    loop[1].type = 2;
    loop[1].target = 1;
    CHECK(!pixel_event_ref_execute(data, loop, 2, 0, 0, &trace));
    loop[1].target = 2;
    CHECK(!pixel_event_ref_execute(data, loop, 2, 0, 0, &trace));
    printf("sgx535 pixel event: %u checks PASS\n", checks);
    return 0;
}
