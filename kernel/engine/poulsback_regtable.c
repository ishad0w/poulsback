// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_regtable.h"
#include "poulsback_vm_layout.h"

/* Value rules. PLAIN: any bits outside `mask` must be zero. VA: the BO address
 * itself, aligned to `align` (a power of two). VA_LOW28 / VA_HIGH4: the low 28 or high 4 bits
 * of a BO address (depth load/store bases); a zero value without a BO means
 * "unused". BGOBJ: BGOBJTAG, TSPDATASIZE (bits 29:24) from the value and the
 * vertex pointer (bits 23:0) from a 16-byte aligned PARAMETER BO address, in
 * 16-byte units from the 3D request base (docs/kernel/execution.md);
 * zero without a BO means no background. */
enum rule { PLAIN, VA, VA_LOW28, VA_HIGH4, BGOBJ };
#define BGOBJ_TSPDATASIZE 0x3f000000U

static const struct entry {
	sgx535_u32 phase, offset, rule, mask_or_align;
} table[] = {
	/* TA prefix: TE/MTE state and the VDM control stream. */
	{POULSBACK_REG_TA, 0x204, PLAIN, 0xffffffffU},
	{POULSBACK_REG_TA, 0x218, PLAIN, 0xfeffffffU}, /* TE_PSG, bit 24 stays 0 */
	{POULSBACK_REG_TA, 0x23c, PLAIN, 0xffffffffU},
	{POULSBACK_REG_TA, 0x240, PLAIN, 0xffffffffU},
	{POULSBACK_REG_TA, 0x244, PLAIN, 0xffffffffU},
	{POULSBACK_REG_TA, 0x250, PLAIN, 0xffffffffU},
	{POULSBACK_REG_TA, 0x238, VA, 4},
	/* Raster prefix: ISP/ZLS/TSP/PBE state, background object, pixel event. */
	{POULSBACK_REG_RASTER, 0x400, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x404, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x40c, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x410, PLAIN, 0xffffffffU}, /* RENDBOX2: 0 under EMGD */
	{POULSBACK_REG_RASTER, 0x414, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x418, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x41c, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x420, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x424, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x42c, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x480, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0xcb0, VA_HIGH4, 0},
	{POULSBACK_REG_RASTER, 0x484, VA_LOW28, 0},
	{POULSBACK_REG_RASTER, 0x488, VA_LOW28, 0},
	{POULSBACK_REG_RASTER, 0x48c, VA_LOW28, 0},
	{POULSBACK_REG_RASTER, 0x490, VA_LOW28, 0},
	{POULSBACK_REG_RASTER, 0x494, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x4c4, BGOBJ, 0},
	{POULSBACK_REG_RASTER, 0x4bc, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x4b8, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x4c8, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x4dc, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0x800, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0xa5c, VA, 16},
	{POULSBACK_REG_RASTER, 0xa60, PLAIN, 0xffffffffU},
	{POULSBACK_REG_RASTER, 0xa64, PLAIN, 0xffffffffU},
};
_Static_assert(sizeof(table) / sizeof(table[0]) ==
		       POULSBACK_REG_TA_SLOTS + POULSBACK_REG_RASTER_SLOTS,
	       "the slot counts cover the table");
_Static_assert(POULSBACK_REG_TA_SLOTS <= 32 && POULSBACK_REG_RASTER_SLOTS <= 32,
	       "`seen` holds one bit per slot of a phase");

enum poulsback_reg_status poulsback_reg_resolve(sgx535_u32 phase,
						const struct poulsback_reg_request *r,
						sgx535_u32 *seen, sgx535_u32 *value)
{
	sgx535_u64 start, end;
	sgx535_u32 slot, rank = 0;
	const struct entry *e = 0;
	for (slot = 0; slot < sizeof(table) / sizeof(table[0]); slot++) {
		if (table[slot].phase != phase)
			continue;
		if (table[slot].offset == r->offset) {
			e = &table[slot];
			break;
		}
		rank++;
	}
	if (!e)
		return POULSBACK_REG_UNKNOWN;
	if (*seen & (1U << rank))
		return POULSBACK_REG_DUPLICATE;
	switch (e->rule) {
	case PLAIN:
		if (r->has_va)
			return POULSBACK_REG_UNEXPECTED_BO;
		if (r->value & ~e->mask_or_align)
			return POULSBACK_REG_BAD_VALUE;
		*value = r->value;
		break;
	case VA:
		if (!r->has_va)
			return POULSBACK_REG_NEEDS_BO;
		if (r->value || r->va >> 32 || (r->va & (e->mask_or_align - 1)))
			return POULSBACK_REG_BAD_ADDRESS;
		*value = (sgx535_u32)r->va;
		break;
	case VA_LOW28:
	case VA_HIGH4:
		if (!r->has_va) {
			if (r->value)
				return POULSBACK_REG_NEEDS_BO;
			*value = 0;
			break;
		}
		if (r->value || r->va >> 32)
			return POULSBACK_REG_BAD_ADDRESS;
		*value = e->rule == VA_LOW28 ? (sgx535_u32)r->va & 0x0fffffffU
					     : (sgx535_u32)r->va & 0xf0000000U;
		break;
	default: /* BGOBJ */
		if (r->value & ~BGOBJ_TSPDATASIZE)
			return POULSBACK_REG_BAD_VALUE;
		if (!r->has_va) {
			if (r->value)
				return POULSBACK_REG_NEEDS_BO;
			*value = 0;
			break;
		}
		if (poulsback_vm_class_range(POULSBACK_VM_PARAMETER, &start, &end) ||
		    r->va < start || r->va >= end || (r->va & 15))
			return POULSBACK_REG_BAD_ADDRESS;
		*value = r->value | (sgx535_u32)((r->va - POULSBACK_VM_3D_REQ_BASE) >> 4);
		break;
	}
	*seen |= 1U << rank;
	return POULSBACK_REG_OK;
}

sgx535_u32 poulsback_reg_complete(sgx535_u32 phase, sgx535_u32 seen)
{
	sgx535_u32 slots = phase == POULSBACK_REG_TA	   ? POULSBACK_REG_TA_SLOTS
			   : phase == POULSBACK_REG_RASTER ? POULSBACK_REG_RASTER_SLOTS
							   : 0;
	return slots && seen == ~0U >> (32 - slots);
}
