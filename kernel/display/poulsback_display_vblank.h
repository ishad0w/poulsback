/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* The vblank count of a display pipe (HW-DISP-004). Pure; no device access. */
#ifndef POULSBACK_DISPLAY_VBLANK_H
#define POULSBACK_DISPLAY_VBLANK_H

#include "sgx535_types.h"

/* The raw frame counter increments at the start of active, but the vblank
 * interrupt runs in vblank. This count of vblank starts is the raw frame plus
 * one from the start of line vblank_start - 1, a margin of up to one line
 * before the vblank start: every reader, the interrupt included, sees one
 * count, and a BASE store made before a count was read has latched once the
 * count passes it. frame and pixel come from one PIPExFRAMEPIXEL read; htotal
 * and vblank are the pipe's raw HTOTAL and VBLANK registers. */
static inline sgx535_u32 poulsback_display_vblank_count(sgx535_u32 frame, sgx535_u32 pixel,
							sgx535_u32 htotal, sgx535_u32 vblank)
{
	sgx535_u32 line = vblank & 0x1fff; /* vblank_start - 1 */
	sgx535_u32 width = ((htotal >> 16) & 0x1fff) + 1;
	return (frame + (pixel >= line * width)) & 0xffffff;
}

#endif
