/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef SGX535_DEBUG_H
#define SGX535_DEBUG_H
#include <linux/types.h>
/* The `debug` module parameter's categories (docs/kernel/diagnostics.md),
 * root-writable at runtime. Errors and reasons are recorded whatever the
 * mask. */
#define SGX535_DEBUG_CORE 0x01U	   /* epoch and lease records are delivered */
#define SGX535_DEBUG_MMU 0x02U	   /* reserved; no MMU records yet */
#define SGX535_DEBUG_IRQ 0x04U	   /* reserved; no IRQ records yet */
#define SGX535_DEBUG_SUBMIT 0x08U  /* refused requests are logged (POULSBACK_REFUSE) */
#define SGX535_DEBUG_DISPLAY 0x10U /* vblank and flip timing (debugfs poulsback_flips) */
#define SGX535_DEBUG_DMESG 0x20U   /* delivered records are also printed, rate-limited */
/* Any of CATEGORIES set in the parameter now. */
bool sgx535_debug_on(u32 categories);
enum sgx535_diag_kind { SGX535_DIAG_EPOCH = 1, SGX535_DIAG_LEASE, SGX535_DIAG_KINDS };
/* Cache only. No pointer, PFN, MMIO, memory content or source revalidation.
 * Read scalars under the caller's lock; call record after releasing it.
 * error/reason records are retained even when verbose debug is off. */
void sgx535_diag_record(u32 kind, u64 id, u64 epoch, u64 serial, u32 state, s32 error, u32 reason,
			u64 bytes);
/* psb_init's: each successful start owns one reference; failure owns none.
 * Final stop takes delivery offline, synchronizes the worker and removes the
 * proc entry. */
int sgx535_diag_start(void);
void sgx535_diag_stop(void);
#endif
