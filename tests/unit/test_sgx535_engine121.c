// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* CPU/static sequence qualification only. No MMIO, device or idle simulation. */
#include "sgx535_engine121.h"
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

static void action(const struct sgx535_engine121_action *a, uint32_t kind, uint32_t reg,
                   uint32_t value, uint32_t mask)
{
    CHECK(a->kind == kind);
    CHECK(a->offset == reg);
    CHECK(a->value == value);
    CHECK(a->mask == mask);
}

/* Scene profile: 16-pixel tiles, four-wide groups of two tiles, a square
 * power-of-two root, then 48 bytes per group and 80/144/64-byte tails.
 * Construct the oracle by counting coverage and laying out storage rather
 * than keeping a single precomputed scene cookie.
 */
static void check_scene(uint32_t width, uint32_t height)
{
    struct sgx535_engine121_scene scene;
    uint32_t groups[2], tiles[2], extent[2] = {width, height};
    uint32_t expected[16] = {0};
    uint64_t root_cells = 1, cursor, pages;
    size_t axis, word;
    for (axis = 0; axis != 2; ++axis) {
        tiles[axis] = 0;
        while (tiles[axis] * 16 < extent[axis])
            ++tiles[axis];
        groups[axis] = 4;
        while (groups[axis] * 2 < tiles[axis])
            groups[axis] += 4;
    }
    while (root_cells < 2 * groups[0] || root_cells < 2 * groups[1])
        root_cells *= 2;
    pages = (root_cells * root_cells * sizeof(uint32_t) + 4095) / 4096;
    expected[1] = expected[4] = groups[0] * groups[1];
    for (axis = 0; axis != 2; ++axis)
        expected[2 + axis] = groups[axis] * ((1u << 22) + (1u << 12) + 1u);
    expected[5] = (tiles[1] - 1) * 4096 + tiles[0] - 1;
    expected[6] = (height - 1) * 4096 + width - 1;
    expected[7] = (uint32_t)(pages * 4096);
    cursor = pages * 4096;
    for (word = 0; word != expected[1]; ++word)
        cursor += 48;
    expected[9] = (uint32_t)cursor;
    cursor += 80;
    expected[10] = (uint32_t)cursor;
    cursor += 144;
    expected[11] = (uint32_t)cursor;
    cursor += 64;
    CHECK(sgx535_engine121_scene_build(width, height, &scene) == SGX535_ENGINE121_OK);
    CHECK(scene.width == width && scene.height == height);
    for (word = 0; word != 16; ++word)
        CHECK(scene.words[word] == expected[word]);
    CHECK(scene.allocation_bytes == cursor);
    CHECK(scene.clear_page_start == 0 && scene.clear_page_count == pages);
}

static void scene_tests(void)
{
    struct sgx535_engine121_scene s, sentinel;
    const uint32_t boundaries[] = {1,   15,  16,  17,  31,   32,   33,   127, 128,
                                   129, 255, 256, 257, 1023, 1024, 1025, 2047};
    uint32_t x, y;
    size_t i, j;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (i = 0; i != sizeof(boundaries) / sizeof(boundaries[0]); ++i)
        for (j = 0; j != sizeof(boundaries) / sizeof(boundaries[0]); ++j)
            check_scene(boundaries[i], boundaries[j]);
    for (x = 1; x < 2048; x += 17)
        for (y = 1; y < 2048; y += 31)
            check_scene(x, y);
    s = sentinel;
    CHECK(sgx535_engine121_scene_build(0, 32, &s) == SGX535_ENGINE121_INVALID);
    CHECK(!memcmp(&s, &sentinel, sizeof(s)));
    CHECK(sgx535_engine121_scene_build(32, 2048, &s) == SGX535_ENGINE121_UNSUPPORTED);
    CHECK(!memcmp(&s, &sentinel, sizeof(s)));
    CHECK(sgx535_engine121_scene_build(UINT32_MAX, UINT32_MAX, &s) == SGX535_ENGINE121_UNSUPPORTED);
    CHECK(!memcmp(&s, &sentinel, sizeof(s)));
    CHECK(sgx535_engine121_scene_build(1, 1, NULL) == SGX535_ENGINE121_INVALID);
}

static void dpm_tests(void)
{
    struct sgx535_engine121_dpm d = {1,          0x23000000, 0x2000000, 2,
                                     0x30000000, 0x2000000,  0x30000000,
                                     SGX535_ENGINE121_THRESHOLDS_LEGACY},
                                changed;
    struct sgx535_engine121_action a[40], saved[40];
    size_t n;
    memset(a, 0xa5, sizeof(a));
    memcpy(saved, a, sizeof(a));
    n = 97;
    CHECK(sgx535_engine121_dpm_build(&d, a, 28, &n) == SGX535_ENGINE121_NO_SPACE);
    CHECK(n == 97 && !memcmp(a, saved, sizeof(a)));
    CHECK(sgx535_engine121_dpm_build(&d, a, 40, &n) == SGX535_ENGINE121_OK);
    CHECK(n == 29);
    action(a, 1, 0x618, 0x23000000, 0);
    action(a + 1, 1, 0x61c, 0x1fff0000, 0);
    action(a + 2, 1, 0x648, 0x1ffe0000, 0);
    action(a + 4, 1, 0x620, 0x1a00, 0);
    action(a + 7, 1, 0x668, 0x19c0, 0);
    action(a + 8, 1, 0x684, 1, 0);
    action(a + 13, 1, 0x680, 1, 0);
    action(a + 17, 1, 0x688, 1, 0);
    action(a + 21, 1, 0x690, 1, 0);
    /* Independent literals protect the DHOST mask8 vs old binary mask4 bug. */
    action(a + 22, 2, 0x118, 0xf, 0xf);
    action(a + 23, 1, 0x114, 0xf, 0);
    action(a + 24, 3, 0x118, 0, 0xf);
    action(a + 25, 1, 0x6a8, 1, 0);
    action(a + 26, 2, 0x12c, 0x400000, 0x400000);
    action(a + 27, 1, 0x134, 0x400000, 0);
    action(a + 28, 3, 0x12c, 0, 0x400000);
    CHECK(!memcmp(a + 29, saved + 29, sizeof(a) - 29 * sizeof(*a)));
    changed = d;
    changed.parameter_bytes = 0x620u * 4096u;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_OK);
    action(a + 4, 1, 0x620, 0x20, 0);
    action(a + 7, 1, 0x668, 0, 0);
    changed.parameter_bytes = 0x640u * 4096u;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_OK);
    action(a + 7, 1, 0x668, 0, 0);
    changed.parameter_bytes = 0x641u * 4096u;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_OK);
    action(a + 7, 1, 0x668, 1, 0);
    /* Thresholds for 0x2000 pages: literals, not the formula. */
    changed = d;
    changed.thresholds = SGX535_ENGINE121_THRESHOLDS_ENGINE;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_OK && n == 29);
    action(a + 4, 1, 0x620, 0x2010, 0);
    action(a + 5, 1, 0x624, 0x1e23, 0);
    action(a + 6, 1, 0x628, 0x12010, 0);
    action(a + 7, 1, 0x668, 0x1c4a, 0);
    changed.thresholds = 2;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_INVALID);
    changed = d;
    changed.parameter_gpu_va_bytes += 0x1000000;
    CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == SGX535_ENGINE121_OK);
    action(a + 1, 1, 0x61c, 0x2fff1000, 0);
#define REFUSE(member, value, status)                                                              \
    do {                                                                                           \
        changed = d;                                                                               \
        changed.member = (value);                                                                  \
        memcpy(a, saved, sizeof(a));                                                               \
        n = 97;                                                                                    \
        CHECK(sgx535_engine121_dpm_build(&changed, a, 40, &n) == (status));                        \
        CHECK(n == 97 && !memcmp(a, saved, sizeof(a)));                                            \
    } while (0)
    REFUSE(parameter_storage, 1, SGX535_ENGINE121_INVALID);
    REFUSE(table_storage, 0, SGX535_ENGINE121_INVALID);
    REFUSE(table_gpu_va_bytes, 0x23000001, SGX535_ENGINE121_ALIGNMENT);
    REFUSE(parameter_gpu_va_bytes, 0x30001000, SGX535_ENGINE121_ALIGNMENT);
    REFUSE(parameter_bytes, 0x2000001, SGX535_ENGINE121_ALIGNMENT);
    REFUSE(parameter_bytes, 0x61fu * 4096u, SGX535_ENGINE121_RANGE);
    REFUSE(parameter_bytes, 0x10000000, SGX535_ENGINE121_RANGE);
    REFUSE(parameter_gpu_va_bytes, 0x3ff00000, SGX535_ENGINE121_RANGE);
    REFUSE(parameter_gpu_va_bytes, 0x2ff00000, SGX535_ENGINE121_RANGE);
    REFUSE(table_allocation_bytes, 0x61ffff, SGX535_ENGINE121_RANGE);
    REFUSE(table_gpu_va_bytes, UINT64_C(0x100000000), SGX535_ENGINE121_RANGE);
    REFUSE(table_allocation_bytes, UINT64_MAX, SGX535_ENGINE121_RANGE);
    REFUSE(table_gpu_va_bytes, 0x30000000, SGX535_ENGINE121_RANGE);
    REFUSE(raster_geometry_base_bytes, 0x30000001, SGX535_ENGINE121_ALIGNMENT);
#undef REFUSE
}

static void plan_tests(void)
{
    struct sgx535_engine121_action a[40], sentinel[40];
    /* Independent complete ordered projection of the matched initializer's
	 * zero-selector assembly, including five observed (not compared) reads. */
    const struct sgx535_engine121_action bootstrap[] = {
        {1, 0xca0, 0xc07c, 0},     {4, 0xca0, 0, 0},       {1, 0x13c, 0, 0},
        {1, 0xa7c, 0, 0},          {1, 0xa80, 0, 0},       {4, 0xa80, 0, 0},
        {1, 0xa74, 0x05188200, 0}, {1, 0xa78, 0, 0},       {4, 0xa78, 0, 0},
        {1, 0xaac, 0x44, 0},       {1, 0xabc, 0xc, 0},     {1, 0x804, 0xffff, 0},
        {4, 0x804, 0, 0},          {1, 0xa00, 0x7c000, 0}, {4, 0xa00, 0, 0},
        {1, 0x630, 0, 0},          {1, 0xa58, 0, 0}};
    const struct sgx535_engine121_action raster[] = {
        {1, 0x65c, 0, 0},          {1, 0x63c, 3, 0},
        {1, 0x658, 0, 0},          {1, 0x408, 0x20001000, 0},
        {1, 0xad4, 1, 0},          {2, 0x138, 0x44, 0x44},
        {1, 0x140, 0x44, 0},       {3, 0x138, 0, 0x44},
        {1, 0xae0, 1, 0},          {2, 0x138, 1, 1},
        {1, 0x140, 1, 0},          {3, 0x138, 0, 1},
        {1, 0x804, 0x1000ffff, 0}, {2, 0x12c, 0x4000000, 0x4000000},
        {1, 0x134, 0x4000000, 0},  {3, 0x12c, 0, 0x4000000},
        {1, 0x43c, 1, 0},          {1, 0xa08, 1, 0},
        {4, 0xa08, 0, 0},          {1, 0x428, 1, 0}};
    struct sgx535_engine121_scene_binding scene = {1, 0x20000000, 0x2000, 32, 32, 0, 0, 0};
    size_t n = 99, i, capacity;
    memset(a, 0xa5, sizeof(a));
    memcpy(sentinel, a, sizeof(a));
    CHECK(SGX535_ENGINE121_WRITE == 1 && SGX535_ENGINE121_WAIT_SET == 2 &&
          SGX535_ENGINE121_WAIT_CLEAR == 3 && SGX535_ENGINE121_READBACK == 4);
    CHECK(SGX535_ENGINE121_BOOTSTRAP_ACTIONS == 17 &&
          sizeof(bootstrap) / sizeof(bootstrap[0]) == 17);
    CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION,
                                           NULL, 40, &n) == SGX535_ENGINE121_INVALID);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION, a,
                                           40, NULL) == SGX535_ENGINE121_INVALID);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    CHECK(sgx535_engine121_bootstrap_build(0, 0, a, 40, &n) == SGX535_ENGINE121_UNSUPPORTED);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION ^ 1u,
                                           a, 40, &n) == SGX535_ENGINE121_UNSUPPORTED);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    for (capacity = 0; capacity < SGX535_ENGINE121_BOOTSTRAP_ACTIONS; capacity++) {
        CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION,
                                               a, capacity, &n) == SGX535_ENGINE121_NO_SPACE);
        CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    }
    CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION, a,
                                           SGX535_ENGINE121_BOOTSTRAP_ACTIONS,
                                           &n) == SGX535_ENGINE121_OK);
    CHECK(n == 17);
    for (i = 0; i < n; i++)
        action(a + i, bootstrap[i].kind, bootstrap[i].offset, bootstrap[i].value,
               bootstrap[i].mask);
    CHECK(!memcmp(a + n, sentinel + n, sizeof(a) - n * sizeof(a[0])));
    memcpy(a, sentinel, sizeof(a));
    n = 99;
    CHECK(sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION, a,
                                           SIZE_MAX, &n) == SGX535_ENGINE121_OK);
    CHECK(n == 17 && !memcmp(a, bootstrap, sizeof(bootstrap)));
    CHECK(!memcmp(a + n, sentinel + n, sizeof(a) - n * sizeof(a[0])));
    /* No context selector or USE cache invalidate: the caller's. */
    CHECK(sgx535_engine121_ta_build(&scene, a, 40, &n) == SGX535_ENGINE121_OK);
    CHECK(n == 30 && SGX535_ENGINE121_TA_ACTIONS == 30);
    action(a, 1, 0x220, 0x20000000, 0);
    action(a + 11, 1, 0x21c, 0x20001000, 0);
    action(a + 13, 2, 0x12c, 0x100a40, 0x100a40);
    action(a + 14, 1, 0x134, 0x100a40, 0);
    action(a + 15, 3, 0x12c, 0, 0x100a40);
    action(a + 16, 1, 0xc90, 0x30000000, 0);
    action(a + 17, 1, 0xad0, 1, 0);
    action(a + 18, 2, 0x138, 0x22, 0x22);
    action(a + 21, 1, 0xae0, 1, 0);
    action(a + 25, 1, 0x804, 0x1000ffff, 0);
    action(a + 29, 1, 0x200, 1, 0);
    for (i = 0; i < n; i++)
        CHECK(a[i].offset != 0x630 && a[i].offset != 0xa08);
    /* Region array 1 of the 32x32 scene: after the control table (0x1420),
     * 64-byte aligned; the TA and the raster use it, everything else as array 0. */
    {
        struct sgx535_engine121_scene s32;
        struct sgx535_engine121_action b[40];
        CHECK(sgx535_engine121_scene_build(32, 32, &s32) == SGX535_ENGINE121_OK);
        CHECK(s32.allocation_bytes == 0x1420);
        CHECK(sgx535_engine121_scene_bytes(&s32, 1) == 0x1420);
        CHECK(sgx535_engine121_scene_bytes(&s32, 2) == 0x1440 + 768);
        CHECK(sgx535_engine121_scene_bytes(&s32, 0) == 0 && sgx535_engine121_scene_bytes(&s32, 3) == 0);
        CHECK(sgx535_engine121_scene_bytes(NULL, 1) == 0);
        scene.region_array = 1;
        CHECK(sgx535_engine121_ta_build(&scene, b, 40, &n) == SGX535_ENGINE121_OK && n == 30);
        for (i = 0; i < n; i++)
            CHECK(i == 11 ? b[i].offset == 0x21c && b[i].value == 0x20001440
                          : !memcmp(&b[i], &a[i], sizeof(b[i])));
        CHECK(sgx535_engine121_raster_build(&scene, b, 40, &n) == SGX535_ENGINE121_OK && n == 20);
        CHECK(b[3].offset == 0x408 && b[3].value == 0x20001440);
        scene.allocation_bytes = 0x1440 + 767;
        n = 99;
        CHECK(sgx535_engine121_ta_build(&scene, b, 40, &n) == SGX535_ENGINE121_RANGE && n == 99);
        scene.allocation_bytes = 0x2000;
        scene.region_array = 2;
        CHECK(sgx535_engine121_raster_build(&scene, b, 40, &n) == SGX535_ENGINE121_INVALID && n == 99);
        scene.region_array = 0;
    }
    /* The 3D pass after the job's own TA: no context selector, scene LOAD or
     * DPM page counts. */
    CHECK(sgx535_engine121_raster_build(&scene, a, 40, &n) == SGX535_ENGINE121_OK);
    CHECK(n == 20 && SGX535_ENGINE121_RASTER_ACTIONS == 20);
    for (i = 0; i < n; i++)
        action(a + i, raster[i].kind, raster[i].offset, raster[i].value, raster[i].mask);
    for (capacity = 0; capacity < SGX535_ENGINE121_RASTER_ACTIONS; capacity++)
        CHECK(sgx535_engine121_raster_build(&scene, a, capacity, &n) == SGX535_ENGINE121_NO_SPACE);
    scene.allocation_bytes = 0x141f;
    memcpy(a, sentinel, sizeof(a));
    n = 99;
    CHECK(sgx535_engine121_ta_build(&scene, a, 40, &n) == SGX535_ENGINE121_RANGE);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
    scene.allocation_bytes = 0x2000;
    scene.gpu_va_bytes = 0xfffff000;
    CHECK(sgx535_engine121_raster_build(&scene, a, 40, &n) == SGX535_ENGINE121_RANGE);
    CHECK(n == 99 && !memcmp(a, sentinel, sizeof(a)));
}

/* A render's region array as the TA plan points TE_RGNBASE at it: its first
 * byte and its size (48 bytes a four-tile group). */
static void region_span(struct sgx535_engine121_scene_binding *binding, uint32_t *first,
                        uint32_t *bytes)
{
    struct sgx535_engine121_scene s;
    struct sgx535_engine121_action b[40], r[40];
    size_t n = 0;
    *first = *bytes = 0;
    CHECK(sgx535_engine121_ta_build(binding, b, 40, &n) == SGX535_ENGINE121_OK && n == 30);
    CHECK(sgx535_engine121_raster_build(binding, r, 40, &n) == SGX535_ENGINE121_OK && n == 20);
    /* The raster walks the array its TA filled. */
    CHECK(b[11].offset == 0x21c && r[3].offset == 0x408 && r[3].value == b[11].value);
    CHECK(sgx535_engine121_scene_build(binding->width, binding->height, &s) == SGX535_ENGINE121_OK);
    *first = b[11].value - (uint32_t)binding->gpu_va_bytes;
    *bytes = s.words[9] - s.words[7];
}

/* Two jobs of one storage may be in flight at once, a TA beside the other's
 * raster, in the other region array. With the storage's layout (the kernel's,
 * 2047x2047) the arrays of any two render sizes are disjoint, above both
 * renders' roots and inside the allocation; with each render's own layout a
 * 64x64 render's array 1 lies inside a 256x256 render's array 0. */
static void layout_tests(void)
{
    static const uint32_t sizes[] = {1, 16, 17, 32, 64, 65, 255, 256, 640, 1024, 2047};
    const size_t count = sizeof(sizes) / sizeof(sizes[0]);
    struct sgx535_engine121_scene max, s;
    struct sgx535_engine121_scene_binding binding = {1, 0x20000000, 0, 0, 0, 0, 2047, 2047};
    struct sgx535_engine121_action b[40];
    uint32_t first[2] = {0, 0}, bytes, end0 = 0, end1 = 0, at;
    size_t i, j, n = 99;
    CHECK(sgx535_engine121_scene_build(2047, 2047, &max) == SGX535_ENGINE121_OK);
    binding.allocation_bytes = sgx535_engine121_scene_bytes(&max, 2);
    for (i = 0; i < count; i++)
        for (j = 0; j < count; j++) {
            binding.width = sizes[i];
            binding.height = sizes[j];
            CHECK(sgx535_engine121_scene_build(sizes[i], sizes[j], &s) == SGX535_ENGINE121_OK);
            for (binding.region_array = 0; binding.region_array < 2; binding.region_array++) {
                region_span(&binding, &at, &bytes);
                if (!i && !j)
                    first[binding.region_array] = at;
                CHECK(at == first[binding.region_array] && s.words[7] <= first[0]);
                if (binding.region_array)
                    end1 = at + bytes > end1 ? at + bytes : end1;
                else
                    end0 = at + bytes > end0 ? at + bytes : end0;
            }
        }
    binding.region_array = 0;
    CHECK(end0 <= first[1] && end1 <= binding.allocation_bytes);
    /* A layout smaller than the render is refused. */
    binding.width = 2047;
    binding.layout_width = 2046;
    CHECK(sgx535_engine121_ta_build(&binding, b, 40, &n) == SGX535_ENGINE121_RANGE && n == 99);
    /* Each render's own layout (no layout given): the overlap the kernel avoids. */
    binding.layout_width = binding.layout_height = 0;
    binding.width = binding.height = 256;
    region_span(&binding, &at, &bytes);
    end0 = at + bytes;
    binding.width = binding.height = 64;
    binding.region_array = 1;
    region_span(&binding, &at, &bytes);
    CHECK(at < end0);
}

int main(void)
{
    scene_tests();
    dpm_tests();
    plan_tests();
    layout_tests();
    printf("PASS %lu CPU/static engine121 checks; hardware and retirement unqualified\n", checks);
    return 0;
}
