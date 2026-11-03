// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/native-2d.md

/* Replays every black-box vector of tests/vectors/2d-encoder against the 2D
 * encoder and its compatibility interface: status, count and words, with
 * canaries around the output and copies of the inputs (the README's output
 * contract), then the RGB565 colour word of all 65,536 pixels. The optional
 * argument is the vector directory; the default is relative to the checkout. */
#include "sgx535_2d_encoder.h"
#include "sgx535_2d_encoder_v1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_WORDS 24u
#define WRITTEN_CANARY ((size_t)0x7e57u)
#define LINE_BYTES 4096u
#define REPORTED_FAILURES 40u

static unsigned long vectors, failed_vectors, reported;

static uint32_t canary(size_t i)
{
    return 0xc5a50000u | (uint32_t)i;
}

/* One failure per vector is counted; the first few are printed. */
static int fail(const char *id, const char *what)
{
    if (reported++ < REPORTED_FAILURES)
        fprintf(stderr, "2d vector %s: %s\n", id, what);
    return 0;
}

/* Minimal JSON reading for the vector lines: objects, arrays, strings without
 * escapes, integers, true/false/null. A parse error marks the vector bad. */
static const char *space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static const char *skip(const char *p)
{
    int depth = 0;

    p = space(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            ;
        return *p ? p + 1 : p;
    }
    if (*p != '{' && *p != '[') {
        while (*p && *p != ',' && *p != '}' && *p != ']')
            p++;
        return p;
    }
    while (*p) {
        if (*p == '"') {
            p = skip(p);
            continue;
        }
        if (*p == '{' || *p == '[')
            depth++;
        else if (*p == '}' || *p == ']')
            depth--;
        p++;
        if (!depth)
            break;
    }
    return p;
}

/* The value of key in the object at p, or NULL. */
static const char *member(const char *p, const char *key)
{
    size_t length = strlen(key);

    if (!p || *(p = space(p)) != '{')
        return NULL;
    p = space(p + 1);
    while (*p == '"') {
        const char *name = p + 1, *end = skip(p), *value = space(end);
        if (*value != ':')
            return NULL;
        value = space(value + 1);
        if ((size_t)(end - 1 - name) == length && !strncmp(name, key, length))
            return value;
        p = space(skip(value));
        if (*p != ',')
            break;
        p = space(p + 1);
    }
    return NULL;
}

static int null(const char *v)
{
    return v && !strncmp(v, "null", 4);
}

static int64_t number(const char *v, int *bad)
{
    char *end;
    long long x;

    if (!v) {
        *bad = 1;
        return 0;
    }
    x = strtoll(v, &end, 10);
    if (end == v)
        *bad = 1;
    return x;
}

static uint64_t hex(const char *v, int *bad)
{
    char *end;
    unsigned long long x;

    if (!v || strncmp(v, "\"0x", 3)) {
        *bad = 1;
        return 0;
    }
    x = strtoull(v + 3, &end, 16);
    if (end == v + 3 || *end != '"')
        *bad = 1;
    return x;
}

static int boolean(const char *v, int *bad)
{
    if (v && !strncmp(v, "true", 4))
        return 1;
    if (!v || strncmp(v, "false", 5))
        *bad = 1;
    return 0;
}

static void string(const char *v, char *buffer, size_t size, int *bad)
{
    size_t n = 0;

    buffer[0] = 0;
    if (!v || *v != '"') {
        *bad = 1;
        return;
    }
    for (v++; *v && *v != '"'; v++)
        if (n + 1 < size)
            buffer[n++] = *v;
    buffer[n] = 0;
}

static int32_t s32(int64_t x, int *bad)
{
    if (x < INT32_MIN || x > INT32_MAX)
        *bad = 1;
    return (int32_t)x;
}

static uint32_t u32(int64_t x, int *bad)
{
    if (x < 0 || x > (int64_t)UINT32_MAX)
        *bad = 1;
    return (uint32_t)x;
}

/* Returns 0 for null (a null pointer argument). */
static int raster_surface(const char *v, struct sgx535_2d_raster_surface *s, int *bad)
{
    if (null(v))
        return 0;
    memset(s, 0, sizeof(*s));
    s->storage_id = hex(member(v, "storage"), bad);
    s->offset_bytes = hex(member(v, "offset"), bad);
    s->allocation_bytes = hex(member(v, "allocation"), bad);
    s->width = u32(number(member(v, "width"), bad), bad);
    s->height = u32(number(member(v, "height"), bad), bad);
    s->stride_bytes = u32(number(member(v, "stride"), bad), bad);
    /* Refusal cases store negative and unknown values. */
    s->format = (enum sgx535_2d_raster_format)(uint32_t)s32(number(member(v, "format"), bad), bad);
    return 1;
}

static int raster_rect(const char *v, struct sgx535_2d_raster_rect *r, int *bad)
{
    if (null(v))
        return 0;
    memset(r, 0, sizeof(*r));
    r->x = s32(number(member(v, "x"), bad), bad);
    r->y = s32(number(member(v, "y"), bad), bad);
    r->width = u32(number(member(v, "width"), bad), bad);
    r->height = u32(number(member(v, "height"), bad), bad);
    return 1;
}

static int v1_surface(const char *v, struct sgx535_2d_surface *s, int *bad)
{
    if (null(v))
        return 0;
    memset(s, 0, sizeof(*s));
    s->offset_bytes = hex(member(v, "offset"), bad);
    s->allocation_bytes = hex(member(v, "allocation"), bad);
    s->width = u32(number(member(v, "width"), bad), bad);
    s->height = u32(number(member(v, "height"), bad), bad);
    s->stride_bytes = u32(number(member(v, "stride"), bad), bad);
    s->format = (enum sgx535_2d_format)(uint32_t)s32(number(member(v, "format"), bad), bad);
    return 1;
}

static int v1_rect(const char *v, struct sgx535_2d_rect *r, int *bad)
{
    if (null(v))
        return 0;
    memset(r, 0, sizeof(*r));
    r->x = u32(number(member(v, "x"), bad), bad);
    r->y = u32(number(member(v, "y"), bad), bad);
    r->width = u32(number(member(v, "width"), bad), bad);
    r->height = u32(number(member(v, "height"), bad), bad);
    return 1;
}

/* The call's common arguments, and its canary-filled outputs. */
struct call {
    char id[128];
    size_t capacity;
    int out_null, written_null, bad;
    uint32_t out[OUT_WORDS];
    size_t written;
};

static void prepare(const char *line, struct call *c)
{
    size_t i;

    memset(c, 0, sizeof(*c));
    string(member(line, "id"), c->id, sizeof(c->id), &c->bad);
    c->capacity = u32(number(member(line, "capacity"), &c->bad), &c->bad);
    if (c->capacity > OUT_WORDS)
        c->bad = 1;
    c->out_null = boolean(member(line, "out_null"), &c->bad);
    c->written_null = boolean(member(line, "written_null"), &c->bad);
    for (i = 0; i < OUT_WORDS; i++)
        c->out[i] = canary(i);
    c->written = WRITTEN_CANARY;
}

/* Status by name, count and words against the call's result. */
static int compare(const char *line, const struct call *c, const char *const names[7],
                   unsigned status)
{
    char expected[32];
    uint32_t words[OUT_WORDS];
    const char *v;
    size_t count = 0, written = 0, i;
    unsigned want = 7;
    int bad = c->bad;

    string(member(line, "status"), expected, sizeof(expected), &bad);
    for (i = 0; i < 7; i++)
        if (!strcmp(expected, names[i]))
            want = (unsigned)i;
    v = member(line, "written");
    if (!null(v))
        written = (size_t)u32(number(v, &bad), &bad);
    v = member(line, "words");
    if (!v || *v != '[')
        bad = 1;
    else
        for (v = space(v + 1); *v == '"' && count < OUT_WORDS; v = space(v)) {
            words[count++] = (uint32_t)hex(v, &bad);
            v = space(skip(v));
            if (*v == ',')
                v++;
        }
    if (bad || want == 7 || (v && *v != ']'))
        return fail(c->id, "unreadable vector");
    if (status != want)
        return fail(c->id, "status");
    if (status) {
        if (count || !null(member(line, "written")))
            return fail(c->id, "an error vector with words");
        if (!c->written_null && c->written != WRITTEN_CANARY)
            return fail(c->id, "an error changed the count");
        written = 0;
    } else {
        if (count != written || written > c->capacity || c->written_null || c->written != written)
            return fail(c->id, "count");
        for (i = 0; i < written; i++)
            if (c->out[i] != words[i])
                return fail(c->id, "words");
    }
    for (i = written; i < OUT_WORDS; i++)
        if (c->out[i] != canary(i))
            return fail(c->id, "a word beyond the count changed");
    return 1;
}

static const char *const raster_names[7] = {"OK",           "INVALID_ARGUMENT", "UNSUPPORTED",
                                            "OUT_OF_RANGE", "BAD_ALIGNMENT",    "UNPROVEN_ALIAS",
                                            "NO_SPACE"};
static const char *const v1_names[7] = {"OK",           "INVALID_ARGUMENT", "UNSUPPORTED",
                                        "OUT_OF_RANGE", "BAD_ALIGNMENT",    "OVERLAP",
                                        "NO_SPACE"};

static int raster_fill(const char *line)
{
    struct sgx535_2d_raster_surface dst, dst_copy;
    struct sgx535_2d_raster_rect rect, rect_copy, clip, clip_copy;
    struct call c;
    enum sgx535_2d_raster_rop rop;
    enum sgx535_2d_raster_status status;
    uint32_t pixel;
    int has_dst, has_rect, has_clip;

    prepare(line, &c);
    has_dst = raster_surface(member(line, "dst"), &dst, &c.bad);
    has_rect = raster_rect(member(line, "rect"), &rect, &c.bad);
    has_clip = raster_rect(member(line, "clip"), &clip, &c.bad);
    pixel = (uint32_t)hex(member(line, "pixel"), &c.bad);
    rop = (enum sgx535_2d_raster_rop)(uint32_t)s32(number(member(line, "rop"), &c.bad), &c.bad);
    memcpy(&dst_copy, &dst, sizeof(dst));
    memcpy(&rect_copy, &rect, sizeof(rect));
    memcpy(&clip_copy, &clip, sizeof(clip));
    status = sgx535_2d_raster_fill(has_dst ? &dst : NULL, has_rect ? &rect : NULL,
                                   has_clip ? &clip : NULL, pixel, rop, c.out_null ? NULL : c.out,
                                   c.capacity, c.written_null ? NULL : &c.written);
    if ((has_dst && memcmp(&dst, &dst_copy, sizeof(dst))) ||
        (has_rect && memcmp(&rect, &rect_copy, sizeof(rect))) ||
        (has_clip && memcmp(&clip, &clip_copy, sizeof(clip))))
        return fail(c.id, "an input changed");
    return compare(line, &c, raster_names, (unsigned)status);
}

static int raster_copy(const char *line)
{
    struct sgx535_2d_raster_surface src, src_copy, dst, dst_copy;
    struct sgx535_2d_raster_rect src_rect, src_rect_copy, dst_rect, dst_rect_copy, clip, clip_copy;
    struct call c;
    enum sgx535_2d_raster_rop rop;
    enum sgx535_2d_raster_status status;
    int has_src, has_dst, has_src_rect, has_dst_rect, has_clip;

    prepare(line, &c);
    has_src = raster_surface(member(line, "src"), &src, &c.bad);
    has_dst = raster_surface(member(line, "dst"), &dst, &c.bad);
    has_src_rect = raster_rect(member(line, "src_rect"), &src_rect, &c.bad);
    has_dst_rect = raster_rect(member(line, "dst_rect"), &dst_rect, &c.bad);
    has_clip = raster_rect(member(line, "clip"), &clip, &c.bad);
    rop = (enum sgx535_2d_raster_rop)(uint32_t)s32(number(member(line, "rop"), &c.bad), &c.bad);
    memcpy(&src_copy, &src, sizeof(src));
    memcpy(&dst_copy, &dst, sizeof(dst));
    memcpy(&src_rect_copy, &src_rect, sizeof(src_rect));
    memcpy(&dst_rect_copy, &dst_rect, sizeof(dst_rect));
    memcpy(&clip_copy, &clip, sizeof(clip));
    status = sgx535_2d_raster_copy(has_src ? &src : NULL, has_src_rect ? &src_rect : NULL,
                                   has_dst ? &dst : NULL, has_dst_rect ? &dst_rect : NULL,
                                   has_clip ? &clip : NULL, rop, c.out_null ? NULL : c.out,
                                   c.capacity, c.written_null ? NULL : &c.written);
    if ((has_src && memcmp(&src, &src_copy, sizeof(src))) ||
        (has_dst && memcmp(&dst, &dst_copy, sizeof(dst))) ||
        (has_src_rect && memcmp(&src_rect, &src_rect_copy, sizeof(src_rect))) ||
        (has_dst_rect && memcmp(&dst_rect, &dst_rect_copy, sizeof(dst_rect))) ||
        (has_clip && memcmp(&clip, &clip_copy, sizeof(clip))))
        return fail(c.id, "an input changed");
    return compare(line, &c, raster_names, (unsigned)status);
}

static int v1_fill(const char *line)
{
    struct sgx535_2d_surface dst, dst_copy;
    struct sgx535_2d_rect rect, rect_copy;
    struct call c;
    enum sgx535_2d_status status;
    uint32_t pixel;
    int has_dst, has_rect;

    prepare(line, &c);
    has_dst = v1_surface(member(line, "dst"), &dst, &c.bad);
    has_rect = v1_rect(member(line, "rect"), &rect, &c.bad);
    pixel = (uint32_t)hex(member(line, "pixel"), &c.bad);
    memcpy(&dst_copy, &dst, sizeof(dst));
    memcpy(&rect_copy, &rect, sizeof(rect));
    status = sgx535_2d_build_fill(has_dst ? &dst : NULL, has_rect ? &rect : NULL, pixel,
                                  c.out_null ? NULL : c.out, c.capacity,
                                  c.written_null ? NULL : &c.written);
    if ((has_dst && memcmp(&dst, &dst_copy, sizeof(dst))) ||
        (has_rect && memcmp(&rect, &rect_copy, sizeof(rect))))
        return fail(c.id, "an input changed");
    return compare(line, &c, v1_names, (unsigned)status);
}

static int v1_copy(const char *line)
{
    struct sgx535_2d_surface src, src_copy, dst, dst_copy;
    struct sgx535_2d_rect src_rect, src_rect_copy, dst_rect, dst_rect_copy;
    struct call c;
    enum sgx535_2d_status status;
    int has_src, has_dst, has_src_rect, has_dst_rect;

    prepare(line, &c);
    has_src = v1_surface(member(line, "src"), &src, &c.bad);
    has_dst = v1_surface(member(line, "dst"), &dst, &c.bad);
    has_src_rect = v1_rect(member(line, "src_rect"), &src_rect, &c.bad);
    has_dst_rect = v1_rect(member(line, "dst_rect"), &dst_rect, &c.bad);
    memcpy(&src_copy, &src, sizeof(src));
    memcpy(&dst_copy, &dst, sizeof(dst));
    memcpy(&src_rect_copy, &src_rect, sizeof(src_rect));
    memcpy(&dst_rect_copy, &dst_rect, sizeof(dst_rect));
    status = sgx535_2d_build_copy(has_src ? &src : NULL, has_src_rect ? &src_rect : NULL,
                                  has_dst ? &dst : NULL, has_dst_rect ? &dst_rect : NULL,
                                  c.out_null ? NULL : c.out, c.capacity,
                                  c.written_null ? NULL : &c.written);
    if ((has_src && memcmp(&src, &src_copy, sizeof(src))) ||
        (has_dst && memcmp(&dst, &dst_copy, sizeof(dst))) ||
        (has_src_rect && memcmp(&src_rect, &src_rect_copy, sizeof(src_rect))) ||
        (has_dst_rect && memcmp(&dst_rect, &dst_rect_copy, sizeof(dst_rect))))
        return fail(c.id, "an input changed");
    return compare(line, &c, v1_names, (unsigned)status);
}

static int replay(const char *line)
{
    char api[16];
    int bad = 0;

    string(member(line, "api"), api, sizeof(api), &bad);
    if (!strcmp(api, "fill"))
        return raster_fill(line);
    if (!strcmp(api, "copy"))
        return raster_copy(line);
    if (!strcmp(api, "v1_fill"))
        return v1_fill(line);
    if (!strcmp(api, "v1_copy"))
        return v1_copy(line);
    return fail(line, "unknown api");
}

/* Every line of one file; the count pins the recorded set. */
static int replay_file(const char *directory, const char *name, unsigned long expected)
{
    static char line[LINE_BYTES];
    char path[512];
    unsigned long lines = 0;
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", directory, name);
    file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "2d vectors: cannot open %s\n", path);
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        if (!length || line[length - 1] != '\n') {
            fprintf(stderr, "2d vectors: %s line %lu is too long\n", name, lines + 1);
            fclose(file);
            return 0;
        }
        lines++;
        vectors++;
        if (!replay(line))
            failed_vectors++;
    }
    fclose(file);
    if (lines != expected) {
        fprintf(stderr, "2d vectors: %s has %lu lines, expected %lu\n", name, lines, expected);
        return 0;
    }
    return 1;
}

/* Entry p is the colour word of a 1x1 RGB565 fill of pixel p. */
static unsigned long rgb565_colours(const char *directory)
{
    static const struct sgx535_2d_raster_surface surface = {
        .storage_id = 1,
        .offset_bytes = 0,
        .allocation_bytes = 128,
        .width = 1,
        .height = 1,
        .stride_bytes = 128,
        .format = SGX535_2D_RASTER_RGB565,
    };
    static const struct sgx535_2d_raster_rect pixel = {0, 0, 1, 1};
    unsigned char raw[4];
    char path[512];
    unsigned long matched = 0;
    uint32_t p;
    FILE *file;

    snprintf(path, sizeof(path), "%s/rgb565-fill-colour.bin", directory);
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "2d vectors: cannot open %s\n", path);
        return 0;
    }
    for (p = 0; p <= 0xffffu && fread(raw, 1, sizeof(raw), file) == sizeof(raw); p++) {
        uint32_t want = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 |
                        (uint32_t)raw[3] << 24;
        uint32_t out[SGX535_2D_RASTER_FILL_WORDS] = {0};
        size_t written = 0;
        if (sgx535_2d_raster_fill(&surface, &pixel, NULL, p, SGX535_2D_RASTER_COPY, out,
                                  SGX535_2D_RASTER_FILL_WORDS, &written) == SGX535_2D_RASTER_OK &&
            written == SGX535_2D_RASTER_FILL_WORDS && out[4] == want)
            matched++;
        else if (reported++ < REPORTED_FAILURES)
            fprintf(stderr, "2d rgb565 colour of 0x%04x: 0x%08x, expected 0x%08x\n", (unsigned)p,
                    (unsigned)out[4], (unsigned)want);
    }
    if (fgetc(file) != EOF)
        matched = 0;
    fclose(file);
    return matched;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        unsigned long vectors;
    } files[] = {
        {"raster-fill.jsonl", 460},
        {"raster-copy.jsonl", 1013},
        {"raster-precedence.jsonl", 569},
        {"raster-random.jsonl", 2000},
        {"v1.jsonl", 173},
    };
    const char *directory = argc > 1 ? argv[1] : "tests/vectors/2d-encoder";
    unsigned long colours;
    int complete = 1;
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        complete &= replay_file(directory, files[i].name, files[i].vectors);
    colours = rgb565_colours(directory);
    printf("2d encoder vectors: %lu/%lu passed; rgb565 colours: %lu/65536\n",
           vectors - failed_vectors, vectors, colours);
    return complete && vectors == 4215 && !failed_vectors && colours == 65536 ? 0 : 1;
}
