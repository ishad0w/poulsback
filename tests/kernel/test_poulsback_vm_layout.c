// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Independent checks of the fixed per-file VA layout; literals, not the table. */
#include "../../kernel/memory/poulsback_vm_layout.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "vm-layout check %u line %d: %s\n", checks, __LINE__, #x);             \
        }                                                                                          \
    } while (0)

static int in_class(sgx535_u64 va)
{
    for (sgx535_u32 c = 0; c < POULSBACK_VM_CLASSES; c++)
        if (poulsback_vm_span_ok(c, va, POULSBACK_VM_PAGE))
            return 1;
    return 0;
}

int main(void)
{
    static const struct {
        sgx535_u32 vm_class;
        sgx535_u64 start, end;
    } ranges[] = {{POULSBACK_VM_GENERAL, 0x60000000, 0xd0000000},
                  {POULSBACK_VM_PDS, 0x20000000, 0x30000000},
                  {POULSBACK_VM_CODE, 0x40000000, 0x40300000},
                  {POULSBACK_VM_DEPTH, 0x50000000, 0x60000000},
                  {POULSBACK_VM_2D, 0xd0001000, 0xe0000000},
                  {POULSBACK_VM_PARAMETER, 0x34000000, 0x40000000}};
    static const sgx535_u64 device[] = {0x30000000, 0x33fff000, 0x48000000, 0x49fff000, 0x4c000000,
                                        0x4ffff000, 0x1ffff000, 0xe0000000, 0};
    sgx535_u64 start, end, size, align;
    sgx535_u32 word;

    for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        CHECK(!poulsback_vm_class_range(ranges[i].vm_class, &start, &end));
        CHECK(start == ranges[i].start && end == ranges[i].end);
        CHECK(poulsback_vm_span_ok(ranges[i].vm_class, ranges[i].start, POULSBACK_VM_PAGE));
        CHECK(poulsback_vm_span_ok(ranges[i].vm_class, ranges[i].end - POULSBACK_VM_PAGE,
                                   POULSBACK_VM_PAGE));
        CHECK(!poulsback_vm_span_ok(ranges[i].vm_class, ranges[i].end - POULSBACK_VM_PAGE,
                                    2 * POULSBACK_VM_PAGE));
        CHECK(!poulsback_vm_span_ok(ranges[i].vm_class, ranges[i].start - POULSBACK_VM_PAGE,
                                    POULSBACK_VM_PAGE));
    }
    CHECK(poulsback_vm_class_range(POULSBACK_VM_CLASSES, &start, &end) == POULSBACK_VM_BAD_CLASS);
    /* Device ranges and the guards belong to no class. */
    for (unsigned i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        CHECK(!in_class(device[i]));
    CHECK(!in_class(POULSBACK_VM_2D_SCRATCH_VA) &&
          POULSBACK_VM_2D_SCRATCH_VA == POULSBACK_VM_TWOD_REQ_BASE);
    /* The named device ranges and bases the engine and contexts map. */
    CHECK(POULSBACK_VM_START == 0x20000000 && POULSBACK_VM_END == 0xe0000000);
    CHECK(POULSBACK_VM_PDS_EXEC_BASE == 0x20000000 && POULSBACK_VM_PARAM_VA == 0x30000000);
    CHECK(POULSBACK_VM_TABLE_VA == 0x48000000 && POULSBACK_VM_CODE_VA == 0x40000000);
    CHECK(POULSBACK_VM_SCENE_VA == 0x4c000000 && POULSBACK_VM_SCENE_END == 0x50000000 &&
          POULSBACK_VM_SCENE_SLOT == 0x100000);
    CHECK(!in_class(POULSBACK_VM_PARAM_VA) && !in_class(POULSBACK_VM_TABLE_VA));
    for (sgx535_u64 va = POULSBACK_VM_SCENE_VA; va < POULSBACK_VM_SCENE_END;
         va += POULSBACK_VM_SCENE_SLOT)
        CHECK(!in_class(va) && !in_class(va + POULSBACK_VM_SCENE_SLOT - POULSBACK_VM_PAGE));

    /* Sizes: zero, limits, rounding. */
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_GENERAL, 0, &size, &align) == POULSBACK_VM_BAD_SIZE);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_GENERAL, 1, &size, &align) == POULSBACK_VM_OK &&
          size == 4096 && align == 4096);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_GENERAL, 64u << 20, &size, &align) ==
              POULSBACK_VM_OK &&
          size == 64u << 20);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_GENERAL, (64u << 20) + 1, &size, &align) ==
          POULSBACK_VM_TOO_BIG);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_PDS, (1u << 20) + 1, &size, &align) ==
          POULSBACK_VM_TOO_BIG);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_DEPTH, 16u << 20, &size, &align) == POULSBACK_VM_OK);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_DEPTH, (16u << 20) + 1, &size, &align) ==
          POULSBACK_VM_TOO_BIG);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_2D, 64u << 20, &size, &align) == POULSBACK_VM_OK);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_PARAMETER, 1u << 20, &size, &align) ==
          POULSBACK_VM_OK);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_PARAMETER, (1u << 20) + 1, &size, &align) ==
          POULSBACK_VM_TOO_BIG);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_CLASSES, 4096, &size, &align) ==
          POULSBACK_VM_BAD_CLASS);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_GENERAL, ~0ULL, &size, &align) ==
          POULSBACK_VM_TOO_BIG);

    /* Code: at most one bank, aligned so it never crosses one. */
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_CODE, 0x80000, &size, &align) == POULSBACK_VM_OK &&
          size == 0x80000 && align == 0x80000);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_CODE, 0x80001, &size, &align) == POULSBACK_VM_TOO_BIG);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_CODE, 0x5000, &size, &align) == POULSBACK_VM_OK &&
          size == 0x5000 && align == 0x8000);
    CHECK(poulsback_vm_check_bo(POULSBACK_VM_CODE, 100, &size, &align) == POULSBACK_VM_OK &&
          size == 0x1000 && align == 0x1000);
    CHECK(poulsback_vm_span_ok(POULSBACK_VM_CODE, 0x40078000, 0x8000));
    CHECK(!poulsback_vm_span_ok(POULSBACK_VM_CODE, 0x4007c000, 0x8000));
    CHECK(!poulsback_vm_span_ok(POULSBACK_VM_GENERAL, 0x60000800, 4096));
    CHECK(!poulsback_vm_span_ok(POULSBACK_VM_GENERAL, 0x60000000, 0));

    /* SCANOUT: GENERAL or 2D, at most 16 MiB. */
    CHECK(poulsback_vm_scanout_ok(POULSBACK_VM_GENERAL, 4096000));
    CHECK(poulsback_vm_scanout_ok(POULSBACK_VM_2D, 16u << 20));
    CHECK(!poulsback_vm_scanout_ok(POULSBACK_VM_GENERAL, (16u << 20) + 4096));
    CHECK(!poulsback_vm_scanout_ok(POULSBACK_VM_GENERAL, 0));
    for (sgx535_u32 c = 0; c < POULSBACK_VM_CLASSES + 1; c++)
        if (c != POULSBACK_VM_GENERAL && c != POULSBACK_VM_2D)
            CHECK(!poulsback_vm_scanout_ok(c, 4096));
    /* Framebuffer views over a 1280x800 BO of 4,096,000 bytes. */
    CHECK(poulsback_vm_scanout_view_ok(1280, 800, 5120, 4096000));
    CHECK(poulsback_vm_scanout_view_ok(1280, 800, 5120, 16u << 20));
    CHECK(!poulsback_vm_scanout_view_ok(1280, 800, 5100, 4096000));
    CHECK(!poulsback_vm_scanout_view_ok(1280, 800, 5056, 4096000));
    CHECK(!poulsback_vm_scanout_view_ok(1280, 800, 5120, 4096000 - 4096));
    CHECK(!poulsback_vm_scanout_view_ok(1280, 800, 5184, 4096000));
    CHECK(poulsback_vm_scanout_view_ok(1270, 800, 5120, 4096000));
    CHECK(!poulsback_vm_scanout_view_ok(0, 800, 5120, 4096000));
    CHECK(!poulsback_vm_scanout_view_ok(1280, 0, 5120, 4096000));
    CHECK(!poulsback_vm_scanout_view_ok(0x40000000, 1, 0, 1ULL << 32));
    CHECK(!poulsback_vm_scanout_view_ok(16, 0x10000, 0xffffffc0, 16u << 20));

    /* Page-table words. */
    CHECK(!poulsback_vm_pte(0x12345000, 1, &word) && word == 0x12345009);
    CHECK(!poulsback_vm_pte(0x12345000, 0, &word) && word == 0x12345001);
    CHECK(poulsback_vm_pte(0x12345800, 0, &word) == POULSBACK_VM_BAD_ADDRESS);
    CHECK(poulsback_vm_pte(0x100000000ULL, 0, &word) == POULSBACK_VM_BAD_ADDRESS);
    CHECK(!poulsback_vm_pde(0x0abcd000, &word) && word == 0x0abcd001);
    CHECK(poulsback_vm_pde(0x0abcd004, &word) == POULSBACK_VM_BAD_ADDRESS);
    CHECK(poulsback_vm_pde_index(0xd0000000) == 832 && poulsback_vm_pte_index(0xd0003000) == 3);
    CHECK(poulsback_vm_pde_index(0x20000000) == 128 && poulsback_vm_pte_index(0x203ff000) == 1023);

    printf("vm-layout: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
