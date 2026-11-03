/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "frame_stats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double frame_clock(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1.0;
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int frame_init(struct frame_stats *s, double now) {
    memset(s, 0, sizeof(*s));
    if (!isfinite(now) || now < 0)
        return -1;
    s->events = calloc(FRAME_CAPACITY, sizeof(*s->events));
    if (!s->events)
        return -1;
    s->origin = s->previous = now;
    return 0;
}

void frame_destroy(struct frame_stats *s) {
    free(s->events);
    memset(s, 0, sizeof(*s));
}

int frame_add(struct frame_stats *s, double now) {
    size_t i;
    if (!s->events || !isfinite(now) || now <= s->previous)
        return -1;
    while (s->count && s->events[s->first].end <= now - 60.0) {
        s->first = (s->first + 1) % FRAME_CAPACITY;
        s->count--;
    }
    /* Never silently replace samples within the advertised minute. */
    if (s->count == FRAME_CAPACITY)
        return -1;
    i = (s->first + s->count) % FRAME_CAPACITY;
    s->events[i].end = now;
    s->events[i].dt = now - s->previous;
    s->count++;
    s->previous = now;
    return 0;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int frame_read(const struct frame_stats *s, double now, double window, struct frame_report *r) {
    size_t i, n = 0;
    double sum = 0, *values;
    memset(r, 0, sizeof(*r));
    if (!s->events || !isfinite(now) || now < s->previous || !isfinite(window) || window <= 0 ||
        window > 60)
        return -1;
    r->window = fmin(window, now - s->origin);
    if (!s->count || r->window <= 0)
        return 0;
    values = malloc(s->count * sizeof(*values));
    if (!values)
        return -1;
    for (i = 0; i < s->count; i++) {
        const struct frame_event *e = &s->events[(s->first + i) % FRAME_CAPACITY];
        if (e->end > now - window && e->end <= now) {
            values[n++] = e->dt * 1000.0;
            sum += e->dt;
            r->last_ms = e->dt * 1000.0;
        }
    }
    if (n) {
        qsort(values, n, sizeof(*values), compare_double);
        r->frames = n;
        r->fps = (double)n / r->window;
        r->mean_ms = sum * 1000.0 / n;
        r->p95_ms = values[(size_t)ceil(n * 0.95) - 1];
        r->p99_ms = values[(size_t)ceil(n * 0.99) - 1];
        r->max_ms = values[n - 1];
    }
    free(values);
    return 0;
}
