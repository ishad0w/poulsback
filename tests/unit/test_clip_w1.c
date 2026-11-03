// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_clip_w1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #x);                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static uint32_t read_le(const unsigned char *raw)
{
    return (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
           ((uint32_t)raw[3] << 24);
}

static void refuse(uint32_t *clip, uint32_t width, uint32_t height,
                   enum sgx535_clip_w1_status status)
{
    uint32_t out[4] = {1u, 2u, 3u, 4u};
    const uint32_t original[4] = {1u, 2u, 3u, 4u};

    CHECK(sgx535_clip_w1_encode(clip, width, height, out) == status);
    CHECK(!memcmp(out, original, sizeof(out)));
}

int main(int argc, char **argv)
{
    FILE *vectors;
    unsigned char raw[40];
    uint32_t clip[4] = {0, 0, 0, 0x3f800000u}, out[4];
    unsigned records = 0, index;
    const uint32_t unsupported[] = {0x3f800001u, 0xbf800001u, 0x7f800000u, 0xff800000u,
                                    0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xffffffffu};

    CHECK(argc == 2);
    vectors = fopen(argv[1], "rb");
    CHECK(vectors != NULL);
    while (fread(raw, 1, sizeof(raw), vectors) == sizeof(raw)) {
        uint32_t expected[4], width = read_le(raw + 16), height = read_le(raw + 20);

        for (index = 0; index < 4u; index++) {
            clip[index] = read_le(raw + index * 4u);
            expected[index] = read_le(raw + 24u + index * 4u);
        }
        CHECK(sgx535_clip_w1_encode(clip, width, height, out) == SGX535_CLIP_W1_OK);
        CHECK(!memcmp(out, expected, sizeof(out)));
        CHECK(sgx535_clip_w1_encode(clip, width, height, clip) == SGX535_CLIP_W1_OK);
        CHECK(!memcmp(clip, expected, sizeof(clip)));
        records++;
    }
    CHECK(feof(vectors) && !ferror(vectors));
    CHECK(fclose(vectors) == 0);
    CHECK(records >= 20000u);
    clip[0] = clip[1] = clip[2] = 0;
    clip[3] = 0x3f800000u;
    refuse(NULL, 64u, 64u, SGX535_CLIP_W1_INVALID);
    CHECK(sgx535_clip_w1_encode(clip, 64u, 64u, NULL) == SGX535_CLIP_W1_INVALID);
    refuse(clip, 0u, 64u, SGX535_CLIP_W1_INVALID);
    refuse(clip, 64u, 0u, SGX535_CLIP_W1_INVALID);
    refuse(clip, 4097u, 64u, SGX535_CLIP_W1_INVALID);
    refuse(clip, 64u, 4097u, SGX535_CLIP_W1_INVALID);
    refuse(clip, 0xffffffffu, 0xffffffffu, SGX535_CLIP_W1_INVALID);
    for (index = 0; index < sizeof(unsupported) / sizeof(unsupported[0]); index++) {
        unsigned axis;

        for (axis = 0; axis < 3u; axis++) {
            clip[axis] = unsupported[index];
            refuse(clip, 64u, 64u, SGX535_CLIP_W1_UNSUPPORTED);
            clip[axis] = 0;
        }
    }
    for (index = 0; index < 32u; index++) {
        clip[3] = 0x3f800000u ^ (1u << index);
        refuse(clip, 64u, 64u, SGX535_CLIP_W1_UNSUPPORTED);
    }
    printf(
        "PASS integer-only W1 viewport: %u rational vectors, %lu checks; hardware format unqualified\n",
        records, checks);
    return 0;
}
