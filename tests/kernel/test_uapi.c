// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Compile-time wire compatibility; no device is opened. The layouts use
 * fixed-width fields and explicit padding, so i386 and x86_64 agree. */
#include <stddef.h>
#include "poulsback_bo_uapi.h"
#include "poulsback_exec_uapi.h"
_Static_assert(sizeof(struct poulsback_bo_create) == 56, "BO create ABI1");
_Static_assert(sizeof(struct poulsback_bo_info) == 56, "BO info ABI1");
_Static_assert(POULSBACK_BO_CPU_WC == 1 && POULSBACK_BO_SCANOUT == 2, "BO placement flags");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_BO_CREATE) == 0x6e, "BO create slot46");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_BO_INFO) == 0x6f, "BO info slot47");
_Static_assert(sizeof(struct poulsback_display_info) == 32, "display info ABI1");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_DISPLAY_INFO) == 0x73, "display info slot51");
_Static_assert(sizeof(struct poulsback_context_create) == 32, "context create ABI1");
_Static_assert(sizeof(struct poulsback_context_destroy) == 16, "context destroy ABI1");
_Static_assert(sizeof(struct poulsback_submit_bo) == 8, "submit BO ABI1");
_Static_assert(sizeof(struct poulsback_submit_reg) == 16, "submit register ABI1");
_Static_assert(sizeof(struct poulsback_submit) == 80, "submit ABI1");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_CONTEXT_CREATE) == 0x70, "context create slot48");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_CONTEXT_DESTROY) == 0x71, "context destroy slot49");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_SUBMIT) == 0x72, "submit slot50");
_Static_assert(sizeof(struct poulsback_2d_surface) == 32, "2D surface ABI1");
_Static_assert(sizeof(struct poulsback_2d_rect) == 16, "2D rectangle ABI1");
_Static_assert(sizeof(struct poulsback_2d_op) == 64, "2D operation ABI1");
_Static_assert(offsetof(struct poulsback_2d_op, clip) == 48, "2D operation clip");
_Static_assert(sizeof(struct poulsback_submit_2d) == 72, "submit 2D ABI1");
_Static_assert(offsetof(struct poulsback_submit_2d, bos) == 24, "submit 2D pointers");
_Static_assert(_IOC_NR(DRM_IOCTL_PB_SUBMIT_2D) == 0x74, "submit 2D slot52");
_Static_assert(POULSBACK_2D_ARGB8888 == 1 && POULSBACK_2D_XRGB8888 == 2 &&
                   POULSBACK_2D_RGB565 == 3 && POULSBACK_2D_FILL == 1 && POULSBACK_2D_COPY == 2 &&
                   POULSBACK_2D_CLIP == 1,
               "2D values ABI1");
int main(void)
{
    return 0;
}
