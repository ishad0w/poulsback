/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors */

#ifndef SGX535_PAGE_VALIDATE_H
#define SGX535_PAGE_VALIDATE_H
#include <linux/types.h>
/* Physical values come only from pinned kernel pages; never public UAPI.
 * The upper edge includes every byte of the aligned 4 KiB page. */
static inline int sgx535_page_addressable(__u64 physical, __u64 dummy)
{
	return !(physical & 4095) && physical <= 0xfffff000ULL && physical != dummy;
}
#endif
