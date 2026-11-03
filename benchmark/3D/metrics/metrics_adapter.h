/* SPDX-License-Identifier: MIT
 * Included only by the separately named instrumented upstream glxgears.
 */
#include "frame_stats.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <time.h>

static struct frame_stats metrics_stats;
static volatile sig_atomic_t metrics_signal;
static int metrics_debug, metrics_debug_frames, metrics_error;
static unsigned long metrics_frames, metrics_max_frames;
static double metrics_begin_time, metrics_last_report, metrics_duration, metrics_limit;

static void metrics_signal_handler(int sig) { metrics_signal = sig; }

static void metrics_config(Display *dpy, GLXContext context, VisualID visual, unsigned int width,
                           unsigned int height, int requested_swap) {
    XVisualInfo template, *info;
    int count, i;
    static const int attributes[] = {GLX_RED_SIZE,       GLX_GREEN_SIZE, GLX_BLUE_SIZE,
                                     GLX_ALPHA_SIZE,     GLX_DEPTH_SIZE, GLX_STENCIL_SIZE,
                                     GLX_SAMPLE_BUFFERS, GLX_SAMPLES};
    static const char *names[] = {"red",   "green",   "blue",           "alpha",
                                  "depth", "stencil", "sample_buffers", "samples"};
    if (!metrics_debug)
        return;
    printf("DEBUG visual=0x%lx width=%u height=%u direct=%d requested_swap=%d\n",
           (unsigned long)visual, width, height, glXIsDirect(dpy, context), requested_swap);
    memset(&template, 0, sizeof(template));
    template.visualid = visual;
    info = XGetVisualInfo(dpy, VisualIDMask, &template, &count);
    if (!info || count != 1) {
        if (info)
            XFree(info);
        return;
    }
    for (i = 0; i < 8; i++) {
        int value;
        if (glXGetConfig(dpy, info, attributes[i], &value) == 0)
            printf("DEBUG visual_%s=%d\n", names[i], value);
    }
    XFree(info);
}

static void metrics_help(void) {
    puts("glxgears-metrics: upstream rendering with application timing\n"
         "  --duration SECONDS  finish normally after this elapsed interval\n"
         "  --frames COUNT      finish normally after this many swaps\n"
         "  --limit-fps FPS     optional CPU-side pacing (default: uncapped)\n"
         "  --debug             setup, lifecycle and periodic GL errors on stdout\n"
         "  --debug-frames      also print every application frame (perturbs timing)\n"
         "  --help              this help, without opening a display\n"
         "Upstream options: -info -geometry WxH+X+Y -fullscreen -swapinterval N\n"
         "Timing is application cadence including swap, not GPU or display completion.");
}

static int metrics_arguments(int argc, char **argv) {
    int i, out = 1;
    for (i = 1; i < argc; i++) {
        double *destination = NULL;
        if (!strcmp(argv[i], "--help")) {
            metrics_help();
            exit(0);
        }
        if (!strcmp(argv[i], "--debug") || !strcmp(argv[i], "--debug-frames")) {
            metrics_debug = 1;
            metrics_debug_frames |= !strcmp(argv[i], "--debug-frames");
            continue;
        }
        if (!strcmp(argv[i], "--duration"))
            destination = &metrics_duration;
        else if (!strcmp(argv[i], "--limit-fps"))
            destination = &metrics_limit;
        if (destination) {
            char *end;
            if (++i == argc)
                goto bad;
            errno = 0;
            *destination = strtod(argv[i], &end);
            if (errno || end == argv[i] || *end || !isfinite(*destination) || *destination <= 0 ||
                *destination > 86400)
                goto bad;
            continue;
        }
        if (!strcmp(argv[i], "--frames")) {
            char *end;
            if (++i == argc || argv[i][0] == '-')
                goto bad;
            errno = 0;
            metrics_max_frames = strtoul(argv[i], &end, 10);
            if (errno || end == argv[i] || *end || !metrics_max_frames)
                goto bad;
            continue;
        }
        /* Reject missing upstream argument operands before upstream consumes them. */
        if (!strcmp(argv[i], "-display") || !strcmp(argv[i], "-geometry") ||
            !strcmp(argv[i], "-samples") || !strcmp(argv[i], "-swapinterval")) {
            if (i + 1 == argc)
                goto bad;
            argv[out++] = argv[i++];
        }
        argv[out++] = argv[i];
    }
    argv[out] = NULL;
    return out;
bad:
    fputs("Invalid or missing metrics/upstream option operand\n", stderr);
    exit(2);
}

static int metrics_stopped(void) {
    double now = frame_clock();
    if (now < 0) {
        metrics_error = 1;
        return 1;
    }
    return metrics_signal || metrics_error ||
           (metrics_duration > 0 && now - metrics_begin_time >= metrics_duration) ||
           (metrics_max_frames && metrics_frames >= metrics_max_frames);
}

static void metrics_report(double now, const char *kind) {
    struct frame_report short_window, minute;
    if (frame_read(&metrics_stats, now, 1, &short_window) ||
        frame_read(&metrics_stats, now, 60, &minute)) {
        metrics_error = 1;
        fputs("metrics allocation/clock failure\n", stderr);
        return;
    }
    printf("METRICS kind=%s elapsed_s=%.3f frames=%lu fps_1s=%.3f window_s=%.3f "
           "fps_window=%.3f last_ms=%.3f mean_ms=%.3f p95_ms=%.3f p99_ms=%.3f "
           "max_ms=%.3f scope=APPLICATION_SWAP\n",
           kind, now - metrics_begin_time, metrics_frames, short_window.fps, minute.window,
           minute.fps, minute.last_ms, minute.mean_ms, minute.p95_ms, minute.p99_ms, minute.max_ms);
    fflush(stdout);
}

static void metrics_begin(void) {
    struct sigaction action;
    metrics_begin_time = metrics_last_report = frame_clock();
    if (frame_init(&metrics_stats, metrics_begin_time)) {
        fputs("Cannot initialize metrics\n", stderr);
        exit(2);
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = metrics_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        fputs("Cannot install termination handlers\n", stderr);
        exit(2);
    }
    if (metrics_debug)
        printf("DEBUG begin renderer=%s version=%s duration_s=%.3f limit_fps=%.3f\n",
               glGetString(GL_RENDERER), glGetString(GL_VERSION), metrics_duration, metrics_limit);
}

static void metrics_tick(void) {
    double now = frame_clock();
    if (frame_add(&metrics_stats, now)) {
        fputs("Metrics clock/capacity failure; minute samples were not silently discarded\n",
              stderr);
        metrics_error = 1;
        return;
    }
    metrics_frames++;
    if (metrics_debug_frames)
        printf("DEBUG frame=%lu monotonic_s=%.9f\n", metrics_frames, now);
    if (now - metrics_last_report >= 1) {
        metrics_report(now, "periodic");
        metrics_last_report = now;
        if (metrics_debug) {
            GLenum error = glGetError();
            printf("DEBUG gl_error=0x%x frame=%lu\n", error, metrics_frames);
            if (error != GL_NO_ERROR)
                metrics_error = 1;
        }
    }
    if (metrics_limit > 0) {
        double remaining = 1.0 / metrics_limit - (frame_clock() - now);
        if (remaining > 0) {
            struct timespec pause, rest;
            pause.tv_sec = (time_t)remaining;
            pause.tv_nsec = (long)((remaining - pause.tv_sec) * 1e9);
            while (nanosleep(&pause, &rest) && errno == EINTR && !metrics_signal)
                pause = rest;
        }
    }
}

static void metrics_end(void) {
    metrics_report(frame_clock(), "final");
    if (metrics_debug)
        printf("DEBUG end frames=%lu signal=%d status=%d\n", metrics_frames, (int)metrics_signal,
               metrics_error);
    frame_destroy(&metrics_stats);
}
