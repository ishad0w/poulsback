// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The region clip words (src/raster/sgx535_region_clip.c) against EMGD-form
 * scissor vectors (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues), the bottom-up form
 * and the refusals. CPU only. */
#include "sgx535_region_clip.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static const struct {
    const char *name;
    struct sgx535_scissor scissor;
    uint32_t words[2];
} cases[] = {
    {"no scissor", {0, 0, 0, 0, 0}, {0x8000000f, 0x0000000f}},
    {"16..48 in tile 0", {1, 16, 16, 32, 32}, {0x80010002, 0x000d000e}},
    {"one pixel", {1, 96, 32, 1, 1}, {0x80060006, 0x000d000d}},
    {"clear in tile 2", {1, 144, 16, 32, 32}, {0x8009000a, 0x000d000e}},
    {"past the window", {1, 176, -16, 316, 96}, {0x800b001e, 0x000b0010}},
    {"empty", {1, 0, 64, 0, 0}, {0x80100010, 0x00000000}},
    {"depth clear in tile 6", {1, 144, 80, 32, 32}, {0x8009000a, 0x0009000a}},
};

int main(void)
{
    uint32_t w[2];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(sgx535_region_clip(&cases[i].scissor, 256, 256, false, w));
        if (w[0] != cases[i].words[0] || w[1] != cases[i].words[1])
            fprintf(stderr, "%s: %08x %08x\n", cases[i].name, w[0], w[1]);
        CHECK(w[0] == cases[i].words[0] && w[1] == cases[i].words[1]);
    }
    /* bottom-up: rows from the bottom */
    CHECK(sgx535_region_clip(&cases[1].scissor, 256, 256, true, w) && w[0] == 0x80010002 &&
          w[1] == 0x00010002);
    /* a 64x64 FBO without a scissor */
    CHECK(sgx535_region_clip(&cases[0].scissor, 64, 64, true, w) && w[0] == 0x80000003 &&
          w[1] == 0x00000003);
    CHECK(!sgx535_region_clip(&(struct sgx535_scissor){1, 0, 0, 5000, 16}, 256, 256, false, w));
    CHECK(!sgx535_region_clip(&cases[0].scissor, 0, 256, false, w));
    CHECK(!sgx535_region_clip(&cases[0].scissor, 8192, 256, false, w));
    CHECK(!sgx535_region_clip(NULL, 256, 256, false, w));
    printf("PASS: sgx535 region clip (%lu checks, %zu EMGD draws)\n", checks,
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
