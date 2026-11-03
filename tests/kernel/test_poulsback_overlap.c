// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The engine's overlap rules: orderings of TA passes, raster passes and 2D
 * jobs, the BANK0 words as literals, refusals, and random event sequences
 * against the invariants. */
#include "../../kernel/engine/poulsback_overlap.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "overlap check %u line %d: %s\n", checks, __LINE__, #x);               \
        }                                                                                          \
    } while (0)

/* BANK0 words: EDM 3:0, TA 7:4, HOST 11:8, 3D 15:12, 2D 19:16. */
#define IDLE 0x000fffffu
#define TA 0x000f0f0fu          /* a TA pass (beside a raster or not): TA and 3D at slot 0 */
#define RASTER 0x000f0f0fu      /* rasters only: 3D and TA at slot 0 (HW-ADDR-007) */
#define TWOD 0x0001ff1fu        /* a 2D job alone: TA and 2D at slot 1 */
#define RASTER_TWOD 0x00010f1fu /* a 2D job beside a raster */

enum {
    A3 = POULSBACK_OVERLAP_ARRIVE3D,
    A2 = POULSBACK_OVERLAP_ARRIVE2D,
    TAF = POULSBACK_OVERLAP_TA_FINISHED,
    D3 = POULSBACK_OVERLAP_DONE3D,
    D2 = POULSBACK_OVERLAP_DONE2D
};
static bool event(struct poulsback_overlap *s, int e, bool joins,
                  struct poulsback_overlap_start *o)
{
    return poulsback_overlap_event(s, (enum poulsback_overlap_event)e, joins, o);
}

/* One event that fits: what it starts (raster, TA, 2D) and the BANK0 after it. */
static void step(struct poulsback_overlap *s, int e, bool joins, bool raster, bool ta, bool twod,
                 sgx535_u32 bank0, int line)
{
    struct poulsback_overlap_start o;
    bool fits = event(s, e, joins, &o);
    checks++;
    if (!fits || o.raster != raster || o.ta != ta || o.twod != twod || o.bank0 != bank0 ||
        poulsback_overlap_bank0(s) != bank0) {
        failures++;
        fprintf(stderr, "overlap line %d: event %d fits %d starts %d %d %d bank0 %08x\n", line, e,
                fits, o.raster, o.ta, o.twod, o.bank0);
    }
}
#define STEP(s, e, j, r, t, d, b) step(s, e, j, r, t, d, b, __LINE__)

/* An event that does not fit: refused, the state unchanged. */
static void refused(struct poulsback_overlap *s, int e, int line)
{
    struct poulsback_overlap before = *s;
    struct poulsback_overlap_start o;
    checks++;
    if (event(s, e, true, &o) || memcmp(&before, s, sizeof(before))) {
        failures++;
        fprintf(stderr, "overlap line %d: event %d not refused\n", line, e);
    }
}
#define REFUSED(s, e) refused(s, e, __LINE__)

static unsigned rng = 1;
static unsigned next_random(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 16;
}

int main(void)
{
    struct poulsback_overlap s = {0};
    CHECK(poulsback_overlap_bank0(&s) == IDLE);
    CHECK(POULSBACK_OVERLAP_BANK0_IDLE == IDLE);

    /* Frames with the next TA inside the raster: frame N's raster, N+1's TA,
     * then the copy of N-1 once the TA finished (docs/kernel/execution.md). */
    STEP(&s, A3, true, false, true, false, TA);
    STEP(&s, A3, true, false, false, false, TA); /* N+1 waits for the TA unit */
    STEP(&s, TAF, true, true, true, false, TA);  /* N's raster, N+1's TA beside it */
    STEP(&s, A2, true, false, false, false, TA); /* the copy waits for the TA */
    STEP(&s, A3, true, false, false, false, TA); /* N+2 waits: credits full */
    REFUSED(&s, A3);
    REFUSED(&s, A2);
    STEP(&s, TAF, true, false, false, true, RASTER_TWOD); /* N+1 ready; both contexts held */
    CHECK(s.ready && s.raster && s.wait3d == 1);
    STEP(&s, D3, true, true, false, false, RASTER_TWOD); /* N+1's raster; N+2 waits for the copy */
    STEP(&s, D2, true, false, true, false, TA);          /* then N+2's TA */
    STEP(&s, A2, true, false, false, false, TA);
    STEP(&s, TAF, true, false, false, true, RASTER_TWOD);
    STEP(&s, D2, true, false, false, false, RASTER);
    STEP(&s, D3, true, true, false, false, RASTER);
    STEP(&s, D3, true, false, false, false, IDLE);
    CHECK(!s.ta && !s.ready && !s.raster && !s.wait3d && !s.run2d && !s.wait2d);

    /* A TA that may not join (another directory, or the raster's region
     * array) waits until no job holds a context. */
    STEP(&s, A3, false, false, true, false, TA);
    STEP(&s, TAF, false, true, false, false, RASTER);
    STEP(&s, A3, false, false, false, false, RASTER);
    STEP(&s, A2, false, false, false, true, RASTER_TWOD);
    STEP(&s, D3, false, false, false, false, TWOD); /* nothing holds, but the copy runs */
    STEP(&s, D2, false, false, true, false, TA);
    STEP(&s, TAF, false, true, false, false, RASTER);
    STEP(&s, D3, false, false, false, false, IDLE);

    /* 2D alone; 3D jobs arriving during it wait, in order. */
    STEP(&s, A2, true, false, false, true, TWOD);
    STEP(&s, A3, true, false, false, false, TWOD);
    STEP(&s, A3, true, false, false, false, TWOD);
    STEP(&s, A3, true, false, false, false, TWOD);
    REFUSED(&s, A3);
    CHECK(s.wait3d == 3);
    STEP(&s, D2, true, false, true, false, TA);
    STEP(&s, TAF, true, true, true, false, TA);
    STEP(&s, TAF, true, false, false, false, RASTER); /* ready behind the raster */
    REFUSED(&s, TAF);
    STEP(&s, D3, true, true, true, false, TA);
    STEP(&s, TAF, true, false, false, false, RASTER);
    STEP(&s, D3, true, true, false, false, RASTER);
    STEP(&s, D3, true, false, false, false, IDLE);

    /* Refusals from idle and per phase. */
    REFUSED(&s, TAF);
    REFUSED(&s, D3);
    REFUSED(&s, D2);
    STEP(&s, A3, true, false, true, false, TA);
    REFUSED(&s, D3); /* no raster before the TA finished */
    REFUSED(&s, D2);
    STEP(&s, TAF, true, true, false, false, RASTER);
    STEP(&s, D3, true, false, false, false, IDLE);

    /* Random sequences of events the schedulers and the GPU can produce. */
    for (unsigned run = 0; run < 4000; run++) {
        struct poulsback_overlap r = {0};
        unsigned in3d = 0;
        for (unsigned n = 0; n < 200; n++) {
            struct poulsback_overlap_start o;
            int e = (int)(next_random() % 5u);
            bool joins = (next_random() & 3u) != 0;
            bool legal = e == A3   ? in3d < (unsigned)POULSBACK_OVERLAP_CREDITS_3D
                         : e == A2  ? !r.wait2d && !r.run2d
                         : e == TAF ? r.ta
                         : e == D3  ? r.raster
                                    : r.run2d;
            struct poulsback_overlap before = r;
            bool fits = event(&r, e, joins, &o);
            int held;
            if (fits != legal) {
                CHECK(fits == legal);
                break;
            }
            if (!fits) {
                CHECK(!memcmp(&before, &r, sizeof(r)));
                continue;
            }
            in3d += e == A3;
            in3d -= e == D3;
            held = r.ready + r.raster;
            checks++;
            /* Never a TA pass beside a 2D job; at most two DPM contexts held; a
             * ready job only behind a raster; a waiting 2D job only behind a TA;
             * a waiting 3D job only behind what blocks it; starts as the state
             * says; BANK0 by the formula. */
            if ((r.ta && r.run2d) || r.ta + held > 2 || (r.ready && !r.raster) ||
                (r.wait2d && !r.ta) ||
                (r.wait3d && !r.ta && !r.run2d && held < 2 && (!held || joins)) ||
                (o.ta && o.twod) || (o.ta && !r.ta) || (o.twod && !r.run2d) ||
                (o.raster && !r.raster) ||
                (unsigned)(r.wait3d + r.ta + held) != in3d ||
                o.bank0 != (r.run2d ? (r.ta || held ? RASTER_TWOD : TWOD)
                            : r.ta  ? TA
                            : held  ? RASTER
                                    : IDLE)) {
                failures++;
                fprintf(stderr, "overlap random run %u step %u event %d: ta %d ready %d raster %d "
                        "run2d %d wait3d %u wait2d %d bank0 %08x\n", run, n, e, r.ta, r.ready,
                        r.raster, r.run2d, r.wait3d, r.wait2d, o.bank0);
                break;
            }
        }
    }

    printf("overlap: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
