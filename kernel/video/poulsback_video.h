/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_VIDEO_H
#define POULSBACK_VIDEO_H
#include <linux/types.h>
struct drm_device;
struct drm_file;
int poulsback_mpeg2_decode_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int poulsback_video_quiesce(struct drm_device *dev);
int poulsback_video_power_down(struct drm_device *dev);
int poulsback_video_resume(struct drm_device *dev, int error);
void poulsback_video_unquiesce(struct drm_device *dev);
bool poulsback_video_retained(struct drm_device *dev);
#endif
