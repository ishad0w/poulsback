// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_vm_layout.h"

/* Device ranges (parameter buffer and kernel objects 0x30000000-0x33FFFFFF,
 * DPM tables 0x48000000-0x49FFFFFF, scene buffers 0x4C000000-0x4FFFFFFF, the
 * 2D scratch page 0xD0000000) lie outside every class and are never handed to
 * a BO. */
static const struct {
	sgx535_u64 start, end, max_bytes;
} classes[POULSBACK_VM_CLASSES] = {
	[POULSBACK_VM_GENERAL] = {0x60000000ULL, 0xD0000000ULL, 64ULL << 20},
	[POULSBACK_VM_PDS] = {POULSBACK_VM_PDS_EXEC_BASE, POULSBACK_VM_3D_REQ_BASE, 1ULL << 20},
	[POULSBACK_VM_CODE] = {POULSBACK_VM_CODE_VA,
			       POULSBACK_VM_CODE_VA +
				       POULSBACK_VM_CODE_WINDOWS * POULSBACK_VM_CODE_BANK,
			       POULSBACK_VM_CODE_BANK},
	[POULSBACK_VM_DEPTH] = {0x50000000ULL, 0x60000000ULL, 16ULL << 20},
	[POULSBACK_VM_2D] = {0xD0001000ULL, 0xE0000000ULL, 64ULL << 20},
	[POULSBACK_VM_PARAMETER] = {0x34000000ULL, 0x40000000ULL, 1ULL << 20},
};

enum poulsback_vm_status poulsback_vm_class_range(sgx535_u32 vm_class, sgx535_u64 *start,
						  sgx535_u64 *end)
{
	if (vm_class >= POULSBACK_VM_CLASSES)
		return POULSBACK_VM_BAD_CLASS;
	*start = classes[vm_class].start;
	*end = classes[vm_class].end;
	return POULSBACK_VM_OK;
}

enum poulsback_vm_status poulsback_vm_check_bo(sgx535_u32 vm_class, sgx535_u64 size,
					       sgx535_u64 *rounded, sgx535_u64 *alignment)
{
	sgx535_u64 pages, align = POULSBACK_VM_PAGE;
	if (vm_class >= POULSBACK_VM_CLASSES)
		return POULSBACK_VM_BAD_CLASS;
	if (!size)
		return POULSBACK_VM_BAD_SIZE;
	if (size > classes[vm_class].max_bytes)
		return POULSBACK_VM_TOO_BIG;
	pages = (size + POULSBACK_VM_PAGE - 1) / POULSBACK_VM_PAGE;
	if (vm_class == POULSBACK_VM_CODE)
		while (align < pages * POULSBACK_VM_PAGE)
			align <<= 1;
	*rounded = pages * POULSBACK_VM_PAGE;
	*alignment = align;
	return POULSBACK_VM_OK;
}

int poulsback_vm_scanout_ok(sgx535_u32 vm_class, sgx535_u64 size)
{
	return (vm_class == POULSBACK_VM_GENERAL || vm_class == POULSBACK_VM_2D) && size &&
	       size <= POULSBACK_VM_SCANOUT_MAX_BYTES;
}

int poulsback_vm_scanout_view_ok(sgx535_u32 width, sgx535_u32 height, sgx535_u32 pitch,
				 sgx535_u64 bo_size)
{
	return width && height && !(pitch % 64) && pitch >= (sgx535_u64)width * 4 &&
	       (sgx535_u64)pitch * height <= bo_size;
}

int poulsback_vm_span_ok(sgx535_u32 vm_class, sgx535_u64 va, sgx535_u64 size)
{
	if (vm_class >= POULSBACK_VM_CLASSES || !size || (va % POULSBACK_VM_PAGE) ||
	    va < classes[vm_class].start || va >= classes[vm_class].end ||
	    size > classes[vm_class].end - va)
		return 0;
	if (vm_class == POULSBACK_VM_CODE &&
	    va / POULSBACK_VM_CODE_BANK != (va + size - 1) / POULSBACK_VM_CODE_BANK)
		return 0;
	return 1;
}

enum poulsback_vm_status poulsback_vm_pte(sgx535_u64 phys, int snooped, sgx535_u32 *word)
{
	if ((phys % POULSBACK_VM_PAGE) || phys >> 32)
		return POULSBACK_VM_BAD_ADDRESS;
	*word = (sgx535_u32)phys | POULSBACK_VM_PTE_VALID |
		(snooped ? POULSBACK_VM_PTE_SNOOPED : 0);
	return POULSBACK_VM_OK;
}

enum poulsback_vm_status poulsback_vm_pde(sgx535_u64 phys, sgx535_u32 *word)
{
	if ((phys % POULSBACK_VM_PAGE) || phys >> 32)
		return POULSBACK_VM_BAD_ADDRESS;
	*word = (sgx535_u32)phys | POULSBACK_VM_PTE_VALID;
	return POULSBACK_VM_OK;
}
