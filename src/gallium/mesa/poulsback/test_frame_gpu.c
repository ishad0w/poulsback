// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The frame writer's GPU-shaded draws, on fake buffers.
 *
 * A frame of a smooth GPU draw, a CPU draw of the same shape, a flat GPU draw
 * and a GPU draw without inputs is ended; the fixture then follows what the
 * GPU reads from the TA register table: the TA stream's entries, each GPU
 * draw's SECONDARY STATEs to its PDS, constant block and nop.end, its
 * INDEXLIST to the index buffer and vertex PDS, the vertex PDS to each
 * element's address and the vertex program's words; the BO list names every
 * buffer read. A GPU draw's STATE equals the CPU draw's (one template), the
 * flat draw's template iterates its colour at vertex 2, and a draw that would
 * overflow the BO list is refused without changing the frame.
 *
 * usage: poulsback-frame-gpu */
#include "poulsback_frame.h"
#include "poulsback_fragment.h"
#include "poulsback_bo_uapi.h"
#include "sgx535_mte_state.h"
#include "sgx535_pixel_pds.h"
#include "sgx535_vertex.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
        checks++;                                                                                  \
    } while (0)
static unsigned checks;
/* An ordinary frame's TA header: the mask entry, then the depth clear. */
static uint32_t header_words[14];

#define CODE_VA 0x40000000u

/* ---- fake buffers (test_frame.c's model) ---- */

struct fake {
    uint32_t handle, bo_class;
    uint64_t va, size;
    uint8_t *bytes;
    bool live;
};
#define FAKES 256
static struct fake fakes[FAKES];
static uint32_t fake_count;
static uint64_t next_va[6] = {0x60000000u, 0x20000000u, 0x40000000u,
                              0x50000000u, 0x70000000u, 0x34000000u};
static int fake_alloc(void *data, uint32_t bo_class, uint64_t bytes, struct poulsback_frame_bo *out)
{
    uint64_t size = 4096;
    struct fake *f;
    (void)data;
    while (size < bytes)
        size <<= 1;
    CHECK(fake_count < FAKES && bo_class < 6);
    f = &fakes[fake_count];
    f->va = (next_va[bo_class] + size - 1u) & ~(size - 1u);
    if (bo_class == POULSBACK_BO_CLASS_CODE && f->va / 0x80000u != (f->va + size - 1u) / 0x80000u)
        f->va = (f->va + 0x7ffffu) & ~UINT64_C(0x7ffff);
    next_va[bo_class] = f->va + size;
    f->handle = 1000u + fake_count++;
    f->bo_class = bo_class;
    f->size = size;
    f->bytes = calloc(1, size);
    f->live = true;
    CHECK(f->bytes);
    *out = (struct poulsback_frame_bo){f->handle, f->va, f->size, f->bytes};
    return 0;
}
static void fake_free(void *data, struct poulsback_frame_bo *bo)
{
    (void)data;
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].handle == bo->handle)
            fakes[i].live = false;
    memset(bo, 0, sizeof(*bo));
}
static unsigned idle_calls;
static int fake_idle(void *data)
{
    (void)data;
    idle_calls++;
    return 0;
}
static const struct fake *by_handle(uint32_t handle)
{
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].handle == handle && fakes[i].live)
            return &fakes[i];
    return NULL;
}
static const uint8_t *at(uint64_t va, uint32_t len)
{
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].live && va >= fakes[i].va && va + len <= fakes[i].va + fakes[i].size)
            return fakes[i].bytes + (va - fakes[i].va);
    return NULL;
}
static uint32_t word_at(uint64_t va)
{
    const uint8_t *p = at(va, 4);
    uint32_t v;
    CHECK(p);
    memcpy(&v, p, 4);
    return v;
}

/* ---- draws ---- */

static struct sgx535_fragment_instruction fragment[2][8];
static const uint32_t uniforms[4] = {0x3f800000u, 0, 0x3f000000u, 0x3f800000u};
static poulsback_frame_shape shape(uint32_t interpolation)
{
    poulsback_frame_shape r;
    struct sgx535_window_mesh_request *m = &r.mesh.scene.foreground;
    bool uniform = interpolation == SGX535_WINDOW_MESH_NO_INPUT;
    memset(&r, 0, sizeof(r));
    for (unsigned c = 0; c < 4; c++) {
        fragment[uniform][c] = (struct sgx535_fragment_instruction){
            .opcode = uniform ? SGX535_FRAGMENT_UNIFORM : SGX535_FRAGMENT_INPUT,
            .index = uniform ? c : 0,
            .component = uniform ? 0 : c};
        fragment[uniform][c + 4] = (struct sgx535_fragment_instruction){
            .opcode = SGX535_FRAGMENT_OUTPUT, .index = c, .source = {c}};
    }
    *m = (struct sgx535_window_mesh_request){.version = SGX535_WINDOW_MESH_CLIP_VERSION,
                                             .profile = SGX535_WINDOW_MESH_PROFILE,
                                             .core_id = SGX535_ENGINE121_CORE_ID,
                                             .revision = SGX535_ENGINE121_REVISION,
                                             .width = 64,
                                             .height = 64,
                                             .order = SGX535_WINDOW_MESH_DEINDEXED,
                                             .interpolation = interpolation,
                                             .fragment = fragment[uniform],
                                             .fragment_count = 8,
                                             .uniforms = uniform ? uniforms : NULL,
                                             .uniform_word_count = uniform ? 4 : 0};
    m->window.origin = 1;
    m->window.sample_mask = 0xffffffffu;
    m->window.half_pixel_center = m->window.bottom_edge_rule = 1;
    m->window.viewport_scale_f32[0] = m->window.viewport_translate_f32[0] = 0x42000000u;
    m->window.viewport_scale_f32[1] = 0xc2000000u;
    m->window.viewport_translate_f32[1] = 0x42000000u;
    m->window.viewport_scale_f32[2] = m->window.viewport_translate_f32[2] = 0x3f000000u;
    r.mesh.version = SGX535_DEPTH_WINDOW_MESH_GL_WINDOW_VERSION;
    r.mesh.format = SGX535_DEPTH_D24S8;
    r.mesh.scene.version = SGX535_WINDOW_MESH_GL_WINDOW_BACKGROUND_VERSION;
    r.mesh.scene.packed_background_argb = 0xff102030u;
    r.samples = (struct sgx535_single_sample_source){
        .version = 1, .effective_framebuffer_samples = 1, .fragment_instructions = 8};
    return r;
}

/* o0..o3 = pa0..pa3 (and with COLOR o4..o7 = pa4..pa7 × sa3): moves and MADs. */
static struct sgx535_vertex_code code[2];
static struct poulsback_frame_gpu_program program(bool color, uint64_t id)
{
    struct sgx535_vertex_instruction in[8];
    unsigned n = 0;
    for (unsigned c = 0; c < 4; c++)
        in[n++] = (struct sgx535_vertex_instruction){
            SGX535_VERTEX_MOV, (int8_t)c, {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)c}}};
    for (unsigned c = 0; color && c < 4; c++)
        in[n++] =
            (struct sgx535_vertex_instruction){SGX535_VERTEX_MAD,
                                               (int8_t)(4 + c),
                                               {{SGX535_VERTEX_ATTRIBUTE, 0, 0, (uint16_t)(4 + c)},
                                                {SGX535_VERTEX_CONSTANT, 0, 0, 3},
                                                {SGX535_VERTEX_CONSTANT, 0, 0, 1}}};
    CHECK(sgx535_vertex_encode(in, n, color ? 8 : 4, &code[color]) == SGX535_VERTEX_OK);
    return (struct poulsback_frame_gpu_program){id, code[color].words,
                                                code[color].instruction_count, code[color].temps};
}

/* ---- following the words ---- */

static uint64_t code_va(uint32_t doutu0, uint32_t data_master)
{
    uint32_t bank = doutu0 & 15u, units = (doutu0 >> 4 & 15u) * 2048u + (doutu0 >> 8 & 0x7ffu);
    CHECK(bank >= 3u + data_master && !((bank - 3u - data_master) & 1u)); /* 3 + 2w, 4 + 2w */
    return CODE_VA + (uint64_t)(bank - 3u - data_master) / 2u * 0x80000u + units * 16u;
}
static uint64_t program_va(uint32_t doutu0)
{
    return code_va(doutu0, 0); /* a vertex bank */
}
/* The SECONDARY STATE pair at ENTRY: its PDS DMAs CONSTANTS into sa0.. and
 * runs nop.end. */
static void check_secondary(const uint32_t *entry, const uint32_t *constants, uint32_t count)
{
    uint64_t pds = (uint64_t)(entry[0] & 0x0fffffffu) << 4;
    uint32_t data = (entry[1] >> 26) * 4u, task, block;
    CHECK(entry[0] >> 28 == 4u && entry[2] == entry[0]);
    CHECK((entry[1] & 0x0001ffffu) == (0x00012100u | (count + 3u) / 4u));
    CHECK((entry[3] & 0x0001ffffu) == (0x00014100u | (count + 3u) / 4u));
    CHECK((entry[1] >> 26) == (entry[3] >> 26) && data == 12);
    block = word_at(pds);
    CHECK(!memcmp(at(block, count * 4u), constants, count * 4u));
    CHECK((word_at(pds + 4) & 0x7ff00u) == 0); /* into sa0 */
    task = word_at(pds + 8);
    CHECK(word_at(program_va(task)) == SGX535_VERTEX_NOP_END_LOW &&
          word_at(program_va(task) + 4) == SGX535_VERTEX_NOP_END_HIGH);
    CHECK(word_at(pds + data * 4u) == 0x07030223u && word_at(pds + data * 4u + 8) == 0xaf000000u);
}
/* The INDEXLIST at ENTRY: COUNT indices at INDEX, word 2 W2 (32-bit indices,
 * FLATSHADE) with the max input instances for N elements (16 up to 7, 14 at 8;
 * EMGD writes 15 in every captured draw), the vertex PDS
 * with ELEMENTS and PROGRAM, output DWORDS. */
static void check_indexlist(const uint32_t *entry, uint32_t count, uint64_t index, uint32_t w2,
                            const struct poulsback_frame_gpu_element *elements, uint32_t n,
                            const struct poulsback_frame_gpu_program *p, uint32_t dwords)
{
    /* EMGD's partition rule: 16-byte units, up to 16 vertices in 64 dwords */
    uint64_t pds = (uint64_t)(entry[3] & 0x0fffffffu) << 4;
    uint32_t data = (entry[4] & 63u) * 4u, size = (dwords + 3u) & ~3u,
             part = 64u / size > 16u ? 15u : 64u / size - 1u, task;
    CHECK(entry[0] == (0x81400000u | count) && entry[1] == index);
    CHECK(entry[2] == ((n <= 7u ? 15u : 13u) << 28 | w2));
    CHECK(entry[3] >> 28 == part);
    CHECK(entry[4] >> 25 == size / 4u && (entry[4] >> 23 & 3u) == (part < 3u ? part : 3u) &&
          (entry[4] >> 8 & 0x1ffu) == n);
    for (uint32_t e = 0; e < n; e++) {
        CHECK(word_at(pds + 8u * e) == (uint32_t)elements[e].address);
        CHECK(word_at(pds + 8u * e + 4) == (4u * e << 8 | (elements[e].dwords - 1u)));
    }
    task = word_at(pds + 8u * n);
    CHECK(task & 0x00200000u);
    CHECK(word_at(pds + 8u * n + 4) == p->temps << 27);
    CHECK(!memcmp(at(program_va(task), p->instruction_count * 8u), p->words,
                  p->instruction_count * 8u));
    if (elements[0].stride) {
        CHECK(word_at(pds + 32) == elements[0].stride); /* ds1[0] */
        CHECK(word_at(pds + data * 4u) == 0x67800072u);
    } else
        CHECK(word_at(pds + data * 4u) == 0x07030223u);
}

/* A backend program's STATE pair at ENTRY: its state task DMAs a record of
 * the pds group (docs/native-3d.md#ta-state-records) into the emit program, naming the pixel PDS
 * (DOUTU to the program's words, its iterations with the last marked) and
 * the SECONDARY program (CONSTANTS into sa0, then nop.end), with the DMS
 * word for them. */
#define PDS_EXEC_BASE 0x20000000u
/* A draw's own ISP words without a program (the stencil object):
 * its state task's record holds GROUPS of ISP alone. */
static void check_isp_state(const uint32_t *entry, uint32_t groups, const uint32_t *isp)
{
    uint64_t task = (uint64_t)(entry[0] & 0x0fffffffu) << 4;
    uint32_t count, words[SGX535_MTE_STATE_WORDS];
    struct sgx535_mte_state rec = {0};
    CHECK(entry[0] >> 28 == 4u && entry[1] == SGX535_MTE_STATE_W1);
    count = word_at(task + 4) + 1u;
    CHECK(count <= SGX535_MTE_STATE_WORDS && (word_at(task + 8) & 0x00200000u));
    memcpy(words, at(word_at(task), count * 4u), count * 4u);
    CHECK(words[0] == groups && sgx535_mte_parse(words, count, &rec));
    for (unsigned g = SGX535_MTE_ISPA; g <= SGX535_MTE_BF_ISPC; g++)
        CHECK(!(groups >> g & 1u) ||
              rec.words[sgx535_mte_group_offset((enum sgx535_mte_group)g)] == isp[g]);
}
/* SELECTS: the draw's output selects and texture-coordinate sizes in its
 * record, with TEXFLOAT 0; NULL: the group's. */
static void check_pixel_outputs(const uint32_t *entry, const struct poulsback_frame_fragment *f,
                                const uint32_t *selects)
{
    const uint32_t outputs =
        1u << SGX535_MTE_OUTSELECTS | 1u << SGX535_MTE_TEXSIZE | 1u << SGX535_MTE_TEXFLOAT;
    uint64_t task = (uint64_t)(entry[0] & 0x0fffffffu) << 4, pixel, sec;
    uint32_t emit[4], dms, last = f->iteration_count, count, words[SGX535_MTE_STATE_WORDS];
    struct sgx535_mte_state rec = {0};
    const uint32_t *pds = rec.words + sgx535_mte_group_offset(SGX535_MTE_PDS);
    CHECK(entry[0] >> 28 == 4u && entry[1] == SGX535_MTE_STATE_W1);
    count = word_at(task + 4) + 1u;
    CHECK(count <= SGX535_MTE_STATE_WORDS && (word_at(task + 8) & 0x00200000u));
    memcpy(words, at(word_at(task), count * 4u), count * 4u);
    CHECK(sgx535_mte_emit_program(emit) &&
          !memcmp(at(program_va(word_at(task + 8) & ~0x00200000u), 16), emit, 16));
    CHECK(word_at(task + 48) == 0x07030223u && word_at(task + 52) == 0x07040345u &&
          word_at(task + 56) == 0xaf000000u);
    /* the record: the draw's ISP groups, the pds group, its viewport and its
     * outputs */
    CHECK(words[0] == (f->isp_groups | 1u << SGX535_MTE_PDS |
                       (f->has_viewport ? 1u << SGX535_MTE_VIEWPORT : 0) |
                       (selects ? outputs : 0)) &&
          sgx535_mte_parse(words, count, &rec));
    CHECK(!f->has_viewport ||
          !memcmp(rec.words + sgx535_mte_group_offset(SGX535_MTE_VIEWPORT), f->viewport, 24));
    CHECK(!selects || (rec.words[sgx535_mte_group_offset(SGX535_MTE_OUTSELECTS)] == selects[0] &&
                       rec.words[sgx535_mte_group_offset(SGX535_MTE_TEXSIZE)] == selects[1] &&
                       rec.words[sgx535_mte_group_offset(SGX535_MTE_TEXFLOAT)] == 0));
    for (unsigned g = SGX535_MTE_ISPA; g <= SGX535_MTE_BF_ISPC; g++)
        CHECK(!(f->isp_groups >> g & 1u) ||
              rec.words[sgx535_mte_group_offset((enum sgx535_mte_group)g)] == f->isp[g]);
    CHECK(sgx535_pixel_pds_dms(f->primary, f->temps, f->constant_count, &dms) && pds[1] == dms);
    pixel = PDS_EXEC_BASE + ((uint64_t)(pds[2] & 0xffffffu) << 4);
    /* without a pixel program (no colour written): nop.end */
    if (!f->instruction_count)
        CHECK(word_at(code_va(word_at(pixel) & 0x7ffffu, 1)) == SGX535_VERTEX_NOP_END_LOW);
    else
        CHECK(!memcmp(at(code_va(word_at(pixel) & 0x7ffffu, 1), f->instruction_count * 8u),
                      f->words, f->instruction_count * 8u));
    CHECK(word_at(pixel + 4) >> 27 == f->temps);
    for (uint32_t k = 0; k < f->iteration_count; k++) /* DS0 dword 2k + 2 */
        CHECK(word_at(pixel + 4u * (2u * k + 2u)) ==
              (f->iteration[k] | (k + 1u == last ? 1u << 25 : 0)));
    if (!f->constant_count) {
        CHECK(pds[0] == 0);
        return;
    }
    sec = PDS_EXEC_BASE + ((uint64_t)(pds[0] & 0xffffffu) << 4);
    CHECK(!memcmp(at(word_at(sec), f->constant_count * 4u), f->constants, f->constant_count * 4u));
    CHECK(word_at(code_va(word_at(sec + 8), 1)) == SGX535_VERTEX_NOP_END_LOW);
}
static void check_pixel(const uint32_t *entry, const struct poulsback_frame_fragment *f)
{
    check_pixel_outputs(entry, f, NULL);
}

static bool listed(const struct poulsback_frame_job *job, uint32_t handle)
{
    for (uint32_t i = 0; i < job->bo_count; i++)
        if (job->bos[i].handle == handle)
            return true;
    return false;
}
static const uint8_t *ta_stream(const struct poulsback_frame_job *job)
{
    for (uint32_t i = 0; i < job->ta_count; i++)
        if (job->ta[i].offset == 0x238) {
            const struct fake *f = by_handle(job->bos[job->ta[i].bo - 1u].handle);
            CHECK(f);
            return f->bytes + job->ta[i].bo_offset;
        }
    CHECK(!"TA stream register");
    return NULL;
}
/* Whether a PDS-class BO of the writer holds WORD. */
static bool pds_holds(uint32_t value)
{
    for (uint32_t i = 0; i < fake_count; i++)
        if (fakes[i].live && fakes[i].bo_class == POULSBACK_BO_CLASS_PDS)
            for (uint64_t k = 0; k + 4 <= fakes[i].size; k += 4) {
                uint32_t v;
                memcpy(&v, fakes[i].bytes + k, 4);
                if (v == value)
                    return true;
            }
    return false;
}

/* The load draw's program: one direct read of unit 0 at TC0's s and t, its
 * DOUTT the unit's words, the BO in the list, TC0 of two components. */
static void load_program(void)
{
    struct poulsback_fragment *f = poulsback_fragment_create_load();
    struct poulsback_fragment_texture t = {
        .words = {0x11u, 0x22u, 0x33u}, .width = 8, .height = 4, .handle = 77};
    struct poulsback_fragment_state s = {.height = 4,
                                         .isp = {.front_ccw = 1, .colormask = 15},
                                         .key = {.colormask = 15},
                                         .texture = {&t}};
    static const uint32_t range[6] = {4, 4, 2, 0xc0000000u, 0x3f400000u, 0x3e800000u};
    struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS];
    const struct poulsback_frame_fragment *x;
    const char *refusal = NULL;
    uint32_t reads = 0;
    CHECK(f && poulsback_fragment_samplers(f) == 1u && !poulsback_fragment_reads_color(f));
    CHECK(poulsback_fragment_tcs(f, tc) == 1u && tc[0].components == 2);
    x = poulsback_fragment_draw(f, &s, &refusal);
    CHECK(x && !refusal);
    for (uint32_t i = 0; x && i < x->iteration_count; i++)
        if (x->texture_mask >> i & 1u) {
            CHECK(x->texture[i][0] == 0x11u && x->texture[i][1] == 0x22u &&
                  x->texture[i][2] == 0x33u);
            reads++;
        }
    CHECK(reads == 1 && x->texture_bo_count == 1 && x->texture_bo[0] == 77 && !x->has_viewport);
    /* the same state again: the same words, the same generation; a texture
     * word changed: a new one */
    {
        const uint32_t generation = x->generation;
        x = poulsback_fragment_draw(f, &s, &refusal);
        CHECK(x && x->generation == generation);
        t.words[0] = 0x44u;
        x = poulsback_fragment_draw(f, &s, &refusal);
        CHECK(x && x->generation == generation + 1u);
        t.words[0] = 0x11u;
        x = poulsback_fragment_draw(f, &s, &refusal);
        CHECK(x && x->generation == generation + 2u);
    }
    /* a depth range: the draw's viewport words */
    s.viewport = range;
    x = poulsback_fragment_draw(f, &s, &refusal);
    CHECK(x && !refusal && x->has_viewport && !memcmp(x->viewport, range, sizeof(range)));
    s.viewport = NULL;
    s.texture[0] = NULL;
    CHECK(!poulsback_fragment_draw(f, &s, &refusal) && refusal);
    poulsback_fragment_destroy(f);
}

int main(void)
{
    load_program();
    const struct poulsback_frame_bo_ops ops = {NULL, fake_alloc, fake_free, fake_idle};
    static struct poulsback_frame_job job;
    struct poulsback_frame_writer *w;
    struct poulsback_frame_bo pds, stream, vbo, ibo, target, depth, many[POULSBACK_FRAME_EXTRA_BOS];
    poulsback_frame_shape smooth = shape(SGX535_VARYING_SMOOTH);
    poulsback_frame_shape flat = shape(SGX535_VARYING_FLAT_REPLICATED);
    poulsback_frame_shape solid = shape(SGX535_WINDOW_MESH_NO_INPUT);
    struct poulsback_frame_gpu_program colour = program(true, 7), position = program(false, 8);
    const uint32_t constants[5] = {0, 0, 0x3f800000u, 0x3f400000u, 0x12345678u};
    const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    uint32_t cpu_vertices[3][8], seq_handle;
    uint64_t seq;
    struct poulsback_frame_gpu_draw d;
    const uint8_t *ta;
    uint32_t entry[16], cpu_state[2];
    CHECK(!poulsback_frame_writer_create(&ops, &w));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_PDS, POULSBACK_FRAME_PDS_BYTES, &pds));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, POULSBACK_FRAME_VERTEX_BYTES, &stream));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, 4096, &vbo));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, 4096, &ibo));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, 256 * 256, &target));
    CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_DEPTH, 64 * 64 * 4, &depth));
    for (unsigned i = 0; i < POULSBACK_FRAME_EXTRA_BOS; i++)
        CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, 4096, &many[i]));
    memcpy(ibo.map + 64, indices, sizeof(indices));
    for (unsigned v = 0; v < 3; v++)
        for (unsigned k = 0; k < 8; k++)
            cpu_vertices[v][k] = k == 3 || k == 7 ? 0x3f800000u : k < 3 ? (v + k) * 0x3e000000u : 0;
    poulsback_frame_begin(w, &pds, &stream);
    poulsback_frame_sequence(w, &seq, &seq_handle);
    CHECK(word_at(seq + 2 * 4095) == 4095u + (4096u << 16) &&
          word_at(seq + 2 * 65534) == 0xffff0000u + 65534u);
    /* 1: smooth, elements in the VBO, u16 indices in place. */
    d = (struct poulsback_frame_gpu_draw){
        .program = &colour,
        .element_count = 2,
        .element = {{vbo.handle, vbo.gpu_va + 16, 4, 32}, {vbo.handle, vbo.gpu_va + 32, 4, 32}},
        .constants = constants,
        .constant_count = 5,
        .index_handle = ibo.handle,
        .index_address = ibo.gpu_va + 64,
        .count = 6,
        .color = true};
    CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
    /* 2: the same shape on the CPU. */
    CHECK(!poulsback_frame_draw(w, &smooth,
                                &(struct poulsback_frame_vertices){3, cpu_vertices[0],
                                                                   &cpu_vertices[0][3],
                                                                   cpu_vertices[0] + 4, 8, 8, 8}));
    /* 3: flat, from the sequence, a 3-dword element. */
    d.element[1].dwords = 3;
    d.flat = true;
    d.index_handle = seq_handle;
    d.index_address = seq + 2 * 10;
    d.count = 3;
    CHECK(!poulsback_frame_draw_gpu(w, &flat, &d));
    /* 4: no inputs, one element, u32 indices. */
    d = (struct poulsback_frame_gpu_draw){.program = &position,
                                          .element_count = 1,
                                          .element = {{vbo.handle, vbo.gpu_va, 3, 0}},
                                          .constants = constants,
                                          .constant_count = 3,
                                          .index_handle = ibo.handle,
                                          .index_address = ibo.gpu_va + 128,
                                          .count = 3,
                                          .index32 = true};
    CHECK(!poulsback_frame_draw_gpu(w, &solid, &d));
    /* A mismatched colour flag, odd counts and unaligned indices are refused. */
    d.color = true;
    CHECK(poulsback_frame_draw_gpu(w, &solid, &d) == -EOPNOTSUPP);
    d.color = false;
    d.count = 4;
    CHECK(poulsback_frame_draw_gpu(w, &solid, &d) == -EINVAL);
    d.count = 3;
    d.index_address += 2;
    CHECK(poulsback_frame_draw_gpu(w, &solid, &d) == -EINVAL);
    d.index_address -= 2;
    /* A BO list that would overflow: refused, the frame unchanged. */
    for (unsigned i = 0; i < POULSBACK_FRAME_EXTRA_BOS; i++) {
        int ret;
        d.element[0].handle = many[i].handle;
        ret = poulsback_frame_draw_gpu(w, &solid, &d);
        CHECK(i < POULSBACK_FRAME_EXTRA_BOS - 2u ? !ret : ret == -EOPNOTSUPP); /* vbo, ibo listed */
        if (ret)
            break;
    }
    CHECK(!poulsback_frame_end(
        w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
        &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
    CHECK(listed(&job, vbo.handle) && listed(&job, ibo.handle) && listed(&job, stream.handle) &&
          listed(&job, seq_handle) && job.bo_count <= POULSBACK_SUBMIT_MAX_BOS);
    CHECK(listed(&job, many[POULSBACK_FRAME_EXTRA_BOS - 3u].handle) &&
          !listed(&job, many[POULSBACK_FRAME_EXTRA_BOS - 2u].handle));
    /* The TA stream: header, draws 1 (GPU, 44 bytes), 2 (CPU, 28), 3, 4 (GPU). */
    memcpy(header_words, ta_stream(&job), sizeof(header_words));
    ta = ta_stream(&job) + 56;
    memcpy(entry, ta, 44);
    memcpy(cpu_state, ta + 44, 8);
    CHECK(!memcmp(entry, cpu_state, 8)); /* one template */
    check_secondary(entry + 2, constants, 5);
    check_indexlist(entry + 6, 6, ibo.gpu_va + 64, 0,
                    (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                                           {vbo.handle, vbo.gpu_va + 32, 4, 32}},
                    2, &colour, 8);
    memcpy(entry, ta + 44 + 28, 44);
    CHECK(memcmp(entry, cpu_state, 8)); /* the flat template */
    check_secondary(entry + 2, constants, 5);
    check_indexlist(entry + 6, 3, seq + 20, 2u << 25,
                    (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                                           {vbo.handle, vbo.gpu_va + 32, 3, 32}},
                    2, &colour, 8);
    memcpy(entry, ta + 44 + 28 + 44, 44);
    check_secondary(entry + 2, constants, 3);
    check_indexlist(entry + 6, 3, ibo.gpu_va + 128, 1u << 24,
                    (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va, 3, 0}}, 1,
                    &position, 4);
    /* The flat draw's template iterates vertex 2 (DOUTI FLATSHADE 27:26 = 2). */
    CHECK(pds_holds(0x0bd0000fu));
    /* A backend program in two draws of one shape with other
     * uniforms: one template, each draw its own pixel state after it. */
    {
        static const uint32_t fs_words[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        const uint32_t u1[3] = {0, 0x3f800000u, 0x3e800000u}, u2[3] = {0, 0x3f000000u, 0};
        struct poulsback_frame_fragment f = {
            (UINT64_C(1) << 63) | 1u, fs_words, 2, 2, 0, 4, 1, {0x0dc0a00fu}, u1, 3};
        struct poulsback_frame_fragment
            g = f,
            h, none = {(UINT64_C(1) << 63) | 2u, NULL, 0, 0, 0, 0, 0, {0}, NULL, 0, 3,
                       {0x0bf00200u, 0}};
        uint32_t first[13], second[13], third[13], fourth[13], fifth[13];
        g.constants = u2;
        g.isp_groups = 7; /* ISPA, ISPB, ISPC */
        g.isp[0] = 0x0bd00340u, g.isp[1] = 0x000000f0u, g.isp[2] = 0x0e92ffffu;
        h = g;
        h.isp_groups = 0x3f; /* and the BF groups: a two-sided stencil */
        h.isp[0] = 0x0bd00b22u, h.isp[3] = 0x0bd00b11u, h.isp[4] = 0, h.isp[5] = 0x0e02ffffu;
        poulsback_frame_begin(w, &pds, &stream);
        d = (struct poulsback_frame_gpu_draw){
            .program = &colour,
            .fragment = &f,
            .element_count = 2,
            .element = {{vbo.handle, vbo.gpu_va + 16, 4, 32}, {vbo.handle, vbo.gpu_va + 32, 4, 32}},
            .constants = constants,
            .constant_count = 5,
            .index_handle = ibo.handle,
            .index_address = ibo.gpu_va + 64,
            .count = 6,
            .color = true};
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        d.fragment = &g;
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        d.fragment = &h;
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        d.fragment = &f; /* the first draw's state again */
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        d.fragment = &none; /* no colour written */
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        CHECK(!poulsback_frame_end(
            w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
            &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
        ta = ta_stream(&job) + 56;
        memcpy(first, ta, 52);
        memcpy(second, ta + 52, 52);
        memcpy(third, ta + 104, 52);
        memcpy(fourth, ta + 156, 52);
        memcpy(fifth, ta + 208, 52);
        check_pixel(fifth + 2, &none);
        /* the same program, iterations, constants and ISP words: the first
         * draw's state task, built once */
        CHECK(!memcmp(first, fourth, 16));
        CHECK(!memcmp(first, second, 8) && !memcmp(first, third, 8)); /* one template */
        check_pixel(first + 2, &f);
        check_pixel(second + 2, &g);
        check_pixel(third + 2, &h);
        CHECK(memcmp(first + 2, second + 2, 8)); /* two state tasks */
        check_secondary(first + 4, constants, 5);
        check_indexlist(
            first + 8, 6, ibo.gpu_va + 64, 0,
            (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                                   {vbo.handle, vbo.gpu_va + 32, 4, 32}},
            2, &colour, 8);
        /* One fragment twice, its constants changed in place between the
         * draws and its generation with them: two state tasks, the second
         * with the new constants (the last state is reused only while the
         * generation holds). */
        {
            uint32_t k[3] = {0, 0x3f800000u, 0};
            struct poulsback_frame_fragment e = f;
            e.constants = k;
            poulsback_frame_begin(w, &pds, &stream);
            d.fragment = &e;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            k[2] = 0x3f000000u;
            e.generation++;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d)); /* the last state again */
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            ta = ta_stream(&job) + 56;
            memcpy(first, ta, 52);
            memcpy(second, ta + 52, 52);
            memcpy(third, ta + 104, 52);
            CHECK(memcmp(first + 2, second + 2, 8) && !memcmp(second + 2, third + 2, 8));
            check_pixel(second + 2, &e);
        }
        /* Two frames: the second's first draw builds its state where the
         * first frame built f's, so f's draw after it builds its own: a memo
         * names its frame's state tasks alone. */
        poulsback_frame_begin(w, &pds, &stream);
        d.fragment = &f;
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        CHECK(!poulsback_frame_end(
            w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
            &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
        poulsback_frame_begin(w, &pds, &stream);
        d.fragment = &g;
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        d.fragment = &f;
        CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        CHECK(!poulsback_frame_end(
            w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
            &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
        ta = ta_stream(&job) + 56;
        memcpy(first, ta, 52);
        memcpy(second, ta + 52, 52);
        CHECK(memcmp(first + 2, second + 2, 8)); /* g's state task, then f's own */
        check_pixel(first + 2, &g);
        check_pixel(second + 2, &f);
        /* A flat draw of a backend program makes its frame F32 (the colour
         * from TC0): its group's template is not the primary family's that
         * the same draw without the program takes. */
        for (unsigned backend = 0; backend < 2; backend++) {
            poulsback_frame_begin(w, &pds, &stream);
            f.iteration[0] = 0x09d0000fu; /* TC0, F32, FLATSHADE vertex 2 */
            d.fragment = backend ? &f : NULL;
            d.flat = true;
            CHECK(!poulsback_frame_draw_gpu(w, &flat, &d));
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            ta = ta_stream(&job) + 56;
            memcpy(backend ? first : second, ta, backend ? 52 : 44);
        }
        CHECK(memcmp(first, second, 8)); /* the F32 template, not the primary one */
        check_pixel(first + 2, &f);
        /* A depth range: the draw's viewport in its record, so not the state
         * of the same draw without it. Both faces' ISP groups fit beside it
         * (16 dwords), not with the draw's own output selects too. */
        {
            static const uint32_t range[6] = {0x41800000u, 0x41800000u, 0x41800000u,
                                              0xc1800000u, 0x3f400000u, 0x3e800000u};
            struct poulsback_frame_fragment v = g, both = h;
            v.has_viewport = both.has_viewport = true;
            memcpy(v.viewport, range, sizeof(range));
            memcpy(both.viewport, range, sizeof(range));
            poulsback_frame_begin(w, &pds, &stream);
            d.flat = false;
            d.fragment = &g;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            d.fragment = &v;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            d.fragment = &both;
            d.tc_count = 1;
            d.tc[0] = 4;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EOPNOTSUPP);
            d.tc_count = 0;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            CHECK(poulsback_frame_draws(w) == 3); /* the refused draw is not one */
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            ta = ta_stream(&job) + 56;
            memcpy(first, ta, 52);
            memcpy(second, ta + 52, 52);
            memcpy(third, ta + 104, 52);
            CHECK(memcmp(first + 2, second + 2, 8));
            check_pixel(first + 2, &g);
            check_pixel(second + 2, &v);
            check_pixel(third + 2, &both);
            CHECK(!poulsback_frame_draws(w)); /* none open */
        }
        /* A frame of POULSBACK_FRAME_MAX_DRAWS draws: the next is refused (the
         * frame is not split), and the frame's PDS words fit. */
        poulsback_frame_begin(w, &pds, &stream);
        d.fragment = &g;
        for (uint32_t i = 0; i < POULSBACK_FRAME_MAX_DRAWS; i++)
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
        CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EOPNOTSUPP);
        CHECK(!poulsback_frame_end(
            w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
            &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
        /* Varyings: texture coordinates of the draw's own, TC0 of
         * two components and TC1 of four (10 dwords: 48-byte vertices, four
         * per partition), then TC0 of three (32 bytes); each record selects
         * its own, so the second draw is not the first's state. */
        {
            const uint32_t own[2] = {0x0a001000u, 1u | 7u << 3}, three[2] = {0x07001000u, 5u};
            poulsback_frame_begin(w, &pds, &stream);
            d.fragment = NULL;
            d.flat = false;
            d.tc_count = 1;
            d.tc[0] = 3;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EINVAL); /* no program */
            d.fragment = &f;
            d.tc[0] = 5;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EINVAL);
            d.tc_count = POULSBACK_FRAME_TCS + 1u;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EINVAL);
            f.iteration[0] = 0x0dc0a00fu;
            d.tc_count = 2;
            d.tc[0] = 2;
            d.tc[1] = 4;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            d.tc_count = 1;
            d.tc[0] = 3;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            ta = ta_stream(&job) + 56;
            memcpy(first, ta, 52);
            memcpy(second, ta + 52, 52);
            check_pixel_outputs(first + 2, &f, own);
            check_pixel_outputs(second + 2, &f, three);
            CHECK(memcmp(first + 2, second + 2, 8));
            check_indexlist(
                first + 8, 6, ibo.gpu_va + 64, 0,
                (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                                       {vbo.handle, vbo.gpu_va + 32, 4, 32}},
                2, &colour, 10);
            check_indexlist(
                second + 8, 6, ibo.gpu_va + 64, 0,
                (struct poulsback_frame_gpu_element[]){{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                                       {vbo.handle, vbo.gpu_va + 32, 4, 32}},
                2, &colour, 7);
            d.tc_count = 0;
        }
        /* Texture reads: the read's iteration a DOUTT of its words in the
         * pixel PDS, the BO sampled in the frame's list; other words another
         * state; a mask past the iterations or too many BOs refused. */
        {
            struct poulsback_frame_bo tex;
            struct poulsback_frame_fragment t = f;
            uint32_t zls = 0, plain = 0;
            for (uint32_t i = 0; i < job.raster_count; i++) /* the last frame's: no load */
                if (job.raster[i].offset == 0x480)
                    plain = job.raster[i].value;
            CHECK((plain & 0x004000d0u) == 0x004000d0u && !(plain & 0x00200026u));
            CHECK(!fake_alloc(NULL, POULSBACK_BO_CLASS_GENERAL, 4096, &tex));
            t.iteration_count = 2;
            t.iteration[1] = 0x0c00f100u; /* TEXISSUE TC0, TEXPROJ RHW, no USE issue */
            t.texture_mask = 2;
            t.texture[1][0] = 0x5a5a0001u, t.texture[1][1] = 0x5a5a0002u,
            t.texture[1][2] = 0x5a5a0003u;
            t.texture_bo_count = 1;
            t.texture_bo[0] = tex.handle;
            poulsback_frame_begin(w, &pds, &stream);
            d.fragment = &t;
            d.tc_count = 1;
            d.tc[0] = 2;
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            poulsback_frame_load(w); /* the render loads its depth and stencil */
            t.texture[1][2] = 0x5a5a0004u;
            t.generation++; /* its words changed */
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            t.texture_mask = 4;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EINVAL);
            t.texture_mask = 2;
            t.texture_bo_count = POULSBACK_FRAME_FRAGMENT_TEXTURES + 1u;
            CHECK(poulsback_frame_draw_gpu(w, &smooth, &d) == -EINVAL);
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            CHECK(listed(&job, tex.handle));
            /* ZLSCTL: the template's store, and the load (SLOADEN, ZLOADEN,
             * FORCEZLOAD, LOADMASK) */
            for (uint32_t i = 0; i < job.raster_count; i++)
                if (job.raster[i].offset == 0x480)
                    zls = job.raster[i].value;
            CHECK((zls & 0x004000d0u) == 0x004000d0u && (zls & 0x00200026u) == 0x00200026u);
            CHECK(pds_holds(0x5a5a0001u) && pds_holds(0x5a5a0002u) && pds_holds(0x5a5a0003u) &&
                  pds_holds(0x5a5a0004u));
            /* the loading render leaves the depth clear out: the mask entry,
             * then the draws */
            {
                uint32_t mask_entry[7], plain_entry[14];
                memcpy(mask_entry, ta_stream(&job), sizeof(mask_entry));
                memcpy(plain_entry, header_words, sizeof(plain_entry));
                CHECK(!memcmp(mask_entry, plain_entry, sizeof(mask_entry)));
                CHECK(memcmp(ta_stream(&job) + 28, plain_entry + 7, 28));
            }
            ta = ta_stream(&job) + 28;
            memcpy(first, ta, 52);
            memcpy(second, ta + 52, 52);
            CHECK(memcmp(first + 2, second + 2, 8)); /* two states */
            /* the target sampled (the load draw): listed once, as written */
            t.texture_bo[0] = target.handle;
            t.texture_bo_count = 1;
            poulsback_frame_begin(w, &pds, &stream);
            CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            {
                unsigned listings = 0;
                for (uint32_t i = 0; i < job.bo_count; i++)
                    listings += job.bos[i].handle == target.handle;
                CHECK(listings == 1);
            }
            d.tc_count = 0;
            d.fragment = &f;
        }
    }
    /* Frames whose draws each bring a new pixel program (relinked variants):
     * the places never run out mid-frame, the chunks are emptied at a
     * frame's first draw. */
    {
        static const uint32_t fs_words[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        unsigned idles = idle_calls;
        for (unsigned frame = 0; frame < 40; frame++) {
            poulsback_frame_begin(w, &pds, &stream);
            for (unsigned k = 0; k < 12; k++) {
                struct poulsback_frame_fragment fresh = {(UINT64_C(1) << 62) | (frame * 12u + k),
                                                         fs_words,
                                                         2,
                                                         2,
                                                         0,
                                                         4,
                                                         1,
                                                         {0x0dc0a00fu},
                                                         constants,
                                                         3};
                d = (struct poulsback_frame_gpu_draw){
                    .program = &colour,
                    .fragment = &fresh,
                    .element_count = 2,
                    .element = {{vbo.handle, vbo.gpu_va + 16, 4, 32},
                                {vbo.handle, vbo.gpu_va + 32, 4, 32}},
                    .constants = constants,
                    .constant_count = 5,
                    .index_handle = ibo.handle,
                    .index_address = ibo.gpu_va + 64,
                    .count = 6,
                    .color = true};
                CHECK(!poulsback_frame_draw_gpu(w, &smooth, &d));
            }
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
        }
        CHECK(idle_calls > idles); /* emptied, between frames */
    }
    /* A CPU-shaded draw with ISP words of its own (the stencil
     * object): the group's STATE, the draw's after it, then the rest of the
     * entry as without them. */
    {
        static const uint32_t isp[6] = {0x01f00307u, 0, 0x0e02ffffu};
        struct poulsback_frame_vertices in = {
            3, cpu_vertices[0], &cpu_vertices[0][3], cpu_vertices[0] + 4, 8, 8, 8, 0, NULL};
        uint32_t plain[7], own[9];
        for (unsigned k = 0; k < 2; k++) {
            poulsback_frame_begin(w, &pds, &stream);
            in.isp_groups = k ? 7u : 0u, in.isp = k ? isp : NULL;
            CHECK(!poulsback_frame_draw(w, &smooth, &in));
            CHECK(!poulsback_frame_end(
                w, &(struct poulsback_frame_buffer){target.handle, 256, target.gpu_va, target.size},
                &(struct poulsback_frame_buffer){depth.handle, 0, depth.gpu_va, depth.size}, &job));
            memcpy(k ? own : plain, ta_stream(&job) + 56, k ? 36 : 28);
        }
        CHECK(!memcmp(plain, own, 8) && !memcmp(plain + 2, own + 4, 20));
        check_isp_state(own + 2, 7, isp);
        in.isp_groups = 6; /* no ISPA */
        poulsback_frame_begin(w, &pds, &stream);
        CHECK(poulsback_frame_draw(w, &smooth, &in) == -EINVAL);
        poulsback_frame_abort(w);
    }
    poulsback_frame_writer_destroy(w);
    printf("poulsback-frame-gpu: %u checks\n", checks);
    return 0;
}
