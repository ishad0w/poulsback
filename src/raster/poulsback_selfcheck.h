/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */
#ifndef POULSBACK_SELFCHECK_H
#define POULSBACK_SELFCHECK_H
/* Re-verification of the driver's own data: second captures of a copy it
 * made, frozen copies and their comparisons, regenerate-and-compare. Inputs
 * are validated once at the trust boundary whatever this says
 * (AGENTS.md, docs/diagnostics.md). Host fixtures and debug builds keep it,
 * the default; the product kernel module and Mesa driver are built with
 * -DPOULSBACK_SELFCHECK=0. Use it as `if (POULSBACK_SELFCHECK && ...)` so
 * the checks stay compiled and type-checked. */
#ifndef POULSBACK_SELFCHECK
#define POULSBACK_SELFCHECK 1
#endif
#endif
