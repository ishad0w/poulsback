// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The engine's teardown decisions (kernel/engine/poulsback_teardown.c):
 * power-down words as literals, drain and idle checks, and release over every
 * combination of engine state, then a lifecycle from claim to free.
 * The contract is in docs/kernel/execution.md. */
#include "../../kernel/engine/poulsback_teardown.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "teardown check %u line %d: %s\n", checks, __LINE__, #x);              \
        }                                                                                          \
    } while (0)

static enum poulsback_teardown_refusal idle(sgx535_u32 e1, sgx535_u32 e2, sgx535_u32 blit,
                                            sgx535_u32 bif)
{
    struct poulsback_teardown_idle r = {e1, e2, blit, bif};
    return poulsback_teardown_idle(&r);
}

int main(void)
{
    /* P: SGXDeinitialise as captured, w: o=a58 ... o=0, every value 0. */
    static const sgx535_u32 captured[8] = {0xa58, 0xa68, 0xa6c, 0xa70, 0xa48, 0x130, 0x110, 0x0};
    struct poulsback_teardown_engine e;
    unsigned ok = 0, combos = 0;

    CHECK(POULSBACK_TEARDOWN_POWER_DOWN_WRITES == 8);
    for (unsigned i = 0; i < 8; i++)
        CHECK(poulsback_teardown_power_down[i].offset == captured[i] &&
              poulsback_teardown_power_down[i].value == 0);

    /* Q's idle reads: EVENT_STATUS's master bit alone is idle; any other
     * event bit, any EVENT_STATUS2 bit, the 2D core busy or a BIF fault is not. */
    CHECK(idle(0, 0, 0, 0) == POULSBACK_TEARDOWN_OK);
    CHECK(idle(0x80000000u, 0, 0, 0) == POULSBACK_TEARDOWN_OK);
    CHECK(idle(0x80000000u, 0, 0x00ffffffu, 0) == POULSBACK_TEARDOWN_OK);
    CHECK(idle(0x80000002u, 0, 0, 0) == POULSBACK_TEARDOWN_EVENT);
    CHECK(idle(0x00040000u, 0, 0, 0) == POULSBACK_TEARDOWN_EVENT);
    CHECK(idle(0, 0x10u, 0, 0) == POULSBACK_TEARDOWN_EVENT);
    CHECK(idle(0, 0, 0x01000000u, 0) == POULSBACK_TEARDOWN_TWOD);
    CHECK(idle(0, 0, 0x01000005u, 0) == POULSBACK_TEARDOWN_TWOD);
    CHECK(idle(0, 0, 0, 0x40u) == POULSBACK_TEARDOWN_FAULT);
    CHECK(idle(0x80000001u, 0, 0x01000000u, 1) == POULSBACK_TEARDOWN_EVENT);

    /* Q's drain: lost first (HOLD), then a held job, then a job in a slot. */
    e = (struct poulsback_teardown_engine){.ready = 0, .slots = 2, .held = 1};
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_LOST);
    e.ready = 1;
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_HELD);
    e.held = 0;
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_BUSY);
    e.slots = 0;
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_OK);

    /* F over every combination: free only when ready, quiesced, and with no
     * job, hold or live fence; otherwise the first refusal in that order. */
    for (int ready = 0; ready < 2; ready++)
        for (int quiesced = 0; quiesced < 2; quiesced++)
            for (sgx535_u32 slots = 0; slots < 3; slots++)
                for (sgx535_u32 held = 0; held < 2; held++)
                    for (sgx535_u32 fences = 0; fences < 4; fences++) {
                        enum poulsback_teardown_refusal want =
                            !ready      ? POULSBACK_TEARDOWN_LOST
                            : held      ? POULSBACK_TEARDOWN_HELD
                            : slots     ? POULSBACK_TEARDOWN_BUSY
                            : !quiesced ? POULSBACK_TEARDOWN_UNQUIESCED
                            : fences    ? POULSBACK_TEARDOWN_FENCES
                                        : POULSBACK_TEARDOWN_OK;
                        e = (struct poulsback_teardown_engine){ready, quiesced, slots, held, fences};
                        CHECK(poulsback_teardown_may_free(&e) == want);
                        combos++;
                        ok += want == POULSBACK_TEARDOWN_OK;
                    }
    CHECK(combos == 96 && ok == 1);

    /* An engine's life: claimed, two jobs in flight, Q refused until they
     * end; F refused before Q and while a sync_file keeps a fence; freed. */
    e = (struct poulsback_teardown_engine){.ready = 1, .slots = 2, .fences = 2};
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_BUSY);
    CHECK(poulsback_teardown_may_free(&e) == POULSBACK_TEARDOWN_BUSY);
    e.slots = 0;
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_OK);
    CHECK(poulsback_teardown_may_free(&e) == POULSBACK_TEARDOWN_UNQUIESCED);
    e.quiesced = 1;
    CHECK(poulsback_teardown_may_free(&e) == POULSBACK_TEARDOWN_FENCES);
    e.fences = 0;
    CHECK(poulsback_teardown_may_free(&e) == POULSBACK_TEARDOWN_OK);
    /* A wedge before Q: lost for good, whatever drains or is released. */
    e = (struct poulsback_teardown_engine){.ready = 0, .held = 1, .fences = 1};
    CHECK(poulsback_teardown_drained(&e) == POULSBACK_TEARDOWN_LOST);
    e.held = e.fences = 0;
    e.quiesced = 1;
    CHECK(poulsback_teardown_may_free(&e) == POULSBACK_TEARDOWN_LOST);

    printf("teardown: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
