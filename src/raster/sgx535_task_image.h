// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_TASK_IMAGE_H
#define SGX535_TASK_IMAGE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* Internal construction contract, not UAPI or an execution certificate. */
#define SGX535_TASK_IMAGE_VERSION 1u
#define SGX535_TASK_IMAGE_BYTES 128u
#define SGX535_TASK_IMAGE_SECTIONS 3u
#define SGX535_TASK_IMAGE_RELOCS 3u
#define SGX535_TASK_IMAGE_ROLES 8u
#define SGX535_TASK_W1_PACKED_CONSTANT 1u

enum sgx535_task_status {
    SGX535_TASK_BLOCKED = 1, /* Valid construction, missing executable contracts. */
    SGX535_TASK_INVALID = -1,
    SGX535_TASK_UNSUPPORTED = -2,
    SGX535_TASK_NOMEM = -3 /* No output or publication on allocation failure. */
};
enum sgx535_task_role {
    SGX535_TASK_PDS = 0,
    SGX535_TASK_USE_VERTEX,
    SGX535_TASK_USE_FRAGMENT,
    SGX535_TASK_VERTEX_DATA,
    SGX535_TASK_SCENE,
    SGX535_TASK_TARGET,
    SGX535_TASK_DPM_TABLES,
    SGX535_TASK_DPM_PARAMETERS
};
/* Hardware address windows of a role's extent. PDS addresses are offsets from
 * PDS_EXEC_BASE (0x20000000) and DPM parameters are relative to the 3D request
 * base (0x30000000), one 256 MiB window each. Every other role, and the depth
 * buffer, carries full 32-bit addresses and lies above both windows. */
#define SGX535_TASK_FULL_ADDRESS_FIRST 0x40000000u
static inline int sgx535_task_extent_in(uint64_t first, uint64_t limit, uint64_t va, uint64_t bytes)
{
    return va >= first && va < limit && bytes <= limit - va;
}
static inline int sgx535_task_role_placed(uint32_t role, uint64_t va, uint64_t bytes)
{
    return role == SGX535_TASK_PDS ? sgx535_task_extent_in(0x20000000u, 0x30000000u, va, bytes)
           : role == SGX535_TASK_DPM_PARAMETERS
               ? sgx535_task_extent_in(0x30000000u, 0x40000000u, va, bytes)
               : sgx535_task_extent_in(SGX535_TASK_FULL_ADDRESS_FIRST, (uint64_t)1 << 32, va,
                                       bytes);
}
enum sgx535_task_section_kind {
    SGX535_TASK_WINDOW_VERTEX_WORDS = 1,
    SGX535_TASK_VERTEX_USE_WORDS,
    SGX535_TASK_FRAGMENT_USE_WORDS
};
/* These are copy placements, not unresolved GPU address fields. No PDS/ISP
 * address patch is emitted until its consuming packet exists and is checked. */
#define SGX535_TASK_RELOC_COPY_SECTION 1u
#define SGX535_TASK_ACCESS_READ 1u
#define SGX535_TASK_ACCESS_WRITE 2u
#define SGX535_TASK_MISSING_BANK_LEASE (1u << 0)
#define SGX535_TASK_MISSING_PDS_TASK_CONTROLS (1u << 1)
#define SGX535_TASK_MISSING_VERTEX_PDS (1u << 2)
#define SGX535_TASK_MISSING_VERTEX_TERMINAL (1u << 3)
#define SGX535_TASK_MISSING_VERTEX_LAYOUT (1u << 4)
#define SGX535_TASK_MISSING_ISP_TSP (1u << 5)
#define SGX535_TASK_MISSING_PBE (1u << 6)
#define SGX535_TASK_MISSING_ALL 0x7fu

struct sgx535_task_request {
    uint32_t version, profile, core_id, revision;
    uint32_t clip_position[3][4]; /* IEEE binary32 words, W exactly +1. */
    uint32_t packed_argb;         /* Data only, arbitrary color. */
    uint32_t reserved[3];
};
struct sgx535_task_section {
    uint32_t kind, byte_offset, byte_count, alignment;
};
struct sgx535_task_reloc {
    uint32_t kind, section, role, resource_byte_offset;
    uint32_t section_byte_offset, span_bytes, alignment, reserved;
};
struct sgx535_task_requirement {
    uint32_t role, access, minimum_bytes, alignment;
};
struct sgx535_task_image {
    uint32_t version, profile, core_id, revision;
    uint32_t width, height, stride_bytes, packed_argb;
    uint32_t missing_contracts, byte_count, section_count, relocation_count;
    struct sgx535_task_section sections[SGX535_TASK_IMAGE_SECTIONS];
    struct sgx535_task_reloc relocations[SGX535_TASK_IMAGE_RELOCS];
    struct sgx535_task_requirement requirements[SGX535_TASK_IMAGE_ROLES];
    /* One vertex's four-word DMA descriptor projection; not PDS instructions.
	 * The source offset is relative to each vertex, not a GPU address. */
    uint32_t vertex_words, vertex_stride_bytes;
    uint32_t dma_descriptor, dma_source_byte_offset, dma_word_count;
    uint32_t reserved[3];
    uint8_t bytes[SGX535_TASK_IMAGE_BYTES]; /* Explicit little-endian sections. */
};

/* Generic bounded triangle construction: no scene/color recognition, clear,
 * depth, blend, culling, allocation, caller task words, GPU VAs or MMIO.
 * BLOCKED publishes all known sections and explicit missing contracts. Errors
 * leave the output unchanged. Stable input/output storage may overlap.
 * Neither a caller-cleared missing mask nor this object grants admission. */
enum sgx535_task_status sgx535_task_image_build(const struct sgx535_task_request *,
                                                struct sgx535_task_image *);
/* Reconstruct and compare every byte, including unused storage. A valid image
 * still returns BLOCKED; there is deliberately no runnable/qualified flag. */
enum sgx535_task_status sgx535_task_image_validate(const struct sgx535_task_request *,
                                                   const struct sgx535_task_image *);
#endif
