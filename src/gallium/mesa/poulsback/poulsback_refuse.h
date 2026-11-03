// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_REFUSE_H
#define POULSBACK_REFUSE_H
#include <stdio.h>

/* Userspace error origin, like the kernel's POULSBACK_ERRSITE: one stderr
 * line naming the refusing site, then the error itself. */
static inline int poulsback_refused_at(int e, const char *function, int line)
{
    fprintf(stderr, "poulsback: refused (%d) in %s:%d\n", e, function, line);
    return e;
}
#define POULSBACK_REFUSED(e) poulsback_refused_at((e), __func__, __LINE__)

/* Opt-in trace of driver decisions (POULSBACK_TRACE=1 in the environment). */
#include <stdlib.h>
static inline int poulsback_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("POULSBACK_TRACE");
        on = v && *v && *v != '0';
    }
    return on;
}
#define POULSBACK_TRACE(...)                                                                       \
    do {                                                                                           \
        if (poulsback_trace_on())                                                                  \
            fprintf(stderr, "poulsback trace: " __VA_ARGS__);                                      \
    } while (0)
#endif
