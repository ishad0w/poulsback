// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Canonical unpublished seed composition. Every source program comes from a
 * checked typed producer; no accepted packet or launch is edited in place. */
#include "sgx535_vertex_use.h"
#include "sgx535_vertex_pds.h"
static void color0_mask_section(struct sgx535_task_packet *p, uint32_t id, uint32_t kind,
                                uint32_t role, uint32_t target, uint32_t alignment,
                                const void *data, uint32_t bytes)
{
    uint32_t start = (p->byte_count + 15u) & ~15u;
    p->sections[id] =
        (struct sgx535_task_packet_section){kind, start, bytes, role, target, alignment};
    memcpy(p->bytes + start, data, bytes);
    p->byte_count = start + bytes;
}
static void color0_mask_reloc(struct sgx535_task_packet *p, uint32_t kind, uint32_t s,
                              uint32_t offset, uint32_t role, uint32_t target, uint32_t bytes,
                              uint32_t alignment, uint32_t mask, uint32_t right, uint32_t left)
{
    p->relocations[p->relocation_count++] = (struct sgx535_task_packet_reloc){
        kind, s, offset, role, target, bytes, alignment, mask, right, left};
}
static void color0_mask_address(struct sgx535_task_packet *p, uint32_t s, uint32_t offset,
                                uint32_t role, uint32_t target, uint32_t bytes, uint32_t alignment,
                                uint32_t mask, uint32_t right, uint32_t left)
{
    color0_mask_reloc(p, SGX535_PACKET_DRI_OFFSET, s, offset, role, target, bytes, alignment, mask,
                      right, left);
}
static void color0_mask_link(struct sgx535_task_packet *p, uint32_t s, uint32_t offset,
                             uint32_t role, uint32_t target, uint32_t bytes)
{
    color0_mask_reloc(p, SGX535_PACKET_USE_BANK, s, offset, role, target, bytes, 16, 0xf, 0, 0);
    color0_mask_reloc(p, SGX535_PACKET_USE_ENTRY, s, offset, role, target, bytes, 16, 0xf0, 15, 4);
    color0_mask_reloc(p, SGX535_PACKET_USE_ENTRY, s, offset, role, target, bytes, 16, 0x7ff00, 4,
                      8);
}
static enum sgx535_task_status color0_mask_seed(struct sgx535_normalized_color0_workspace *w,
                                                int no_input)
{
    enum { V, I, VU, FU, SU, EU, VP, FP, SP, STP, TP, EP, MTE, TS, PBE, TA };
    const struct sgx535_task_packet *fg = &w->varying.result, *mask = &w->varying.base;
    struct sgx535_task_packet *p = &w->seed;
    struct sgx535_vertex_use_projection use;
    struct sgx535_vertex_pds_program pds;
    uint8_t aggregate[160] = {0};
    static const uint32_t kinds[16] = {1, 2, 3, 4, 5, 6, 7, 8, 14, 9, 15, 10, 11, 16, 12, 13};
    static const uint32_t mask_bytes[16] = {160, 24,  16, 8,   48, 32, 64, 56,
                                            4,   124, 60, 148, 92, 8,  16, 68};
    static const uint32_t indices[5] = {0x00010000, 0x00030002, 0x00010002, 0x00010000, 2};
    static const uint32_t stream[17] = {0x40000000, 0x0c02a203, 0x81400006, 0, 0, 0, 0x02000103,
                                        0x40000000, 0x0c02a203, 0x81400003, 0, 0, 0, 0x04000203,
                                        0x60000000, 0x0c02a201, 0xc0000000};
    uint32_t j, foreground_bytes = no_input ? 48u : 96u;
    uint32_t vertex_bytes = foreground_bytes + 64u;
    uint32_t fragment_bytes = no_input ? 8u : 64u, pds_bytes = no_input ? 56u : 60u;
    if (fg->section_count != 16 || mask->section_count != 16 ||
        fg->width != (no_input ? 64u : 16u) || fg->height != (no_input ? 64u : 16u) ||
        mask->width != 16 || mask->height != 16)
        return SGX535_TASK_UNSUPPORTED;
    for (j = 0; j < 16; j++)
        if (fg->sections[j].kind != kinds[j] || mask->sections[j].kind != kinds[j] ||
            mask->sections[j].byte_count != mask_bytes[j] ||
            fg->sections[j].byte_offset > fg->byte_count ||
            fg->sections[j].byte_count > fg->byte_count - fg->sections[j].byte_offset ||
            mask->sections[j].byte_offset > mask->byte_count ||
            mask->sections[j].byte_count > mask->byte_count - mask->sections[j].byte_offset)
            return SGX535_TASK_UNSUPPORTED;
    if (fg->sections[V].byte_count != foreground_bytes ||
        fg->sections[FU].byte_count != fragment_bytes || fg->sections[FP].byte_count != pds_bytes ||
        fg->sections[MTE].byte_count != 40)
        return SGX535_TASK_UNSUPPORTED;
    *p = no_input ? *mask : *fg;
    p->version = SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION;
    p->profile = SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE;
    p->byte_count = p->relocation_count = 0;
    memset(p->bytes, 0, sizeof(p->bytes));
    memset(p->relocations, 0, sizeof(p->relocations));
    memcpy(aggregate, fg->bytes + fg->sections[V].byte_offset, foreground_bytes);
    memcpy(aggregate + foreground_bytes, mask->bytes + mask->sections[V].byte_offset + 96, 64);
    color0_mask_section(p, V, kinds[V], SGX535_TASK_VERTEX_DATA, 0, 16, aggregate, vertex_bytes);
    for (j = 0; j < 5; j++)
        put(aggregate + j * 4u, indices[j]);
    color0_mask_section(p, I, kinds[I], SGX535_TASK_VERTEX_DATA, vertex_bytes, 2, aggregate, 20);
    if (sgx535_vertex_use_project(no_input ? 4u : 8u, &use) != SGX535_VERTEX_USE_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (j = 0; j < 4; j++)
        put(aggregate + j * 4u, use.words[j]);
    if (sgx535_vertex_use_project(4, &use) != SGX535_VERTEX_USE_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (j = 0; j < 4; j++)
        put(aggregate + 16u + j * 4u, use.words[j]);
    color0_mask_section(p, VU, kinds[VU], SGX535_TASK_USE_VERTEX, 0, 32, aggregate, 32);
#define COPY(id, source, target, alignment)                                                        \
    color0_mask_section(p, id, kinds[id], source->sections[id].role, target, alignment,            \
                        source->bytes + source->sections[id].byte_offset,                          \
                        source->sections[id].byte_count)
    COPY(FU, fg, 0, 32);
    COPY(SU, mask, 32, 32);
    COPY(EU, mask, 32, 32);
    if (sgx535_vertex_pds_project(no_input ? 4u : 8u, no_input ? 4u : 8u, &pds) !=
        SGX535_VERTEX_DMA_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (j = 0; j < 16; j++)
        put(aggregate + j * 4u, pds.words[j]);
    if (sgx535_vertex_pds_project(4, 4, &pds) != SGX535_VERTEX_DMA_OK)
        return SGX535_TASK_UNSUPPORTED;
    for (j = 0; j < 16; j++)
        put(aggregate + 64u + j * 4u, pds.words[j]);
    color0_mask_section(p, VP, kinds[VP], SGX535_TASK_PDS, 0, 32, aggregate, 128);
    COPY(FP, fg, 128, 32);
    COPY(SP, mask, 192, 32);
    COPY(STP, mask, 224, 32);
    COPY(TP, mask, 352, 32);
    COPY(EP, mask, 416, 32);
    memset(aggregate, 0, 92);
    memcpy(aggregate, fg->bytes + fg->sections[MTE].byte_offset, 40);
    memcpy(aggregate + 48, mask->bytes + mask->sections[MTE].byte_offset + 48, 44);
    color0_mask_section(p, MTE, kinds[MTE], SGX535_TASK_PDS, 576, 16, aggregate, 92);
    COPY(TS, mask, 208, 16);
    COPY(PBE, mask, 672, 16);
#undef COPY
    for (j = 0; j < 17; j++)
        put(aggregate + j * 4u, stream[j]);
    color0_mask_section(p, TA, kinds[TA], SGX535_TASK_PDS, 688, 16, aggregate, 68);
#define A(s, o, r, t, n, a, m, x, y) color0_mask_address(p, s, o, r, t, n, a, m, x, y)
#define U(s, o, r, t, n) color0_mask_link(p, s, o, r, t, n)
    A(VP, 0, 3, 0, foreground_bytes, 16, 0xffffffffu, 0, 0);
    U(VP, 16, 1, 0, 16);
    A(VP, 64, 3, foreground_bytes, 64, 16, 0xffffffffu, 0, 0);
    U(VP, 80, 1, 16, 16);
    U(FP, 0, 2, 0, fragment_bytes);
    A(STP, 0, 0, 624, 44, 16, 0xffffffffu, 0, 0);
    U(STP, 8, 1, 32, 16);
    A(STP, 64, 0, 576, 40, 16, 0xffffffffu, 0, 0);
    U(STP, 72, 1, 48, 16);
    A(TP, 0, 0, 208, 8, 16, 0xffffffffu, 0, 0);
    U(TP, 8, 1, 64, 16);
    A(MTE, 8, 0, 192, 4, 32, 0xffffff, 4, 0);
    A(MTE, 16, 0, 128, pds_bytes, 32, 0xffffff, 4, 0);
    A(EP, 8, 0, 672, 16, 16, 0xffffffffu, 0, 0);
    U(EP, 0, 2, 32, 8);
    U(EP, 16, 2, 48, 16);
    A(PBE, 4, 5, 0, 2048, 4, 0xfffffffcu, 2, 2);
    A(TA, 0, 0, 224, 60, 32, 0xfffffff, 4, 0);
    A(TA, 12, 3, vertex_bytes, 12, 2, 0xffffffffu, 0, 0);
    A(TA, 20, 0, 64, 64, 32, 0xfffffff, 4, 0);
    A(TA, 28, 0, 288, 60, 32, 0xfffffff, 4, 0);
    A(TA, 40, 3, vertex_bytes + 12u, 6, 2, 0xffffffffu, 0, 0);
    A(TA, 48, 0, 0, 64, 32, 0xfffffff, 4, 0);
    A(TA, 56, 0, 352, 60, 32, 0xfffffff, 4, 0);
#undef U
#undef A
    return p->relocation_count == SGX535_TASK_PACKET_RELOCS ? SGX535_TASK_BLOCKED
                                                            : SGX535_TASK_UNSUPPORTED;
}
static enum sgx535_task_status
color0_mask_seed_request(const struct sgx535_normalized_color0_tile16_request *r,
                         struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_varying_request varying = {0};
    struct sgx535_task_pair_request mask = {0};
    enum sgx535_task_status status;
    uint32_t i;
    if (r->version != SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION ||
        r->profile != SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE || r->reserved[0] ||
        r->reserved[1] || r->reserved[2])
        return SGX535_TASK_UNSUPPORTED;
    varying.version = SGX535_VARYING_PACKET_VERSION;
    varying.profile = SGX535_VARYING_W1_COLOR0;
    varying.core_id = r->core_id;
    varying.revision = r->revision;
    varying.interpolation = r->interpolation;
    memcpy(varying.clip_position, r->clip_position, sizeof(varying.clip_position));
    memcpy(varying.rgba, r->rgba, sizeof(varying.rgba));
    status = sgx535_varying_clip16_seed(&varying, &w->seed, &w->varying);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    mask.version = SGX535_TASK_PAIR_PACKET_VERSION;
    mask.profile = SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT;
    mask.core_id = r->core_id;
    mask.revision = r->revision;
    mask.width = mask.height = 16;
    for (i = 0; i < 2; i++)
        memcpy(mask.clip_position[i], r->clip_position, sizeof(r->clip_position));
    status = sgx535_task_pair_mask_set_packet_build_words(&mask, &w->varying.base, &w->seed,
                                                          &w->packet_words);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = color0_mask_seed(w, 0);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return SGX535_TASK_BLOCKED;
}
/* The template depends on the core alone, and window meshes ask for it per
 * draw group and frame (0.2 ms each on the N270): the last one is kept. Its
 * callers read only OUT, so a kept result leaves the workspace as it was. */
#ifdef __KERNEL__
#include <linux/mutex.h>
static DEFINE_MUTEX(mask_template_lock);
#define mask_template_take() mutex_lock(&mask_template_lock)
#define mask_template_drop() mutex_unlock(&mask_template_lock)
#else
#include <pthread.h>
static pthread_mutex_t mask_template_lock = PTHREAD_MUTEX_INITIALIZER;
#define mask_template_take() pthread_mutex_lock(&mask_template_lock)
#define mask_template_drop() pthread_mutex_unlock(&mask_template_lock)
#endif
static struct {
    bool kept;
    uint32_t core, revision;
    struct sgx535_task_packet seed;
} mask_template;
/* Private complete unpublished template: programs and DMA/state/stream grammar
 * remain owned by their existing checked producers. External mesh composition
 * supplies its own independent geometry and compiles its one immutable IR. */
enum sgx535_task_status
sgx535_normalized_color0_mask_mesh_template(uint32_t core, uint32_t revision,
                                            struct sgx535_task_packet *out,
                                            struct sgx535_normalized_color0_workspace *w)
{
    static const uint32_t clip[3][4] = {{0xbf400000u, 0x3f400000u, 0, 0x3f800000u},
                                        {0x3f400000u, 0x3f400000u, 0, 0x3f800000u},
                                        {0x3f200000u, 0xbf000000u, 0, 0x3f800000u}};
    struct sgx535_normalized_color0_tile16_request r = {0};
    enum sgx535_task_status status;
    bool kept;
    mask_template_take();
    kept = mask_template.kept && mask_template.core == core && mask_template.revision == revision;
    if (kept)
        *out = mask_template.seed;
    mask_template_drop();
    if (kept)
        return SGX535_TASK_BLOCKED;
    r.version = SGX535_NORMALIZED_COLOR0_MASK_TILE16_VERSION;
    r.profile = SGX535_NORMALIZED_COLOR0_MASK_TILE16_PROFILE;
    r.core_id = core;
    r.revision = revision;
    r.interpolation = SGX535_VARYING_SMOOTH;
    memcpy(r.clip_position, clip, sizeof(clip));
    status = color0_mask_seed_request(&r, w);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->seed;
        mask_template_take();
        mask_template.seed = *out;
        mask_template.core = core;
        mask_template.revision = revision;
        mask_template.kept = true;
        mask_template_drop();
    }
    return status;
}
/* The no-input seed retains the checked packed-constant producer's four-word
 * vertices, empty iterator PDS and output state. No COLOR0 record is introduced. */
enum sgx535_task_status
sgx535_normalized_no_input_mask_mesh_template(const struct sgx535_task_packet *foreground,
                                              struct sgx535_task_packet *out,
                                              struct sgx535_normalized_color0_workspace *w)
{
    static const uint32_t clip[3][4] = {{0xbf400000u, 0x3f400000u, 0, 0x3f800000u},
                                        {0x3f400000u, 0x3f400000u, 0, 0x3f800000u},
                                        {0x3f200000u, 0xbf000000u, 0, 0x3f800000u}};
    struct sgx535_task_pair_request mask = {0};
    enum sgx535_task_status status;
    if (!foreground || !out || !w || foreground->version != SGX535_TASK_PACKET_DRI_VERSION ||
        foreground->profile != SGX535_TASK_W1_PACKED_CONSTANT)
        return SGX535_TASK_UNSUPPORTED;
    w->varying.result = *foreground;
    mask.version = SGX535_TASK_PAIR_PACKET_VERSION;
    mask.profile = SGX535_TASK_PAIR_MASK_SET_W1_PACKED_CONSTANT;
    mask.core_id = foreground->core_id;
    mask.revision = foreground->revision;
    mask.width = mask.height = 16;
    for (uint32_t i = 0; i < 2; i++)
        memcpy(mask.clip_position[i], clip, sizeof(clip));
    status = sgx535_task_pair_mask_set_packet_build_words(&mask, &w->varying.base, &w->seed,
                                                          &w->packet_words);
    if (status == SGX535_TASK_BLOCKED)
        status = color0_mask_seed(w, 1);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->seed;
    return status;
}
static enum sgx535_task_status
color0_mask_construct(const struct sgx535_normalized_color0_tile16_request *r,
                      struct sgx535_normalized_color0_workspace *w)
{
    enum sgx535_task_status status = color0_mask_seed_request(r, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return compose_program(r->fragment, r->fragment_count, r->version, r->profile, 1, w);
}
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_task_build(
    const struct sgx535_normalized_color0_tile16_request *r,
    struct sgx535_normalized_color0_task *out, struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_tile16_request frozen;
    enum sgx535_task_status status;
    if (!tile16_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_mask_construct(&frozen, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_task_validate(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *out, struct sgx535_normalized_color0_workspace *w)
{
    struct sgx535_normalized_color0_tile16_request frozen;
    enum sgx535_task_status status;
    if (!tile16_arguments(r, out, w))
        return SGX535_TASK_INVALID;
    frozen = *r;
    status = color0_mask_construct(&frozen, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_UNSUPPORTED : status;
}
