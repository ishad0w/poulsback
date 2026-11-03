// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The frame writer's GPU side (poulsback_frame.h): program places in the
 * code chunks, the stream's PDS programs and SECONDARY blocks, backend
 * programs' pixel state and the GPU draws. */
#include "poulsback_frame_private.h"
#include "poulsback_refuse.h"
#include "sgx535_pds_use_link.h"
#include "sgx535_pixel_pds.h"
#include "sgx535_use_code_bank.h"
#include "sgx535_vertex.h"
#include <errno.h>
#include <string.h>

static int frame_bo(struct poulsback_frame_writer *w, uint32_t handle)
{
    if (!handle)
        return POULSBACK_REFUSED(-EINVAL);
    if (handle == w->pds.handle || handle == w->vertices.handle || handle == w->indices.handle)
        return 0;
    for (uint32_t i = 0; i < w->extra_count; i++)
        if (w->extra[i] == handle)
            return 0;
    if (w->extra_count == POULSBACK_FRAME_EXTRA_BOS)
        return -ENOSPC;
    w->extra[w->extra_count++] = handle;
    return 0;
}
/* Program ID's DOUTU0 address fields, placing it first if needed. */
static int program_place(struct poulsback_frame_writer *w, uint64_t id, const uint32_t *words,
                         uint32_t instructions, uint32_t data_master, uint32_t *link)
{
    struct program_place *p = NULL;
    struct sgx535_use_code_bank_input code;
    struct sgx535_pds_use_link_projection l;
    const struct poulsback_frame_bo *bo;
    for (uint32_t i = 0; !p && i < w->places; i++)
        if (w->place[i].id == id)
            p = &w->place[i];
    if (!p) {
        uint32_t bytes = instructions * 8u, at = align(w->program_used, 16);
        if (!instructions || bytes > PROGRAM_CHUNK)
            return POULSBACK_REFUSED(-E2BIG);
        if (w->places == PROGRAMS)
            return -ENOSPC;
        if (!w->program_chunks || at + bytes > PROGRAM_CHUNK) {
            uint32_t next = w->program_chunks ? w->program_current + 1u : 0;
            if (next == PROGRAM_CHUNKS)
                return -ENOSPC;
            if (next == w->program_chunks) {
                int ret = w->ops.alloc(w->ops.data, POULSBACK_BO_CLASS_CODE, PROGRAM_CHUNK,
                                       &w->program[next]);
                if (ret)
                    return ret;
                w->program_chunks++;
            }
            w->program_current = next;
            at = 0;
        }
        memcpy(w->program[w->program_current].map + at, words, bytes);
        p = &w->place[w->places++];
        *p = (struct program_place){id, w->program_current, at};
        w->program_used = at + bytes;
    }
    bo = &w->program[p->chunk];
    code = (struct sgx535_use_code_bank_input){(uint32_t)bo->gpu_va, (uint32_t)bo->size,
                                               (uint32_t)(bo->gpu_va + p->offset),
                                               code_bank(bo->gpu_va, data_master), data_master};
    if (sgx535_pds_use_link_project(&code, &l) != SGX535_PDS_USE_LINK_OK)
        return POULSBACK_REFUSED(-EINVAL);
    *link = l.address_fields;
    return 0;
}
/* The chunks emptied once the GPU is done with them. */
static int programs_recycle(struct poulsback_frame_writer *w)
{
    int ret = w->ops.idle(w->ops.data);
    if (!ret)
        w->places = w->program_current = w->program_used = 0;
    return ret;
}
/* At a frame's first draw: room for each of its draws to place new vertex and
 * pixel programs (relinked variants would fill the places mid-frame), else
 * the chunks emptied first. */
static int programs_room(struct poulsback_frame_writer *w)
{
    bool low = w->places > PROGRAMS - 2u * POULSBACK_FRAME_MAX_DRAWS ||
               (w->program_current + 1u == PROGRAM_CHUNKS && w->program_used > PROGRAM_CHUNK / 2u);
    return !w->draw_count && w->places && low ? programs_recycle(w) : 0;
}
/* The SECONDARY task's nop.end and PROGRAM; once per frame without draws, a
 * full set of chunks is emptied after the GPU is done with them. */
static int programs(struct poulsback_frame_writer *w,
                    const struct poulsback_frame_gpu_program *program, uint32_t *nop,
                    uint32_t *link)
{
    static const uint32_t nop_end[2] = {SGX535_VERTEX_NOP_END_LOW, SGX535_VERTEX_NOP_END_HIGH};
    int ret = programs_room(w);
    for (int attempt = 0; !ret && attempt < 2; attempt++) {
        ret = program_place(w, NOP_END_ID, nop_end, 1, 0, nop);
        if (!ret)
            ret =
                program_place(w, program->id, program->words, program->instruction_count, 0, link);
        if (ret != -ENOSPC || attempt || w->draw_count)
            break;
        ret = programs_recycle(w);
    }
    return ret;
}
/* COUNT words into the GPU-draw area of the frame's PDS buffer. */
static int gpu_pds(struct poulsback_frame_writer *w, const uint32_t *words, uint32_t count,
                   uint64_t *va)
{
    uint32_t at = POULSBACK_FRAME_GPU_PDS + align(w->gpu_pds_bytes, 16);
    if (at > w->pds.size || count * 4u > w->pds.size - at)
        return -ENOSPC;
    memcpy(w->pds.map + at, words, count * 4u);
    w->gpu_pds_bytes = at + count * 4u - POULSBACK_FRAME_GPU_PDS;
    *va = w->pds.gpu_va + at;
    return 0;
}
int poulsback_frame_stream(struct poulsback_frame_writer *w, uint32_t bytes, uint32_t alignment,
                           void **cpu, uint64_t *address, uint32_t *handle)
{
    uint32_t at = align(w->vertex_bytes, alignment);
    if (!w->open || !bytes || !alignment || (alignment & (alignment - 1u)) ||
        at > w->vertices.size || bytes > w->vertices.size - at)
        return POULSBACK_REFUSED(-E2BIG);
    *cpu = w->vertices.map + at;
    *address = w->vertices.gpu_va + at;
    *handle = w->vertices.handle;
    w->vertex_bytes = at + bytes;
    return 0;
}
void poulsback_frame_sequence(const struct poulsback_frame_writer *w, uint64_t *address,
                              uint32_t *handle)
{
    *address = w->indices.gpu_va;
    *handle = w->indices.handle;
}

/* A PDS program's address and data size as a TA pds group word. */
static uint32_t pds_word(uint64_t va, uint32_t data_words)
{
    return (uint32_t)((va - PDS_EXEC_BASE) >> 4 & 0xffffffu) | data_words / 4u << 26;
}
/* COUNT CONSTANTS into the frame stream and the SECONDARY PDS program that
 * DMAs them into sa0 and runs NOP (a nop.end task word): its address and data
 * words. */
static int secondary(struct poulsback_frame_writer *w, const uint32_t *constants, uint32_t count,
                     uint32_t nop, uint64_t *va, uint32_t *data_words)
{
    const uint32_t task[3] = {nop, 0, 0};
    struct sgx535_vertex_secondary sec;
    uint64_t block_va;
    uint32_t handle;
    void *block;
    int ret;
    if (sgx535_vertex_secondary_build(count, task, &sec) != SGX535_VERTEX_OK)
        return POULSBACK_REFUSED(-EINVAL);
    ret = poulsback_frame_stream(w, count * 4u, 16, &block, &block_va, &handle);
    if (ret)
        return ret;
    memcpy(block, constants, count * 4u);
    for (uint32_t t = 0; t < sec.transfer_count; t++)
        sec.words[sec.source_word[t]] = (uint32_t)(block_va + sec.source_offset[t]);
    *data_words = sec.data_words;
    return gpu_pds(w, sec.words, sec.word_count, va);
}
/* ISP groups GROUPS (ISPA's bit set, BF-ISPC's highest) of WORDS into REC. */
static void isp_groups(struct sgx535_mte_state *rec, uint32_t groups, const uint32_t *words)
{
    for (unsigned g = SGX535_MTE_ISPA; g <= SGX535_MTE_BF_ISPC; g++)
        if (groups >> g & 1u)
            sgx535_mte_set(rec, (enum sgx535_mte_group)g, &words[g]);
}
/* REC's TA state record in the stream, the state task that delivers it
 * through the copy-and-emit program at EMIT_LINK, and X's STATE entry naming
 * the task. */
static int state_task(struct poulsback_frame_writer *w, const struct sgx535_mte_state *rec,
                      uint32_t emit_link, struct draw *x)
{
    uint32_t record[SGX535_MTE_STATE_WORDS], task[SGX535_MTE_TASK_WORDS], handle, count;
    uint64_t record_va, task_va;
    void *block;
    int ret;
    count = sgx535_mte_record(rec, ~0u, record);
    /* the emit program emits SGX535_MTE_EMIT_COUNT dwords at most */
    if (count > SGX535_MTE_EMIT_COUNT)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    ret = count ? poulsback_frame_stream(w, count * 4u, 16, &block, &record_va, &handle)
                : POULSBACK_REFUSED(-EINVAL);
    if (ret)
        return ret;
    memcpy(block, record, count * 4u);
    if (record_va > UINT32_MAX || !sgx535_mte_task((uint32_t)record_va, count, emit_link, task))
        return POULSBACK_REFUSED(-EINVAL);
    ret = gpu_pds(w, task, SGX535_MTE_TASK_WORDS, &task_va);
    if (ret)
        return ret;
    x->state[0] = 0x40000000u | (uint32_t)(task_va >> 4);
    x->state[1] = SGX535_MTE_STATE_W1;
    return 0;
}
/* A CPU-shaded draw's own ISP words (a clear object): a record
 * of the ISP groups alone, after the group's STATE. */
int poulsback_frame_cpu_state(struct poulsback_frame_writer *w,
                              const struct poulsback_frame_vertices *in, struct draw *x)
{
    struct sgx535_mte_state rec = {0};
    uint32_t emit[4], emit_link;
    int ret;
    if (in->isp_groups > 0x3fu || !(in->isp_groups & 1u) || !in->isp ||
        !sgx535_mte_emit_program(emit))
        return POULSBACK_REFUSED(-EINVAL);
    ret = programs_room(w);
    if (!ret)
        ret = program_place(w, STATE_EMIT_ID, emit, 2, 0, &emit_link);
    if (ret)
        return ret;
    isp_groups(&rec, in->isp_groups, in->isp);
    return state_task(w, &rec, emit_link, x);
}
/* What makes F's pixel state, as words: its program, iterations, ISP words
 * and constants (a valid F: pixel_state's checks). */
static uint32_t memo_key(const struct poulsback_frame_fragment *f, const uint32_t outputs[2],
                         uint32_t key[MEMO_KEY])
{
    static const uint32_t group_outputs[2] = {0, 0};
    const uint32_t *o = outputs ? outputs : group_outputs;
    const uint32_t head[10] = {
        (uint32_t)f->id,    (uint32_t)(f->id >> 32), f->temps,      f->phase2, f->primary,
        f->iteration_count, f->constant_count,       f->isp_groups, o[0],      o[1]};
    uint32_t n = 10u + f->iteration_count;
    memcpy(key, head, sizeof(head));
    memcpy(key + 10, f->iteration, f->iteration_count * 4u);
    memcpy(key + n, f->isp, 6u * 4u);
    n += 6u;
    if (f->constant_count)
        memcpy(key + n, f->constants, f->constant_count * 4u);
    n += f->constant_count;
    /* the texture reads' words and the BOs sampled */
    key[n++] = f->texture_mask;
    key[n++] = f->texture_bo_count;
    for (uint32_t i = 0; i < f->iteration_count; i++)
        if (f->texture_mask >> i & 1u) {
            memcpy(key + n, f->texture[i], 12);
            n += 3;
        }
    memcpy(key + n, f->texture_bo, f->texture_bo_count * 4u);
    n += f->texture_bo_count;
    /* the draw's viewport */
    key[n++] = f->has_viewport;
    if (f->has_viewport) {
        memcpy(key + n, f->viewport, sizeof(f->viewport));
        n += 6;
    }
    return n;
}
/* F's state X as the frame's last pixel state. */
static void remember(struct poulsback_frame_writer *w, const struct poulsback_frame_fragment *f,
                     const uint32_t outputs[2], const struct draw *x)
{
    w->last_fragment = f;
    w->last_id = f->id;
    w->last_generation = f->generation;
    w->last_outputs_set = outputs != NULL;
    if (outputs)
        memcpy(w->last_outputs, outputs, sizeof(w->last_outputs));
    memcpy(w->last_state, x->state, sizeof(w->last_state));
}
/* A backend program's per-draw pixel state: its USE code, its
 * pixel PDS program, the SECONDARY program and block, and the TA state
 * record of the pds group naming them, delivered by a state task; STATE is
 * the TA STATE entry that follows the group's. OUTPUTS: the draw's output
 * selects and texture-coordinate sizes when they are not the group's. */
static int pixel_state(struct poulsback_frame_writer *w, const struct poulsback_frame_fragment *f,
                       const uint32_t outputs[2], struct draw *x)
{
    static const uint32_t texfloat = 0;
    static const uint32_t nop_end[2] = {SGX535_VERTEX_NOP_END_LOW, SGX535_VERTEX_NOP_END_HIGH};
    struct sgx535_pixel_pds_request r = {0};
    struct sgx535_pixel_pds pds;
    struct sgx535_mte_state rec = {0};
    uint32_t link, emit_link, nop = 0, emit[4], group[3], sec_words, key[MEMO_KEY], words;
    uint64_t pds_va, sec_va;
    int ret;
    if (!f->id || f->id >= STATE_EMIT_ID ||
        (f->instruction_count ? !f->words : f->iteration_count || f->constant_count) ||
        f->iteration_count > POULSBACK_FRAME_FRAGMENT_ITERATIONS ||
        (f->constant_count && !f->constants) || f->constant_count > 128u || f->isp_groups > 0x3fu ||
        (f->isp_groups && !(f->isp_groups & 1u)) || f->texture_mask >> f->iteration_count ||
        f->texture_bo_count > POULSBACK_FRAME_FRAGMENT_TEXTURES)
        return POULSBACK_REFUSED(-EINVAL);
    /* the frame's last draw's fragment, its words unchanged: its state */
    if (f == w->last_fragment && f->id == w->last_id && f->generation == w->last_generation &&
        (outputs ? w->last_outputs_set && !memcmp(outputs, w->last_outputs, 8)
                 : !w->last_outputs_set)) {
        memcpy(x->state, w->last_state, sizeof(x->state));
        return 0;
    }
    words = memo_key(f, outputs, key);
    for (uint32_t i = 0; i < w->memos; i++)
        if (w->memo[i].words == words && !memcmp(w->memo[i].key, key, words * 4u)) {
            memcpy(x->state, w->memo[i].state, sizeof(x->state));
            remember(w, f, outputs, x);
            return 0;
        }
    if (!sgx535_mte_emit_program(emit))
        return POULSBACK_REFUSED(-EINVAL);
    /* no pixel program: the PDS's DOUTU names nop.end */
    ret = f->instruction_count ? program_place(w, f->id, f->words, f->instruction_count, 1, &link)
                               : program_place(w, NOP_END_ID, nop_end, 1, 1, &link);
    if (!ret)
        ret = program_place(w, STATE_EMIT_ID, emit, 2, 0, &emit_link);
    if (!ret && f->constant_count)
        ret = program_place(w, NOP_END_ID, nop_end, 1, 1, &nop);
    for (uint32_t i = 0; i < f->texture_bo_count && !ret; i++)
        ret = frame_bo(w, f->texture_bo[i]);
    if (ret)
        return ret;
    r = (struct sgx535_pixel_pds_request){link, f->temps,        f->phase2, f->iteration_count,
                                          {0},  f->texture_mask, {{0}}};
    memcpy(r.iteration, f->iteration, f->iteration_count * 4u);
    memcpy(r.texture, f->texture, sizeof(r.texture));
    if (!sgx535_pixel_pds_build(&r, &pds) ||
        !sgx535_pixel_pds_dms(f->primary, f->temps, f->constant_count, &group[1]))
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    ret = gpu_pds(w, pds.words, pds.word_count, &pds_va);
    if (ret)
        return ret;
    /* the pds group holds 24 bits of 16-byte units above PDS_EXEC_BASE */
    if (pds_va < PDS_EXEC_BASE || pds_va - PDS_EXEC_BASE >= UINT64_C(1) << 28)
        return POULSBACK_REFUSED(-EINVAL);
    group[0] = 0;
    group[2] = pds_word(pds_va, pds.data_words);
    if (f->constant_count) {
        ret = secondary(w, f->constants, f->constant_count, nop, &sec_va, &sec_words);
        if (ret)
            return ret;
        group[0] = pds_word(sec_va, sec_words);
    }
    /* the record: the draw's ISP groups, the pds group, its viewport, its
     * outputs (F32 texture coordinates: TEXFLOAT 0) */
    isp_groups(&rec, f->isp_groups, f->isp);
    sgx535_mte_set(&rec, SGX535_MTE_PDS, group);
    if (f->has_viewport)
        sgx535_mte_set(&rec, SGX535_MTE_VIEWPORT, f->viewport);
    if (outputs) {
        sgx535_mte_set(&rec, SGX535_MTE_OUTSELECTS, &outputs[0]);
        sgx535_mte_set(&rec, SGX535_MTE_TEXSIZE, &outputs[1]);
        sgx535_mte_set(&rec, SGX535_MTE_TEXFLOAT, &texfloat);
    }
    ret = state_task(w, &rec, emit_link, x);
    if (!ret) {
        struct pixel_memo *m = &w->memo[w->memo_next];
        w->memo_next = (w->memo_next + 1u) % PIXEL_STATES;
        w->memos += w->memos < PIXEL_STATES;
        memcpy(m->key, key, words * 4u);
        m->words = words;
        memcpy(m->state, x->state, sizeof(m->state));
        remember(w, f, outputs, x);
    }
    return ret;
}
/* INDEXLIST w2's max input instances - 1 (31:28), sized on SGX535 as a third
 * of the 1,632 attribute registers left beside the largest SA (128) and the
 * pixel secondaries (128), over the vertex's attribute registers, at most 16
 * (EMGD writes 15). Fewer instances than the output partition's vertices must
 * not be sent (HW-SHADER-013): beside a raster the vertex tasks overran the
 * attributes of pixel tasks (the PBE wrote through garbage addresses), and an
 * index list naming one vertex stalled the TA. */
static uint32_t max_instances_field(uint32_t element_count)
{
    uint32_t instances = 458u / (4u * element_count);
    return (instances < 16u ? instances : 16u) - 1u;
}

static int draw_gpu(struct poulsback_frame_writer *w, const poulsback_frame_shape *s,
                    const struct poulsback_frame_gpu_draw *d)
{
    /* TEXSIZE per texture coordinate of 1-4 components: U 0, UV 1, UVS 5,
     * UVST 7 (EMGD's) */
    static const uint8_t texsize[5] = {0, 0, 1, 5, 7};
    const struct sgx535_window_mesh_request *m = &s->mesh.scene.foreground;
    uint32_t outputs = d->color ? 8u : 4u, selects[2] = {0, 0}, size, part;
    struct sgx535_vertex_pds_layout layout = {d->element_count, {{0}}, {0}};
    struct sgx535_vertex_pds vpds;
    struct state *fs;
    struct group *g;
    struct draw *x;
    uint32_t link, nop, sec_words, secondary_words;
    uint64_t vpds_va, sec_va;
    int ret;
    if (!d->program || !d->program->id || !d->count || d->count % 3u || d->count > 0x3fffffu ||
        !d->element_count || d->element_count > POULSBACK_FRAME_GPU_ELEMENTS || !d->constants ||
        !d->constant_count || d->constant_count > SGX535_VERTEX_CONSTANTS ||
        d->index_address > UINT32_MAX || (d->index_address & (d->index32 ? 3u : 1u)) ||
        (d->tc_count && !d->fragment) || d->tc_count > POULSBACK_FRAME_TCS)
        return POULSBACK_REFUSED(-EINVAL);
    /* the vertex: POSITION and W, then the texture coordinates */
    if (d->tc_count) {
        outputs = 4;
        for (uint32_t t = 0; t < d->tc_count; t++) {
            if (!d->tc[t] || d->tc[t] > 4u)
                return POULSBACK_REFUSED(-EINVAL);
            outputs += d->tc[t];
            selects[1] |= (uint32_t)texsize[d->tc[t]] << 3u * t;
        }
        selects[0] = outputs << 24 | 1u << 12;
    }
    /* EMGD's partition rule: vertices of whole 16-byte units, up to 16 in 64
     * dwords */
    size = (outputs + 3u) & ~3u;
    part = 64u / size > 16u ? 15u : 64u / size - 1u;
    if (w->draw_count == POULSBACK_FRAME_MAX_DRAWS || m->order != SGX535_WINDOW_MESH_DEINDEXED ||
        m->version != SGX535_WINDOW_MESH_CLIP_VERSION || !m->width || !m->height ||
        m->width >= 2048 || m->height >= 2048 ||
        d->color != (m->interpolation != SGX535_WINDOW_MESH_NO_INPUT) ||
        d->flat != (m->interpolation == SGX535_VARYING_FLAT_REPLICATED))
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    fs = poulsback_frame_state_get(w, s);
    if (!fs || (w->fs && fs != w->fs))
        return POULSBACK_REFUSED(fs ? -EOPNOTSUPP : -ENOMEM);
    g = poulsback_frame_group_get(w, fs, s, d->flat);
    if (!g)
        return POULSBACK_REFUSED(-ENOMEM);
    ret = programs(w, d->program, &nop, &link);
    if (ret)
        return ret;
    /* The vertex PDS: element E's dwords into PA 4E, from its address plus
     * the vertex index times its stride. */
    for (uint32_t e = 0; e < d->element_count; e++) {
        const struct poulsback_frame_gpu_element *a = &d->element[e];
        layout.element[e] = (struct sgx535_vertex_pds_element){4u * e, a->dwords, a->stride};
        ret = frame_bo(w, a->handle);
        if (ret)
            return ret;
    }
    sgx535_vertex_task(link, d->program->temps, layout.task);
    if (sgx535_vertex_pds_build(&layout, &vpds) != SGX535_VERTEX_OK)
        return POULSBACK_REFUSED(-EOPNOTSUPP);
    for (uint32_t e = 0; e < d->element_count; e++)
        vpds.words[vpds.source_word[e]] = (uint32_t)d->element[e].address;
    /* The constant block in the stream, and its SECONDARY PDS. */
    ret = secondary(w, d->constants, d->constant_count, nop, &sec_va, &sec_words);
    if (!ret)
        ret = gpu_pds(w, vpds.words, vpds.word_count, &vpds_va);
    if (!ret)
        ret = frame_bo(w, d->index_handle);
    if (!ret && d->fragment)
        ret = pixel_state(w, d->fragment, d->tc_count ? selects : NULL, &w->draws[w->draw_count]);
    if (ret)
        return ret;
    /* The entry after the template's STATE: a SECONDARY STATE per USE pipe
     * (EMGD's: its data and SA sizes in 16-byte units), then the INDEXLIST
     * (EMGD's partition rule; flat: vertex 2). */
    x = &w->draws[w->draw_count];
    secondary_words = sec_words / 4u << 26 | 0x00010100u | (d->constant_count + 3u) / 4u;
    x->tail[0] = x->tail[2] = 0x40000000u | (uint32_t)(sec_va >> 4);
    x->tail[1] = secondary_words | 1u << 13;
    x->tail[3] = secondary_words | 2u << 13;
    x->tail[4] = 0x81400000u | d->count;
    x->tail[5] = (uint32_t)d->index_address;
    x->tail[6] = max_instances_field(d->element_count) << 28 | (d->index32 ? 1u << 24 : 0) |
                 (d->flat ? 2u << 25 : 0);
    x->tail[7] = part << 28 | (uint32_t)(vpds_va >> 4);
    x->tail[8] = size / 4u << 25 | (part < 3u ? part : 3u) << 23 | d->element_count << 8 |
                 vpds.data_words / 4u;
    x->g = g;
    x->offset = x->count = 0;
    x->gpu = true;
    x->pixel = d->fragment != NULL;
    w->fs = fs;
    /* A backend program iterates the colour from TC0, which the F32 family's
     * templates deliver (V0 exchanges R and B; docs/compiler.md#window-integration). */
    w->f32 |= g->inputs && (!g->flat || x->pixel);
    w->draw_count++;
    return 0;
}
int poulsback_frame_draw_gpu(struct poulsback_frame_writer *w, const poulsback_frame_shape *s,
                             const struct poulsback_frame_gpu_draw *d)
{
    uint32_t extra = w->extra_count, gpu = w->gpu_pds_bytes, stream = w->vertex_bytes;
    int ret;
    if (!w->open || !s || !d)
        return POULSBACK_REFUSED(-EINVAL);
    ret = draw_gpu(w, s, d);
    if (ret) {
        w->extra_count = extra;
        w->gpu_pds_bytes = gpu;
        w->vertex_bytes = stream;
    }
    /* the frame's PDS area, BO list or program places full: refused as a
     * full frame is (the frame stays as it was) */
    return ret == -ENOSPC ? POULSBACK_REFUSED(-EOPNOTSUPP) : ret;
}
