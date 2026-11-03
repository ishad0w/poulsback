// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_COMPILER_COMPAT_H
#define SGX535_COMPILER_COMPAT_H
#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#else
#include <stddef.h>
#include <stdint.h>
#endif
#endif
