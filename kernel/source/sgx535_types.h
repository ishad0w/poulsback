/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* Internal fixed-width SGX535 model types. No wire structures or device access. */
#ifndef POULSBACK_SGX535_TYPES_H
#define POULSBACK_SGX535_TYPES_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 sgx535_u32;
typedef u64 sgx535_u64;
typedef u8 sgx535_u8;
#else
#include <stdint.h>
typedef uint32_t sgx535_u32;
typedef uint64_t sgx535_u64;
typedef uint8_t sgx535_u8;
#endif
#endif
