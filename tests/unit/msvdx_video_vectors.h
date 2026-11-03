// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT
// Interface contract: docs/video-protocol.md

/* Test-only reader for tests/vectors/msvdx-video (grammar in its README.md).
 * A vector passes when the actual results, printed in the vector's own key
 * order and number format, equal the text after "=>" exactly. Unknown or
 * missing keys abort; mismatches are counted and the first few printed. */
#ifndef MSVDX_VIDEO_VECTORS_H
#define MSVDX_VIDEO_VECTORS_H
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VECTOR_DIRECTORY "tests/vectors/msvdx-video"
#define VECTOR_MAX_KEYS 64u
#define VECTOR_MAX_BYTES 4096u
/* Unlike VECTOR_REQUIRE(), never compiled out. */
#define VECTOR_REQUIRE(x)                                                                          \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

struct vector_file {
    char name[64];
    char *text;
    size_t at, size;
    unsigned line, vectors, failures;
};
struct vector_line {
    const char *op, *result;
    unsigned inputs;
    const char *key[VECTOR_MAX_KEYS], *value[VECTOR_MAX_KEYS];
};
struct vector_out {
    char text[8192];
    size_t used;
};

static unsigned vector_total, vector_failures;

static inline void vector_open(struct vector_file *f, const char *directory, const char *name)
{
    char path[512];
    FILE *file;
    long size;
    memset(f, 0, sizeof(*f));
    VECTOR_REQUIRE(strlen(name) < sizeof(f->name));
    strcpy(f->name, name);
    VECTOR_REQUIRE(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr,
                "cannot open %s (run from the repository root or pass the vector directory)\n",
                path);
        exit(1);
    }
    VECTOR_REQUIRE(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    VECTOR_REQUIRE(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    f->size = (size_t)size;
    f->text = malloc(f->size + 1u);
    VECTOR_REQUIRE(f->text);
    VECTOR_REQUIRE(fread(f->text, 1, f->size, file) == f->size);
    VECTOR_REQUIRE(fclose(file) == 0);
    f->text[f->size] = '\0';
}

/* Next vector or directive (op starting with '@', no result); 0 at the end. */
static inline int vector_next(struct vector_file *f, struct vector_line *l)
{
    while (f->at < f->size) {
        char *line = f->text + f->at, *end = strchr(line, '\n'), *arrow, *token;
        if (!end)
            end = f->text + f->size;
        f->at = (size_t)(end - f->text) + (end < f->text + f->size);
        *end = '\0';
        ++f->line;
        if (!*line || *line == '#')
            continue;
        memset(l, 0, sizeof(*l));
        arrow = strstr(line, " => ");
        if (arrow) {
            *arrow = '\0';
            l->result = arrow + 4;
        }
        l->op = strtok(line, " ");
        VECTOR_REQUIRE(l->op);
        while ((token = strtok(NULL, " ")) != NULL) {
            char *equals = strchr(token, '=');
            if (!equals) {
                /* Directive argument without a key, e.g. "@packet x:...". */
                VECTOR_REQUIRE(*l->op == '@' && !l->inputs);
                l->key[l->inputs] = "";
                l->value[l->inputs++] = token;
                continue;
            }
            VECTOR_REQUIRE(l->inputs < VECTOR_MAX_KEYS);
            *equals = '\0';
            l->key[l->inputs] = token;
            l->value[l->inputs++] = equals + 1;
        }
        VECTOR_REQUIRE((*l->op == '@') == !l->result);
        return 1;
    }
    return 0;
}

static inline const char *vector_text(const struct vector_line *l, const char *key)
{
    unsigned i;
    for (i = 0; i < l->inputs; ++i)
        if (!strcmp(l->key[i], key))
            return l->value[i];
    fprintf(stderr, "vector input %s missing in %s\n", key, l->op);
    exit(1);
}
static inline int vector_has(const struct vector_line *l, const char *key)
{
    unsigned i;
    for (i = 0; i < l->inputs; ++i)
        if (!strcmp(l->key[i], key))
            return 1;
    return 0;
}
/* "null=<name>": that pointer argument is absent. */
static inline int vector_null(const struct vector_line *l, const char *name)
{
    unsigned i;
    for (i = 0; i < l->inputs; ++i)
        if (!strcmp(l->key[i], "null") && !strcmp(l->value[i], name))
            return 1;
    return 0;
}
static inline uint64_t vector_parse_integer(const char *text)
{
    int hex = text[0] == '0' && text[1] == 'x';
    const char *digits = hex ? text + 2 : text;
    char *end;
    unsigned long long value;
    VECTOR_REQUIRE(*digits);
    value = strtoull(digits, &end, hex ? 16 : 10);
    VECTOR_REQUIRE(!*end && *digits != '-' && *digits != '+');
    return value;
}
static inline uint64_t vector_integer(const struct vector_line *l, const char *key)
{
    return vector_parse_integer(vector_text(l, key));
}
static inline uint32_t vector_u32(const struct vector_line *l, const char *key)
{
    uint64_t value = vector_integer(l, key);
    VECTOR_REQUIRE(value <= UINT32_MAX);
    return (uint32_t)value;
}
static inline uint16_t vector_u16(const struct vector_line *l, const char *key)
{
    uint64_t value = vector_integer(l, key);
    VECTOR_REQUIRE(value <= UINT16_MAX);
    return (uint16_t)value;
}
static inline unsigned vector_hex_digit(char c)
{
    VECTOR_REQUIRE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
}
static inline size_t vector_parse_bytes(const char *text, uint8_t *out, size_t capacity)
{
    size_t n = 0;
    VECTOR_REQUIRE(text[0] == 'x' && text[1] == ':');
    for (text += 2; *text; text += 2) {
        VECTOR_REQUIRE(text[1] && n < capacity);
        out[n++] = (uint8_t)(vector_hex_digit(text[0]) << 4 | vector_hex_digit(text[1]));
    }
    return n;
}
static inline size_t vector_bytes(const struct vector_line *l, const char *key, uint8_t *out,
                                  size_t capacity)
{
    return vector_parse_bytes(vector_text(l, key), out, capacity);
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static inline void emit(struct vector_out *t, const char *format, ...)
{
    va_list arguments;
    int n;
    va_start(arguments, format);
    n = vsnprintf(t->text + t->used, sizeof(t->text) - t->used, format, arguments);
    va_end(arguments);
    VECTOR_REQUIRE(n >= 0 && (size_t)n < sizeof(t->text) - t->used);
    t->used += (size_t)n;
}
static inline void emit_bytes(struct vector_out *t, const char *key, const uint8_t *bytes, size_t n)
{
    size_t i;
    emit(t, " %s=x:", key);
    for (i = 0; i < n; ++i)
        emit(t, "%02x", bytes[i]);
}
static inline void vector_reset(struct vector_out *t)
{
    t->used = 0;
    t->text[0] = '\0';
}

static inline void vector_expect(struct vector_file *f, const struct vector_line *l,
                                 const struct vector_out *got)
{
    if (!strcmp(l->result, got->text))
        return;
    if (++f->failures <= 5)
        fprintf(stderr, "%s:%u: %s\n  expected: %s\n  actual:   %s\n", f->name, f->line, l->op,
                l->result, got->text);
}
/* An extra property of the current vector (aliasing, untouched bytes). */
static inline void vector_check(struct vector_file *f, int condition, const char *what)
{
    if (condition)
        return;
    if (++f->failures <= 5)
        fprintf(stderr, "%s:%u: %s\n", f->name, f->line, what);
}
static inline void vector_close(struct vector_file *f, unsigned expected)
{
    if (f->vectors != expected)
        vector_check(f, 0, "vector count differs from the README table");
    printf("%-22s %5u vectors, %u failures\n", f->name, f->vectors, f->failures);
    vector_total += f->vectors;
    vector_failures += f->failures;
    free(f->text);
}
#endif
