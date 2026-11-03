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

int main(void)
{
    unsigned a, b, numeric, i;
    for (a = 0; a < 2; a++)
        for (b = 0; b < 2; b++)
            for (numeric = 0; numeric < 2; numeric++) {
                struct sgx535_fragment_instruction ir[9] = {0};
                struct sgx535_fragment_program program, refused, old;
                uint32_t output[4], input[4] = {0};
                uint32_t converted_true = numeric ? UINT32_C(0x3f800000) : 1u;
                /* Independent boolean oracle: logical conjunction, then exact
                 * +0/+1 or integer0/1 bits. No floating evaluation or tolerance. */
                uint32_t expected = a && b ? converted_true : 0;
                ir[0].opcode = ir[1].opcode = ir[3].opcode = SGX535_FRAGMENT_CONSTANT;
                ir[0].immediate = a ? UINT32_MAX : 0;
                ir[1].immediate = b ? UINT32_MAX : 0;
                ir[2].opcode = ir[4].opcode = SGX535_FRAGMENT_RAW_AND;
                ir[2].source[0] = 0;
                ir[2].source[1] = 1;
                ir[3].immediate = converted_true;
                ir[4].source[0] = 2;
                ir[4].source[1] = 3;
                for (i = 0; i < 4; i++) {
                    ir[5 + i].opcode = SGX535_FRAGMENT_OUTPUT;
                    ir[5 + i].source[0] = 4;
                    ir[5 + i].index = i;
                }
                CHECK(sgx535_fragment_compile_raw_sign_candidate(ir, 9, &program) == SGX535_NIR_OK);
                CHECK(sgx535_fragment_native_admission(&program) == SGX535_NIR_ERR_UNSUPPORTED);
                raw_words(&program.encoding, input, 0, output);
                for (i = 0; i < 4; i++)
                    CHECK(output[i] == expected);
                memset(&refused, 0x9d, sizeof(refused));
                old = refused;
                CHECK(sgx535_fragment_compile(ir, 9, SGX535_FRAGMENT_RAW_F32_WORDS, &refused) ==
                      SGX535_NIR_ERR_UNSUPPORTED);
                CHECK(!memcmp(&old, &refused, sizeof(old)));
            }
    printf("canonical bool32/iand raw-word consumer: %u checks PASS (CPU only; NIR NOTRUN)\n",
           checks);
    return 0;
}
