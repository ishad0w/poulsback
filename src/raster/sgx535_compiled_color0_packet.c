// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_compiled_color0_packet.h"
#include "sgx535_packet_internal.h"
#include "sgx535_fragment_linkage.h"
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int span(const void *p, unsigned long bytes)
{
    return p && bytes <= ~(unsigned long)0 - (unsigned long)p;
}
static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long av = (unsigned long)a, bv = (unsigned long)b;
    return av <= bv ? bv - av < an : av - bv < bn;
}
static int arguments(const struct sgx535_compiled_color0_request *r,
                     const struct sgx535_task_packet *p,
                     const struct sgx535_compiled_color0_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !w || r->fragment_count < 5 || r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r, sizeof(*r)) && span(p, sizeof(*p)) && span(w, sizeof(*w)) &&
           span(r->fragment, bytes) && !overlap(r, sizeof(*r), p, sizeof(*p)) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(p, sizeof(*p), w, sizeof(*w)) &&
           !overlap(r->fragment, bytes, p, sizeof(*p)) &&
           !overlap(r->fragment, bytes, w, sizeof(*w));
}
/* One little-endian word: a single store (the byte loop was one store per
 * byte). Every target, the i686 kernel and userspace, is little-endian. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian words");
static void put(uint8_t *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}
static enum sgx535_task_status construct_program(const struct sgx535_fragment_instruction *fragment,
                                                 uint32_t fragment_count, uint32_t version,
                                                 uint32_t profile,
                                                 struct sgx535_compiled_color0_workspace *w)
{
    struct sgx535_fragment_linkage linkage = {0};
    struct sgx535_task_packet *p = &w->result;
    enum sgx535_nir_status status;
    uint32_t i, j, code_bytes, fragment_section = SGX535_TASK_PACKET_SECTIONS;
    /* The seed checks replicated-flat equality before this compiler-local
     * smooth binding. The complete program and physical iterator stay generic;
     * no caller-selected native flat control or provoking vertex is needed. */
    linkage.count = 1;
    linkage.varying[0] =
        (struct sgx535_fragment_varying){SGX535_FRAGMENT_COLOR, 0, 0, 4, SGX535_FRAGMENT_SMOOTH};
    status = sgx535_fragment_compile_dri_color0(fragment, fragment_count, &linkage,
                                                SGX535_FRAGMENT_U8_WORD_PROJECTION, &w->program);
    if (status != SGX535_NIR_OK)
        return status == SGX535_NIR_ERR_NOMEM ? SGX535_TASK_NOMEM : SGX535_TASK_UNSUPPORTED;
    if (w->program.primary_register_count != 4 || w->program.secondary_register_count ||
        w->program.encoding.temp_register_count > SGX535_COMPILED_COLOR0_MAX_TEMPS ||
        w->program.encoding.word_count > SGX535_COMPILED_COLOR0_MAX_WORDS ||
        !w->program.encoding.word_count || (w->program.encoding.word_count & 1u))
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < 4; i++)
        if (w->program.uniform_words_used[i])
            return SGX535_TASK_UNSUPPORTED;
    *p = w->seed;
    p->version = version;
    p->profile = profile;
    p->missing_contracts = SGX535_COMPILED_COLOR0_MISSING_ALL;
    p->byte_count = 0;
    memset(p->bytes, 0, sizeof(p->bytes));
    code_bytes = w->program.encoding.word_count * 4u;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *old = &w->seed.sections[i];
        struct sgx535_task_packet_section *s = &p->sections[i];
        uint32_t offset = (p->byte_count + 15u) & ~15u;
        uint32_t bytes = old->kind == SGX535_PACKET_FRAGMENT_USE ? code_bytes : old->byte_count;
        if (offset > SGX535_TASK_PACKET_BYTES || bytes > SGX535_TASK_PACKET_BYTES - offset)
            return SGX535_TASK_UNSUPPORTED;
        s->byte_offset = offset;
        s->byte_count = bytes;
        if (s->kind == SGX535_PACKET_FRAGMENT_USE) {
            if (fragment_section != SGX535_TASK_PACKET_SECTIONS || s->resource_byte_offset)
                return SGX535_TASK_UNSUPPORTED;
            fragment_section = i;
            for (j = 0; j < w->program.encoding.word_count; j++)
                put(p->bytes + offset + j * 4u, w->program.encoding.words[j]);
        } else
            memcpy(p->bytes + offset, w->seed.bytes + old->byte_offset, bytes);
        p->byte_count = offset + bytes;
    }
    if (fragment_section == SGX535_TASK_PACKET_SECTIONS)
        return SGX535_TASK_UNSUPPORTED;
    for (i = 0; i < p->section_count; i++) {
        const struct sgx535_task_packet_section *s = &p->sections[i];
        if (i != fragment_section && s->role == SGX535_TASK_USE_FRAGMENT &&
            s->resource_byte_offset < code_bytes)
            return SGX535_TASK_UNSUPPORTED;
    }
    for (i = 0; i < p->relocation_count; i++) {
        struct sgx535_task_packet_reloc *rel = &p->relocations[i];
        if (p->sections[rel->section].kind == SGX535_PACKET_FRAGMENT_PDS &&
            rel->role == SGX535_TASK_USE_FRAGMENT && !rel->resource_byte_offset)
            rel->span_bytes = code_bytes;
    }
    return SGX535_TASK_BLOCKED;
}
static enum sgx535_task_status construct(const struct sgx535_compiled_color0_request *r,
                                         struct sgx535_compiled_color0_workspace *w)
{
    struct sgx535_varying_request seed = {0};
    if (r->version != SGX535_COMPILED_COLOR0_PACKET_VERSION ||
        r->profile != SGX535_COMPILED_COLOR0_PROFILE ||
        (r->interpolation != SGX535_VARYING_SMOOTH &&
         r->interpolation != SGX535_VARYING_FLAT_REPLICATED) ||
        r->reserved[0] || r->reserved[1])
        return SGX535_TASK_UNSUPPORTED;
    seed.version = SGX535_VARYING_PACKET_VERSION;
    seed.profile = SGX535_VARYING_W1_COLOR0;
    seed.core_id = r->core_id;
    seed.revision = r->revision;
    seed.interpolation = r->interpolation;
    memcpy(seed.clip_position, r->clip_position, sizeof(seed.clip_position));
    memcpy(seed.rgba, r->rgba, sizeof(seed.rgba));
    if (sgx535_varying_packet_build(&seed, &w->seed, &w->varying) != SGX535_TASK_BLOCKED)
        return SGX535_TASK_UNSUPPORTED;
    return construct_program(r->fragment, r->fragment_count, SGX535_COMPILED_COLOR0_PACKET_VERSION,
                             SGX535_COMPILED_COLOR0_PROFILE, w);
}
static int window_arguments(const struct sgx535_window_color0_request *r,
                            const struct sgx535_task_packet *p,
                            const struct sgx535_compiled_color0_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !w || r->fragment_count < 5 || r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return span(r, sizeof(*r)) && span(p, sizeof(*p)) && span(w, sizeof(*w)) &&
           span(r->fragment, bytes) && !overlap(r, sizeof(*r), p, sizeof(*p)) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(p, sizeof(*p), w, sizeof(*w)) &&
           !overlap(r->fragment, bytes, p, sizeof(*p)) &&
           !overlap(r->fragment, bytes, w, sizeof(*w));
}
static enum sgx535_task_status window_construct(const struct sgx535_window_color0_request *r,
                                                struct sgx535_compiled_color0_workspace *w)
{
    enum sgx535_task_status status;
    if (r->version != SGX535_WINDOW_COLOR0_PACKET_VERSION ||
        r->profile != SGX535_WINDOW_COLOR0_PROFILE || r->reserved[0] || r->reserved[1] ||
        r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    status =
        sgx535_varying_window_seed(r->core_id, r->revision, &r->vertices, &w->seed, &w->varying);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return construct_program(r->fragment, r->fragment_count, SGX535_WINDOW_COLOR0_PACKET_VERSION,
                             SGX535_WINDOW_COLOR0_PROFILE, w);
}
enum sgx535_task_status
sgx535_window_color0_packet_build(const struct sgx535_window_color0_request *r,
                                  struct sgx535_task_packet *out,
                                  struct sgx535_compiled_color0_workspace *w)
{
    struct sgx535_window_color0_request frozen;
    enum sgx535_task_status status;
    if (!window_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status
sgx535_window_color0_packet_validate(const struct sgx535_window_color0_request *r,
                                     const struct sgx535_task_packet *p,
                                     struct sgx535_compiled_color0_workspace *w)
{
    struct sgx535_window_color0_request frozen;
    enum sgx535_task_status status;
    if (!window_arguments(r, p, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = window_construct(&frozen, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(p, &w->result, sizeof(*p)) ? SGX535_TASK_UNSUPPORTED : status;
}

enum sgx535_task_status
sgx535_compiled_color0_packet_build(const struct sgx535_compiled_color0_request *request,
                                    struct sgx535_task_packet *output,
                                    struct sgx535_compiled_color0_workspace *workspace)
{
    struct sgx535_compiled_color0_request frozen;
    enum sgx535_task_status status;
    if (!arguments(request, output, workspace))
        return SGX535_TASK_INVALID;
    frozen = *request;
    status = construct(&frozen, workspace);
    if (status == SGX535_TASK_BLOCKED)
        *output = workspace->result;
    return status;
}
enum sgx535_task_status
sgx535_compiled_color0_packet_validate(const struct sgx535_compiled_color0_request *request,
                                       const struct sgx535_task_packet *packet,
                                       struct sgx535_compiled_color0_workspace *workspace)
{
    struct sgx535_compiled_color0_request frozen;
    enum sgx535_task_status status;
    if (!arguments(request, packet, workspace))
        return SGX535_TASK_INVALID;
    frozen = *request;
    status = construct(&frozen, workspace);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(packet, &workspace->result, sizeof(*packet)) ? SGX535_TASK_UNSUPPORTED : status;
}
