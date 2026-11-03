// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef POULSBACK_SGX535_PACK_H
#define POULSBACK_SGX535_PACK_H
#include "sgx535_usse.h"

enum sgx535_pack_status {
    SGX535_PACK_OK = 0,
    SGX535_PACK_ERR_ARGUMENT = -1,
    SGX535_PACK_ERR_RANGE = -2,
    SGX535_PACK_ERR_UNSUPPORTED = -3
};
struct sgx535_pack_f32_u8 {
    struct sgx535_mov_operand destination;
    struct sgx535_mov_operand source[2];
    uint32_t write_mask;
    uint32_t scale;
    uint32_t end;
};

/* Bounded direct F32->U8 word profile, repeat=1, SKIPINV, source component=0.
 * Destinations TEMP/OUTPUT; sources TEMP/OUTPUT/PRIMARY/SECONDARY. Masks are
 * four bits, excluding three-bit masks. Scale and END are boolean fields.
 * Byte order, scaling/saturation/rounding and PBE semantics are unqualified.
 * No GPU dispatch. Errors preserve output; valid stable objects may overlap.
 */
enum sgx535_pack_status sgx535_pack_f32_u8_encode(const struct sgx535_pack_f32_u8 *, uint32_t[2]);
enum sgx535_pack_status sgx535_pack_f32_u8_decode(const uint32_t[2], struct sgx535_pack_f32_u8 *);

/* Single physical-byte write: src1 is the selected scalar, src2 is immediate
 * zero, with its fixed PACK bank extension. byte_index0..3 identifies the
 * encoded lane, not a logical color component. Conversion remains numerical
 * and hardware-unqualified; the original two-register profile is unchanged. */
struct sgx535_pack_f32_u8_channel {
    struct sgx535_mov_operand destination, source;
    uint32_t byte_index, scale, end;
};
enum sgx535_pack_status sgx535_pack_f32_u8_channel_encode(const struct sgx535_pack_f32_u8_channel *,
                                                          uint32_t[2]);
enum sgx535_pack_status sgx535_pack_f32_u8_channel_decode(const uint32_t[2],
                                                          struct sgx535_pack_f32_u8_channel *);

/* One U8 component from a single packed source DWORD to one full F32 word.
 * The source descriptor (including byte_index0..3) is repeated in both native
 * operands. SCALE=1, full destination mask, repeat=1 and SKIPINV are fixed.
 * Destination TEMP/OUTPUT; source TEMP/OUTPUT/PRIMARY/SECONDARY. TEMP0..123,
 * other direct registers0..127 and END0/1 follow the existing bounded policy.
 * This codec allocates no sample registers or images and proves no numerical,
 * cache, dependency or GPU semantics. Errors preserve output; stable objects
 * may overlap. It does not admit the special two-DWORD sampling layout.
 */
struct sgx535_unpack_u8_f32_component {
    struct sgx535_mov_operand destination, source;
    uint32_t byte_index, end;
};
enum sgx535_pack_status
sgx535_unpack_u8_f32_component_encode(const struct sgx535_unpack_u8_f32_component *, uint32_t[2]);
enum sgx535_pack_status
sgx535_unpack_u8_f32_component_decode(const uint32_t[2], struct sgx535_unpack_u8_f32_component *);
#endif
