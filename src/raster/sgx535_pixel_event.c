/* SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
 * SPDX-License-Identifier: MIT
 */

#include "sgx535_pixel_event.h"

enum pds_store { PDS_DS0, PDS_DS1 };
enum pds_input { PDS_IR0, PDS_IR1 };
enum pds_source { PDS_SOURCE_DS0, PDS_SOURCE_INPUT };
enum pds_logic { PDS_OR, PDS_AND };
enum pds_output { PDS_DOUTD = 3, PDS_DOUTU = 5 };
enum pds_lane { PDS_DS0_LOW, PDS_DS0_HIGH, PDS_DS1_LOW, PDS_DS1_HIGH };

struct pds_writer {
    struct sgx535_pixel_event_program *program;
    uint32_t code_count;
    uint32_t code_limit;
};

enum use_bank { USE_TEMP, USE_PRIMARY };

struct use_register {
    enum use_bank bank;
    uint32_t number;
};

enum use_pixel_form { USE_PIXEL_FIRST, USE_PIXEL_FINAL };

struct use_pixel_message {
    enum use_pixel_form form;
    struct use_register source[3];
    uint32_t freep;
    uint32_t end;
    uint32_t increment;
    uint32_t sideband;
};

static int pds_append(struct pds_writer *writer, uint32_t instruction)
{
    if (writer->code_count >= writer->code_limit || writer->code_count >= 21)
        return -1;
    writer->program->pds_words[16 + writer->code_count++] = instruction;
    return 0;
}

/* This owner uses the non-extended SGX535 grammar and unconditional ALU work. */
static int pds_logic(struct pds_writer *writer, enum pds_logic operation, enum pds_source source,
                     uint32_t source1, uint32_t source2, enum pds_store store, uint32_t destination)
{
    if ((operation != PDS_OR && operation != PDS_AND) ||
        (source != PDS_SOURCE_DS0 && source != PDS_SOURCE_INPUT) ||
        (store != PDS_DS0 && store != PDS_DS1) || source1 > 63 || source2 > 63 ||
        destination > 63 || (source == PDS_SOURCE_INPUT && source1 > PDS_IR1))
        return -1;

    return pds_append(writer, (3U << 30) | ((uint32_t)operation << 27) | (7U << 24) |
                                  ((uint32_t)source << 23) | (source1 << 17) | (source2 << 10) |
                                  ((uint32_t)store << 6) | destination);
}

static int pds_shift_input(struct pds_writer *writer, enum pds_input input, uint32_t shift,
                           uint32_t destination)
{
    if ((input != PDS_IR0 && input != PDS_IR1) || shift > 31 || destination > 63)
        return -1;
    return pds_append(writer, (3U << 30) | (6U << 27) | (7U << 24) | (2U << 22) |
                                  ((uint32_t)input << 16) | (shift << 8) | destination);
}

static int pds_copy_ds1(struct pds_writer *writer, uint32_t source, uint32_t destination)
{
    if (source > 63 || destination > 63)
        return -1;
    return pds_append(writer, (2U << 27) | (7U << 24) | (1U << 15) | (source << 9) | (1U << 7) |
                                  (destination << 1));
}

static int pds_test_zero(struct pds_writer *writer, uint32_t source)
{
    if (source > 63)
        return -1;
    /* Source1 is DS0; predicate P0 and the unused source2 fields are zero. */
    return pds_append(writer, (2U << 30) | (7U << 24) | (source << 17));
}

static int pds_branch_p0(struct pds_writer *writer, uint32_t code_destination)
{
    if (code_destination >= writer->code_limit)
        return -1;
    /* The branch field counts absolute code DWORDs, excluding the data area. */
    return pds_append(writer, (2U << 30) | (2U << 27) | code_destination);
}

static int pds_halt(struct pds_writer *writer)
{
    return pds_append(writer, (2U << 30) | (5U << 27) | (7U << 24));
}

static int pds_output(struct pds_writer *writer, enum pds_output output, uint32_t ds0_pair,
                      uint32_t ds1_pair, enum pds_lane lane0, enum pds_lane lane1,
                      enum pds_lane lane2, enum pds_lane lane3)
{
    if ((output != PDS_DOUTD && output != PDS_DOUTU) || ds0_pair > 31 || ds1_pair > 31 ||
        (unsigned)lane0 > PDS_DS1_HIGH || (unsigned)lane1 > PDS_DS1_HIGH ||
        (unsigned)lane2 > PDS_DS1_HIGH || (unsigned)lane3 > PDS_DS1_HIGH)
        return -1;
    /* Pair indices and DWORD lane selectors have deliberately different units. */
    return pds_append(writer, (7U << 24) | (ds0_pair << 18) | (ds1_pair << 13) |
                                  ((uint32_t)lane0 << 11) | ((uint32_t)lane1 << 9) |
                                  ((uint32_t)lane2 << 7) | ((uint32_t)lane3 << 5) |
                                  (uint32_t)output);
}

static int pds_payload_field(struct pds_writer *writer, enum pds_input input, uint32_t shift,
                             uint32_t temporary, uint32_t mask)
{
    if (pds_shift_input(writer, input, shift, temporary) ||
        pds_logic(writer, PDS_AND, PDS_SOURCE_DS0, temporary, mask, PDS_DS0, temporary) ||
        pds_logic(writer, PDS_OR, PDS_SOURCE_DS0, temporary, 48, PDS_DS1, 48))
        return -1;
    return 0;
}

static int use_pixelbe(const struct use_pixel_message *message, uint32_t words[2])
{
    uint32_t low, high;

    if ((message->form != USE_PIXEL_FIRST && message->form != USE_PIXEL_FINAL) ||
        message->source[0].bank != USE_PRIMARY || message->source[1].bank != USE_PRIMARY ||
        message->source[2].bank != USE_PRIMARY || message->source[0].number > 127 ||
        message->source[1].number > 127 || message->source[2].number > 127 || message->freep > 1 ||
        message->end > 1 || message->increment > 3 || message->sideband > 0x3fff)
        return -1;
    if (message->form == USE_PIXEL_FIRST &&
        (message->sideband != (1U << 4) || message->source[2].number != 2 || message->end))
        return -1;
    /* The second message's selected 0x200 sideband bit has no wider admitted meaning. */
    if (message->form == USE_PIXEL_FINAL && (message->sideband != 0x204 || !message->end))
        return -1;

    /* S1 and S2 banks: primary attributes (the PDS put the PBE words there). */
    low = (2U << 30) | (2U << 28) | ((message->sideband & 0x3f) << 22) | (message->freep << 21) |
          (message->source[0].number << 14) | (message->source[1].number << 7) |
          message->source[2].number;
    high = (31U << 27) | (3U << 24) | (((message->sideband >> 12) & 3U) << 22) | (2U << 20) |
           (message->end << 18) | (((message->sideband >> 6) & 0x3f) << 3) | (1U << 2) |
           message->increment;
    words[0] = low;
    words[1] = high;
    return 0;
}

static void use_ptoff_end(uint32_t words[2])
{
    /* SPECIAL / VISTEST / PTCTRL, operand-free PTOFF, followed by task END. */
    words[0] = 0;
    words[1] = (31U << 27) | (3U << 20) | (1U << 18) | (1U << 15);
}

int sgx535_pixel_event_build(enum sgx535_pixel_event_variant variant,
                             struct sgx535_pixel_event_program *output)
{
    struct sgx535_pixel_event_program result = {0};
    const struct use_pixel_message first = {
        .form = USE_PIXEL_FIRST,
        /* src2, the first PBE word, comes from pa2: the task declares no
         * temporaries, so r0 would be another task's. */
        .source = {{USE_PRIMARY, 2}, {USE_PRIMARY, 3}, {USE_PRIMARY, 2}},
        .freep = 1,
        .end = 0,
        .increment = 0,
        .sideband = 1U << 4,
    };
    const struct use_pixel_message final = {
        .form = USE_PIXEL_FINAL,
        .source = {{USE_PRIMARY, 0}, {USE_PRIMARY, 1}, {USE_PRIMARY, 2}},
        .freep = 1,
        .end = 1,
        .increment = 0,
        .sideband = 0x204,
    };
    struct pds_writer writer = {.program = &result};
    uint32_t state, pbe, event_mask, payload_base, pbe_use;
    int initial;

    if (!output ||
        (variant != SGX535_PIXEL_EVENT_PBE_ONLY && variant != SGX535_PIXEL_EVENT_PTOFF_PBE))
        return -1;
    initial = variant == SGX535_PIXEL_EVENT_PTOFF_PBE;
    writer.code_limit = initial ? 21 : 16;
    state = initial ? 2 : 0;
    pbe = initial ? 4 : 2;
    event_mask = initial ? 2 : 0;
    payload_base = event_mask + 1;
    pbe_use = initial ? 4 : 0;

    /* Resource-relative relocations later supply addresses in the zero link fields. */
    /* The selected descriptor's high bit remains profile-specific. */
    result.pds_words[state + 1] = 0x80000003;
    result.pds_words[pbe] = 1U << 21;
    result.pds_words[8 + event_mask] = 1U << 1;
    result.pds_words[8 + payload_base] = 1U << 28;
    result.pds_words[8 + payload_base + 1] = 0x0007f800;
    /* Keep the selected gap at input bit8 / output bit19 in the Y transform. */
    result.pds_words[8 + payload_base + 2] = 0x07f00000;
    result.pds_words[8 + payload_base + 3] = 1U << 27;

    if (initial) {
        result.pds_words[8] = 1U << 3;
        if (pds_logic(&writer, PDS_AND, PDS_SOURCE_INPUT, PDS_IR1, 0, PDS_DS0, 48) ||
            pds_test_zero(&writer, 48) || pds_branch_p0(&writer, 5) ||
            pds_output(&writer, PDS_DOUTU, 0, 0, PDS_DS0_LOW, PDS_DS0_HIGH, PDS_DS1_HIGH,
                       PDS_DS1_HIGH) ||
            pds_halt(&writer))
            return -1;
        use_ptoff_end(result.use_words);
    }

    if (pds_logic(&writer, PDS_AND, PDS_SOURCE_INPUT, PDS_IR1, event_mask, PDS_DS0, 48) ||
        pds_test_zero(&writer, 48) || pds_branch_p0(&writer, writer.code_limit - 1) ||
        pds_output(&writer, PDS_DOUTD, state / 2, event_mask / 2, PDS_DS0_LOW, PDS_DS0_HIGH,
                   PDS_DS1_LOW, PDS_DS1_HIGH) ||
        pds_copy_ds1(&writer, payload_base, 48) ||
        pds_payload_field(&writer, PDS_IR0, 11, 49, payload_base + 1) ||
        pds_payload_field(&writer, PDS_IR0, 11, 50, payload_base + 2) ||
        pds_payload_field(&writer, PDS_IR1, 25, 51, payload_base + 3) ||
        pds_output(&writer, PDS_DOUTU, pbe / 2, 24, PDS_DS0_LOW, PDS_DS0_HIGH, PDS_DS1_LOW,
                   initial ? PDS_DS1_LOW : PDS_DS1_HIGH) ||
        pds_halt(&writer) || writer.code_count != writer.code_limit ||
        use_pixelbe(&first, result.use_words + pbe_use) ||
        use_pixelbe(&final, result.use_words + pbe_use + 2))
        return -1;

    result.pds_data_words = 16;
    result.pds_word_count = 16 + writer.code_count;
    result.use_word_count = initial ? 8 : 4;
    *output = result;
    return 0;
}
