// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_PDS_SECONDARY_H
#define POULSBACK_SGX535_PDS_SECONDARY_H

#include <stdint.h>

#define SGX535_PDS_SECONDARY_MAX_DWORDS 128u
#define SGX535_PDS_SECONDARY_DATA_WORDS 12u
#define SGX535_PDS_SECONDARY_MAX_WORDS 16u

enum sgx535_pds_secondary_status {
    SGX535_PDS_SECONDARY_OK = 0,
    SGX535_PDS_SECONDARY_ERR_ARGUMENT = -1,
    SGX535_PDS_SECONDARY_ERR_RANGE = -2,
    SGX535_PDS_SECONDARY_ERR_RESOURCE = -3,
    SGX535_PDS_SECONDARY_ERR_UNSUPPORTED = -4
};

struct sgx535_pds_secondary_resource {
    uint32_t gpu_address;
    uint32_t size_bytes;
};

struct sgx535_pds_secondary {
    uint32_t source_address;
    uint32_t dword_count;
    uint32_t task_control[3];
};

struct sgx535_pds_secondary_program {
    uint32_t words[SGX535_PDS_SECONDARY_MAX_WORDS];
    uint32_t word_count;
};

/* CPU construction only: 1..128 DWORDs copied to SECONDARY offset zero,
 * one/two DMA kicks, an opaque trusted USE task, then HALT. No tile-position
 * write, depth iteration or arbitrary destination. The source must be aligned
 * to four bytes and its entire read interval must lie inside the supplied
 * resource. Resource intervals use an exclusive end, which may equal 2^32.
 * These arithmetic bounds do not establish mapping ownership, GPU access,
 * cache visibility, task safety, scheduling or retirement.
 *
 * Encoded programs have 15/16 words and zero all unused storage. Decode
 * accepts only that canonical profile, including unused word 15 for the
 * 15-word case. Errors preserve output; stable input/output objects may
 * overlap. This is not a wire ABI or an execution interface.
 */
enum sgx535_pds_secondary_status
sgx535_pds_secondary_encode(const struct sgx535_pds_secondary *,
                            const struct sgx535_pds_secondary_resource *,
                            struct sgx535_pds_secondary_program *);
enum sgx535_pds_secondary_status
sgx535_pds_secondary_decode(const struct sgx535_pds_secondary_program *,
                            const struct sgx535_pds_secondary_resource *,
                            struct sgx535_pds_secondary *);

#endif
