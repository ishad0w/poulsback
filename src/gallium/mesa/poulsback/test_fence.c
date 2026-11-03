// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Real Mesa callbacks + deterministic Linux sync-file syscall boundary. */
#define _POSIX_C_SOURCE 200809L
#include "poulsback_fence.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int status, query_error, query_eintr, duplicate_error, poll_error;
static int poll_eintr, readable_pending, revents_override;
static bool malformed, zero_fences;
static uint64_t now;
static unsigned duplicates, closes, polls, queries;
static int last_timeout;
static bool regress_poll_clock;
static unsigned regression_polls;
int fcntl(int fd, int command, ...)
{
    va_list ap;
    assert((fd == 7 || fd == 17) && command == F_DUPFD_CLOEXEC);
    va_start(ap, command);
    assert(va_arg(ap, int) == 0);
    va_end(ap);
    if (duplicate_error) {
        errno = duplicate_error;
        return -1;
    }
    duplicates++;
    return fd == 7 ? 17 : 27;
}
int close(int fd)
{
    assert(fd == 17 || fd == 27);
    closes++;
    return 0;
}
int ioctl(int fd, unsigned long request, ...)
{
    struct sync_file_info *info;
    va_list ap;
    assert(fd == 17 && request == SYNC_IOC_FILE_INFO);
    va_start(ap, request);
    info = va_arg(ap, struct sync_file_info *);
    va_end(ap);
    assert(!info->flags && !info->num_fences && !info->sync_fence_info);
    queries++;
    if (query_eintr) {
        query_eintr--;
        now += 1000000;
        errno = EINTR;
        return -1;
    }
    if (query_error) {
        errno = query_error;
        return -1;
    }
    info->status = status;
    info->flags = malformed ? 1u : 0u;
    info->num_fences = zero_fences ? 0u : 1u;
    return 0;
}
int clock_gettime(clockid_t clock, struct timespec *t)
{
    assert(clock == CLOCK_MONOTONIC);
    t->tv_sec = (time_t)(now / UINT64_C(1000000000));
    t->tv_nsec = (long)(now % UINT64_C(1000000000));
    return 0;
}
int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds->fd == 17 && fds->events == POLLIN && timeout >= 0);
    polls++;
    last_timeout = timeout;
    if (regress_poll_clock && regression_polls < 2) {
        /* Interrupted waits may wake before the timeout. Only the second
         * clock observation is faulty, remaining above the initial epoch. */
        now = ++regression_polls == 1 ? 3000000 : 1000000;
        errno = EINTR;
        return -1;
    }
    if (poll_eintr) {
        poll_eintr--;
        now += 1000000;
        errno = EINTR;
        return -1;
    }
    if (poll_error) {
        errno = poll_error;
        return -1;
    }
    if (readable_pending || revents_override) {
        fds->revents = revents_override ? (short)revents_override : POLLIN;
        return 1;
    }
    if (timeout == INT32_MAX) {
        status = 1;
        fds->revents = POLLIN;
        return 1;
    }
    now += (uint64_t)(unsigned)timeout * 1000000;
    return 0;
}
static void release(struct pipe_screen *screen, struct pipe_fence_handle **f)
{
    screen->fence_reference(screen, f, NULL);
    assert(!*f);
}
/* Imports through the product's create_fence_fd; NULL on any refusal. */
static struct pipe_context importer;
static struct pipe_fence_handle *imported(int fd)
{
    struct pipe_fence_handle *f;
    importer.create_fence_fd(&importer, &f, fd, PIPE_FD_TYPE_NATIVE_SYNC);
    return f;
}
/* A sticky error: the fence does not finish even once its sync-file reads as
 * signalled. */
static bool sticky(struct pipe_screen *screen, struct pipe_fence_handle *f)
{
    int saved = status, eintr = query_eintr;
    bool done;
    status = 1;
    query_eintr = 0;
    done = screen->fence_finish(screen, NULL, f, 0);
    status = saved;
    query_eintr = eintr;
    return !done;
}
static void context_imports(struct pipe_screen *screen)
{
    struct pipe_context ctx = {.screen = screen}, invalid = {0};
    struct pipe_screen missing = {0};
    struct pipe_fence_handle *f, *old = NULL;
    const enum pipe_fd_type refused[] = {
        PIPE_FD_TYPE_SYNCOBJ, PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12,
        PIPE_FD_TYPE_TIMELINE_SEMAPHORE_VK, (enum pipe_fd_type) - 1, (enum pipe_fd_type)99};
    unsigned i, q, d, c;
    assert(poulsback_fence_context_init(NULL) == -EINVAL);
    assert(poulsback_fence_context_init(&invalid) == -EINVAL && !invalid.create_fence_fd);
    invalid.screen = &missing;
    assert(poulsback_fence_context_init(&invalid) == -EINVAL && !invalid.create_fence_fd);
    assert(!poulsback_fence_context_init(&ctx) && ctx.create_fence_fd);
    assert(!ctx.fence_server_sync && !ctx.fence_server_signal && !screen->caps.native_fence_fd);
    assert(poulsback_fence_context_init(&ctx) == -EINVAL && ctx.create_fence_fd);
    q = queries;
    d = duplicates;
    c = closes;
    ctx.create_fence_fd(&ctx, NULL, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(queries == q && duplicates == d && closes == c);
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        /* Output has deliberately unusable prior bits. No reference read is
         * permitted, even on a refused type or import failure. */
        f = (struct pipe_fence_handle *)(uintptr_t)1;
        ctx.create_fence_fd(&ctx, &f, 7, refused[i]);
        assert(!f && queries == q && duplicates == d && closes == c);
    }
    f = (struct pipe_fence_handle *)(uintptr_t)1;
    ctx.create_fence_fd(&ctx, &f, -1, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(!f && queries == q && duplicates == d && closes == c);
    duplicate_error = EMFILE;
    f = (struct pipe_fence_handle *)(uintptr_t)1;
    ctx.create_fence_fd(&ctx, &f, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(!f && queries == q && duplicates == d && closes == c);
    duplicate_error = 0;
    query_error = ENOTTY;
    f = (struct pipe_fence_handle *)(uintptr_t)1;
    ctx.create_fence_fd(&ctx, &f, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(!f && queries == q + 1 && duplicates == d + 1 && closes == c + 1);
    query_error = 0;
    malformed = true;
    ctx.create_fence_fd(&ctx, &f, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(!f && duplicates == closes);
    malformed = false;
    /* Match Mesa's actual consumer: no input initialization of this slot. */
    {
        struct pipe_fence_handle *fresh;
        ctx.create_fence_fd(&ctx, &fresh, 7, PIPE_FD_TYPE_NATIVE_SYNC);
        assert(fresh && !sticky(screen, fresh));
        release(screen, &fresh);
    }
    status = 1;
    ctx.create_fence_fd(&ctx, &old, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(old && screen->fence_finish(screen, &ctx, old, 0));
    screen->fence_reference(screen, &f, old);
    c = closes;
    /* The reference callback is the separate old-output cleanup boundary.
     * Retain an alias independently while the output slot is reused. */
    release(screen, &f);
    assert(closes == c);
    status = 0;
    ctx.create_fence_fd(&ctx, &f, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(f && f != old && !screen->fence_finish(screen, &ctx, f, 0));
    release(screen, &f);
    assert(closes == c + 1 && !sticky(screen, old));
    release(screen, &old);
    assert(closes == c + 2);
    status = -EIO;
    ctx.create_fence_fd(&ctx, &f, 7, PIPE_FD_TYPE_NATIVE_SYNC);
    assert(f && sticky(screen, f) && !screen->fence_finish(screen, &ctx, f, 0));
    release(screen, &f);
    status = 0;
    assert(duplicates == closes && !ctx.fence_server_sync && !ctx.fence_server_signal);
    puts(
        "Mesa context native-sync imports: fresh/poisoned outputs, five FD-type refusals, borrowed FD, old-reference cleanup and failed-fence distinction PASS");
}
static void failure_only_fences(struct pipe_screen *screen)
{
    struct pipe_fence_handle *old = NULL, *failure, *alias = NULL;
    unsigned q = queries, p = polls, d = duplicates, c = closes;
    const int errors[] = {-EIO, -ENOMEM, -ENODEV, -EOPNOTSUPP};
    assert(!poulsback_fence_create_failed(0) && !poulsback_fence_create_failed(1));
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        failure = poulsback_fence_create_failed(errors[i]);
        assert(failure);
        assert(sticky(screen, failure));
        screen->fence_reference(screen, &alias, failure);
        screen->fence_reference(screen, &failure, failure);
        assert(!screen->fence_finish(screen, NULL, failure, 0));
        assert(!screen->fence_finish(screen, NULL, alias, UINT64_MAX));
        assert(screen->fence_get_fd(screen, failure) == -1);
        release(screen, &failure);
        assert(sticky(screen, alias));
        release(screen, &alias);
    }
    assert(queries == q && polls == p && duplicates == d && closes == c);
    status = 1;
    assert((old = imported(7)));
    assert(screen->fence_finish(screen, NULL, old, 0));
    failure = poulsback_fence_create_failed(-EIO);
    assert(failure);
    screen->fence_reference(screen, &alias, old);
    c = closes;
    screen->fence_reference(screen, &alias, failure);
    assert(closes == c && !sticky(screen, old));
    assert(!screen->fence_finish(screen, NULL, alias, UINT64_MAX));
    assert(screen->fence_finish(screen, NULL, old, 0));
    release(screen, &failure);
    release(screen, &alias);
    assert(closes == c);
    release(screen, &old);
    assert(closes == c + 1);
    status = 0;
    puts(
        "Mesa failure-only fences: never success/export/IO, replacement refs preserve old native success PASS");
}
static void regressing_wait_clock(struct pipe_screen *screen)
{
    struct pipe_fence_handle *f = NULL;
    unsigned q, p, c;
    status = 0;
    now = 0;
    assert((f = imported(7)));
    p = polls;
    c = closes;
    regress_poll_clock = true;
    regression_polls = 0;
    assert(!screen->fence_finish(screen, NULL, f, 4000000));
    assert(regression_polls == 2 && polls == p + 2 && closes == c && sticky(screen, f));
    regress_poll_clock = false;
    /* A later signalled sync-file cannot erase an earlier protocol/clock
     * error. Retain the FD until the ordinary final reference is dropped. */
    status = 1;
    q = queries;
    p = polls;
    assert(!screen->fence_finish(screen, NULL, f, UINT64_MAX) && queries == q && polls == p &&
           closes == c && sticky(screen, f));
    release(screen, &f);
    assert(closes == c + 1);
    status = 0;
    now = 0;
    puts(
        "Mesa fence wait: forward then backward clock above start refuses with sticky error, no extra poll or premature FD close PASS");
}
int main(void)
{
    struct pipe_screen screen = {0};
    struct pipe_fence_handle *f = NULL, *alias = NULL;
    unsigned before;
    assert(!poulsback_fence_screen_init(&screen));
    importer.screen = &screen;
    assert(!poulsback_fence_context_init(&importer));
    assert(poulsback_fence_screen_init(&screen) == -EINVAL);
    context_imports(&screen);
    failure_only_fences(&screen);
    regressing_wait_clock(&screen);
    assert(!(f = imported(-1)));
    duplicate_error = EMFILE;
    assert(!(f = imported(7)));
    duplicate_error = 0;
    query_error = ENOTTY;
    assert(!(f = imported(7)));
    query_error = 0;
    malformed = true;
    assert(!(f = imported(7)));
    malformed = false;
    zero_fences = true;
    assert(!(f = imported(7)));
    zero_fences = false;
    status = 2;
    assert(!(f = imported(7)));
    status = 0;
    query_eintr = 1;
    assert(!(f = imported(7)));
    assert((f = imported(7)));
    before = polls;
    assert(!screen.fence_finish(&screen, NULL, f, 0) && polls == before);
    assert(!sticky(&screen, f));
    query_eintr = 3;
    assert(!screen.fence_finish(&screen, NULL, f, 2000000) && query_eintr == 1);
    assert(!sticky(&screen, f));
    query_eintr = 0;
    now = 0;
    poll_eintr = 1;
    assert(!screen.fence_finish(&screen, NULL, f, 3000000));
    assert(last_timeout == 2 && now == 3000000 && !sticky(&screen, f));
    assert(screen.fence_finish(&screen, NULL, f, UINT64_MAX));
    assert(last_timeout == INT32_MAX && !sticky(&screen, f));
    screen.fence_reference(&screen, &alias, f);
    screen.fence_reference(&screen, &f, f);
    assert(screen.fence_get_fd(&screen, alias) == 27);
    assert(!close(27));
    before = closes;
    release(&screen, &alias);
    assert(closes == before);
    release(&screen, &f);
    assert(closes == before + 1);
    status = -EIO;
    assert((f = imported(7)));
    assert(sticky(&screen, f));
    assert(!screen.fence_finish(&screen, NULL, f, UINT64_MAX));
    release(&screen, &f);
    status = 0;
    assert((f = imported(7)));
    readable_pending = 1;
    assert(!screen.fence_finish(&screen, NULL, f, 1000000));
    assert(sticky(&screen, f)); /* POLLIN != GPU success. */
    readable_pending = 0;
    release(&screen, &f);
    assert((f = imported(7)));
    poll_error = EIO;
    assert(!screen.fence_finish(&screen, NULL, f, 1000000));
    assert(sticky(&screen, f));
    poll_error = 0;
    release(&screen, &f);
    assert((f = imported(7)));
    revents_override = POLLNVAL;
    assert(!screen.fence_finish(&screen, NULL, f, 1000000));
    assert(sticky(&screen, f));
    revents_override = 0;
    release(&screen, &f);
    status = 1;
    assert((f = imported(7)));
    query_error = EBADF;
    assert(!screen.fence_finish(&screen, NULL, f, 0));
    assert(sticky(&screen, f));
    query_error = 0;
    release(&screen, &f);
    assert(duplicates == closes);
    /* A job's own fresh sync_file is taken as is: no duplicate and no query
     * until a wait; a refused adoption closes it. */
    {
        unsigned c = closes, d = duplicates, q = queries;
        assert(poulsback_fence_adopt(-1, &f) == -EINVAL && !f && closes == c);
        assert(poulsback_fence_adopt(17, NULL) == -EINVAL && closes == c + 1);
        assert(!poulsback_fence_adopt(17, &f) && f);
        assert(duplicates == d && queries == q && closes == c + 1);
        status = 1;
        assert(screen.fence_finish(&screen, NULL, f, UINT64_MAX) && queries == q + 1);
        release(&screen, &f);
        assert(closes == c + 2);
        status = 0;
    }
    printf(
        "Mesa sync-file fences: validation/ref/FD ownership/deadline/EINTR/error/readability refusal PASS (%u queries,%u polls); no native job retirement\n",
        queries, polls);
    return 0;
}
