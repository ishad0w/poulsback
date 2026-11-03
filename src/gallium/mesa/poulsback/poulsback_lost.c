// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_lost.h"
#include <errno.h>
#include <stdatomic.h>

static atomic_bool lost;

int poulsback_lost_note(int error)
{
    if (error == -EIO)
        atomic_store(&lost, true);
    return error;
}

static bool poulsback_lost(void)
{
    return atomic_load(&lost);
}

enum pipe_reset_status poulsback_lost_status(struct poulsback_lost_view *v)
{
    if (v->reported || !poulsback_lost())
        return PIPE_NO_RESET;
    v->reported = true;
    return PIPE_UNKNOWN_CONTEXT_RESET;
}

void poulsback_lost_set_callback(struct poulsback_lost_view *v,
                                 const struct pipe_device_reset_callback *cb)
{
    v->callback = cb ? *cb : (struct pipe_device_reset_callback){0};
}

void poulsback_lost_fire(struct poulsback_lost_view *v)
{
    if (v->reported || !v->callback.reset || !poulsback_lost())
        return;
    v->reported = true;
    v->callback.reset(v->callback.data, PIPE_UNKNOWN_CONTEXT_RESET);
}
