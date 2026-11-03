// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#define _POSIX_C_SOURCE 200809L
#include "poulsback_fence.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct pipe_fence_handle {
    struct pipe_reference reference;
    int fd;
    atomic_int error;
};
static int query(int fd, int *status)
{
    struct sync_file_info info = {0};
    int ret;
    ret = ioctl(fd, SYNC_IOC_FILE_INFO, &info);
    if (ret)
        return -errno;
    if (info.flags || !info.num_fences || info.status > 1)
        return -EPROTO;
    *status = info.status;
    return 0;
}
static int sticky(struct pipe_fence_handle *f, int ret)
{
    int expected = 0;
    if (ret < 0)
        atomic_compare_exchange_strong(&f->error, &expected, ret);
    return ret;
}
/* Borrows fd and owns a validated CLOEXEC duplicate; errors preserve output.
 * Only sync-file FDs pass SYNC_IOC_FILE_INFO; a readable ordinary FD cannot
 * become a completed fence. Externally failed fences keep their error. */
static int fence_import(int fd, struct pipe_fence_handle **out)
{
    struct pipe_fence_handle *f;
    int owned, status, ret;
    if (fd < 0 || !out)
        return -EINVAL;
    owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned < 0)
        return -errno;
    ret = query(owned, &status);
    if (ret) {
        close(owned);
        return ret;
    }
    f = calloc(1, sizeof(*f));
    if (!f) {
        close(owned);
        return -ENOMEM;
    }
    pipe_reference_init(&f->reference, 1);
    f->fd = owned;
    atomic_init(&f->error, status < 0 ? status : 0);
    *out = f;
    return 0;
}
int poulsback_fence_adopt(int fd, struct pipe_fence_handle **out)
{
    struct pipe_fence_handle *f = fd >= 0 && out ? calloc(1, sizeof(*f)) : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        return fd < 0 || !out ? -EINVAL : -ENOMEM;
    }
    pipe_reference_init(&f->reference, 1);
    f->fd = fd;
    atomic_init(&f->error, 0);
    *out = f;
    return 0;
}
struct pipe_fence_handle *poulsback_fence_create_failed(int error)
{
    struct pipe_fence_handle *f;
    if (error >= 0)
        return NULL;
    f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    pipe_reference_init(&f->reference, 1);
    f->fd = -1;
    atomic_init(&f->error, error);
    return f;
}
static void reference(struct pipe_screen *screen, struct pipe_fence_handle **ptr,
                      struct pipe_fence_handle *fence)
{
    struct pipe_fence_handle *old = *ptr;
    (void)screen;
    if (pipe_reference(old ? &old->reference : NULL, fence ? &fence->reference : NULL)) {
        if (old->fd >= 0)
            close(old->fd);
        free(old);
    }
    *ptr = fence;
}
static int now_ns(uint64_t *out)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -errno;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000 ||
        (uint64_t)t.tv_sec > (UINT64_MAX - (uint64_t)t.tv_nsec) / UINT64_C(1000000000))
        return -EOVERFLOW;
    *out = (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
    return 0;
}
/* Only a finite, non-zero timeout reads the clock. */
static bool finish(struct pipe_screen *screen, struct pipe_context *ctx,
                   struct pipe_fence_handle *f, uint64_t timeout)
{
    uint64_t start = 0, now, previous, elapsed, remaining = UINT64_MAX;
    int status, ret, milliseconds;
    bool infinite = timeout == UINT64_MAX;
    (void)screen;
    (void)ctx;
    if (!f || atomic_load(&f->error))
        return false;
    if (!infinite && timeout) {
        ret = now_ns(&start);
        if (ret) {
            sticky(f, ret);
            return false;
        }
    }
    previous = start;
    for (;;) {
        struct pollfd pfd = {.fd = f->fd, .events = POLLIN};
        ret = query(f->fd, &status);
        if (ret && ret != -EINTR) {
            sticky(f, ret);
            return false;
        }
        if (!ret && status != 0)
            return sticky(f, status) == 1;
        if (!timeout)
            return false;
        if (!infinite) {
            int clock_error = now_ns(&now);
            if (clock_error || now < previous) {
                sticky(f, clock_error ? clock_error : -EOVERFLOW);
                return false;
            }
            previous = now;
            elapsed = now - start;
            if (elapsed >= timeout)
                return false;
            remaining = timeout - elapsed;
        }
        if (ret == -EINTR)
            continue;
        milliseconds =
            remaining / UINT64_C(1000000) >= INT_MAX
                ? INT_MAX
                : (int)(remaining / UINT64_C(1000000) + !!(remaining % UINT64_C(1000000)));
        ret = poll(&pfd, 1, milliseconds);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            sticky(f, -errno);
            return false;
        }
        if (ret > 0) {
            if (pfd.revents & (POLLNVAL | POLLHUP)) {
                sticky(f, -EIO);
                return false;
            }
            /* POLLIN alone is not success; re-read the immutable sync-file's
             * status and propagate its negative error. */
            ret = query(f->fd, &status);
            if (ret == -EINTR)
                continue;
            if (ret) {
                sticky(f, ret);
                return false;
            }
            if (status == 1)
                return true;
            sticky(f, status < 0 ? status : -EPROTO);
            return false;
        }
    }
}
static int get_fd(struct pipe_screen *screen, struct pipe_fence_handle *f)
{
    (void)screen;
    return f && f->fd >= 0 ? fcntl(f->fd, F_DUPFD_CLOEXEC, 0) : -1;
}
static void create_fd(struct pipe_context *ctx, struct pipe_fence_handle **out, int fd,
                      enum pipe_fd_type type)
{
    (void)ctx;
    if (!out)
        return;
    /* Mesa's DRI image consumer supplies uninitialized output storage.
     * Reference replacement belongs to screen->fence_reference, not this
     * fresh-output callback. Import borrows fd and owns only its duplicate. */
    *out = NULL;
    if (type == PIPE_FD_TYPE_NATIVE_SYNC)
        (void)fence_import(fd, out);
}
int poulsback_fence_context_init(struct pipe_context *ctx)
{
    if (!ctx || !ctx->screen || !ctx->screen->fence_reference || ctx->create_fence_fd)
        return -EINVAL;
    ctx->create_fence_fd = create_fd;
    return 0;
}
int poulsback_fence_screen_init(struct pipe_screen *screen)
{
    if (!screen || screen->fence_reference || screen->fence_finish || screen->fence_get_fd)
        return -EINVAL;
    screen->fence_reference = reference;
    screen->fence_finish = finish;
    screen->fence_get_fd = get_fd;
    return 0;
}
