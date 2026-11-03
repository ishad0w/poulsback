// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The vblank count and the flip completion it gives (HW-DISP-004): a flip's
 * event is due at the first interrupt after the count read that follows its
 * BASE store whose count is past that read. Never before the latch, at most
 * one frame after it. */
#include "../../kernel/display/poulsback_display_vblank.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "display-vblank check %u line %d: %s\n", checks, __LINE__, #x);        \
        }                                                                                          \
    } while (0)

/* The Dell's panel mode: 1419 x 823 pixel clocks per frame. */
#define HTOTAL 0x058a04ffu
#define VBLANK 0x0336031fu
#define WIDTH 1419u
#define FRAME (WIDTH * 823u)
#define THRESHOLD (799u * WIDTH)

static sgx535_u32 count_at(sgx535_u64 t)
{
    return poulsback_display_vblank_count((sgx535_u32)(t / FRAME) & 0xffffff,
                                          (sgx535_u32)(t % FRAME), HTOTAL, VBLANK);
}

/* Index of the vblank at which an interrupt delivers a flip stored at ts whose
 * count was read at tr; the interrupt of vblank k runs latency clocks after
 * the vblank start at k * FRAME + latch. */
static sgx535_u64 delivered(sgx535_u64 tr, sgx535_u32 latch, sgx535_u32 latency)
{
    sgx535_u32 target = (count_at(tr) + 1) & 0xffffff;
    for (sgx535_u64 k = tr / FRAME - 1;; k++) {
        sgx535_u64 ti = k * FRAME + latch + latency;
        if (ti > tr && ((count_at(ti) - target) & 0xffffff) < 0x800000)
            return k;
    }
}

int main(void)
{
    static const sgx535_u32 latches[] = {THRESHOLD, 799u * WIDTH + 1328u, 800u * WIDTH};
    static const sgx535_u32 latencies[] = {0, 2318, 30000, 500000};
    static const sgx535_u32 delays[] = {0, 60, 2500};
    static const sgx535_u64 starts[] = {5, 0xfffffe};
    unsigned exact = 0, late = 0;

    CHECK(poulsback_display_vblank_count(10, 0, HTOTAL, VBLANK) == 10);
    CHECK(poulsback_display_vblank_count(10, THRESHOLD - 1, HTOTAL, VBLANK) == 10);
    CHECK(poulsback_display_vblank_count(10, THRESHOLD, HTOTAL, VBLANK) == 11);
    CHECK(poulsback_display_vblank_count(10, FRAME - 1, HTOTAL, VBLANK) == 11);
    CHECK(poulsback_display_vblank_count(0xffffff, 0, HTOTAL, VBLANK) == 0xffffff);
    CHECK(poulsback_display_vblank_count(0xffffff, THRESHOLD, HTOTAL, VBLANK) == 0);

    for (unsigned s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
        for (unsigned l = 0; l < sizeof(latches) / sizeof(latches[0]); l++)
            for (unsigned i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++)
                for (unsigned d = 0; d < sizeof(delays) / sizeof(delays[0]); d++)
                    for (sgx535_u64 p = 0; p < 2 * FRAME; p += 97) {
                        sgx535_u64 ts = starts[s] * FRAME + p, tr = ts + delays[d];
                        /* The store latches at the first vblank start after it. */
                        sgx535_u64 k_latch = ts / FRAME + (ts % FRAME >= latches[l]);
                        sgx535_u64 k = delivered(tr, latches[l], latencies[i]);
                        CHECK(k >= k_latch);
                        CHECK(k <= k_latch + 1);
                        if (k == k_latch)
                            exact++;
                        else
                            late++;
                    }
    /* Late deliveries come only from reads in the margin before the vblank
     * start or across it: a small share. */
    CHECK(late * 50 < exact);
    printf("display-vblank: %u checks, %u failures (%u exact, %u one frame late)\n", checks,
           failures, exact, late);
    return failures != 0;
}
