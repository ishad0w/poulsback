// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_PDS_LINEAR_H
#define POULSBACK_SGX535_PDS_LINEAR_H
#include <stddef.h>
#include <stdint.h>
#define SGX535_PDS_LINEAR_DATA_WORDS 12u
#define SGX535_PDS_LINEAR_WORDS 14u
enum sgx535_pds_linear_status {
    SGX535_PDS_LINEAR_OK = 0,
    SGX535_PDS_LINEAR_ERR_ARGUMENT = -1,
    SGX535_PDS_LINEAR_ERR_CAPACITY = -2,
    SGX535_PDS_LINEAR_ERR_UNSUPPORTED = -3
};
struct sgx535_pds_linear {
    uint32_t task_control[3];
    uint32_t iterator_count;
};
/* No-iterator pixel-task CPU layout. Controls are opaque, not owned GPU
 * addresses. Alignment, launch, cache visibility and completion are outside
 * this API. Errors preserve output; valid stable input/output may overlap.
 */
enum sgx535_pds_linear_status sgx535_pds_linear_encode(const struct sgx535_pds_linear *, uint32_t *,
                                                       size_t);
enum sgx535_pds_linear_status sgx535_pds_linear_decode(const uint32_t *, size_t,
                                                       struct sgx535_pds_linear *);
#endif
