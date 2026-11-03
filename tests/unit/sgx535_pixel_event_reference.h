// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#ifndef SGX535_PIXEL_EVENT_REFERENCE_H
#define SGX535_PIXEL_EVENT_REFERENCE_H

#include <stdint.h>
#include <string.h>

/* This test model reads encoded fields independently of the production emitter.
 * It covers only the two selected event profiles, not general GPU execution. */
struct pixel_event_ref_op {
    uint32_t group, type, condition, destination, destination_bank;
    uint32_t source, source_bank, other, shift, target;
    uint32_t port, pair[2], selector[4];
};

static inline uint32_t pixel_event_ref_word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int pixel_event_ref_decode(uint32_t word, struct pixel_event_ref_op *op)
{
    uint32_t allowed = 0xff000000u;
    struct pixel_event_ref_op d = {0};
    d.group = word >> 30;
    d.type = (word >> 27) & 7u;
    d.condition = (word >> 24) & 7u;
    if (d.group == 3 && d.type <= 1) {
        allowed |= 0x00fe0000u | 0x0000fc00u | 0x0000007fu;
        d.source_bank = (word >> 23) & 1u;
        d.source = (word >> 17) & 63u;
        d.other = (word >> 10) & 63u;
        d.destination_bank = (word >> 6) & 1u;
        d.destination = word & 63u;
    } else if (d.group == 3 && d.type == 6) {
        allowed |= 0x00ff0000u | 0x00007f00u | 0x0000007fu;
        d.source_bank = (word >> 22) & 3u;
        d.source = (word >> 16) & 63u;
        d.shift = (word >> 8) & 127u;
        d.destination_bank = (word >> 6) & 1u;
        d.destination = word & 63u;
        if (d.source_bank != 2 || d.source > 1 || d.shift > 31)
            return 0;
    } else if (d.group == 2 && d.type == 0) {
        allowed |= 0x00fe0000u;
        d.source_bank = (word >> 23) & 1u;
        d.source = (word >> 17) & 63u;
    } else if (d.group == 2 && d.type == 2) {
        allowed |= 0x0007ffffu;
        d.target = word & 0x0007ffffu;
        if (d.condition != 0)
            return 0;
    } else if (d.group == 2 && d.type == 5) {
        /* HALT has no selected operands. */
    } else if (d.group == 0 && d.type == 2) {
        allowed |= 0x0001fe00u | 0x000000feu;
        d.source_bank = (word >> 15) & 3u;
        d.source = (word >> 9) & 63u;
        d.destination_bank = (word >> 7) & 1u;
        d.destination = (word >> 1) & 63u;
        if (d.source_bank > 1)
            return 0;
    } else if (d.group == 0 && d.type == 0) {
        allowed |= 0x007c0000u | 0x0003e000u | 0x00000fe0u | 0x0000000fu;
        d.pair[0] = (word >> 18) & 31u;
        d.pair[1] = (word >> 13) & 31u;
        for (uint32_t i = 0; i < 4; ++i)
            d.selector[i] = (word >> (11u - 2u * i)) & 3u;
        d.port = word & 15u;
        if (d.port != 3 && d.port != 5)
            return 0;
    } else {
        return 0;
    }
    if (word & ~allowed)
        return 0;
    if (!(d.group == 2 && d.type == 2) && d.condition != 7)
        return 0;
    *op = d;
    return 1;
}

static inline int pixel_event_ref_movs(const struct pixel_event_ref_op *op, uint32_t port,
                                       uint32_t pair0, uint32_t pair1, uint32_t last)
{
    return op->group == 0 && op->type == 0 && op->port == port && op->pair[0] == pair0 &&
           op->pair[1] == pair1 && op->selector[0] == 0 && op->selector[1] == 1 &&
           op->selector[2] == (last == 3u && pair0 == 0u && port == 5u ? 3u : 2u) &&
           op->selector[3] == last;
}

static inline int pixel_event_ref_logic(const struct pixel_event_ref_op *op, uint32_t type,
                                        uint32_t destination_bank, uint32_t destination,
                                        uint32_t source_bank, uint32_t source, uint32_t other)
{
    return op->group == 3 && op->type == type && op->destination_bank == destination_bank &&
           op->destination == destination && op->source_bank == source_bank &&
           op->source == source && op->other == other;
}

static inline int pixel_event_ref_test_branch(const struct pixel_event_ref_op *ops, uint32_t mask,
                                              uint32_t target)
{
    return pixel_event_ref_logic(&ops[0], 1, 0, 48, 1, 1, mask) && ops[1].group == 2 &&
           ops[1].type == 0 && ops[1].source_bank == 0 && ops[1].source == 48 &&
           ops[2].group == 2 && ops[2].type == 2 && ops[2].target == target;
}

static inline int pixel_event_ref_structure(const struct pixel_event_ref_op *ops, int with_initial)
{
    uint32_t base = with_initial ? 5u : 0u;
    uint32_t last = with_initial ? 20u : 15u;
    const struct pixel_event_ref_op *move = &ops[base + 4u];
    if ((with_initial &&
         (!pixel_event_ref_test_branch(ops, 0, 5) || ops[4].group != 2 || ops[4].type != 5)) ||
        !pixel_event_ref_test_branch(ops + base, with_initial ? 2u : 0u, last) ||
        ops[last].group != 2 || ops[last].type != 5 || move->group != 0 || move->type != 2 ||
        move->source_bank != 1 || move->source != (with_initial ? 3u : 1u) ||
        move->destination_bank != 1 || move->destination != 48)
        return 0;
    /* Each group has a fixed semantic role. Equivalent temporary renaming or
     * reordered instructions cannot silently weaken exact-profile coverage. */
    for (uint32_t group = 0; group < 3; ++group) {
        const struct pixel_event_ref_op *shift = &ops[base + 5u + group * 3u];
        if (shift->group != 3 || shift->type != 6 || shift->source_bank != 2 ||
            shift->source != (group == 2 ? 1u : 0u) || shift->shift != (group == 2 ? 25u : 11u) ||
            shift->destination_bank != 0 || shift->destination != 49u + group ||
            !pixel_event_ref_logic(shift + 1, 1, 0, 49u + group, 0, 49u + group,
                                   (with_initial ? 4u : 2u) + group) ||
            !pixel_event_ref_logic(shift + 2, 0, 1, 48, 0, 49u + group, 48))
            return 0;
    }
    return 1;
}

static inline int pixel_event_ref_emit(const uint8_t *bytes, uint32_t message)
{
    uint32_t low = pixel_event_ref_word(bytes), high = pixel_event_ref_word(bytes + 4);
    uint32_t sideband =
        ((low >> 22) & 63u) | (((high >> 3) & 63u) << 6) | (((high >> 22) & 3u) << 12);
    uint32_t allowed_high =
        0xf8000000u | 0x07000000u | 0x00300000u | 0x00c00000u | 0x00040000u | 0x000001f8u | 7u;
    return (high & ~allowed_high) == 0 && high >> 27 == 31 && ((high >> 24) & 7u) == 3 &&
           ((high >> 20) & 3u) == 2 && ((high >> 18) & 1u) == message && (high & 7u) == 4 &&
           low >> 30 == 2 && ((low >> 28) & 3u) == 2 && ((low >> 21) & 1u) == 1 &&
           ((low >> 14) & 127u) == (message ? 0u : 2u) &&
           ((low >> 7) & 127u) == (message ? 1u : 3u) && (low & 127u) == 2 &&
           sideband == (message ? 0x204u : 0x10u);
}

static inline int pixel_event_ref_use(const uint8_t *bytes, int with_initial)
{
    if (with_initial) {
        uint32_t high = pixel_event_ref_word(bytes + 4);
        uint32_t allowed = 0xf8000000u | 0x00300000u | 0x00040000u | 0x00008000u;
        if (pixel_event_ref_word(bytes) || (high & ~allowed) || high >> 27 != 31 ||
            ((high >> 20) & 3u) != 3 || ((high >> 18) & 1u) != 1 || ((high >> 15) & 1u) != 1 ||
            pixel_event_ref_word(bytes + 8) || pixel_event_ref_word(bytes + 12))
            return 0;
        bytes += 16;
    }
    return pixel_event_ref_emit(bytes, 0) && pixel_event_ref_emit(bytes + 8, 1);
}

struct pixel_event_ref_trace {
    uint32_t count;
    uint32_t port[2], words[2][3];
};

static inline int pixel_event_ref_execute(const uint32_t data[16],
                                          const struct pixel_event_ref_op *ops, uint32_t code_count,
                                          uint32_t ir0, uint32_t ir1,
                                          struct pixel_event_ref_trace *trace)
{
    uint32_t stores[2][64] = {{0}}, valid[2][64] = {{0}};
    uint32_t pc = 0, predicate = 0;
    struct pixel_event_ref_trace t = {0};
    for (uint32_t i = 0; i < 16; ++i) {
        stores[i / 8u][i % 8u] = data[i];
        valid[i / 8u][i % 8u] = 1;
    }
    for (uint32_t steps = 0; steps < 32; ++steps) {
        const struct pixel_event_ref_op *op;
        uint32_t a, result;
        if (pc >= code_count)
            return 0;
        op = &ops[pc++];
        if (op->group == 2 && op->type == 5) {
            *trace = t;
            return 1;
        }
        if (op->group == 2 && op->type == 2) {
            if (predicate)
                pc = op->target;
            continue;
        }
        if (op->group == 0 && op->type == 0) {
            uint32_t count = op->port == 3 ? 2u : 3u;
            if (t.count >= 2)
                return 0;
            t.port[t.count] = op->port;
            for (uint32_t lane = 0; lane < count; ++lane) {
                uint32_t bank = op->selector[lane] / 2u;
                uint32_t index = op->pair[bank] * 2u + op->selector[lane] % 2u;
                if (!valid[bank][index])
                    return 0;
                t.words[t.count][lane] = stores[bank][index];
            }
            ++t.count;
            continue;
        }
        if (op->group == 3 && op->type == 6) {
            a = op->source ? ir1 : ir0;
            result = a << op->shift;
        } else {
            uint32_t bank = op->group == 0 ? op->source_bank : 0u;
            int input_register = op->group != 0 && op->source_bank;
            if (input_register) {
                if (op->source > 1)
                    return 0;
                a = op->source ? ir1 : ir0;
            } else {
                if (!valid[bank][op->source])
                    return 0;
                a = stores[bank][op->source];
            }
            if (op->group == 2) {
                predicate = a == 0;
                continue;
            }
            result = a;
            if (op->group == 3) {
                if (!valid[1][op->other])
                    return 0;
                result = op->type ? a & stores[1][op->other] : a | stores[1][op->other];
            }
        }
        stores[op->destination_bank][op->destination] = result;
        valid[op->destination_bank][op->destination] = 1;
    }
    return 0;
}

static inline int pixel_event_ref_observe(const uint32_t data[16],
                                          const struct pixel_event_ref_op *ops, uint32_t code_count,
                                          int with_initial, uint32_t ir0, uint32_t ir1,
                                          uint32_t state, uint32_t initial, uint32_t pbe)
{
    struct pixel_event_ref_trace actual, expected = {0};
    if (with_initial && (ir1 & 8u)) {
        expected.count = 1;
        expected.port[0] = 5;
        expected.words[0][0] = initial;
    } else if (ir1 & 2u) {
        expected.count = 2;
        expected.port[0] = 3;
        expected.words[0][0] = state;
        expected.words[0][1] = 0x80000003u;
        expected.port[1] = 5;
        expected.words[1][0] = pbe;
        /* Check bit transfers directly, independently of instruction execution. */
        expected.words[1][2] =
            0x10000000u | ((ir0 & 255u) << 11) | ((ir0 & 0xfe00u) << 11) | ((ir1 & 4u) << 25);
    }
    return pixel_event_ref_execute(data, ops, code_count, ir0, ir1, &actual) &&
           !memcmp(&actual, &expected, sizeof(actual));
}

static inline int sgx535_pixel_event_reference_check(const uint8_t *pds_bytes,
                                                     uint32_t pds_byte_count,
                                                     const uint8_t *use_bytes,
                                                     uint32_t use_byte_count, int with_initial,
                                                     uint32_t state, uint32_t initial, uint32_t pbe)
{
    uint32_t data[16], expected[16] = {0};
    struct pixel_event_ref_op ops[21];
    uint32_t code_count = with_initial ? 21u : 16u;
    uint32_t base = with_initial ? 5u : 0u;
    if (!pds_bytes || !use_bytes || (with_initial != 0 && with_initial != 1) ||
        pds_byte_count != (16u + code_count) * 4u || use_byte_count != (with_initial ? 32u : 16u) ||
        !pixel_event_ref_use(use_bytes, with_initial))
        return 0;
    if (with_initial) {
        expected[0] = initial;
        expected[2] = state;
        expected[3] = 0x80000003u;
        expected[4] = pbe;
        expected[8] = 8;
    } else {
        expected[0] = state;
        expected[1] = 0x80000003u;
        expected[2] = pbe;
    }
    expected[8u + (with_initial ? 2u : 0u)] = 2;
    expected[9u + (with_initial ? 2u : 0u)] = 0x10000000u;
    expected[10u + (with_initial ? 2u : 0u)] = 0x0007f800u;
    expected[11u + (with_initial ? 2u : 0u)] = 0x07f00000u;
    expected[12u + (with_initial ? 2u : 0u)] = 0x08000000u;
    for (uint32_t i = 0; i < 16; ++i) {
        data[i] = pixel_event_ref_word(pds_bytes + i * 4u);
        if (data[i] != expected[i])
            return 0;
    }
    for (uint32_t i = 0; i < code_count; ++i) {
        if (!pixel_event_ref_decode(pixel_event_ref_word(pds_bytes + (16u + i) * 4u), &ops[i]) ||
            (ops[i].group == 2 && ops[i].type == 2 && ops[i].target >= code_count))
            return 0;
    }
    /* Nonconsumed selectors are canonical encodings, not additional GPU reads. */
    if (!pixel_event_ref_structure(ops, with_initial) ||
        (with_initial && !pixel_event_ref_movs(&ops[3], 5, 0, 0, 3)) ||
        !pixel_event_ref_movs(&ops[base + 3u], 3, with_initial ? 1u : 0u, with_initial ? 1u : 0u,
                              3) ||
        !pixel_event_ref_movs(&ops[base + 14u], 5, with_initial ? 2u : 1u, 24,
                              with_initial ? 2u : 3u))
        return 0;
    for (uint32_t flags = 0; flags < 8; ++flags) {
        uint32_t ir1 = flags << 1;
        for (uint32_t sample = 0; sample < 36; ++sample) {
            uint32_t ir0 = sample < 32    ? 1u << sample
                           : sample == 32 ? 0u
                           : sample == 33 ? UINT32_MAX
                           : sample == 34 ? 0xaaaaaaaau
                                          : 0x55555555u;
            if (!pixel_event_ref_observe(data, ops, code_count, with_initial, ir0, ir1, state,
                                         initial, pbe) ||
                !pixel_event_ref_observe(data, ops, code_count, with_initial, ir0,
                                         ir1 | 0xfffffff1u, state, initial, pbe))
                return 0;
        }
    }
    return 1;
}

static inline int sgx535_pixel_event_reference_packet(const uint8_t *actual,
                                                      const uint8_t *expected, uint32_t byte_count,
                                                      uint32_t pds_offset, uint32_t use_offset,
                                                      int with_initial, uint32_t state,
                                                      uint32_t initial, uint32_t pbe)
{
    uint32_t pds_bytes = with_initial ? 148u : 128u;
    uint32_t use_bytes = with_initial ? 32u : 16u;
    if (!actual || !expected || (with_initial != 0 && with_initial != 1) ||
        pds_offset > byte_count || pds_bytes > byte_count - pds_offset || use_offset > byte_count ||
        use_bytes > byte_count - use_offset ||
        (pds_offset < use_offset + use_bytes && use_offset < pds_offset + pds_bytes) ||
        !sgx535_pixel_event_reference_check(actual + pds_offset, pds_bytes, actual + use_offset,
                                            use_bytes, with_initial, state, initial, pbe))
        return 0;
    for (uint32_t i = 0; i < byte_count; ++i)
        if (!(i >= pds_offset && i - pds_offset < pds_bytes) &&
            !(i >= use_offset && i - use_offset < use_bytes) && actual[i] != expected[i])
            return 0;
    return 1;
}

#endif
