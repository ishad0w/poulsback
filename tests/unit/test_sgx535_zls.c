// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The ZLS words (src/raster/sgx535_zls.c): EMGD's window renders
 * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues), the D24S8 load and store word
 * the window depth path uses on the Dell (sgx535_depth_state.c), D16, and the
 * refusals. CPU only. */
#include "sgx535_zls.h"
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

int main(void)
{
    struct sgx535_zls_words w;
    /* EMGD's window: extent only, clear depth 1.0, BGOBJ 0x300 */
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D24S8, 640, 0, 0, 1.0f, 0}, &w));
    CHECK(w.zlsctl == 0x2700 && w.bgobjdepth == 0x3f800000 && w.bgobj == 0x300);
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D24S8, 300, 0, 0, 1.0f, 0}, &w));
    CHECK(w.zlsctl == 0x1200);
    /* no depth buffer at all */
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_NONE, 256, 0, 0, 1.0f, 0}, &w));
    CHECK(w.zlsctl == 0x0f00);
    /* the window depth path's D24S8 load and store at 64x64 */
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D24S8, 64, 1, 1, 0.25f, 0x33},
                           &w));
    CHECK(w.zlsctl == 0x006503f6 && w.bgobjdepth == 0x3e800000 && w.bgobj == 0x333);
    /* store only, load only; D16 at 300 (pitch 320: extent 19) */
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D24S8, 64, 0, 1, 1.0f, 0}, &w));
    CHECK(w.zlsctl == 0x004503d0);
    CHECK(sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D16, 300, 1, 0, 1.0f, 0}, &w));
    CHECK(w.zlsctl == 0x002a1326);
    /* refusals */
    CHECK(!sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_NONE, 64, 1, 0, 1.0f, 0}, &w));
    CHECK(!sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D16, 0, 0, 0, 1.0f, 0}, &w));
    CHECK(!sgx535_zls_words(&(struct sgx535_zls_request){SGX535_ZLS_D16, 8192, 0, 0, 1.0f, 0}, &w));
    CHECK(!sgx535_zls_words(&(struct sgx535_zls_request){3, 64, 0, 0, 1.0f, 0}, &w));
    CHECK(!sgx535_zls_words(NULL, &w));
    printf("PASS: sgx535 ZLS words (%lu checks)\n", checks);
    return 0;
}
