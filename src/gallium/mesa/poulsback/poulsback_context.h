// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_CONTEXT_H
#define POULSBACK_CONTEXT_H
struct pipe_screen;
struct pipe_context;
/* The context of a screen whose render client has the kernel's depth-scene
 * ABI and native fences; NULL otherwise. Frames are SUBMIT jobs of the frame
 * writer; there is no CPU fallback. A frame waits at most 2 s for a free
 * frame slot (poulsback_engine.c); flush only polls. */
struct pipe_context *poulsback_context_create(struct pipe_screen *, void *, unsigned);
#endif
