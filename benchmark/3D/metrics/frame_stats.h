/* SPDX-License-Identifier: MIT */
#ifndef FRAME_STATS_H
#define FRAME_STATS_H
#include <stddef.h>

#define FRAME_CAPACITY 131072
struct frame_event {
    double end;
    double dt;
};
struct frame_stats {
    struct frame_event *events;
    size_t first, count;
    double origin, previous;
};
struct frame_report {
    size_t frames;
    double window, fps, mean_ms, last_ms, p95_ms, p99_ms, max_ms;
};
int frame_init(struct frame_stats *s, double now);
void frame_destroy(struct frame_stats *s);
int frame_add(struct frame_stats *s, double now);
int frame_read(const struct frame_stats *s, double now, double window, struct frame_report *r);
double frame_clock(void);
#endif
