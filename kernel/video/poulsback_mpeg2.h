/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_MPEG2_H
#define POULSBACK_MPEG2_H
#include "poulsback_video_uapi.h"

/* Commands and DMA records occupy separate parts of one kernel-owned page. */
#define PB_VIDEO_DMA_OFFSET 2048U
#define PB_VIDEO_VLC_OFFSET 3072U
#define PB_VIDEO_VLC_BYTES 764U
int poulsback_mpeg2_validate(const struct poulsback_mpeg2_decode *p,
			   const struct poulsback_mpeg2_slice *s, const u8 *data);
u32 poulsback_mpeg2_mode(const struct poulsback_mpeg2_decode *p);
u32 poulsback_mpeg2_commands(const struct poulsback_mpeg2_decode *p,
			   const struct poulsback_mpeg2_slice *s, u32 previous_row,
			   const u32 surfaces[3], u32 command_va, u32 bitstream_va,
			   void *page, u32 *render_dma);
void poulsback_mpeg2_vlc(void *page);
#endif
