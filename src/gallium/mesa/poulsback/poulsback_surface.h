// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SURFACE_H
#define POULSBACK_SURFACE_H
struct pipe_screen;
struct pipe_framebuffer_state;
/* Mesa26 surfaces are inline descriptors. Validate all attachments before any
 * reference change, retain incoming references before releasing old ones, and
 * leave dst unchanged on failure. dst is initialized owned state; src/dst may
 * be identical. Color storage is canonical single-level storage. A zsbuf may
 * name only the explicit canonical CPU depth-capture metadata object; all
 * format/level/layer/sample/dimension fields must match it exactly. This owns
 * descriptors/references, not depth backing, rendering or GPU visibility. */
int poulsback_surface_framebuffer_set(struct pipe_screen *, struct pipe_framebuffer_state *dst,
                                      const struct pipe_framebuffer_state *src);
#endif
