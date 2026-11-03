// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_texture2d.h"
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

static struct sgx535_texture2d_nearest_request request(void)
{
    struct sgx535_texture2d_nearest_request r = {0};
    r.texture_slot = 3;
    r.sampler_slot = 5;
    r.texture_slots = 16;
    r.sampler_slots = 16;
    r.coordinate_ssa = 7;
    r.result_ssa = 11;
    r.ssa_values = 12;
    r.coordinate_components = 2;
    r.coordinate_bits = 32;
    r.result_components = 4;
    r.result_bits = 32;
    r.skip_helpers = 1;
    r.can_speculate = 1;
    r.width = 17;
    r.height = 19;
    r.stride_bytes = 128;
    r.format = SGX535_TEXTURE2D_BGRA8_UNORM;
    r.wrap_s = SGX535_TEXTURE2D_CLAMP_TO_EDGE;
    r.wrap_t = SGX535_TEXTURE2D_CLAMP_TO_EDGE;
    r.normalized_coords = 1;
    r.raw_samples = 1;
    r.byte_offset = 4;
    r.allocation_bytes = 4096;
    return r;
}

static void words_and_spans(void)
{
    /* The selected LINEAR producer fixes clamp on both axes. Its separate
     * wrap-table branch does not qualify repeat for this surface class. */
    static const struct {
        uint32_t width, stride;
    } rows[] = {{1, 128},  {17, 128},  {31, 128},    {32, 128},
                {33, 256}, {127, 512}, {2048, 8192}, {4096, 16384}};
    struct sgx535_texture2d_nearest_request r = request();
    struct sgx535_texture2d_nearest result;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned t = 0; t < 2; t++)
            for (unsigned helpers = 0; helpers < 2; helpers++)
                for (unsigned speculate = 0; speculate < 2; speculate++) {
                    r.wrap_s = s;
                    r.wrap_t = t;
                    r.skip_helpers = helpers;
                    r.can_speculate = speculate;
                    memset(&result, 0xad, sizeof(result));
                    if (!s || !t) {
                        struct sgx535_texture2d_nearest before;
                        memcpy(&before, &result, sizeof(before));
                        CHECK(sgx535_texture2d_nearest_build(&r, &result) ==
                              SGX535_TEXTURE2D_ERR_UNSUPPORTED);
                        CHECK(!memcmp(&result, &before, sizeof(result)));
                        continue;
                    }
                    CHECK(sgx535_texture2d_nearest_build(&r, &result) == SGX535_TEXTURE2D_OK);
                    CHECK(result.sampler_controls == UINT32_C(0x03fe0090));
                    CHECK(((result.sampler_controls >> 17) & 15u) == 15u &&
                          ((result.sampler_controls >> 21) & 63u) == 31u &&
                          !(result.sampler_controls & 0x1e00u));
                    CHECK(((result.sampler_controls >> 6) & 7u) == (s ? 2u : 0u) &&
                          ((result.sampler_controls >> 3) & 7u) == (t ? 2u : 0u));
                    CHECK(result.texture_slot == 3 && result.sampler_slot == 5 &&
                          result.coordinate_ssa == 7 && result.result_ssa == 11 &&
                          result.skip_helpers == helpers && result.can_speculate == speculate);
                    CHECK(result.width == 17 && result.height == 19 && result.stride_bytes == 128 &&
                          result.format == SGX535_TEXTURE2D_BGRA8_UNORM &&
                          result.byte_offset == 4 && result.span_bytes == 2432 &&
                          result.allocation_bytes == 4096 && result.raw_samples == 1 &&
                          !result.raw_storage_samples);
                    CHECK(result.result.format == SGX535_TEXTURE2D_RESULT_BGRA8_U8 &&
                          result.result.dwords == 1 && result.result.rgba_byte[0] == 2 &&
                          result.result.rgba_byte[1] == 1 && result.result.rgba_byte[2] == 0 &&
                          result.result.rgba_byte[3] == 3);
                }
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        r.width = rows[i].width;
        r.height = 3;
        r.stride_bytes = rows[i].stride;
        r.byte_offset = 0;
        r.allocation_bytes = (uint64_t)rows[i].stride * 3;
        CHECK(sgx535_texture2d_nearest_build(&r, &result) == SGX535_TEXTURE2D_OK);
        CHECK(result.span_bytes == r.allocation_bytes && result.stride_bytes == rows[i].stride);
    }
    r = request();
    r.coordinate_ssa = 511;
    r.result_ssa = 0;
    r.ssa_values = 512;
    r.raw_samples = 0;
    r.raw_storage_samples = 1;
    CHECK(sgx535_texture2d_nearest_build(&r, &result) == SGX535_TEXTURE2D_OK);
    CHECK(result.coordinate_ssa == 511 && !result.result_ssa && !result.raw_samples &&
          result.raw_storage_samples == 1);
}

static void refuse(const struct sgx535_texture2d_nearest_request *r,
                   enum sgx535_texture2d_status status)
{
    struct sgx535_texture2d_nearest output, before;
    memset(&output, 0x67, sizeof(output));
    memcpy(&before, &output, sizeof(before));
    CHECK(sgx535_texture2d_nearest_build(r, &output) == status);
    CHECK(!memcmp(&output, &before, sizeof(output)));
}

static void invalid(void)
{
    struct sgx535_texture2d_nearest_request r;
    CHECK(sgx535_texture2d_nearest_build(NULL, NULL) == SGX535_TEXTURE2D_ERR_ARGUMENT);
    r = request();
    CHECK(sgx535_texture2d_nearest_build(&r, NULL) == SGX535_TEXTURE2D_ERR_ARGUMENT);
    refuse(NULL, SGX535_TEXTURE2D_ERR_ARGUMENT);
#define BAD(field, value, status)                                                                  \
    do {                                                                                           \
        r = request();                                                                             \
        r.field = value;                                                                           \
        refuse(&r, status);                                                                        \
    } while (0)
    BAD(texture_slots, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(texture_slot, 16, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(sampler_slots, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(sampler_slot, UINT32_MAX, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(ssa_values, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(ssa_values, 513, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(coordinate_ssa, 12, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(result_ssa, 12, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(result_ssa, 7, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(width, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(height, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(width, UINT32_MAX, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(byte_offset, 2, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(byte_offset, UINT64_MAX - 3u, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(allocation_bytes, 2435, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(stride_bytes, 256, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(stride_bytes, 68, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(coordinate_components, 3, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(coordinate_bits, 16, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(result_components, 3, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(result_bits, 16, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(skip_helpers, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(can_speculate, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(format, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(wrap_s, SGX535_TEXTURE2D_REPEAT, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(wrap_t, SGX535_TEXTURE2D_REPEAT, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(wrap_s, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(wrap_t, UINT32_MAX, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(min_filter, 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(mag_filter, 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(mip_filter, 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(normalized_coords, 0, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(normalized_coords, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(compare_enable, 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(reduction_mode, 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(max_anisotropy, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(raw_samples, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(raw_storage_samples, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    for (size_t i = 0; i < 4; i++) {
        r = request();
        r.reserved[i] = 1;
        refuse(&r, SGX535_TEXTURE2D_ERR_ARGUMENT);
    }
#undef BAD
    r = request();
    r.allocation_bytes = UINT64_MAX;
    r.byte_offset = UINT64_MAX - 2431u;
    refuse(&r, SGX535_TEXTURE2D_ERR_RANGE);
    r.byte_offset = UINT64_MAX - 2435u;
    struct sgx535_texture2d_nearest result;
    CHECK(sgx535_texture2d_nearest_build(&r, &result) == SGX535_TEXTURE2D_OK);
    CHECK(result.byte_offset == UINT64_MAX - 2435u && result.span_bytes == 2432);
    union {
        struct sgx535_texture2d_nearest_request input;
        struct sgx535_texture2d_nearest output;
    } alias;
    alias.input = request();
    CHECK(sgx535_texture2d_nearest_build(&alias.input, &alias.output) == SGX535_TEXTURE2D_OK);
    CHECK(alias.output.sampler_controls == UINT32_C(0x03fe0090) &&
          alias.output.coordinate_ssa == 7 && alias.output.result_ssa == 11 &&
          alias.output.span_bytes == 2432);
}

static struct sgx535_texture2d_unpack_request unpack_request(void)
{
    return (struct sgx535_texture2d_unpack_request){
        {SGX535_TEXTURE2D_RESULT_BGRA8_U8, 1, {2, 1, 0, 3}}, {SGX535_MOV_PRIMARY, 9}, 11};
}

static void unpack_words(void)
{
    /* Independent native field vectors, not a round trip through our codec.
     * The chosen native surface row and the final U8 consumer agree on
     * bytes2,1,0,3; no source/view swizzle is used to derive this vector. */
    static const uint32_t primary[] = {0xa1668489, 0x40a001bc, 0xa1854489, 0x40a001bc,
                                       0xa1a40489, 0x40a001bc, 0xa1c7c489, 0x40a001bc};
    static const uint32_t temporary[] = {0x01668489, 0x40a001bc, 0x01854489, 0x40a001bc,
                                         0x01a40489, 0x40a001bc, 0x01c7c489, 0x40a001bc};
    static const uint32_t rgba[] = {0xa7, 0x35, 0x19, 0xdd};
    struct sgx535_texture2d_unpack_request r = unpack_request();
    uint32_t words[8];
    CHECK(sgx535_texture2d_unpack_build(&r, words) == SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(words, primary, sizeof(words)));
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t low = words[2 * i], high = words[2 * i + 1];
        CHECK(((low >> 21) & 127u) == 11 + i && ((low >> 7) & 127u) == 9 && (low & 127u) == 9 &&
              (low >> 28) == 10 && ((low >> 18) & 1u) && ((high >> 2) & 15u) == 15 &&
              !(high & 0x40001u));
        uint32_t byte = (low >> 16) & 3u;
        CHECK(byte == ((low >> 14) & 3u));
        CHECK(((UINT32_C(0xdda73519) >> (8 * byte)) & 255u) == rgba[i]);
        /* A host reference value is not an observation of GPU conversion. */
        CHECK((float)((UINT32_C(0xdda73519) >> (8 * byte)) & 255u) / 255.0f ==
              (float)rgba[i] / 255.0f);
    }
    r.packed_source.bank = SGX535_MOV_TEMP;
    CHECK(sgx535_texture2d_unpack_build(&r, words) == SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(words, temporary, sizeof(words)));
    for (uint32_t source = 0; source <= 127; source++) {
        r = unpack_request();
        r.packed_source.number = source;
        r.first_destination = 120;
        CHECK(sgx535_texture2d_unpack_build(&r, words) == SGX535_TEXTURE2D_OK);
        CHECK(((words[0] >> 7) & 127u) == source && (words[6] >> 21 & 127u) == 123);
    }
    r = unpack_request();
    r.packed_source = (struct sgx535_mov_operand){SGX535_MOV_TEMP, 123};
    r.first_destination = 0;
    CHECK(sgx535_texture2d_unpack_build(&r, words) == SGX535_TEXTURE2D_OK);
    union {
        struct sgx535_texture2d_unpack_request input;
        uint32_t output[8];
    } alias;
    alias.input = unpack_request();
    CHECK(sgx535_texture2d_unpack_build(&alias.input, alias.output) == SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(alias.output, primary, sizeof(primary)));
}

static void refuse_unpack(const struct sgx535_texture2d_unpack_request *r,
                          enum sgx535_texture2d_status status)
{
    uint32_t output[8], before[8];
    memset(output, 0x91, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(sgx535_texture2d_unpack_build(r, output) == status);
    CHECK(!memcmp(output, before, sizeof(output)));
}

static void invalid_unpack(void)
{
    struct sgx535_texture2d_unpack_request r = unpack_request();
    CHECK(sgx535_texture2d_unpack_build(NULL, NULL) == SGX535_TEXTURE2D_ERR_ARGUMENT);
    CHECK(sgx535_texture2d_unpack_build(&r, NULL) == SGX535_TEXTURE2D_ERR_ARGUMENT);
    refuse_unpack(NULL, SGX535_TEXTURE2D_ERR_ARGUMENT);
#define BAD(field, value, status)                                                                  \
    do {                                                                                           \
        r = unpack_request();                                                                      \
        r.field = value;                                                                           \
        refuse_unpack(&r, status);                                                                 \
    } while (0)
    BAD(layout.format, 0, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(layout.format, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(layout.dwords, 0, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(layout.dwords, 2, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(packed_source.bank, SGX535_MOV_OUTPUT, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(packed_source.bank, SGX535_MOV_SECONDARY, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(packed_source.bank, (enum sgx535_mov_bank) - 1, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    BAD(packed_source.number, 128, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(packed_source.number, UINT32_MAX, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(first_destination, 121, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(first_destination, UINT32_MAX, SGX535_TEXTURE2D_ERR_RANGE);
    for (uint32_t i = 0; i < 4; i++) {
        r = unpack_request();
        r.layout.rgba_byte[i] ^= 1u;
        refuse_unpack(&r, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
        r = unpack_request();
        r.packed_source = (struct sgx535_mov_operand){SGX535_MOV_TEMP, r.first_destination + i};
        refuse_unpack(&r, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    }
    r = unpack_request();
    r.packed_source = (struct sgx535_mov_operand){SGX535_MOV_TEMP, 124};
    refuse_unpack(&r, SGX535_TEXTURE2D_ERR_RANGE);
#undef BAD
}

static void primary_allocation(void)
{
    struct sgx535_texture2d_nearest_request r = request();
    struct sgx535_texture2d_nearest nearest;
    struct sgx535_texture2d_primary_input records[] = {{7, 2}, {8, 3}, {11, 1}, {12, 4}};
    struct sgx535_texture2d_primary_inputs inputs = {records, 4, 10, 11};
    struct sgx535_texture2d_primary_plan plan;
    static const uint32_t expected[] = {0xa1668285, 0x40a001bc, 0xa1854285, 0x40a001bc,
                                        0xa1a40285, 0x40a001bc, 0xa1c7c285, 0x40a001bc};
    CHECK(sgx535_texture2d_nearest_build(&r, &nearest) == SGX535_TEXTURE2D_OK);
    CHECK(sgx535_texture2d_primary_unpack_build(&nearest, &inputs, &plan) == SGX535_TEXTURE2D_OK);
    CHECK(plan.result_ssa == 11 && plan.record_index == 2 && plan.result_dwords == 1 &&
          plan.primary_dwords == 10 && plan.packed_source.bank == SGX535_MOV_PRIMARY &&
          plan.packed_source.number == 5 && !memcmp(plan.unpack, expected, sizeof(expected)));
    /* A different valid record order changes the PA base; it is not PA0. */
    struct sgx535_texture2d_primary_input reordered[] = {{11, 1}, {12, 4}, {7, 2}, {8, 3}};
    inputs.records = reordered;
    CHECK(sgx535_texture2d_primary_unpack_build(&nearest, &inputs, &plan) == SGX535_TEXTURE2D_OK);
    CHECK(plan.record_index == 0 && !plan.packed_source.number && plan.primary_dwords == 10 &&
          plan.unpack[0] == UINT32_C(0xa1668000));
    struct sgx535_texture2d_primary_input maximum[128];
    for (uint32_t i = 0; i < 128; i++)
        maximum[i] = (struct sgx535_texture2d_primary_input){i, 1};
    nearest.result_ssa = 127;
    inputs = (struct sgx535_texture2d_primary_inputs){maximum, 128, 128, 120};
    CHECK(sgx535_texture2d_primary_unpack_build(&nearest, &inputs, &plan) == SGX535_TEXTURE2D_OK);
    CHECK(plan.record_index == 127 && plan.packed_source.number == 127 && plan.result_dwords == 1 &&
          plan.primary_dwords == 128 && ((plan.unpack[0] >> 7) & 127u) == 127 &&
          ((plan.unpack[6] >> 21) & 127u) == 123);
    nearest.result_ssa = 11;
    inputs = (struct sgx535_texture2d_primary_inputs){records, 4, 10, 11};
    union {
        struct sgx535_texture2d_nearest texture;
        struct sgx535_texture2d_primary_plan output;
    } texture_alias;
    texture_alias.texture = nearest;
    CHECK(sgx535_texture2d_primary_unpack_build(&texture_alias.texture, &inputs,
                                                &texture_alias.output) == SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(texture_alias.output.unpack, expected, sizeof(expected)));
    union {
        struct sgx535_texture2d_primary_input records[4];
        struct sgx535_texture2d_primary_plan output;
    } records_alias;
    memcpy(records_alias.records, records, sizeof(records));
    inputs.records = records_alias.records;
    CHECK(sgx535_texture2d_primary_unpack_build(&nearest, &inputs, &records_alias.output) ==
          SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(records_alias.output.unpack, expected, sizeof(expected)));
}

static void refuse_primary(const struct sgx535_texture2d_nearest *nearest,
                           const struct sgx535_texture2d_primary_inputs *inputs,
                           enum sgx535_texture2d_status status)
{
    struct sgx535_texture2d_primary_plan result, before;
    memset(&result, 0x93, sizeof(result));
    memcpy(&before, &result, sizeof(before));
    CHECK(sgx535_texture2d_primary_unpack_build(nearest, inputs, &result) == status);
    CHECK(!memcmp(&result, &before, sizeof(result)));
}

static void invalid_primary(void)
{
    struct sgx535_texture2d_nearest_request r = request();
    struct sgx535_texture2d_nearest nearest;
    struct sgx535_texture2d_primary_input records[] = {{7, 2}, {11, 1}, {3, 4}};
    struct sgx535_texture2d_primary_inputs inputs = {records, 3, 7, 11};
    CHECK(sgx535_texture2d_nearest_build(&r, &nearest) == SGX535_TEXTURE2D_OK);
    refuse_primary(NULL, &inputs, SGX535_TEXTURE2D_ERR_ARGUMENT);
    refuse_primary(&nearest, NULL, SGX535_TEXTURE2D_ERR_ARGUMENT);
    CHECK(sgx535_texture2d_primary_unpack_build(&nearest, &inputs, NULL) ==
          SGX535_TEXTURE2D_ERR_ARGUMENT);
#define BAD(field, value, status)                                                                  \
    do {                                                                                           \
        struct sgx535_texture2d_primary_inputs changed = inputs;                                   \
        changed.field = value;                                                                     \
        refuse_primary(&nearest, &changed, status);                                                \
    } while (0)
    BAD(records, NULL, SGX535_TEXTURE2D_ERR_ARGUMENT);
    BAD(count, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(count, 129, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(primary_dwords, 0, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(primary_dwords, 129, SGX535_TEXTURE2D_ERR_RANGE);
    BAD(primary_dwords, 6, SGX535_TEXTURE2D_ERR_ARGUMENT);
    BAD(primary_dwords, 8, SGX535_TEXTURE2D_ERR_ARGUMENT);
    BAD(first_destination, 121, SGX535_TEXTURE2D_ERR_RANGE);
#undef BAD
    for (uint32_t i = 0; i < 3; i++) {
        struct sgx535_texture2d_primary_input original = records[i];
        records[i].dwords = 0;
        refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_RANGE);
        records[i].dwords = UINT32_MAX;
        refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_RANGE);
        records[i] = original;
        records[i].source_ssa = 512;
        refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_RANGE);
        records[i] = original;
    }
    records[1].dwords = 4;
    refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
    records[1].dwords = 1;
    records[2].source_ssa = 7;
    refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_ARGUMENT);
    records[2].source_ssa = 3;
    nearest.result_ssa = 511;
    refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_ARGUMENT);
    nearest.result_ssa = 512;
    refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_RANGE);
    nearest.result_ssa = 11;
    nearest.result.format = 2;
    refuse_primary(&nearest, &inputs, SGX535_TEXTURE2D_ERR_UNSUPPORTED);
}

static void descriptor_words(void)
{
    struct sgx535_texture2d_nearest_request r = request();
    struct sgx535_texture2d_nearest nearest, changed;
    struct sgx535_texture2d_cpu_binding binding = {UINT64_C(0x00500000), 4096};
    uint32_t words[3], before[3];
    const uint32_t expected[] = {0x03fe0090, 0x6c010012, 0x00500004};
    CHECK(sgx535_texture2d_nearest_build(&r, &nearest) == SGX535_TEXTURE2D_OK);
    CHECK(sgx535_texture2d_nearest_descriptor(&nearest, &binding, words) == SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(words, expected, sizeof(words)));
    CHECK(!(words[0] & UINT32_C(0x08000000)) && ((words[1] >> 24) & 31u) == 12u);
    for (unsigned i = 0; i < 8; i++) {
        struct sgx535_texture2d_cpu_binding memory = binding;
        changed = nearest;
        switch (i) {
        case 0:
            memory.gpu_byte_address = 0x500002;
            break;
        case 1:
            memory.gpu_byte_address = UINT64_C(0x100000000);
            break;
        case 2:
            memory.gpu_byte_address = UINT64_C(0xfffff004);
            break;
        case 3:
            memory.allocation_bytes = 4095;
            break;
        case 4:
            changed.width = 4097;
            break;
        case 5:
            changed.byte_offset = UINT64_MAX - 3;
            break;
        case 6:
            changed.span_bytes--;
            break;
        default:
            changed.height = 0;
            break;
        }
        memset(words, 0x65, sizeof(words));
        memcpy(before, words, sizeof(words));
        CHECK(sgx535_texture2d_nearest_descriptor(&changed, &memory, words) ==
              SGX535_TEXTURE2D_ERR_RANGE);
        CHECK(!memcmp(words, before, sizeof(words)));
    }
    for (unsigned i = 0; i < 6; i++) {
        changed = nearest;
        switch (i) {
        case 0:
            changed.sampler_controls |= UINT32_C(0x08000000);
            break;
        case 1:
            changed.stride_bytes = 132;
            break;
        case 2:
            changed.result.dwords = 2;
            break;
        case 3:
            changed.result.rgba_byte[0] = 0;
            break;
        case 4:
            changed.format = 2;
            break;
        default:
            changed.raw_samples = 2;
            break;
        }
        CHECK(sgx535_texture2d_nearest_descriptor(&changed, &binding, words) ==
              SGX535_TEXTURE2D_ERR_UNSUPPORTED);
        CHECK(!memcmp(words, before, sizeof(words)));
    }
    r.width = r.height = 1;
    r.byte_offset = 0;
    r.allocation_bytes = 128;
    binding = (struct sgx535_texture2d_cpu_binding){UINT64_C(0xffffff80), 128};
    CHECK(sgx535_texture2d_nearest_build(&r, &nearest) == SGX535_TEXTURE2D_OK);
    CHECK(sgx535_texture2d_nearest_descriptor(&nearest, &binding, words) == SGX535_TEXTURE2D_OK);
    CHECK(words[1] == UINT32_C(0x6c000000) && words[2] == UINT32_C(0xffffff80));
    union {
        struct sgx535_texture2d_nearest nearest;
        uint32_t words[3];
    } alias = {.nearest = nearest};
    CHECK(sgx535_texture2d_nearest_descriptor(&alias.nearest, &binding, alias.words) ==
          SGX535_TEXTURE2D_OK);
    CHECK(!memcmp(alias.words, words, sizeof(words)));
    CHECK(sgx535_texture2d_nearest_descriptor(NULL, &binding, words) ==
          SGX535_TEXTURE2D_ERR_ARGUMENT);
}

int main(void)
{
    words_and_spans();
    invalid();
    unpack_words();
    invalid_unpack();
    primary_allocation();
    invalid_primary();
    descriptor_words();
    printf("Nearest2D sampler/span/layout/PA/UNPACK CPU checks: %u; native sampling unqualified\n",
           checks);
    return 0;
}
