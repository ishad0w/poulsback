// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Who holds a BO's mapping in one address space (kernel/gem/poulsback_bo_use.c):
 * handles and jobs as sequences, against when the mapping may go. The
 * contract is in docs/kernel/memory.md. */
#include "../../kernel/gem/poulsback_bo_use.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "bo-use check %u line %d: %s\n", checks, __LINE__, #x);                \
        }                                                                                          \
    } while (0)

int main(void)
{
    struct poulsback_bo_use u;

    /* A job resolves the BO, then the last handle closes before the job's
     * fence is published: the mapping stays until the job is released. */
    u = (struct poulsback_bo_use){1, 0, 0};
    u.jobs++;
    CHECK(!poulsback_bo_use_close(&u));
    CHECK(u.handles == 0 && u.jobs == 1);
    CHECK(poulsback_bo_use_release(&u));

    /* Without a job, the last handle's close drops it; earlier ones do not. */
    u = (struct poulsback_bo_use){2, 0, 0};
    CHECK(!poulsback_bo_use_close(&u));
    CHECK(poulsback_bo_use_close(&u));

    /* Jobs released while a handle is open leave it mapped; the close drops it. */
    u = (struct poulsback_bo_use){1, 2, 0};
    CHECK(!poulsback_bo_use_release(&u));
    CHECK(!poulsback_bo_use_release(&u));
    CHECK(poulsback_bo_use_close(&u));

    /* Several jobs past the last handle: only the last release drops it. */
    u = (struct poulsback_bo_use){1, 3, 0};
    CHECK(!poulsback_bo_use_close(&u));
    CHECK(!poulsback_bo_use_release(&u));
    CHECK(!poulsback_bo_use_release(&u));
    CHECK(poulsback_bo_use_release(&u));

    /* A kept mapping (HOLD) is never dropped again, by a close or a release. */
    u = (struct poulsback_bo_use){1, 1, 1};
    CHECK(!poulsback_bo_use_close(&u));
    CHECK(!poulsback_bo_use_release(&u));

    printf("bo-use: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
