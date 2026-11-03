/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM sgx535
#if !defined(SGX535_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define SGX535_TRACE_H
#include <linux/tracepoint.h>
TRACE_EVENT(
	sgx535_cached_lifecycle,
	TP_PROTO(u64 seq, u32 kind, u64 id, u64 epoch, u64 serial, u32 state, s32 error, u32 reason,
		 u64 bytes),
	TP_ARGS(seq, kind, id, epoch, serial, state, error, reason, bytes),
	TP_STRUCT__entry(__field(u64, seq) __field(u32, kind) __field(u64, id) __field(u64, epoch)
				 __field(u64, serial) __field(u32, state) __field(s32, error)
					 __field(u32, reason) __field(u64, bytes)),
	TP_fast_assign(__entry->seq = seq; __entry->kind = kind; __entry->id = id;
		       __entry->epoch = epoch; __entry->serial = serial; __entry->state = state;
		       __entry->error = error; __entry->reason = reason; __entry->bytes = bytes;),
	TP_printk(
		"seq=%llu kind=%u id=%llu epoch=%llu serial=%llu state=%u error=%d reason=%u bytes=%llu",
		__entry->seq, __entry->kind, __entry->id, __entry->epoch, __entry->serial,
		__entry->state, __entry->error, __entry->reason, __entry->bytes));
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE sgx535_trace
#include <trace/define_trace.h>
