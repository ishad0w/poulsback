// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The loss as Mesa sees it (poulsback_lost.h): no reset before it, errors
 * other than -EIO never mark it, then each context reports an unknown reset
 * exactly once, through its status or its callback, whichever comes first,
 * and a context without a callback reports through its status only.
 *
 * usage: poulsback-lost */
#include "poulsback_lost.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, calls;
static enum pipe_reset_status called_with;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)

/* Whether the loss is marked: a fresh view's status. */
static bool lost_now(void)
{
    struct poulsback_lost_view fresh = {0};
    return poulsback_lost_status(&fresh) == PIPE_UNKNOWN_CONTEXT_RESET;
}

static void reset(void *data, enum pipe_reset_status status)
{
    CHECK(data == &calls);
    calls++;
    called_with = status;
}

int main(void)
{
    const struct pipe_device_reset_callback cb = {.reset = reset, .data = &calls};
    struct poulsback_lost_view robust = {0}, polled = {0}, plain = {0}, cleared = {0};

    /* Before the loss: no reset, no callback; refusals do not mark it. */
    poulsback_lost_set_callback(&robust, &cb);
    poulsback_lost_set_callback(&cleared, &cb);
    poulsback_lost_set_callback(&cleared, NULL);
    CHECK(poulsback_lost_status(&robust) == PIPE_NO_RESET);
    CHECK(poulsback_lost_note(-EINVAL) == -EINVAL && poulsback_lost_note(-ENOMEM) == -ENOMEM);
    CHECK(poulsback_lost_note(0) == 0 && poulsback_lost_note(1) == 1);
    poulsback_lost_fire(&robust);
    CHECK(!lost_now() && !calls && poulsback_lost_status(&polled) == PIPE_NO_RESET);

    /* The loss: a robust context's failure fires its callback once; its
     * status then reports nothing more. */
    CHECK(poulsback_lost_note(-EIO) == -EIO && lost_now());
    poulsback_lost_fire(&robust);
    CHECK(calls == 1 && called_with == PIPE_UNKNOWN_CONTEXT_RESET);
    poulsback_lost_fire(&robust);
    CHECK(calls == 1 && poulsback_lost_status(&robust) == PIPE_NO_RESET);

    /* A context polled first reports once by its status; a later callback
     * does not fire again. */
    CHECK(poulsback_lost_status(&polled) == PIPE_UNKNOWN_CONTEXT_RESET);
    CHECK(poulsback_lost_status(&polled) == PIPE_NO_RESET);
    poulsback_lost_set_callback(&polled, &cb);
    poulsback_lost_fire(&polled);
    CHECK(calls == 1);

    /* Without a callback (not LOSE_CONTEXT_ON_RESET, or cleared) a failure
     * fires nothing, and the status still reports once. */
    poulsback_lost_fire(&plain);
    poulsback_lost_fire(&cleared);
    CHECK(calls == 1);
    CHECK(poulsback_lost_status(&plain) == PIPE_UNKNOWN_CONTEXT_RESET);
    CHECK(poulsback_lost_status(&cleared) == PIPE_UNKNOWN_CONTEXT_RESET);
    CHECK(poulsback_lost_status(&plain) == PIPE_NO_RESET);

    /* The loss is for good. */
    CHECK(poulsback_lost_note(-EINVAL) == -EINVAL && lost_now());
    printf("poulsback-lost: %u checks\n", checks);
    return 0;
}
