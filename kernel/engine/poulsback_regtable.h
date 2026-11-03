/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* The SUBMIT register table (docs/kernel/execution.md): the only registers a
 * job may set, with their value rules. Pure functions; no device access. */
#ifndef POULSBACK_REGTABLE_H
#define POULSBACK_REGTABLE_H

#include "sgx535_types.h"

enum poulsback_reg_phase { POULSBACK_REG_TA = 0, POULSBACK_REG_RASTER = 1 };

enum poulsback_reg_status {
	POULSBACK_REG_OK = 0,
	POULSBACK_REG_UNKNOWN,	     /* not in the table for this phase */
	POULSBACK_REG_DUPLICATE,     /* set twice in one phase */
	POULSBACK_REG_BAD_VALUE,     /* outside the register's value rule */
	POULSBACK_REG_NEEDS_BO,	     /* an address register without a BO */
	POULSBACK_REG_UNEXPECTED_BO, /* a plain register with a BO */
	POULSBACK_REG_BAD_ADDRESS    /* the BO address does not fit the register */
};

/* One register of a job as the kernel resolved it: `has_va` when the request
 * named a BO, `va` its GPU address (BO VA + offset) in the file's space. */
struct poulsback_reg_request {
	sgx535_u32 offset, value;
	sgx535_u32 has_va;
	sgx535_u64 va;
};

/* Resolves one register: the value to write, or why it is refused. `seen` is
 * a bitmap of the phase's table slots already set, so a phase sets at most its
 * slot count of registers. */
#define POULSBACK_REG_TA_SLOTS 7U
#define POULSBACK_REG_RASTER_SLOTS 26U
enum poulsback_reg_status poulsback_reg_resolve(sgx535_u32 phase,
						const struct poulsback_reg_request *request,
						sgx535_u32 *seen, sgx535_u32 *value);
/* Whether `seen` holds every slot of the phase. A job sets the whole table:
 * the engine keeps no register state a job may inherit from an earlier one. */
sgx535_u32 poulsback_reg_complete(sgx535_u32 phase, sgx535_u32 seen);
#endif
