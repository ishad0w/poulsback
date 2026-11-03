// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_engine121.h"

/* The supported branch has at most 128 tiles per axis. Its region arrays use
 * four-entry groups; allocation arithmetic is bounded independently of the
 * packed 12-bit tile/pixel end coordinates.
 */
enum sgx535_engine121_status sgx535_engine121_scene_build(uint32_t width, uint32_t height,
                                                          struct sgx535_engine121_scene *out)
{
    struct sgx535_engine121_scene candidate = {0};
    uint32_t tiles_x, tiles_y, groups_x, groups_y, span, side, root;
    if (!out || !width || !height)
        return SGX535_ENGINE121_INVALID;
    if (width >= 2048 || height >= 2048)
        return SGX535_ENGINE121_UNSUPPORTED;
    tiles_x = (width + 15u) / 16u;
    tiles_y = (height + 15u) / 16u;
    groups_x = (((tiles_x + 1u) / 2u) + 3u) & ~3u;
    groups_y = (((tiles_y + 1u) / 2u) + 3u) & ~3u;
    span = 2u * (groups_x > groups_y ? groups_x : groups_y);
    for (side = 1; side < span; side *= 2u)
        ;
    root = (4u * side * side + 4095u) & ~4095u;
    candidate.width = width;
    candidate.height = height;
    candidate.words[1] = groups_x * groups_y;
    candidate.words[2] = (groups_x << 22) | (groups_x << 12) | groups_x;
    candidate.words[3] = (groups_y << 22) | (groups_y << 12) | groups_y;
    candidate.words[4] = groups_x * groups_y;
    candidate.words[5] = ((tiles_y - 1u) << 12) | (tiles_x - 1u);
    candidate.words[6] = ((height - 1u) << 12) | (width - 1u);
    candidate.words[7] = root;
    candidate.words[9] = root + 48u * groups_x * groups_y;
    candidate.words[10] = candidate.words[9] + 80u;
    candidate.words[11] = candidate.words[9] + 224u;
    candidate.allocation_bytes = candidate.words[9] + 288u;
    candidate.clear_page_count = root / 4096u;
    *out = candidate;
    return SGX535_ENGINE121_OK;
}

/* Region array 1 follows the control table, 64-byte aligned (a TA fills one array while the raster
 * reads the other). */
static uint32_t region_offset(const struct sgx535_engine121_scene *scene, uint32_t array)
{
    return array ? (scene->allocation_bytes + 63u) & ~63u : scene->words[7];
}

uint32_t sgx535_engine121_scene_bytes(const struct sgx535_engine121_scene *scene, uint32_t arrays)
{
    if (!scene || !arrays || arrays > SGX535_ENGINE121_REGION_ARRAYS)
        return 0;
    return arrays == 1 ? scene->allocation_bytes
                       : region_offset(scene, 1) + (scene->words[9] - scene->words[7]);
}

static int span_valid(uint64_t base, uint64_t bytes)
{
    return bytes && base < UINT64_C(0x100000000) && bytes <= UINT64_C(0x100000000) - base;
}

static int overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

enum sgx535_engine121_status sgx535_engine121_dpm_build(const struct sgx535_engine121_dpm *dpm,
                                                        struct sgx535_engine121_action *out,
                                                        size_t capacity, size_t *written)
{
    struct sgx535_engine121_action candidate[SGX535_ENGINE121_DPM_ACTIONS];
    uint32_t pages, start, end, ta, zls, global, tail, range;
    uint64_t relative;
    size_t n = 0, i;
    if (!dpm || !out || !written)
        return SGX535_ENGINE121_INVALID;
    if (capacity < SGX535_ENGINE121_DPM_ACTIONS)
        return SGX535_ENGINE121_NO_SPACE;
    if (!dpm->table_storage || !dpm->parameter_storage ||
        dpm->table_storage == dpm->parameter_storage ||
        dpm->thresholds > SGX535_ENGINE121_THRESHOLDS_ENGINE)
        return SGX535_ENGINE121_INVALID;
    if (!span_valid(dpm->table_gpu_va_bytes, dpm->table_allocation_bytes) ||
        !span_valid(dpm->parameter_gpu_va_bytes, dpm->parameter_bytes) ||
        dpm->table_allocation_bytes < SGX535_ENGINE121_DPM_INFO_BYTES ||
        dpm->raster_geometry_base_bytes >= UINT64_C(0x100000000))
        return SGX535_ENGINE121_RANGE;
    if ((dpm->table_gpu_va_bytes & 4095u) || (dpm->parameter_gpu_va_bytes & UINT64_C(0xfffff)) ||
        (dpm->parameter_bytes & 4095u) ||
        (dpm->raster_geometry_base_bytes & (SGX535_ENGINE121_ADDRESS_WINDOW - 1u)))
        return SGX535_ENGINE121_ALIGNMENT;
    if (dpm->parameter_gpu_va_bytes < dpm->raster_geometry_base_bytes)
        return SGX535_ENGINE121_RANGE;
    relative = dpm->parameter_gpu_va_bytes - dpm->raster_geometry_base_bytes;
    if (relative >= SGX535_ENGINE121_ADDRESS_WINDOW ||
        dpm->parameter_bytes > SGX535_ENGINE121_ADDRESS_WINDOW - relative ||
        overlap(dpm->table_gpu_va_bytes, dpm->table_allocation_bytes, dpm->parameter_gpu_va_bytes,
                dpm->parameter_bytes))
        return SGX535_ENGINE121_RANGE;
    pages = (uint32_t)(dpm->parameter_bytes / 4096u);
    start = (uint32_t)(relative / 4096u);
    if (pages <= UINT32_C(0x61f) || pages > UINT32_C(0xffff) ||
        start > UINT32_C(0xffff) - (pages - 1u))
        return SGX535_ENGINE121_RANGE;
    end = start + pages - 1u;
    range = (end << 16) | start;
    if (dpm->thresholds == SGX535_ENGINE121_THRESHOLDS_ENGINE) {
        ta = pages + UINT32_C(0x10);
        global = ta | UINT32_C(0x10000);
        zls = pages - UINT32_C(477);
        tail = zls - UINT32_C(473);
    } else {
        ta = global = zls = pages - UINT32_C(0x600);
        /* The binary clamps this extra reserve; do not subtract unsigned 0x640
         * from an accepted 0x620-page allocation and silently wrap.
         */
        tail = pages > UINT32_C(0x640) ? pages - UINT32_C(0x640) : 0;
    }
#define WRITE(reg, word)                                                                           \
    do {                                                                                           \
        candidate[n++] =                                                                           \
            (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, (reg), (word), 0};            \
    } while (0)
#define WAIT(kind, reg, word, bits)                                                                \
    do {                                                                                           \
        candidate[n++] = (struct sgx535_engine121_action){(kind), (reg), (word), (bits)};          \
    } while (0)
    WRITE(0x618u, (uint32_t)dpm->table_gpu_va_bytes);
    WRITE(0x61cu, range);
    WRITE(0x648u, (end - 1u) << 16);
    WRITE(0x638u, 0);
    WRITE(0x620u, ta);
    WRITE(0x624u, zls);
    WRITE(0x628u, global);
    WRITE(0x668u, tail);
    WRITE(0x684u, 1);
    WRITE(0x600u, (uint32_t)dpm->table_gpu_va_bytes);
    WRITE(0x604u, range);
    WRITE(0x64cu, (end - 1u) << 16);
    WRITE(0x660u, 0);
    WRITE(0x680u, 1);
    WRITE(0x610u, (uint32_t)dpm->table_gpu_va_bytes);
    WRITE(0x614u, range);
    WRITE(0x654u, (end - 1u) << 16);
    WRITE(0x688u, 1);
    WRITE(0x608u, (uint32_t)dpm->table_gpu_va_bytes);
    WRITE(0x60cu, range);
    WRITE(0x650u, (end - 1u) << 16);
    WRITE(0x690u, 1);
    WAIT(SGX535_ENGINE121_WAIT_SET, 0x118u, 15u, 15u);
    WRITE(0x114u, 15u);
    WAIT(SGX535_ENGINE121_WAIT_CLEAR, 0x118u, 0, 15u);
    WRITE(0x6a8u, 1);
    WAIT(SGX535_ENGINE121_WAIT_SET, 0x12cu, UINT32_C(0x400000), UINT32_C(0x400000));
    WRITE(0x134u, UINT32_C(0x400000));
    WAIT(SGX535_ENGINE121_WAIT_CLEAR, 0x12cu, 0, UINT32_C(0x400000));
#undef WAIT
#undef WRITE
    for (i = 0; i < n; i++)
        out[i] = candidate[i];
    *written = n;
    return SGX535_ENGINE121_OK;
}

static void write_action(struct sgx535_engine121_action *a, size_t *n, uint32_t reg, uint32_t value)
{
    a[(*n)++] = (struct sgx535_engine121_action){SGX535_ENGINE121_WRITE, reg, value, 0};
}
static void readback_action(struct sgx535_engine121_action *a, size_t *n, uint32_t reg)
{
    a[(*n)++] = (struct sgx535_engine121_action){SGX535_ENGINE121_READBACK, reg, 0, 0};
}

static void event_barrier(struct sgx535_engine121_action *a, size_t *n, uint32_t status,
                          uint32_t ack, uint32_t mask)
{
    a[(*n)++] = (struct sgx535_engine121_action){SGX535_ENGINE121_WAIT_SET, status, mask, mask};
    write_action(a, n, ack, mask);
    a[(*n)++] = (struct sgx535_engine121_action){SGX535_ENGINE121_WAIT_CLEAR, status, 0, mask};
}

/* PDS_INV0 (0xad0, DSC0/1 status 0x22) before TA, PDS_INV1 (0xad4, 0x44)
 * before 3D, each with the code cache (CSC); then the MADD/data cache. */
static void cache_kicks(struct sgx535_engine121_action *a, size_t *n, uint32_t pds_inv,
                        uint32_t pds_status)
{
    write_action(a, n, pds_inv, 1);
    event_barrier(a, n, 0x138, 0x140, pds_status);
    write_action(a, n, 0xae0, 1);
    event_barrier(a, n, 0x138, 0x140, 1);
    write_action(a, n, 0x804, UINT32_C(0x1000ffff));
    event_barrier(a, n, 0x12c, 0x134, UINT32_C(0x04000000));
}

static void copy_actions(const struct sgx535_engine121_action *a, size_t n,
                         struct sgx535_engine121_action *out, size_t *written)
{
    size_t i;
    for (i = 0; i < n; ++i)
        out[i] = a[i];
    *written = n;
}

enum sgx535_engine121_status sgx535_engine121_bootstrap_build(uint32_t core_id, uint32_t revision,
                                                              struct sgx535_engine121_action *out,
                                                              size_t capacity, size_t *written)
{
    struct sgx535_engine121_action a[SGX535_ENGINE121_BOOTSTRAP_ACTIONS];
    size_t n = 0;
    if (!out || !written)
        return SGX535_ENGINE121_INVALID;
    if (core_id != SGX535_ENGINE121_CORE_ID || revision != SGX535_ENGINE121_REVISION)
        return SGX535_ENGINE121_UNSUPPORTED;
    if (capacity < SGX535_ENGINE121_BOOTSTRAP_ACTIONS)
        return SGX535_ENGINE121_NO_SPACE;
    write_action(a, &n, 0xca0, 0xc07c);
    readback_action(a, &n, 0xca0);
    write_action(a, &n, 0x13c, 0);
    write_action(a, &n, 0xa7c, 0);
    write_action(a, &n, 0xa80, 0);
    readback_action(a, &n, 0xa80);
    write_action(a, &n, 0xa74, UINT32_C(0x05188200));
    write_action(a, &n, 0xa78, 0);
    readback_action(a, &n, 0xa78);
    write_action(a, &n, 0xaac, 0x44);
    write_action(a, &n, 0xabc, 0xc);
    write_action(a, &n, 0x804, 0xffff);
    readback_action(a, &n, 0x804);
    write_action(a, &n, 0xa00, 0x7c000);
    readback_action(a, &n, 0xa00);
    write_action(a, &n, 0x630, 0);
    write_action(a, &n, 0xa58, 0);
    copy_actions(a, n, out, written);
    return SGX535_ENGINE121_OK;
}

/* The render's scene (its screen words and root) and the storage's layout (its
 * region arrays' offsets). The layout is not the render's own size: a context
 * has one scene buffer, and two of its jobs of different sizes may be in
 * flight at once (a TA beside the other's raster); arrays placed by each
 * render's size would overlap. */
static enum sgx535_engine121_status
bound_scene(const struct sgx535_engine121_scene_binding *binding,
            struct sgx535_engine121_scene *scene, struct sgx535_engine121_scene *layout)
{
    enum sgx535_engine121_status r;
    if (!binding || !binding->storage || binding->region_array >= SGX535_ENGINE121_REGION_ARRAYS)
        return SGX535_ENGINE121_INVALID;
    r = sgx535_engine121_scene_build(binding->width, binding->height, scene);
    if (r != SGX535_ENGINE121_OK)
        return r;
    *layout = *scene;
    if (binding->layout_width || binding->layout_height) {
        if (binding->layout_width < binding->width || binding->layout_height < binding->height)
            return SGX535_ENGINE121_RANGE;
        r = sgx535_engine121_scene_build(binding->layout_width, binding->layout_height, layout);
        if (r != SGX535_ENGINE121_OK)
            return r;
    }
    if (!span_valid(binding->gpu_va_bytes, binding->allocation_bytes) ||
        binding->allocation_bytes < sgx535_engine121_scene_bytes(layout, binding->region_array + 1u))
        return SGX535_ENGINE121_RANGE;
    if (binding->gpu_va_bytes & 4095u)
        return SGX535_ENGINE121_ALIGNMENT;
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status
sgx535_engine121_ta_build(const struct sgx535_engine121_scene_binding *binding,
                          struct sgx535_engine121_action *out, size_t capacity, size_t *written)
{
    struct sgx535_engine121_scene scene, layout;
    struct sgx535_engine121_action a[SGX535_ENGINE121_TA_ACTIONS];
    enum sgx535_engine121_status r;
    size_t n = 0;
    uint32_t base;
    if (!out || !written)
        return SGX535_ENGINE121_INVALID;
    if (capacity < SGX535_ENGINE121_TA_ACTIONS)
        return SGX535_ENGINE121_NO_SPACE;
    r = bound_scene(binding, &scene, &layout);
    if (r != SGX535_ENGINE121_OK)
        return r;
    base = (uint32_t)binding->gpu_va_bytes;
    write_action(a, &n, 0x220, base + scene.words[8]);
    write_action(a, &n, 0x65c, 0);
    write_action(a, &n, 0x694, 2);
    write_action(a, &n, 0x698, 2);
    write_action(a, &n, 0x24c, 1);
    write_action(a, &n, 0x224, UINT32_C(0x80000000));
    write_action(a, &n, 0x208, scene.words[2]);
    write_action(a, &n, 0x20c, scene.words[3]);
    write_action(a, &n, 0x214, scene.words[4]);
    write_action(a, &n, 0x210, scene.words[5]);
    write_action(a, &n, 0x248, scene.words[6]);
    write_action(a, &n, 0x21c, base + region_offset(&layout, binding->region_array));
    write_action(a, &n, 0x274, 0);
    event_barrier(a, &n, 0x12c, 0x134, UINT32_C(0x00100a40));
    write_action(a, &n, 0xc90, UINT32_C(0x30000000));
    cache_kicks(a, &n, 0xad0, 0x22);
    /* The USE cache invalidate is the caller's (beside a raster only with the
     * 3D's USSE tasks idle). */
    write_action(a, &n, 0x200, 1);
    copy_actions(a, n, out, written);
    return SGX535_ENGINE121_OK;
}

enum sgx535_engine121_status
sgx535_engine121_raster_build(const struct sgx535_engine121_scene_binding *binding,
                              struct sgx535_engine121_action *out, size_t capacity, size_t *written)
{
    struct sgx535_engine121_scene scene, layout;
    struct sgx535_engine121_action a[SGX535_ENGINE121_RASTER_ACTIONS];
    enum sgx535_engine121_status r;
    size_t n = 0;
    uint32_t base;
    if (!out || !written)
        return SGX535_ENGINE121_INVALID;
    if (capacity < SGX535_ENGINE121_RASTER_ACTIONS)
        return SGX535_ENGINE121_NO_SPACE;
    r = bound_scene(binding, &scene, &layout);
    if (r != SGX535_ENGINE121_OK)
        return r;
    base = (uint32_t)binding->gpu_va_bytes;
    write_action(a, &n, 0x65c, 0);
    write_action(a, &n, 0x63c, 3);
    write_action(a, &n, 0x658, 0);
    write_action(a, &n, 0x408, base + region_offset(&layout, binding->region_array));
    cache_kicks(a, &n, 0xad4, 0x44);
    write_action(a, &n, 0x43c, 1);
    write_action(a, &n, 0xa08, 1);
    readback_action(a, &n, 0xa08);
    write_action(a, &n, 0x428, 1);
    copy_actions(a, n, out, written);
    return SGX535_ENGINE121_OK;
}
