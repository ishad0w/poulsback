// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef MSVDX_OWNED_H
#define MSVDX_OWNED_H
#include <stddef.h>
#include <stdint.h>

#define MSVDX_OWNED_MAX_BUFFERS 64u
#define MSVDX_OWNED_MAX_RELOCS 4096u
#define MSVDX_OWNED_READ 1u
#define MSVDX_OWNED_PATCH 2u

/* Trusted caller-resolved registry snapshot, NOT a userspace wire structure.
 * Caller pins every allocation and excludes concurrent changes for both passes.
 * owner/epoch are checked against the whole snapshot; this is not
 * authentication of a registry fabricated by a client. GPU addresses must
 * already be qualified by the caller's MMU allocator; this codec neither
 * allocates nor maps them.
 */
struct msvdx_owned_buffer {
    uint32_t handle;
    uint32_t access;
    uint64_t owner;
    uint64_t epoch;
    uint64_t gpu_base;
    uint8_t *mapping;
    size_t bytes;
};
struct msvdx_owned_reloc {
    uint32_t destination;
    uint32_t source;
    size_t destination_offset;
    size_t source_offset;
    size_t source_bytes;
    uint32_t alignment; /* caller-required power of two, 1..4096 */
};
enum msvdx_owned_status {
    MSVDX_OWNED_OK,
    MSVDX_OWNED_ARGUMENT,
    MSVDX_OWNED_LIMIT,
    MSVDX_OWNED_IDENTITY,
    MSVDX_OWNED_ACCESS,
    MSVDX_OWNED_BOUNDS,
    MSVDX_OWNED_ADDRESS,
    MSVDX_OWNED_ALIAS
};

/* Validate or patch complete little-endian address words in CPU-owned mappings.
 * Complete validation precedes any edit; failure leaves all mappings unchanged.
 * All mappings are disjoint, destinations are aligned distinct DWORD slots,
 * and metadata must not overlap any mapping. Inputs are valid stable C objects.
 * A nonempty relocation set is required. No allocation, I/O, submit,
 * completion, cache handoff, fence advancement or resource release occurs here.
 */
enum msvdx_owned_status msvdx_owned_validate(const struct msvdx_owned_buffer *buffers,
                                             size_t buffer_count,
                                             const struct msvdx_owned_reloc *relocs,
                                             size_t reloc_count, uint64_t owner, uint64_t epoch);
enum msvdx_owned_status msvdx_owned_patch(const struct msvdx_owned_buffer *buffers,
                                          size_t buffer_count,
                                          const struct msvdx_owned_reloc *relocs,
                                          size_t reloc_count, uint64_t owner, uint64_t epoch);
#endif
