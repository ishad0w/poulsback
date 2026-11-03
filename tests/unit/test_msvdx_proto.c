// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

/* Every envelope, stream and ring vector of tests/vectors/msvdx-video against
 * msvdx_proto. Byte inputs sit at odd addresses; refusals must leave outputs
 * untouched; encode, decode and the ring plan are also run with overlapping
 * input and output. Optional argument: the vector directory. */
#include "msvdx_proto.h"
#include "msvdx_video_vectors.h"

static const char *const status_names[] = {"ok",        "argument", "length", "unsupported",
                                           "direction", "range",    "limit"};
static const char *const ring_names[] = {"ok", "argument", "index", "packet", "full"};
/* "dir=bad" stands for any value other than the two directions. */
static const enum msvdx_direction bad_directions[] = {
    (enum msvdx_direction)0, (enum msvdx_direction)3, (enum msvdx_direction)0x7f};

static const char *status_name(enum msvdx_status s)
{
    VECTOR_REQUIRE((unsigned)s < sizeof(status_names) / sizeof(status_names[0]));
    return status_names[s];
}
static const char *ring_name(enum msvdx_ring_status s)
{
    VECTOR_REQUIRE((unsigned)s < sizeof(ring_names) / sizeof(ring_names[0]));
    return ring_names[s];
}
static unsigned directions(const struct vector_line *l, enum msvdx_direction out[3])
{
    const char *d = vector_text(l, "dir");
    if (!strcmp(d, "host")) {
        out[0] = MSVDX_HOST_TO_MTX;
        return 1;
    }
    if (!strcmp(d, "mtx")) {
        out[0] = MSVDX_MTX_TO_HOST;
        return 1;
    }
    VECTOR_REQUIRE(!strcmp(d, "bad"));
    memcpy(out, bad_directions, sizeof(bad_directions));
    return 3;
}
static uint8_t vector_id(const struct vector_line *l)
{
    uint32_t id = vector_u32(l, "id");
    VECTOR_REQUIRE(id <= UINT8_MAX);
    return (uint8_t)id;
}

/* Fields outside the ID's layout hold garbage the codec must ignore. */
static void message_fields(const struct vector_line *l, struct msvdx_message *m)
{
    memset(m, 0x5a, sizeof(*m));
    m->message_id = vector_id(l);
    if (m->message_id == MSVDX_RENDER_ID) {
        struct msvdx_render_fields *r = &m->payload.render;
        r->buffer_size = vector_u16(l, "bsize");
        r->buffer_size_high = vector_u16(l, "bsize_hi");
        r->mmu_ptd = vector_u32(l, "ptd");
        r->lldma_address = vector_u32(l, "lldma");
        r->context = vector_u32(l, "ctx");
        r->fence_value = vector_u32(l, "fence");
        r->operating_mode = vector_u32(l, "mode");
        r->first_mb = vector_u16(l, "mb_first");
        r->last_mb = vector_u16(l, "mb_last");
        r->flags = vector_u32(l, "flags");
    } else if (m->message_id == MSVDX_COMPLETED_ID) {
        m->payload.completed.header = vector_u16(l, "hdr");
        m->payload.completed.fence_value = vector_u32(l, "fence");
        m->payload.completed.flags = vector_u32(l, "flags");
    } else if (m->message_id == MSVDX_DEBLOCK_REQUIRED_ID) {
        m->payload.deblock.header = vector_u16(l, "hdr");
        m->payload.deblock.context = vector_u32(l, "ctx");
    } else if (m->message_id == MSVDX_FAILED_ID || m->message_id == MSVDX_PANIC_ID) {
        m->payload.failed.flags = vector_u16(l, "flags16");
        m->payload.failed.fence_value = vector_u32(l, "fence");
        m->payload.failed.irq_status = vector_u32(l, "irq");
    }
}
static void emit_message(struct vector_out *o, const struct msvdx_message *m)
{
    emit(o, " id=0x%02x", (unsigned)m->message_id);
    if (m->message_id == MSVDX_RENDER_ID) {
        const struct msvdx_render_fields *r = &m->payload.render;
        emit(o,
             " bsize=0x%04x bsize_hi=0x%04x ptd=0x%08x lldma=0x%08x ctx=0x%08x fence=0x%08x"
             " mode=0x%08x mb_first=0x%04x mb_last=0x%04x flags=0x%08x",
             (unsigned)r->buffer_size, (unsigned)r->buffer_size_high, (unsigned)r->mmu_ptd,
             (unsigned)r->lldma_address, (unsigned)r->context, (unsigned)r->fence_value,
             (unsigned)r->operating_mode, (unsigned)r->first_mb, (unsigned)r->last_mb,
             (unsigned)r->flags);
    } else if (m->message_id == MSVDX_COMPLETED_ID) {
        emit(o, " hdr=0x%04x fence=0x%08x flags=0x%08x", (unsigned)m->payload.completed.header,
             (unsigned)m->payload.completed.fence_value, (unsigned)m->payload.completed.flags);
    } else if (m->message_id == MSVDX_DEBLOCK_REQUIRED_ID) {
        emit(o, " hdr=0x%04x ctx=0x%08x", (unsigned)m->payload.deblock.header,
             (unsigned)m->payload.deblock.context);
    } else {
        emit(o, " flags16=0x%04x fence=0x%08x irq=0x%08x", (unsigned)m->payload.failed.flags,
             (unsigned)m->payload.failed.fence_value, (unsigned)m->payload.failed.irq_status);
    }
}
static void emit_plan(struct vector_out *o, const struct msvdx_ring_plan *p)
{
    unsigned i;
    emit(o, " pad=%u count=%u final=%u words=", (unsigned)p->padding_words, (unsigned)p->word_count,
         (unsigned)p->final_write);
    for (i = 0; i < MSVDX_RING_PLAN_WORDS; ++i)
        emit(o, "%s0x%08x", i ? "," : "", (unsigned)p->words[i]);
}

static void size_vector(struct vector_file *f, const struct vector_line *l)
{
    struct vector_out o;
    vector_reset(&o);
    emit(&o, "bytes=%zu", msvdx_proto_envelope_size(vector_id(l)));
    vector_expect(f, l, &o);
}

static void encode_vector(struct vector_file *f, const struct vector_line *l)
{
    enum msvdx_direction d[3];
    unsigned i, n = directions(l, d);
    size_t capacity = (size_t)vector_integer(l, "cap");
    struct msvdx_message m;
    VECTOR_REQUIRE(capacity <= 64);
    message_fields(l, &m);
    for (i = 0; i < n; ++i) {
        uint8_t storage[1 + 64 + 16], before[sizeof(storage)], *out = storage + 1;
        union {
            struct msvdx_message m;
            uint8_t b[64];
        } alias;
        struct vector_out o;
        enum msvdx_status s;
        size_t size;
        memset(storage, 0xa5, sizeof(storage));
        memcpy(before, storage, sizeof(storage));
        s = msvdx_message_encode(vector_null(l, "message") ? NULL : &m, d[i],
                                 vector_null(l, "output") ? NULL : out, capacity);
        size = s == MSVDX_OK ? msvdx_proto_envelope_size(m.message_id) : 0;
        vector_reset(&o);
        emit(&o, "status=%s", status_name(s));
        if (s == MSVDX_OK)
            emit_bytes(&o, "out", out, size);
        vector_expect(f, l, &o);
        vector_check(f,
                     storage[0] == before[0] &&
                         !memcmp(out + size, before + 1 + size, sizeof(storage) - 1 - size),
                     "encode wrote past the envelope or on a refusal");
        if (s != MSVDX_OK)
            continue;
        /* Output overlapping the message object, at an odd address. */
        memset(&alias, 0, sizeof(alias));
        alias.m = m;
        s = msvdx_message_encode(&alias.m, d[i], alias.b + 1,
                                 capacity < sizeof(alias.b) - 1 ? capacity : sizeof(alias.b) - 1);
        vector_check(f, s == MSVDX_OK && !memcmp(alias.b + 1, out, size),
                     "encode differs when the output overlaps the message");
    }
}

static void decode_vector(struct vector_file *f, const struct vector_line *l)
{
    enum msvdx_direction d[3];
    unsigned i, n = directions(l, d);
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    for (i = 0; i < n; ++i) {
        struct msvdx_message out, before, zeroed;
        union {
            struct msvdx_message m;
            uint8_t b[64];
        } alias;
        struct vector_out o;
        enum msvdx_status s;
        memset(&out, 0xa5, sizeof(out));
        memcpy(&before, &out, sizeof(out));
        s = msvdx_message_decode(vector_null(l, "input") ? NULL : storage + 1, bytes, d[i],
                                 vector_null(l, "output") ? NULL : &out);
        vector_reset(&o);
        emit(&o, "status=%s", status_name(s));
        if (s == MSVDX_OK)
            emit_message(&o, &out);
        vector_expect(f, l, &o);
        if (s != MSVDX_OK) {
            vector_check(f, !memcmp(&out, &before, sizeof(out)),
                         "decode refusal changed the record");
            continue;
        }
        /* The whole record is replaced, whatever it held before. */
        memset(&zeroed, 0, sizeof(zeroed));
        s = msvdx_message_decode(storage + 1, bytes, d[i], &zeroed);
        vector_check(f, s == MSVDX_OK && !memcmp(&zeroed, &out, sizeof(out)),
                     "decode kept bytes of the previous record");
        /* Input overlapping the output record, at an odd address. */
        VECTOR_REQUIRE(bytes < sizeof(alias.b));
        memset(&alias, 0x3c, sizeof(alias));
        memcpy(alias.b + 1, storage + 1, bytes);
        s = msvdx_message_decode(alias.b + 1, bytes, d[i], &alias.m);
        vector_check(f, s == MSVDX_OK && !memcmp(&alias.m, &out, sizeof(out)),
                     "decode differs when the input overlaps the record");
    }
}

static void validate_vector(struct vector_file *f, const struct vector_line *l)
{
    enum msvdx_direction d[3];
    unsigned i, n = directions(l, d);
    static uint8_t storage[1 + VECTOR_MAX_BYTES];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    for (i = 0; i < n; ++i) {
        struct vector_out o;
        struct msvdx_proto_scan r =
            msvdx_proto_scan(vector_null(l, "input") ? NULL : storage + 1, bytes, d[i],
                             (size_t)vector_integer(l, "max_bytes"), vector_u32(l, "max_msgs"));
        vector_reset(&o);
        emit(&o, "status=%s messages=%u renders=%u", status_name(r.status), (unsigned)r.messages,
             (unsigned)r.renders);
        vector_expect(f, l, &o);
    }
}

static void patch_vector(struct vector_file *f, const struct vector_line *l)
{
    static uint8_t storage[1 + VECTOR_MAX_BYTES + 16], before[sizeof(storage)];
    size_t bytes = vector_bytes(l, "in", storage + 1, VECTOR_MAX_BYTES);
    uint32_t invalidate = vector_u32(l, "invalidate");
    struct msvdx_proto_scan r;
    struct vector_out o;
    VECTOR_REQUIRE(invalidate <= 1);
    storage[0] = 0x77;
    memset(storage + 1 + bytes, 0x77, sizeof(storage) - 1 - bytes);
    memcpy(before, storage, sizeof(storage));
    r = msvdx_proto_stamp_render(vector_null(l, "input") ? NULL : storage + 1, bytes,
                                 (size_t)vector_integer(l, "max_bytes"), vector_u32(l, "max_msgs"),
                                 vector_u32(l, "ptd"), vector_u32(l, "fence"), invalidate != 0);
    vector_reset(&o);
    emit(&o, "status=%s messages=%u renders=%u", status_name(r.status), (unsigned)r.messages,
         (unsigned)r.renders);
    emit_bytes(&o, "out", storage + 1, bytes);
    vector_expect(f, l, &o);
    vector_check(f,
                 storage[0] == before[0] &&
                     !memcmp(storage + 1 + bytes, before + 1 + bytes, sizeof(storage) - 1 - bytes),
                 "stamp wrote outside the span");
    vector_check(f, r.status == MSVDX_OK || !memcmp(storage, before, sizeof(storage)),
                 "stamp refusal changed the span");
}

static void grid_vector(struct vector_file *f, const struct vector_line *l, const uint8_t *packet,
                        size_t bytes)
{
    uint32_t r = vector_u32(l, "r"), w = vector_u32(l, "w");
    struct msvdx_ring_plan p;
    struct vector_out o;
    enum msvdx_ring_status s = msvdx_ring_plan_render(packet, bytes, r, w, &p);
    vector_reset(&o);
    emit(&o, "status=%s", ring_name(s));
    if (s == MSVDX_RING_OK)
        emit(&o, " pad=%u count=%u final=%u", (unsigned)p.padding_words, (unsigned)p.word_count,
             (unsigned)p.final_write);
    vector_expect(f, l, &o);
    vector_check(f, s != MSVDX_RING_OK || (p.read_snapshot == r && p.initial_write == w),
                 "plan snapshot differs from its inputs");
}

static void plan_vector(struct vector_file *f, const struct vector_line *l)
{
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    uint32_t r = vector_u32(l, "r"), w = vector_u32(l, "w");
    struct msvdx_ring_plan p, before;
    union {
        struct msvdx_ring_plan p;
        uint8_t b[sizeof(struct msvdx_ring_plan)];
    } alias;
    struct vector_out o;
    enum msvdx_ring_status s;
    memset(&p, 0xa5, sizeof(p));
    memcpy(&before, &p, sizeof(p));
    s = msvdx_ring_plan_render(vector_null(l, "packet") ? NULL : storage + 1, bytes, r, w,
                               vector_null(l, "output") ? NULL : &p);
    vector_reset(&o);
    emit(&o, "status=%s", ring_name(s));
    if (s == MSVDX_RING_OK)
        emit_plan(&o, &p);
    vector_expect(f, l, &o);
    if (s != MSVDX_RING_OK) {
        vector_check(f, !memcmp(&p, &before, sizeof(p)), "ring refusal changed the plan");
        return;
    }
    vector_check(f, p.read_snapshot == r && p.initial_write == w,
                 "plan snapshot differs from its inputs");
    /* Packet inside the plan object, at an odd address. */
    VECTOR_REQUIRE(bytes < sizeof(alias.b));
    memset(&alias, 0x3c, sizeof(alias));
    memcpy(alias.b + 1, storage + 1, bytes);
    s = msvdx_ring_plan_render(alias.b + 1, bytes, r, w, &alias.p);
    vector_check(f, s == MSVDX_RING_OK && !memcmp(&alias.p, &p, sizeof(p)),
                 "plan differs when the packet overlaps it");
}

struct ring_state {
    char sequence[64];
    uint32_t next, words[MSVDX_HOST_RING_WORDS];
};
/* Applies each accepted plan to a model ring that starts as 0xeeeeeeee. */
static void step_vector(struct vector_file *f, const struct vector_line *l, struct ring_state *ring)
{
    const char *sequence = vector_text(l, "seq");
    uint8_t storage[1 + 64];
    size_t bytes = vector_bytes(l, "in", storage + 1, sizeof(storage) - 1);
    uint32_t i, w = vector_u32(l, "w");
    struct msvdx_ring_plan p;
    struct vector_out o;
    enum msvdx_ring_status s;
    if (strcmp(ring->sequence, sequence)) {
        VECTOR_REQUIRE(strlen(sequence) < sizeof(ring->sequence));
        strcpy(ring->sequence, sequence);
        ring->next = 0;
        for (i = 0; i < MSVDX_HOST_RING_WORDS; ++i)
            ring->words[i] = UINT32_C(0xeeeeeeee);
    }
    VECTOR_REQUIRE(vector_u32(l, "n") == ring->next++);
    s = msvdx_ring_plan_render(storage + 1, bytes, vector_u32(l, "r"), w, &p);
    vector_reset(&o);
    emit(&o, "status=%s", ring_name(s));
    if (s == MSVDX_RING_OK) {
        emit_plan(&o, &p);
        for (i = 0; i < p.word_count; ++i)
            ring->words[(p.initial_write + i) % MSVDX_HOST_RING_WORDS] = p.words[i];
    }
    emit(&o, " ring=");
    for (i = 0; i < MSVDX_HOST_RING_WORDS; ++i)
        emit(&o, "%s%08x", i ? "," : "", (unsigned)ring->words[i]);
    vector_expect(f, l, &o);
}

static void run(const char *directory, const char *name, unsigned expected)
{
    struct vector_file f;
    struct vector_line l;
    uint8_t packet[1 + 64];
    size_t packet_bytes = 0;
    static struct ring_state ring;
    memset(&ring, 0, sizeof(ring));
    vector_open(&f, directory, name);
    while (vector_next(&f, &l)) {
        if (!strcmp(l.op, "@packet")) {
            VECTOR_REQUIRE(l.inputs == 1);
            packet_bytes = vector_parse_bytes(l.value[0], packet + 1, sizeof(packet) - 1);
            continue;
        }
        ++f.vectors;
        if (!strcmp(l.op, "size"))
            size_vector(&f, &l);
        else if (!strcmp(l.op, "encode"))
            encode_vector(&f, &l);
        else if (!strcmp(l.op, "decode"))
            decode_vector(&f, &l);
        else if (!strcmp(l.op, "validate"))
            validate_vector(&f, &l);
        else if (!strcmp(l.op, "patch"))
            patch_vector(&f, &l);
        else if (!strcmp(l.op, "grid"))
            grid_vector(&f, &l, packet + 1, packet_bytes);
        else if (!strcmp(l.op, "plan"))
            plan_vector(&f, &l);
        else if (!strcmp(l.op, "step"))
            step_vector(&f, &l, &ring);
        else
            vector_check(&f, 0, "unknown operation");
    }
    vector_close(&f, expected);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : VECTOR_DIRECTORY;
    run(directory, "message-size.txt", 256);
    run(directory, "message-encode.txt", 727);
    run(directory, "message-decode.txt", 1013);
    run(directory, "stream-validate.txt", 116);
    run(directory, "stream-patch.txt", 63);
    run(directory, "ring-grid.txt", 10000);
    run(directory, "ring-plan.txt", 512);
    run(directory, "ring-sequence.txt", 92);
    if (vector_failures) {
        printf("FAIL msvdx_proto: %u of %u vectors or properties failed\n", vector_failures,
               vector_total);
        return 1;
    }
    printf("PASS msvdx_proto: %u vectors (envelopes, streams, rings); CPU only, no device\n",
           vector_total);
    return 0;
}
