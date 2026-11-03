/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

/* Fixed per-file SGX VA layout and page-table words (docs/kernel/memory.md).
 * Pure functions; no device access, allocation or locking. */
#ifndef POULSBACK_VM_LAYOUT_H
#define POULSBACK_VM_LAYOUT_H

#include "sgx535_types.h"

/* Same values as the POULSBACK_BO_CLASS_* UAPI constants. */
enum poulsback_vm_class {
	POULSBACK_VM_GENERAL = 0,
	POULSBACK_VM_PDS,
	POULSBACK_VM_CODE,
	POULSBACK_VM_DEPTH,
	POULSBACK_VM_2D,
	POULSBACK_VM_PARAMETER,
	POULSBACK_VM_CLASSES
};

enum poulsback_vm_status {
	POULSBACK_VM_OK = 0,
	POULSBACK_VM_BAD_CLASS,
	POULSBACK_VM_BAD_SIZE,
	POULSBACK_VM_TOO_BIG,
	POULSBACK_VM_BAD_ADDRESS
};

#define POULSBACK_VM_PAGE 4096U
/* Every directory spans [START, END); below and above stay unmapped. */
#define POULSBACK_VM_START 0x20000000ULL
#define POULSBACK_VM_END 0xE0000000ULL
/* PDS_EXEC_BASE: PDS pointers are offsets from the PDS class's start. */
#define POULSBACK_VM_PDS_EXEC_BASE 0x20000000U
/* The 3D request base (BIF 3D_REQ_BASE). ISP addresses relative to it, such as
 * BGOBJTAG's vertex pointer, reach [base, base + 256 MiB): the device range and
 * the PARAMETER class. */
#define POULSBACK_VM_3D_REQ_BASE 0x30000000U
/* Device ranges outside every class, at the same VAs in the kernel directory
 * and in every file's directory: the parameter buffer and kernel objects at
 * the 3D request base, the DPM tables, and the scene buffers, one 1 MiB slot
 * per execution context. */
#define POULSBACK_VM_PARAM_VA POULSBACK_VM_3D_REQ_BASE
#define POULSBACK_VM_TABLE_VA 0x48000000U
#define POULSBACK_VM_SCENE_VA 0x4C000000ULL
#define POULSBACK_VM_SCENE_END 0x50000000ULL
#define POULSBACK_VM_SCENE_SLOT (1ULL << 20)
/* Six 512 KiB code windows from CODE_VA, each covered by one vertex and
 * one pixel USE code bank (docs/kernel/memory.md). */
#define POULSBACK_VM_CODE_VA 0x40000000U
#define POULSBACK_VM_CODE_WINDOWS 6U
#define POULSBACK_VM_CODE_BANK 0x80000U
/* The 2D request base (BIF TWOD_REQ_BASE): the 2D core reaches [base,
 * base + 256 MiB), the 2D class above its first page. That page is the file's
 * 2D scratch page in every address space: after a job's last operation the 2D
 * core touches the last page it wrote again when the next 2D job starts, under
 * that job's directory (HW-2D-004), so every job ends with a write here. */
#define POULSBACK_VM_TWOD_REQ_BASE 0xD0000000U
#define POULSBACK_VM_2D_SCRATCH_VA POULSBACK_VM_TWOD_REQ_BASE
#define POULSBACK_VM_PTE_VALID 1U
#define POULSBACK_VM_PTE_SNOOPED 8U

/* [start, end) of a class's range. */
enum poulsback_vm_status poulsback_vm_class_range(sgx535_u32 vm_class, sgx535_u64 *start,
						  sgx535_u64 *end);

/* Checks a BO request: page-rounded size within the class limit, and the
 * allocator alignment that keeps a code BO inside one 512 KiB bank. */
enum poulsback_vm_status poulsback_vm_check_bo(sgx535_u32 vm_class, sgx535_u64 size,
					       sgx535_u64 *rounded, sgx535_u64 *alignment);

/* SCANOUT BOs: GENERAL or 2D class (the 2D core reaches only the 2D range),
 * at most 16 MiB. */
#define POULSBACK_VM_SCANOUT_MAX_BYTES (16U << 20)
int poulsback_vm_scanout_ok(sgx535_u32 vm_class, sgx535_u64 size);

/* One LINEAR 32 bpp plane of a framebuffer over a SCANOUT BO of bo_size
 * bytes: nonzero size, pitch a multiple of 64 holding a row, all rows inside. */
int poulsback_vm_scanout_view_ok(sgx535_u32 width, sgx535_u32 height, sgx535_u32 pitch,
				 sgx535_u64 bo_size);

/* Whether [va, va + size) lies in the class range (and, for code, one bank). */
int poulsback_vm_span_ok(sgx535_u32 vm_class, sgx535_u64 va, sgx535_u64 size);

/* PTE for a page at phys; snooped for write-back CPU pages. */
enum poulsback_vm_status poulsback_vm_pte(sgx535_u64 phys, int snooped, sgx535_u32 *word);

/* PDE for a page table at phys. */
enum poulsback_vm_status poulsback_vm_pde(sgx535_u64 phys, sgx535_u32 *word);

static inline sgx535_u32 poulsback_vm_pde_index(sgx535_u64 va)
{
	return (sgx535_u32)(va >> 22) & 1023U;
}

static inline sgx535_u32 poulsback_vm_pte_index(sgx535_u64 va)
{
	return (sgx535_u32)(va >> 12) & 1023U;
}
#endif
