// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_secondary.h"
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

static const struct sgx535_pds_secondary_resource ordinary = {0x30000000U, 1024};

static void literals(void)
{
    static const struct {
        uint32_t count, control, second_source, second_control;
    } vectors[] = {{1, 0x00000000U, 0, 0},  {4, 0x00000003U, 0, 0},
                   {16, 0x0000000fU, 0, 0}, {17, 0x01e0000fU, 0x30000040U, 0x00001000U},
                   {18, 0x01000018U, 0, 0}, {32, 0x01e0001fU, 0, 0},
                   {64, 0x01e0003fU, 0, 0}, {127, 0x01e0006fU, 0x300001c0U, 0x0000700eU},
                   {128, 0x01e0007fU, 0, 0}};
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct sgx535_pds_secondary op = {
            0x30000000U, vectors[i].count, {0x12345678U, 0x87654321U, 0xfedcba98U}};
        struct sgx535_pds_secondary_program program, expected = {{0}, 0};
        struct sgx535_pds_secondary decoded;
        uint32_t task_offset = vectors[i].second_source ? 4 : 2;

        expected.words[0] = 0x30000000U;
        expected.words[1] = vectors[i].control;
        expected.words[2] = vectors[i].second_source;
        expected.words[3] = vectors[i].second_control;
        expected.words[task_offset] = op.task_control[0];
        expected.words[task_offset + 1] = op.task_control[1];
        expected.words[8] = op.task_control[2];
        expected.words[12] = 0x07030223U;
        if (vectors[i].second_source) {
            expected.words[13] = 0x07070223U;
            expected.words[14] = 0x07080345U;
            expected.words[15] = 0xaf000000U;
            expected.word_count = 16;
        } else {
            expected.words[13] = 0x07040345U;
            expected.words[14] = 0xaf000000U;
            expected.word_count = 15;
        }
        memset(&program, 0xa5, sizeof(program));
        CHECK(sgx535_pds_secondary_encode(&op, &ordinary, &program) == SGX535_PDS_SECONDARY_OK);
        CHECK(memcmp(&program, &expected, sizeof(program)) == 0);
        CHECK(sgx535_pds_secondary_decode(&expected, &ordinary, &decoded) ==
              SGX535_PDS_SECONDARY_OK);
        CHECK(memcmp(&decoded, &op, sizeof(op)) == 0);
    }
}

/* Independent DMA-field reader checks coverage and offsets, rather than just
 * comparing the serializer with its own decoder. */
static void all_counts_and_mutations(void)
{
    uint32_t count;

    for (count = 1; count <= SGX535_PDS_SECONDARY_MAX_DWORDS; ++count) {
        struct sgx535_pds_secondary op = {0x30000000U, count, {count, ~count, 0}};
        struct sgx535_pds_secondary_program program;
        struct sgx535_pds_secondary decoded;
        uint32_t width, lines, covered, stride;
        unsigned bit;

        CHECK(sgx535_pds_secondary_encode(&op, &ordinary, &program) == SGX535_PDS_SECONDARY_OK);
        CHECK(sgx535_pds_secondary_decode(&program, &ordinary, &decoded) ==
              SGX535_PDS_SECONDARY_OK);
        CHECK(memcmp(&decoded, &op, sizeof(op)) == 0);
        CHECK(program.words[0] == op.source_address);
        width = (program.words[1] & 15) + 1;
        lines = ((program.words[1] >> 4) & 15) + 1;
        stride = ((program.words[1] >> 21) & 15) + 1;
        covered = width * lines;
        CHECK((program.words[1] >> 8 & 0x1fffU) == 0);
        CHECK(covered <= count);
        if (count > 16)
            CHECK(width == stride);
        else
            CHECK(lines == 1 && width == count && stride == 1);
        if (program.word_count == 16) {
            CHECK(program.words[2] == op.source_address + covered * 4);
            CHECK((program.words[3] >> 8 & 0x1fffU) == covered);
            CHECK((program.words[3] >> 4 & 15) == 0);
            covered += (program.words[3] & 15) + 1;
        }
        CHECK(covered == count);
        /* Every altered accepted object must reconstruct exactly. Rejected
         * objects must preserve the caller's prior descriptor. */
        for (bit = 0; bit < 17 * 32; ++bit) {
            struct sgx535_pds_secondary_program altered = program, canonical;
            enum sgx535_pds_secondary_status status;

            if (bit < 16 * 32)
                altered.words[bit / 32] ^= UINT32_C(1) << (bit % 32);
            else
                altered.word_count ^= UINT32_C(1) << (bit % 32);
            decoded = op;
            status = sgx535_pds_secondary_decode(&altered, &ordinary, &decoded);
            if (status == SGX535_PDS_SECONDARY_OK) {
                CHECK(sgx535_pds_secondary_encode(&decoded, &ordinary, &canonical) ==
                      SGX535_PDS_SECONDARY_OK);
                CHECK(memcmp(&altered, &canonical, sizeof(altered)) == 0);
            } else {
                CHECK(memcmp(&decoded, &op, sizeof(op)) == 0);
            }
        }
    }
}

static void failures_and_address_boundaries(void)
{
    const struct sgx535_pds_secondary good = {0x30000000U, 17, {1, 2, 3}};
    struct sgx535_pds_secondary_program guard, output;
    struct sgx535_pds_secondary decoded = good;
    unsigned i;

    memset(&guard, 0x5a, sizeof(guard));
    output = guard;
    CHECK(sgx535_pds_secondary_encode(NULL, &ordinary, &output) ==
          SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(sgx535_pds_secondary_encode(&good, NULL, &output) == SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(sgx535_pds_secondary_encode(&good, &ordinary, NULL) == SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(memcmp(&output, &guard, sizeof(output)) == 0);
    CHECK(sgx535_pds_secondary_decode(NULL, &ordinary, &decoded) ==
          SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(sgx535_pds_secondary_decode(&guard, NULL, &decoded) == SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(sgx535_pds_secondary_decode(&guard, &ordinary, NULL) ==
          SGX535_PDS_SECONDARY_ERR_ARGUMENT);
    CHECK(memcmp(&decoded, &good, sizeof(good)) == 0);

    for (i = 0; i < 10; ++i) {
        struct sgx535_pds_secondary op = good;
        struct sgx535_pds_secondary_resource extent = ordinary;
        enum sgx535_pds_secondary_status expected = SGX535_PDS_SECONDARY_ERR_RESOURCE;

        switch (i) {
        case 0:
            op.dword_count = 0;
            expected = SGX535_PDS_SECONDARY_ERR_RANGE;
            break;
        case 1:
            op.dword_count = 129;
            expected = SGX535_PDS_SECONDARY_ERR_RANGE;
            break;
        case 2:
            op.dword_count = UINT32_MAX;
            expected = SGX535_PDS_SECONDARY_ERR_RANGE;
            break;
        case 3:
            op.source_address++;
            expected = SGX535_PDS_SECONDARY_ERR_RANGE;
            break;
        case 4:
            extent.size_bytes = 0;
            break;
        case 5:
            extent.gpu_address = op.source_address + 4;
            break;
        case 6:
            extent.size_bytes = op.dword_count * 4 - 1;
            break;
        case 7:
            extent.gpu_address = UINT32_MAX;
            extent.size_bytes = 2;
            break;
        case 8:
            op.source_address = 0xffffffc0U;
            extent.gpu_address = op.source_address;
            extent.size_bytes = 64;
            break;
        case 9:
            op.source_address = 0xfffffffcU;
            extent.gpu_address = op.source_address;
            extent.size_bytes = UINT32_MAX;
            break;
        }
        output = guard;
        CHECK(sgx535_pds_secondary_encode(&op, &extent, &output) == expected);
        CHECK(memcmp(&output, &guard, sizeof(output)) == 0);
    }
    {
        struct sgx535_pds_secondary op = {0xfffffffcU, 1, {0, 0, 0}};
        struct sgx535_pds_secondary_resource extent = {0xfffffffcU, 4};

        CHECK(sgx535_pds_secondary_encode(&op, &extent, &output) == SGX535_PDS_SECONDARY_OK);
        CHECK(sgx535_pds_secondary_decode(&output, &extent, &decoded) == SGX535_PDS_SECONDARY_OK);
        CHECK(memcmp(&decoded, &op, sizeof(op)) == 0);
        /* A remainder kick whose read ends exactly at 2^32 does not wrap. */
        op.source_address = 0xffffffbcU;
        op.dword_count = 17;
        extent.gpu_address = op.source_address;
        extent.size_bytes = 68;
        CHECK(sgx535_pds_secondary_encode(&op, &extent, &output) == SGX535_PDS_SECONDARY_OK);
        CHECK(output.words[2] == 0xfffffffcU);
        CHECK(sgx535_pds_secondary_decode(&output, &extent, &decoded) == SGX535_PDS_SECONDARY_OK);
        extent.size_bytes--;
        decoded = good;
        CHECK(sgx535_pds_secondary_decode(&output, &extent, &decoded) ==
              SGX535_PDS_SECONDARY_ERR_RESOURCE);
        CHECK(memcmp(&decoded, &good, sizeof(good)) == 0);
    }
    {
        /* Two-kick source, destination, reserved fields and operation shape
         * cannot be changed independently of the derived canonical packet. */
        static const unsigned word[] = {2, 3, 6, 7, 9, 10, 11, 12, 13, 14, 15};
        size_t n;
        CHECK(sgx535_pds_secondary_encode(&good, &ordinary, &output) == SGX535_PDS_SECONDARY_OK);
        for (n = 0; n < sizeof(word) / sizeof(word[0]); ++n) {
            struct sgx535_pds_secondary_program bad = output;
            bad.words[word[n]] ^= 1;
            decoded = good;
            CHECK(sgx535_pds_secondary_decode(&bad, &ordinary, &decoded) ==
                  SGX535_PDS_SECONDARY_ERR_UNSUPPORTED);
            CHECK(memcmp(&decoded, &good, sizeof(good)) == 0);
        }
    }
}

static void aliasing(void)
{
    const struct sgx535_pds_secondary good = {0x30000000U, 17, {7, 8, 9}};
    struct sgx535_pds_secondary_program expected;
    union {
        struct sgx535_pds_secondary_program program;
        struct sgx535_pds_secondary descriptor;
        struct {
            uint32_t prefix[2];
            struct sgx535_pds_secondary descriptor;
            struct sgx535_pds_secondary_resource resource;
        } partial;
    } alias;

    CHECK(sgx535_pds_secondary_encode(&good, &ordinary, &expected) == SGX535_PDS_SECONDARY_OK);
    alias.descriptor = good;
    CHECK(sgx535_pds_secondary_encode(&alias.descriptor, &ordinary, &alias.program) ==
          SGX535_PDS_SECONDARY_OK);
    CHECK(memcmp(&alias.program, &expected, sizeof(expected)) == 0);
    CHECK(sgx535_pds_secondary_decode(&alias.program, &ordinary, &alias.descriptor) ==
          SGX535_PDS_SECONDARY_OK);
    CHECK(memcmp(&alias.descriptor, &good, sizeof(good)) == 0);
    alias.partial.descriptor = good;
    alias.partial.resource = ordinary;
    CHECK(sgx535_pds_secondary_encode(&alias.partial.descriptor, &alias.partial.resource,
                                      &alias.program) == SGX535_PDS_SECONDARY_OK);
    CHECK(memcmp(&alias.program, &expected, sizeof(expected)) == 0);
    alias.program = expected;
    CHECK(sgx535_pds_secondary_decode(&alias.program, &ordinary, &alias.partial.descriptor) ==
          SGX535_PDS_SECONDARY_OK);
    CHECK(memcmp(&alias.partial.descriptor, &good, sizeof(good)) == 0);
}

int main(void)
{
    literals();
    all_counts_and_mutations();
    failures_and_address_boundaries();
    aliasing();
    printf("pds-secondary: %u checks passed\n", checks);
    return 0;
}
