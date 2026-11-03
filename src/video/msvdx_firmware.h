// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef MSVDX_FIRMWARE_H
#define MSVDX_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

#define MSVDX_FW_V2_HEADER_BYTES 16u
#define MSVDX_FW_MAX_CHUNKS 16u
#define MSVDX_FW_MAX_REGION_BYTES 32768u
#define MSVDX_FW_CODE_BASE UINT32_C(0x80900000)
#define MSVDX_FW_DATA_BASE UINT32_C(0x82880000)
#define MSVDX_FW_SIGNATURE UINT32_C(0xa5a5a5a5)
enum msvdx_fw_status {
    MSVDX_FW_OK,
    MSVDX_FW_ARGUMENT,
    MSVDX_FW_LENGTH,
    MSVDX_FW_VERSION,
    MSVDX_FW_RANGE,
    MSVDX_FW_GEOMETRY,
    MSVDX_FW_ALIAS
};
enum msvdx_fw_space { MSVDX_FW_CODE, MSVDX_FW_DATA };
struct msvdx_fw_view {
    uint32_t text_words, data_words, data_location;
    size_t text_offset, data_offset, bytes;
};
/* Positive CPU policy limits. Bank size/capacities must eventually come from
 * an authenticated device profile; accepting them here does not qualify one.
 * This bounded profile permits at most eight banks per code/data region. */
struct msvdx_fw_geometry {
    uint32_t bank_bytes, code_bytes, data_bytes;
};
struct msvdx_fw_chunk {
    enum msvdx_fw_space space;
    uint32_t bank, region_byte_offset, word_address, words;
    size_t source_offset;
};
struct msvdx_fw_plan {
    struct msvdx_fw_view image;
    struct msvdx_fw_geometry geometry;
    uint32_t count;
    struct msvdx_fw_chunk chunks[MSVDX_FW_MAX_CHUNKS];
};
/* Exact old four-LE-DWORD version-2 container, NOT an EMGD seven-field i386
 * allocation descriptor or AOSP firmware object. Nonempty code/data required.
 * Complete length/address validation precedes output. No casting, allocation,
 * upload, firmware execution, signature admission or cryptographic identity.
 * The caller retains the byte-original/hash separately. All spans must be
 * valid stable objects; output must not overlap the input. Errors preserve it. */
enum msvdx_fw_status msvdx_fw_parse_v2(const uint8_t *, size_t, struct msvdx_fw_view *);
/* Split at bank boundaries using region-relative byte and DWORD units.
 * word_address deliberately remains region-relative, not bank-relative,
 * matching the selected old host producer. This is not a register encoder.
 * Both metadata and bytes remain stable during the call. Output is separate
 * from both inputs; every failure leaves it unchanged. */
enum msvdx_fw_status msvdx_fw_plan_v2(const uint8_t *, size_t, const struct msvdx_fw_geometry *,
                                      struct msvdx_fw_plan *);
#endif
