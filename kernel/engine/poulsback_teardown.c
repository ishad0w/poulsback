// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_teardown.h"

#define EVENT_MASTER 0x80000000U
#define TWOD_BUSY 0x01000000U

/* SGXDeinitialise's writes in SGXPrePowerState's OFF path (EMGD sgxpower.c,
 * sgxinit.c): 0xa58, the four 0xa48-0xa70
 * registers, HOST_ENABLE, HOST_ENABLE2, then CLKGATECTL. */
const struct poulsback_teardown_write
	poulsback_teardown_power_down[POULSBACK_TEARDOWN_POWER_DOWN_WRITES] = {
		{0x0a58U, 0}, {0x0a68U, 0}, {0x0a6cU, 0}, {0x0a70U, 0},
		{0x0a48U, 0}, {0x0130U, 0}, {0x0110U, 0}, {0x0000U, 0},
};

enum poulsback_teardown_refusal poulsback_teardown_drained(const struct poulsback_teardown_engine *e)
{
	if (!e->ready)
		return POULSBACK_TEARDOWN_LOST;
	if (e->held)
		return POULSBACK_TEARDOWN_HELD;
	return e->slots ? POULSBACK_TEARDOWN_BUSY : POULSBACK_TEARDOWN_OK;
}

enum poulsback_teardown_refusal poulsback_teardown_idle(const struct poulsback_teardown_idle *r)
{
	if ((r->event1 & ~EVENT_MASTER) || r->event2)
		return POULSBACK_TEARDOWN_EVENT;
	if (r->blit_status & TWOD_BUSY)
		return POULSBACK_TEARDOWN_TWOD;
	return r->bif_int_stat ? POULSBACK_TEARDOWN_FAULT : POULSBACK_TEARDOWN_OK;
}

enum poulsback_teardown_refusal poulsback_teardown_may_free(const struct poulsback_teardown_engine *e)
{
	enum poulsback_teardown_refusal r = poulsback_teardown_drained(e);
	if (r)
		return r;
	if (!e->quiesced)
		return POULSBACK_TEARDOWN_UNQUIESCED;
	return e->fences ? POULSBACK_TEARDOWN_FENCES : POULSBACK_TEARDOWN_OK;
}
