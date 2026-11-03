// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The draw-mask objects (src/raster/sgx535_draw_mask.c): EMGD-form word vectors
 * and the scissor sequence (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues), the
 * bottom-up rectangle and
 * the refusals. CPU only. */
#include "sgx535_draw_mask.h"
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

static bool rect_is(const struct sgx535_rect *r, int x0, int y0, int x1, int y1)
{
    return r->x0 == x0 && r->y0 == y0 && r->x1 == x1 && r->y1 == y1;
}

int main(void)
{
    struct sgx535_rect a, b, empty;
    struct sgx535_mask_object o[2];
    /* GL (16, 16, 32, 32) in the 256-high window: rows 208..240 */
    CHECK(sgx535_scissor_rect(16, 16, 32, 32, 256, false, &a) && rect_is(&a, 16, 208, 48, 240));
    CHECK(sgx535_scissor_rect(16, 16, 32, 32, 256, true, &b) && rect_is(&b, 16, 16, 48, 48));
    CHECK(sgx535_scissor_rect(176, -16, 316, 96, 256, false, &b) &&
          rect_is(&b, 176, 176, 492, 272));
    CHECK(sgx535_scissor_rect(0, 64, 0, 0, 256, false, &empty));
    /* from the whole target: clear it, set the scissor (EMGD's words) */
    CHECK(sgx535_draw_mask_objects(NULL, &a, 256, 256, o) == 2);
    CHECK(o[0].ispa == 0x07f00300 && o[0].ispb == 0xf0 && o[0].ispc == 0 &&
          rect_is(&o[0].rect, 0, 0, 256, 256));
    CHECK(o[1].ispa == 0x07f00300 && o[1].ispc == 0x02000000 &&
          rect_is(&o[1].rect, 16, 208, 48, 240));
    /* one scissor to the next: clear the old, set the new */
    CHECK(sgx535_draw_mask_objects(&a, &b, 256, 256, o) == 2 && o[0].ispc == 0 &&
          rect_is(&o[0].rect, 16, 208, 48, 240) && o[1].ispc == 0x02000000 &&
          rect_is(&o[1].rect, 176, 176, 492, 272));
    /* to an empty scissor: only the clear; from it: only the set */
    CHECK(sgx535_draw_mask_objects(&b, &empty, 256, 256, o) == 1 && o[0].ispc == 0);
    CHECK(sgx535_draw_mask_objects(&empty, &a, 256, 256, o) == 1 && o[0].ispc == 0x02000000);
    /* scissor off: the whole target set; unchanged: nothing */
    CHECK(sgx535_draw_mask_objects(&a, NULL, 256, 256, o) == 1 && o[0].ispc == 0x02000000 &&
          rect_is(&o[0].rect, 0, 0, 256, 256));
    CHECK(sgx535_draw_mask_objects(NULL, NULL, 256, 256, o) == 0);
    CHECK(sgx535_draw_mask_objects(&a, &a, 256, 256, o) == 0);
    /* refusals */
    CHECK(!sgx535_scissor_rect(0, 0, -1, 4, 256, false, &a));
    CHECK(!sgx535_scissor_rect(0, 0, 1, 1, 8192, false, &a));
    CHECK(sgx535_draw_mask_objects(NULL, &a, 0, 256, o) < 0);
    CHECK(sgx535_draw_mask_objects(NULL, &a, 256, 8192, o) < 0);
    CHECK(sgx535_draw_mask_objects(NULL, &a, 256, 256, NULL) < 0);
    printf("PASS: sgx535 draw mask (%lu checks)\n", checks);
    return 0;
}
