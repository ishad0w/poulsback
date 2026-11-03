// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_ENGINE121_H
#define SGX535_ENGINE121_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* CPU construction only. A kernel-owned physical adapter must authenticate
 * namespace/ownership/publication and execute actions inside its source/IRQ
 * transaction. These types are neither an exported UAPI nor device admission.
 */
#define SGX535_ENGINE121_CORE_ID UINT32_C(0x01130000)
#define SGX535_ENGINE121_REVISION UINT32_C(0x00010201)
#define SGX535_ENGINE121_DPM_ACTIONS 29u
#define SGX535_ENGINE121_BOOTSTRAP_ACTIONS 17u
#define SGX535_ENGINE121_TA_ACTIONS 30u
#define SGX535_ENGINE121_RASTER_ACTIONS 20u
#define SGX535_ENGINE121_DPM_INFO_BYTES UINT32_C(0x620000)
#define SGX535_ENGINE121_ADDRESS_WINDOW UINT64_C(0x10000000)

enum sgx535_engine121_status {
    SGX535_ENGINE121_OK = 0,
    SGX535_ENGINE121_INVALID,
    SGX535_ENGINE121_UNSUPPORTED,
    SGX535_ENGINE121_RANGE,
    SGX535_ENGINE121_ALIGNMENT,
    SGX535_ENGINE121_NO_SPACE,
    SGX535_ENGINE121_STALE,
    SGX535_ENGINE121_FAULT,
    SGX535_ENGINE121_PENDING,
    SGX535_ENGINE121_HELD
};

enum sgx535_engine121_action_kind {
    SGX535_ENGINE121_WRITE = 1,
    SGX535_ENGINE121_WAIT_SET,
    SGX535_ENGINE121_WAIT_CLEAR,
    /* An ordered read observation only: value/mask are zero, no compare or
	 * posted-write, READY, cache or retirement certificate is implied. */
    SGX535_ENGINE121_READBACK = 4
};

struct sgx535_engine121_action {
    uint32_t kind;
    uint32_t offset;
    uint32_t value;
    uint32_t mask;
};

/* Standard small-scene branch: no wide scene or doubled-region variant.
 * words are host uint32 values, not an endian-independent byte stream.
 * allocation_bytes includes the scene root, region lists and control tail.
 */
struct sgx535_engine121_scene {
    uint32_t width, height;
    uint32_t words[16];
    uint32_t allocation_bytes;
    uint32_t clear_page_start, clear_page_count;
};

enum sgx535_engine121_status sgx535_engine121_scene_build(uint32_t width, uint32_t height,
                                                          struct sgx535_engine121_scene *out);

/* Bytes a scene binding needs with ARRAYS region-header arrays (1 or 2); 0 for
 * another count. Array 1 follows the control table, 64-byte aligned. */
#define SGX535_ENGINE121_REGION_ARRAYS 2u
uint32_t sgx535_engine121_scene_bytes(const struct sgx535_engine121_scene *scene, uint32_t arrays);

/* GPU byte addresses, never CPU pointers or physical pages. Parameter storage
 * belongs to one aligned 256-MiB RASTGEOM address window; its page indices are
 * relative to that window. The free-list table address is a full SGX VA.
 * Canonical nonzero storage IDs must be authenticated by the kernel owner;
 * different IDs mean nonaliasing backing storage. This builder cannot prove
 * that assertion, mappings, WB/device visibility or valid table contents.
 */
/* DPM page thresholds. LEGACY construction profile: TA, ZLS and global at
 * pages - 0x600, PDS at pages - 0x640. ENGINE (the GPU engine's, SGX535 rev 1.2.1):
 * TA and global beyond the pool (pages + 0x10, global with its policy bit),
 * ZLS at pages - 477 and PDS at ZLS - 473. */
enum sgx535_engine121_thresholds {
    SGX535_ENGINE121_THRESHOLDS_LEGACY = 0,
    SGX535_ENGINE121_THRESHOLDS_ENGINE
};

struct sgx535_engine121_dpm {
    uint64_t table_storage, table_gpu_va_bytes, table_allocation_bytes;
    uint64_t parameter_storage, parameter_gpu_va_bytes, parameter_bytes;
    uint64_t raster_geometry_base_bytes;
    uint32_t thresholds;
};

/* All four loads, including DHOST, must be observed before ACK15. No ACK can
 * establish completion. Every wait result must be checked by the executor.
 * Errors leave output and written untouched. Inputs, output and written must
 * not overlap. A valid plan is static construction, not hardware readiness.
 */
enum sgx535_engine121_status sgx535_engine121_dpm_build(const struct sgx535_engine121_dpm *dpm,
                                                        struct sgx535_engine121_action *out,
                                                        size_t capacity, size_t *written);

/* Exact revision121 static zero-selector branch: twelve writes and the five
 * ordered noncomparative reads present in the matched initializer. Incoming
 * context mode selectors are constrained to zero, not recovered from stepping
 * or arbitrary existing state. Reset, PD ownership, clock policy, CPU/device
 * visibility and initial event clearing are external prerequisites. No reset,
 * MMU invalidation or IRQ writes are hidden in this construction. Errors leave
 * output/written untouched; output and written must not overlap.
 */
enum sgx535_engine121_status sgx535_engine121_bootstrap_build(uint32_t core_id, uint32_t revision,
                                                              struct sgx535_engine121_action *out,
                                                              size_t capacity, size_t *written);

struct sgx535_engine121_scene_binding {
    uint64_t storage, gpu_va_bytes, allocation_bytes;
    uint32_t width, height;
    uint32_t region_array; /* the region-header array the TA fills and the raster reads */
    /* The size the storage was laid out for, at least the render's: its region
     * arrays sit at that layout's offsets, so renders of different sizes in one
     * storage never share region-header bytes. 0: the render's own size. */
    uint32_t layout_width, layout_height;
};

/* New scene, no OOM continuation or rerender. The DPM context selector (0x630)
 * is the caller's: it sets ALLOC and LS to the TA's context before this plan
 * (whose CLEARs act on LS) and DALLOC to the job's context before the raster's
 * (docs/native-3d.md#engine-plans). So is the USE cache invalidate before the
 * TA (0xa08; beside a raster only with the 3D's USSE tasks idle);
 * the raster plan keeps its own, as no TA runs at a raster kick. These plans
 * cover scene load and cache/kick registers only. Independently checked
 * TA/PDS/USE, vertex and raster state must already have been installed by the
 * kernel packet owner. They do not synthesize those missing packets or qualify
 * their address units.
 */
enum sgx535_engine121_status
sgx535_engine121_ta_build(const struct sgx535_engine121_scene_binding *scene,
                          struct sgx535_engine121_action *out, size_t capacity, size_t *written);
/* The 3D pass of the same job: the TA left the scene resident in its DPM
 * context, so no state/control memory LOAD is emitted (a full reload would load
 * a state table this TA never stored); the caller points DALLOC at
 * that context. This builder grants no handoff authority. */
enum sgx535_engine121_status
sgx535_engine121_raster_build(const struct sgx535_engine121_scene_binding *scene,
                              struct sgx535_engine121_action *out, size_t capacity,
                              size_t *written);

#endif
