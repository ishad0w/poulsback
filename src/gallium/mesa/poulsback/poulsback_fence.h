// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_FENCE_H
#define POULSBACK_FENCE_H
struct pipe_screen;
struct pipe_context;
struct pipe_fence_handle;

/* Real Gallium reference/wait/export callbacks for imported Linux sync-files.
 * This does not create native poulsback job fences, signal a fence, submit,
 * implement server-side waiting or certify SGX/cache/MMU retirement. */
int poulsback_fence_screen_init(struct pipe_screen *);
/* Installs only create_fence_fd on an unpublished context. The callback writes
 * fresh output storage without reading its old bits; callers release an owned
 * old fence with screen->fence_reference before reusing that storage. Invalid
 * types/import failures write NULL. No server wait/signal or cap is enabled. */
int poulsback_fence_context_init(struct pipe_context *);
/* Takes fd, a sync_file the driver's own submission just exported: no
 * duplicate and no status query. fd is closed on failure. */
int poulsback_fence_adopt(int fd, struct pipe_fence_handle **);
/* Owned failure-only fence for a stopped CPU/driver operation. Requires a
 * negative error; invalid input or allocation failure returns NULL. It cannot
 * finish successfully or export a sync-file FD and has no GPU authority.
 * Release through screen->fence_reference; existing native fences are not
 * modified. A context preallocates one before accepting work, so a later
 * failed flush needs no allocation and cannot return a stale success fence. */
struct pipe_fence_handle *poulsback_fence_create_failed(int error);
#endif
