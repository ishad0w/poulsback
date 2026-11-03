// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The GPU's loss as Mesa sees it (docs/kernel/execution.md "The loss"): the
 * kernel publishes it as -EIO from CONTEXT_CREATE, SUBMIT,
 * SUBMIT_2D and BO_CREATE and as a job fence's status. One SGX per system,
 * so the loss is one flag per process; each context reports it once, as an
 * unknown-context reset (the kernel names no guilty context). */
#ifndef POULSBACK_LOST_H
#define POULSBACK_LOST_H
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include <stdbool.h>

/* An engine ioctl's negative errno: -EIO marks the loss for good. Returns
 * error unchanged. */
int poulsback_lost_note(int error);

/* A context's view of the loss: reported once, through whichever comes first
 * of its reset status and its reset callback. */
struct poulsback_lost_view {
    bool reported;
    struct pipe_device_reset_callback callback;
};
/* PIPE_UNKNOWN_CONTEXT_RESET once after the loss, else PIPE_NO_RESET. */
enum pipe_reset_status poulsback_lost_status(struct poulsback_lost_view *);
/* Keeps a copy of cb (NULL clears it). */
void poulsback_lost_set_callback(struct poulsback_lost_view *,
                                 const struct pipe_device_reset_callback *cb);
/* After the context's own operation failed: the callback, once, if the GPU
 * is lost and the view has not reported it. */
void poulsback_lost_fire(struct poulsback_lost_view *);
#endif
