// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "msvdx_owned.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static uint8_t command[64], source[64], before[64];
static struct msvdx_owned_buffer buffers[2];
static struct msvdx_owned_reloc relocs[2];
static uint8_t max_maps[MSVDX_OWNED_MAX_BUFFERS][256];
static struct msvdx_owned_buffer max_buffers[MSVDX_OWNED_MAX_BUFFERS];
static struct msvdx_owned_reloc max_relocs[MSVDX_OWNED_MAX_RELOCS];
static void fixture(void)
{
    memset(command, 0xa5, sizeof(command));
    memset(source, 0x5a, sizeof(source));
    memcpy(before, command, sizeof(before));
    buffers[0] = (struct msvdx_owned_buffer){1, MSVDX_OWNED_PATCH, 9, 7, 0x1000, command, 64};
    buffers[1] = (struct msvdx_owned_buffer){2, MSVDX_OWNED_READ, 9, 7, 0x2000, source, 64};
    relocs[0] = (struct msvdx_owned_reloc){1, 2, 0, 4, 4, 4};
    relocs[1] = (struct msvdx_owned_reloc){1, 2, 60, 60, 4, 4};
}
static void refusal(enum msvdx_owned_status expected)
{
    assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == expected);
    assert(memcmp(command, before, 64) == 0);
    ++checks;
}
int main(void)
{
    size_t i, n;
    fixture();
    assert(msvdx_owned_validate(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
    assert(memcmp(command, before, 64) == 0);
    assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
    assert(command[0] == 4 && command[1] == 0x20 && command[2] == 0 && command[3] == 0);
    assert(command[60] == 60 && command[61] == 0x20 && command[62] == 0 && command[63] == 0);
    for (i = 4; i < 60; ++i)
        assert(command[i] == 0xa5);
    for (i = 0; i < 64; ++i)
        assert(source[i] == 0x5a);
    ++checks;
    fixture();
    buffers[1].owner++;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    buffers[1].epoch++;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    buffers[1].handle = 1;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    buffers[1].handle = 0;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    relocs[1].source = 3;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    relocs[1].destination = 3;
    refusal(MSVDX_OWNED_IDENTITY);
    fixture();
    buffers[1].access = MSVDX_OWNED_PATCH;
    refusal(MSVDX_OWNED_ACCESS);
    fixture();
    buffers[0].access = MSVDX_OWNED_READ;
    refusal(MSVDX_OWNED_ACCESS);
    fixture();
    buffers[1].access = 8;
    refusal(MSVDX_OWNED_ACCESS);
    fixture();
    buffers[1].access = 0;
    refusal(MSVDX_OWNED_ACCESS);
    fixture();
    buffers[1].bytes = 0;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    buffers[1].mapping = NULL;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    buffers[1].mapping = (uint8_t *)UINTPTR_MAX;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    buffers[1].mapping = command + 63;
    refusal(MSVDX_OWNED_ALIAS);
    fixture();
    buffers[1].mapping = (uint8_t *)buffers;
    refusal(MSVDX_OWNED_ALIAS);
    fixture();
    buffers[1].mapping = (uint8_t *)relocs;
    refusal(MSVDX_OWNED_ALIAS);
    fixture();
    buffers[1].gpu_base = 0x103f;
    refusal(MSVDX_OWNED_ALIAS);
    fixture();
    buffers[1].gpu_base = UINT64_C(0x100000000);
    refusal(MSVDX_OWNED_ADDRESS);
    fixture();
    buffers[1].gpu_base = UINT32_MAX - 62u;
    refusal(MSVDX_OWNED_ADDRESS);
    fixture();
    relocs[1].destination_offset = 0;
    refusal(MSVDX_OWNED_ALIAS);
    fixture();
    relocs[1].source_bytes = 0;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    relocs[1].source_offset = SIZE_MAX;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    relocs[1].source_bytes = SIZE_MAX;
    refusal(MSVDX_OWNED_BOUNDS);
    fixture();
    relocs[1].destination_offset = SIZE_MAX;
    refusal(MSVDX_OWNED_BOUNDS);
    for (n = 0; n < 128; ++n) {
        fixture();
        relocs[1].destination_offset = n;
        if (n > 60 || n % 4)
            refusal(MSVDX_OWNED_BOUNDS);
        else if (!n)
            refusal(MSVDX_OWNED_ALIAS);
        else {
            assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
            ++checks;
        }
        fixture();
        relocs[1].source_offset = n;
        if (n > 60)
            refusal(MSVDX_OWNED_BOUNDS);
        else if (n % 4)
            refusal(MSVDX_OWNED_ADDRESS);
        else {
            assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
            ++checks;
        }
    }
    for (n = 0; n <= 8192; ++n) {
        fixture();
        relocs[1].alignment = (uint32_t)n;
        if (!n || n > 4096 || (n & (n - 1)) || (0x203cu & (n - 1)))
            refusal(MSVDX_OWNED_ADDRESS);
        else {
            assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
            ++checks;
        }
    }
    fixture();
    buffers[1].gpu_base = UINT32_MAX - 63u;
    assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 7) == MSVDX_OWNED_OK);
    assert(command[60] == 0xfc && command[61] == 0xff && command[62] == 0xff &&
           command[63] == 0xff);
    assert(msvdx_owned_patch(NULL, 2, relocs, 2, 9, 7) == MSVDX_OWNED_ARGUMENT);
    assert(msvdx_owned_patch(buffers, 0, relocs, 2, 9, 7) == MSVDX_OWNED_ARGUMENT);
    assert(msvdx_owned_patch(buffers, 2, relocs, 0, 9, 7) == MSVDX_OWNED_ARGUMENT);
    assert(msvdx_owned_patch(buffers, 2, relocs, 2, 0, 7) == MSVDX_OWNED_ARGUMENT);
    assert(msvdx_owned_patch(buffers, 2, relocs, 2, 9, 0) == MSVDX_OWNED_ARGUMENT);
    assert(msvdx_owned_patch(buffers, 65, relocs, 2, 9, 7) == MSVDX_OWNED_LIMIT);
    assert(msvdx_owned_patch(buffers, 2, relocs, 4097, 9, 7) == MSVDX_OWNED_LIMIT);
    for (i = 0; i < MSVDX_OWNED_MAX_BUFFERS; ++i) {
        max_buffers[i] = (struct msvdx_owned_buffer){(uint32_t)i + 1,
                                                     MSVDX_OWNED_READ | MSVDX_OWNED_PATCH,
                                                     9,
                                                     7,
                                                     0x1000 + i * 0x1000,
                                                     max_maps[i],
                                                     256};
        for (n = 0; n < 64; ++n)
            max_relocs[i * 64 + n] = (struct msvdx_owned_reloc){
                (uint32_t)i + 1, (uint32_t)((i + 1) % 64) + 1, n * 4, 0, 256, 4096};
    }
    memset(max_maps, 0xa5, sizeof(max_maps));
    max_relocs[4095].source_bytes = 257;
    assert(msvdx_owned_patch(max_buffers, 64, max_relocs, 4096, 9, 7) == MSVDX_OWNED_BOUNDS);
    for (i = 0; i < 64; ++i)
        for (n = 0; n < 256; ++n)
            assert(max_maps[i][n] == 0xa5);
    max_relocs[4095].source_bytes = 256;
    assert(msvdx_owned_patch(max_buffers, 64, max_relocs, 4096, 9, 7) == MSVDX_OWNED_OK);
    for (i = 0; i < 64; ++i)
        for (n = 0; n < 64; ++n) {
            uint32_t expected = (uint32_t)(0x1000 + ((i + 1) % 64) * 0x1000);
            assert(max_maps[i][n * 4] == (uint8_t)expected);
            assert(max_maps[i][n * 4 + 1] == (uint8_t)(expected >> 8));
            assert(max_maps[i][n * 4 + 2] == (uint8_t)(expected >> 16));
            assert(max_maps[i][n * 4 + 3] == (uint8_t)(expected >> 24));
        }
    puts("msvdx_owned: PASS transactional owner/epoch/range/access/address/alias "
         "boundary checks");
    printf("cases=%u\n", checks + 10);
    return 0;
}
