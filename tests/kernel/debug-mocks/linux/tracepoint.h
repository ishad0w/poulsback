// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The `debug` host suite's stand-in for <linux/tracepoint.h>: the mocks
 * define trace_sgx535_cached_lifecycle() as a recorder, so the event itself
 * becomes a declaration that takes its semicolon. */
#include "../debug_kernel_mocks.h"
#define TRACE_EVENT(name, ...) extern int mock_trace_event_##name
