// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include <assert.h>
#include <stdio.h>
#include "sgx535_page_validate.h"
int main(void)
{
    assert(sgx535_page_addressable(0x1000, 0x2000));
    assert(sgx535_page_addressable(0xfffff000ULL, 0x2000));
    assert(!sgx535_page_addressable(0x100000000ULL, 0x2000));
    assert(!sgx535_page_addressable(0x500000000ULL, 0x2000));
    assert(!sgx535_page_addressable(0x600001000ULL, 0x2000));
    assert(!sgx535_page_addressable(0x1000, 0x1000));
    assert(!sgx535_page_addressable(0x1001, 0x2000));
    assert(!sgx535_page_addressable(0xffffffffULL, 0x2000));
    puts("PASS_PINNED_PAGE_ADDRESS_BOUNDARIES_CPU_ONLY");
    return 0;
}
