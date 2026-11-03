// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_public.h"
#include <string.h>
#include <xf86drm.h>

enum poulsback_drm_identity poulsback_drm_identify(int fd)
{
    drmVersionPtr version;
    enum poulsback_drm_identity result = POULSBACK_DRM_OTHER_DRIVER;
    static const char name[] = "poulsback";

    if (fd < 0)
        return POULSBACK_DRM_INVALID_FD;
    version = drmGetVersion(fd);
    if (!version)
        return POULSBACK_DRM_QUERY_FAILED;

    /* The kernel string carries an explicit length. A prefix, embedded NUL,
     * another DRM driver, or a missing name must never select this backend. */
    if (version->name && version->name_len == (int)(sizeof(name) - 1) &&
        memcmp(version->name, name, sizeof(name) - 1) == 0)
        result = POULSBACK_DRM_NATIVE_UNQUALIFIED;
    drmFreeVersion(version);
    return result;
}
