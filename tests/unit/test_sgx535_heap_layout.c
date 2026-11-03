// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_heap_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static struct sgx535_heap_layout_input good_request(void)
{
    struct sgx535_heap_layout_input r = {{1, 0x20004000u, 0x2000u},
                                         {2, 0x32000000u, 0x02000000u},
                                         {3, 0x40000000u, 0x02000000u},
                                         {4, 0x44000000u, 0x3000u}};
    return r;
}

static struct sgx535_heap_extent *extent(struct sgx535_heap_layout_input *r, unsigned int index)
{
    switch (index) {
    case 0:
        return &r->pds;
    case 1:
        return &r->parameters;
    case 2:
        return &r->tables;
    default:
        return &r->scene;
    }
}

static void check_result(const struct sgx535_heap_layout_input *r)
{
    struct sgx535_heap_layout_projection p;
    CHECK(sgx535_heap_layout_project(r, &p) == SGX535_HEAP_LAYOUT_OK);
    CHECK(p.pds_exec_base.register_offset_bytes == 2744u);
    CHECK(p.pds_exec_base.word == 536870912u);
    CHECK(p.ta_request_base.register_offset_bytes == 3216u);
    CHECK(p.ta_request_base.word == 805306368u);
    CHECK(p.three_d_request_base.register_offset_bytes == 3244u);
    CHECK(p.three_d_request_base.word == 805306368u);
    CHECK(p.pds_allocation_offset_bytes == r->pds.gpu_va_bytes - 536870912u);
    CHECK(p.parameter_first_page == (r->parameters.gpu_va_bytes - 805306368u) / 4096u);
    CHECK(p.parameter_last_page ==
          (r->parameters.gpu_va_bytes + r->parameters.allocation_bytes - 805306368u) / 4096u - 1u);
    CHECK(p.table_va_bytes == r->tables.gpu_va_bytes);
    CHECK(p.scene_va_bytes == r->scene.gpu_va_bytes);
}

static void ranges(void)
{
    struct sgx535_heap_layout_input r = good_request();
    uint64_t page, mib;
    check_result(&r);
    /* Every whole-page PDS start in the exact allocation class. */
    r.pds.allocation_bytes = 4096;
    for (page = 0; page < 65536u; ++page) {
        r.pds.gpu_va_bytes = 536870912u + page * 4096u;
        check_result(&r);
    }
    /* Every 1-MiB parameter start with both a one-page extent and the complete
     * remaining window. The last page oracle uses the checked byte endpoint. */
    r = good_request();
    for (mib = 0; mib < 256u; ++mib) {
        r.parameters.gpu_va_bytes = 805306368u + mib * 1048576u;
        r.parameters.allocation_bytes = 4096;
        check_result(&r);
        r.parameters.allocation_bytes = 1073741824u - r.parameters.gpu_va_bytes;
        check_result(&r);
    }
    r = good_request();
    r.pds.gpu_va_bytes = 0x20000000u;
    r.pds.allocation_bytes = 0x10000000u;
    r.tables.gpu_va_bytes = 0x40000000u;
    r.tables.allocation_bytes = 0x1000u;
    r.scene.gpu_va_bytes = 0x40001000u;
    r.scene.allocation_bytes = 0x0ffff000u;
    check_result(&r); /* Adjacent generic extents and exact class endpoints. */
    r.tables.gpu_va_bytes = 0x4ffff000u;
    r.scene.gpu_va_bytes = 0x40000000u;
    r.scene.allocation_bytes = 0x0ffff000u;
    check_result(&r); /* Either table/scene ordering. */
}

static void refusal(const struct sgx535_heap_layout_input *r, enum sgx535_heap_layout_status status)
{
    struct sgx535_heap_layout_projection p, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    p = sentinel;
    CHECK(sgx535_heap_layout_project(r, &p) == status);
    CHECK(!memcmp(&p, &sentinel, sizeof(p)));
}

static void malformed(void)
{
    struct sgx535_heap_layout_input r;
    struct sgx535_heap_layout_projection p, expected;
    union {
        struct sgx535_heap_layout_input input;
        struct sgx535_heap_layout_projection output;
    } overlap;
    unsigned int i, j;
    uint64_t first[] = {0x20000000u, 0x30000000u, 0x40000000u, 0x40000000u};
    uint64_t limit[] = {0x30000000u, 0x40000000u, 0x50000000u, 0x50000000u};

    r = good_request();
    CHECK(sgx535_heap_layout_project(NULL, &p) == SGX535_HEAP_LAYOUT_INVALID);
    CHECK(sgx535_heap_layout_project(&r, NULL) == SGX535_HEAP_LAYOUT_INVALID);
    CHECK(sgx535_heap_layout_project(NULL, NULL) == SGX535_HEAP_LAYOUT_INVALID);
    for (i = 0; i < 4; ++i) {
        r = good_request();
        extent(&r, i)->storage_id = 0;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->allocation_bytes = 0;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->gpu_va_bytes |= 1u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->allocation_bytes |= 1u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->gpu_va_bytes = first[i] - 4096u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->gpu_va_bytes = limit[i];
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->allocation_bytes = limit[i] - extent(&r, i)->gpu_va_bytes + 4096u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->gpu_va_bytes = UINT64_MAX - 4095u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        r = good_request();
        extent(&r, i)->allocation_bytes = UINT64_MAX - 4095u;
        refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
        for (j = i + 1; j < 4; ++j) {
            r = good_request();
            extent(&r, j)->storage_id = extent(&r, i)->storage_id;
            refusal(&r, SGX535_HEAP_LAYOUT_ALIAS);
        }
    }
    r = good_request();
    r.parameters.gpu_va_bytes += 4096;
    refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
    r = good_request();
    r.parameters.gpu_va_bytes = 0x42000000u;
    refusal(&r, SGX535_HEAP_LAYOUT_RANGE); /* Current all-4xx layout is not this profile. */
    r = good_request();
    r.pds.gpu_va_bytes = 0x40004000u;
    refusal(&r, SGX535_HEAP_LAYOUT_RANGE);
    r = good_request();
    r.scene.gpu_va_bytes = r.tables.gpu_va_bytes;
    refusal(&r, SGX535_HEAP_LAYOUT_ALIAS);
    r = good_request();
    r.scene.gpu_va_bytes = r.tables.gpu_va_bytes + 4096;
    refusal(&r, SGX535_HEAP_LAYOUT_ALIAS);
    r = good_request();
    r.scene.gpu_va_bytes = r.tables.gpu_va_bytes - 4096;
    refusal(&r, SGX535_HEAP_LAYOUT_RANGE); /* Before the generic subset. */

    r = good_request();
    CHECK(sgx535_heap_layout_project(&r, &expected) == SGX535_HEAP_LAYOUT_OK);
    overlap.input = r;
    CHECK(sgx535_heap_layout_project(&overlap.input, &overlap.output) == SGX535_HEAP_LAYOUT_OK);
    CHECK(!memcmp(&overlap.output, &expected, sizeof(expected)));
    overlap.input = r;
    overlap.input.parameters.storage_id = overlap.input.pds.storage_id;
    r = overlap.input;
    CHECK(sgx535_heap_layout_project(&overlap.input, &overlap.output) == SGX535_HEAP_LAYOUT_ALIAS);
    CHECK(!memcmp(&overlap.input, &r, sizeof(r)));
}

int main(void)
{
    ranges();
    malformed();
    printf("PASS %lu legacy heap CPU projection checks; no mapping or hardware permission\n",
           checks);
    return 0;
}
