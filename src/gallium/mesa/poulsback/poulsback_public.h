// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Independent Mesa loader admission interface; no rendering capability. */
#ifndef POULSBACK_GALLIUM_PUBLIC_H
#define POULSBACK_GALLIUM_PUBLIC_H

struct pipe_screen;
struct pipe_screen_config;

enum poulsback_drm_identity {
    POULSBACK_DRM_INVALID_FD,
    POULSBACK_DRM_QUERY_FAILED,
    POULSBACK_DRM_OTHER_DRIVER,
    POULSBACK_DRM_NATIVE_UNQUALIFIED
};

/* fd is borrowed. These functions neither close it nor allocate GPU objects. */
enum poulsback_drm_identity poulsback_drm_identify(int fd);
struct pipe_screen *poulsback_drm_screen_create(int fd, const struct pipe_screen_config *config);
#endif
