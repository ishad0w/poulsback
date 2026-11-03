/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef POULSBACK_ERRSITE_H
#define POULSBACK_ERRSITE_H

/* Names the source line that produced an error code, so a failed device run
 * identifies its origin from dmesg alone. The record is one constant string:
 * no device access, allocation or extra module frame. KERN_NOTICE stays below
 * the quiet console level, so the failing path only stores the record. Each
 * site is rate-limited on its own (printk_ratelimited: 10 lines per 5 s), so
 * a client refused at ioctl rate cannot flood the log, and a site's first
 * lines still reach a dmesg scan for errsite records. Kbuild objects log; RAM
 * fixtures without Kbuild get the plain value. */
#ifdef KBUILD_MODNAME
#include <linux/printk.h>
#include <linux/stringify.h>
#define POULSBACK_ERRSITE(err)                                                                     \
	({                                                                                         \
		printk_ratelimited(KERN_NOTICE "poulsback errsite " #err " " __FILE__              \
				   ":" __stringify(__LINE__) "\n");                                \
		(err);                                                                             \
	})
/* A refused request (a caller's arguments, handles or registers, a full
 * window it may retry): the same record, only with the submit debug category
 * set, so a client refused at ioctl rate logs nothing by default. */
#include "sgx535_debug.h"
#define POULSBACK_REFUSE(err)                                                                      \
	({                                                                                         \
		if (sgx535_debug_on(SGX535_DEBUG_SUBMIT))                                          \
			printk_ratelimited(KERN_NOTICE "poulsback errsite " #err " " __FILE__      \
					   ":" __stringify(__LINE__) "\n");                        \
		(err);                                                                             \
	})
#else
#define POULSBACK_ERRSITE(err) (err)
#define POULSBACK_REFUSE(err) (err)
#endif

#endif
