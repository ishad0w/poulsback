// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_fragment_raw_sign.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#include "fragment_raw_sign_reference.h"

static void word(uint32_t input)
{
    struct sgx535_fragment_instruction ir[7] = {0};
    struct sgx535_fragment_program program, refused, old;
    uint32_t output[4], dummy[4] = {0}, expected = ~input;
    unsigned lane;
    ir[0].opcode = ir[1].opcode = SGX535_FRAGMENT_CONSTANT;
    ir[0].immediate = input;
    ir[1].immediate = UINT32_MAX;
    ir[2].opcode = SGX535_FRAGMENT_RAW_XOR;
    ir[2].source[0] = 0;
    ir[2].source[1] = 1;
    for (lane = 0; lane < 4; lane++) {
        ir[3 + lane].opcode = SGX535_FRAGMENT_OUTPUT;
        ir[3 + lane].source[0] = 2;
        ir[3 + lane].index = lane;
    }
    CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 7, &program) == SGX535_NIR_OK);
    CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
    raw_words(&program.encoding, dummy, 0, output);
    for (lane = 0; lane < 4; lane++)
        CHECK(output[lane] == expected);
    memset(&refused, 0x37, sizeof(refused));
    old = refused;
    CHECK(sgx535_fragment_compile(ir, 7, SGX535_FRAGMENT_RAW_F32_WORDS, &refused) ==
          SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!memcmp(&refused, &old, sizeof(old)));
}

int main(void)
{
    static const uint32_t values[] = {
        0,           UINT32_MAX,  0x80000000u, 1u,          0x7fffffffu,
        0x7fc12345u, 0xff800000u, 0x7f800000u, 0xaaaaaaaau, 0x55555555u,
    };
    unsigned i;
    for (i = 0; i < sizeof(values) / sizeof(*values); i++)
        word(values[i]);
    for (i = 0; i < 32; i++) {
        word(UINT32_C(1) << i);
        word(~(UINT32_C(1) << i));
    }
    printf("INOT XOR-all-ones full-word consumer: %u checks PASS (CPU only; NIR NOTRUN)\n", checks);
    return 0;
}
