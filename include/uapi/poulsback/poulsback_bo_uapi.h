/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_BO_UAPI_H
#define POULSBACK_BO_UAPI_H
#include <linux/types.h>

/* Buffer objects in a per-file SGX address space (docs/kernel/memory.md). */
#define POULSBACK_BO_ABI 1U

/* CPU placement: write-back pages with snooped GPU access (default), or
 * write-combined pages with non-snooped GPU access. */
#define POULSBACK_BO_CPU_WC 1U
/* Scanout: a KMS framebuffer may be made of it (ADDFB2). Only with CPU_WC, in
 * the GENERAL or 2D class, at most 16 MiB. */
#define POULSBACK_BO_SCANOUT 2U

/* VA class: which fixed range of the file's address space the BO lives in. */
#define POULSBACK_BO_CLASS_GENERAL 0U
#define POULSBACK_BO_CLASS_PDS 1U
#define POULSBACK_BO_CLASS_CODE 2U
#define POULSBACK_BO_CLASS_DEPTH 3U
#define POULSBACK_BO_CLASS_2D 4U
/* Background objects: addressed relative to the 3D request base 0x30000000. */
#define POULSBACK_BO_CLASS_PARAMETER 5U

struct poulsback_bo_create {
    __u32 abi, flags, bo_class, handle;
    __u64 size, gpu_va, mmap_offset;
    __u32 reserved[4];
};
struct poulsback_bo_info {
    __u32 abi, handle, flags, bo_class;
    __u64 size, gpu_va, mmap_offset;
    __u32 reserved[4];
};
/* The geometry a SCANOUT buffer needs to be page-flipped onto the display: the
 * first enabled CRTC's mode, its 64-byte-aligned XRGB8888 pitch and format
 * (a DRM fourcc). All zero when no CRTC shows anything. Read-only. */
struct poulsback_display_info {
    __u32 abi, width, height, stride, format;
    __u32 reserved[3];
};

#ifdef __KERNEL__
#include <drm/drm.h>
#else
#include <drm.h>
#endif
#define DRM_IOCTL_PB_BO_CREATE DRM_IOWR(DRM_COMMAND_BASE + 46, struct poulsback_bo_create)
#define DRM_IOCTL_PB_BO_INFO DRM_IOWR(DRM_COMMAND_BASE + 47, struct poulsback_bo_info)
#define DRM_IOCTL_PB_DISPLAY_INFO DRM_IOWR(DRM_COMMAND_BASE + 51, struct poulsback_display_info)
#endif
