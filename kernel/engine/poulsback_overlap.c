// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_overlap.h"

#define SLOT_3D 0U
#define SLOT_2D 1U
#define SLOT_KERNEL 15U

sgx535_u32 poulsback_overlap_bank0(const struct poulsback_overlap *s)
{
	sgx535_u32 ta = SLOT_KERNEL, d3 = SLOT_KERNEL, d2 = SLOT_KERNEL;
	if (s->ta || s->ready || s->raster)
		ta = d3 = SLOT_3D;
	if (s->run2d)
		ta = d2 = SLOT_2D;
	return SLOT_KERNEL | ta << 4 | SLOT_KERNEL << 8 | d3 << 12 | d2 << 16;
}

/* Starts what may start now: the ready job's raster; then the first waiting
 * TA when the TA unit and a DPM context are free, no 2D job runs, and it may
 * join the jobs holding contexts; else the waiting 2D job when no TA runs. */
static void next(struct poulsback_overlap *s, bool joins, struct poulsback_overlap_start *out)
{
	int held;
	out->raster = out->ta = out->twod = false;
	if (s->ready && !s->raster) {
		s->ready = false;
		s->raster = out->raster = true;
	}
	held = s->ready + s->raster;
	if (s->wait3d && !s->ta && !s->run2d && held < 2 && (!held || joins)) {
		s->wait3d--;
		s->ta = out->ta = true;
	}
	if (s->wait2d && !s->run2d && !s->ta) {
		s->wait2d = false;
		s->run2d = out->twod = true;
	}
	out->bank0 = poulsback_overlap_bank0(s);
}

bool poulsback_overlap_event(struct poulsback_overlap *s, enum poulsback_overlap_event event,
			     bool joins, struct poulsback_overlap_start *out)
{
	switch (event) {
	case POULSBACK_OVERLAP_ARRIVE3D:
		if (s->wait3d + s->ta + s->ready + s->raster >= POULSBACK_OVERLAP_CREDITS_3D)
			return false;
		s->wait3d++;
		break;
	case POULSBACK_OVERLAP_ARRIVE2D:
		if (s->wait2d || s->run2d)
			return false;
		s->wait2d = true;
		break;
	case POULSBACK_OVERLAP_TA_FINISHED:
		if (!s->ta)
			return false;
		s->ta = false;
		s->ready = true;
		break;
	case POULSBACK_OVERLAP_DONE3D:
		if (!s->raster)
			return false;
		s->raster = false;
		break;
	case POULSBACK_OVERLAP_DONE2D:
		if (!s->run2d)
			return false;
		s->run2d = false;
		break;
	default:
		return false;
	}
	next(s, joins, out);
	return true;
}
