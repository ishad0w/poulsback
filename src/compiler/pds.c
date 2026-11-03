// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_pds_linear.h"
#include <string.h>

static const uint32_t empty_task[SGX535_PDS_LINEAR_WORDS] = {
    [12] = UINT32_C(0x07000365), /* DOUTU MOVS */
    [13] = UINT32_C(0xaf000000)  /* HALT */
};

enum sgx535_pds_linear_status sgx535_pds_linear_encode(const struct sgx535_pds_linear *input,
                                                       uint32_t *words, size_t capacity)
{
    uint32_t result[SGX535_PDS_LINEAR_WORDS];
    struct sgx535_pds_linear task;
    if (!input || !words)
        return SGX535_PDS_LINEAR_ERR_ARGUMENT;
    if (capacity < SGX535_PDS_LINEAR_WORDS)
        return SGX535_PDS_LINEAR_ERR_CAPACITY;
    task = *input;
    if (task.iterator_count)
        return SGX535_PDS_LINEAR_ERR_UNSUPPORTED;
    memcpy(result, empty_task, sizeof(result));
    result[0] = task.task_control[0];
    result[1] = task.task_control[1];
    result[8] = task.task_control[2];
    memcpy(words, result, sizeof(result));
    return SGX535_PDS_LINEAR_OK;
}

enum sgx535_pds_linear_status sgx535_pds_linear_decode(const uint32_t *words, size_t capacity,
                                                       struct sgx535_pds_linear *output)
{
    struct sgx535_pds_linear task;
    uint32_t canonical[SGX535_PDS_LINEAR_WORDS];
    if (!words || !output)
        return SGX535_PDS_LINEAR_ERR_ARGUMENT;
    if (capacity < SGX535_PDS_LINEAR_WORDS)
        return SGX535_PDS_LINEAR_ERR_CAPACITY;
    task = (struct sgx535_pds_linear){{words[0], words[1], words[8]}, 0};
    if (sgx535_pds_linear_encode(&task, canonical, SGX535_PDS_LINEAR_WORDS) !=
            SGX535_PDS_LINEAR_OK ||
        memcmp(canonical, words, sizeof(canonical)))
        return SGX535_PDS_LINEAR_ERR_UNSUPPORTED;
    *output = task;
    return SGX535_PDS_LINEAR_OK;
}
