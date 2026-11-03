// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The shared USSE encoder against words whose rev121 text was checked
 * (docs/re/sgx535/usse-instruction-contracts.md; --dump prints the words and text). No GPU
 * execution is implied. */
#include "sgx535_use_encode.h"
#include <stdio.h>
#include <string.h>

#define R(n) {SGX535_USE_R, n, 0}
#define O(n) {SGX535_USE_O, n, 0}
#define PA(n) {SGX535_USE_PA, n, 0}
#define SA(n) {SGX535_USE_SA, n, 0}
#define C(n) {SGX535_USE_C, n, 0}
#define G(n) {SGX535_USE_G, n, 0}
#define IMM(n) {SGX535_USE_IMM, n, 0}
#define NEG(b, n) {SGX535_USE_##b, n, SGX535_USE_NEG}
#define ABS(b, n) {SGX535_USE_##b, n, SGX535_USE_ABS}
#define NONE {SGX535_USE_R, 0, 0}

struct encoded {
    struct sgx535_use_instruction in;
    uint32_t w0, w1;
    const char *text; /* rev121's text for w0, w1 */
};

#define OP(o, ...) {.opcode = SGX535_USE_##o, __VA_ARGS__}
static const struct encoded cases[] = {
#include "sgx535_use_encode_cases.inc"
};

/* Instructions the encoder must refuse. */
static const struct sgx535_use_instruction refused[] = {
    OP(FMAD, .dst = R(124), .src = {R(0), R(0), R(0)}),
    OP(FMAD, .dst = R(1), .src = {SA(0), R(0), R(0)}), /* src0 has no extension */
    OP(FDSX, .dst = R(1), .src = {NONE, R(2), R(3)}),  /* sources differ */
    OP(FDSX, .dst = R(1), .src = {NONE, SA(2), SA(2)}),
    OP(FRCP, .dst = R(1), .src = {NONE, R(2), R(3)}),
    OP(MOV, .dst = R(1), .src = {NONE, NEG(R, 2), NONE}),
    OP(TEST, .test = 6, .src = {NONE, R(1), R(2)}),
    OP(TEST, .end = 1, .src = {NONE, R(1), R(2)}),
    OP(PCKU8F32, .mask = 7, .dst = O(0), .src = {NONE, R(0), R(1)}),
    OP(SMP, .dst = O(0), .src = {R(0), SA(3), NONE}),
    OP(SMP, .end = 1, .dst = R(0), .src = {R(0), SA(3), NONE}),
    OP(SOP2, .dst = O(0), .src = {NONE, PA(64), O(0)}),
    OP(BR, .immediate = 0x1000),
    OP(LIMM, .dst = SA(1)),
    {.opcode = 200},
};

int main(int argc, char **argv)
{
    unsigned failures = 0, i;
    int dump = argc == 2 && !strcmp(argv[1], "--dump");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t w[2] = {0, 0};
        enum sgx535_use_status status = sgx535_use_encode(&cases[i].in, w);
        if (dump) {
            printf("%08x %08x %s\n", w[0], w[1], cases[i].text);
            continue;
        }
        if (status != SGX535_USE_OK || w[0] != cases[i].w0 || w[1] != cases[i].w1) {
            fprintf(stderr, "case %u (%s): status %d words %08x %08x\n", i, cases[i].text, status,
                    w[0], w[1]);
            failures++;
        }
    }
    for (i = 0; !dump && i < sizeof(refused) / sizeof(refused[0]); i++) {
        uint32_t w[2] = {0x12345678, 0x9abcdef0};
        if (sgx535_use_encode(&refused[i], w) == SGX535_USE_OK || w[0] != 0x12345678 ||
            w[1] != 0x9abcdef0) {
            fprintf(stderr, "refusal %u encoded\n", i);
            failures++;
        }
    }
    if (!dump && sgx535_use_encode(NULL, NULL) != SGX535_USE_ERR_ARGUMENT)
        failures++;
    if (!dump)
        printf("use-encode: %zu encodings, %zu refusals, %u failures\n",
               sizeof(cases) / sizeof(cases[0]), sizeof(refused) / sizeof(refused[0]), failures);
    return failures ? 1 : 0;
}
