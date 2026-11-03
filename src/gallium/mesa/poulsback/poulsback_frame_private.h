// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The frame writer's own state (poulsback_frame.h), shared by its two files:
 * poulsback_frame.c (frame state, templates, CPU draws, the frame's end) and
 * poulsback_frame_gpu.c (program places, pixel state, GPU draws). */
#ifndef POULSBACK_FRAME_PRIVATE_H
#define POULSBACK_FRAME_PRIVATE_H
#include "poulsback_frame.h"
#include "poulsback_bo_uapi.h"
#include "sgx535_depth_window_mesh.h"
#include "sgx535_mte_state.h"
#include <stdbool.h>
#include <stdint.h>

/* The engine's fixed layout (kernel/engine/poulsback_engine.c): the kernel owns
 * the scene buffer, DPM tables and parameter buffer and programs them itself;
 * the builders only need them placed where the engine has them. No user word
 * refers to them. */
#define KERNEL_STORAGE (UINT64_C(1) << 32)
#define PARAMETER_VA 0x30000000u
#define PARAMETER_BYTES 0x02000000u
#define TABLES_VA 0x48000000u
#define ZLSCTL 0x480u /* ISP_ZLSCTL in the raster stage */
#define SCENE_VA 0x4c000000u
#define CODE_VA 0x40000000u
#define PDS_EXEC_BASE 0x20000000u
/* Stand-ins for the frame state's one bind: the target and depth of each
 * frame are relocated per frame (PBE word, depth registers). */
#define TARGET_STAND_IN 0xc0000000u
#define DEPTH_STAND_IN 0x50000000u

/* The ordered TA stream: the mask and clear draws, 28 bytes per draw, then
 * the last-task STATE and TERMINATE (sgx535_depth_window_mesh.c). */
#define TA_HEADER 56u
#define TA_DEPTH_CLEAR 28u /* the header's second entry: the depth clear */
#define TA_ENTRY 28u
#define TA_TRAILER 12u
#define TA_COUNT 8u /* INDEXLIST word 0: 0x81400000 | index count */
/* A GPU draw's entry: the template's STATE, then TA_GPU_TAIL words: a
 * SECONDARY STATE per USE pipe and the INDEXLIST (words 0, 1, 2, 4, 5). */
#define TA_STATE 8u
#define TA_GPU_TAIL 9u
#define TA_GPU_ENTRY (TA_STATE + 4u * TA_GPU_TAIL)
/* A vertex PDS section holds the draw's program, then the mask draw's. */
#define VERTEX_PDS_DRAW 64u
/* A frame's foreground indices start after the six mask indices. */
#define FOREGROUND_INDICES 12u

/* Frame-state buffers: the roles of one bound single-draw frame. */
enum { FS_PDS, FS_USE_VERTEX, FS_USE_FRAGMENT, FS_VERTICES, FS_BACKGROUND, FS_BOS };
/* Arenas of draw programs. A code chunk is one 32 KiB block: a PDS links to a
 * USE program in the block its owner starts in (sgx535_pds_use_link.c). */
enum { ARENA_PDS, ARENA_USE_VERTEX, ARENA_USE_FRAGMENT, ARENAS };
#define ARENA_CHUNKS 4u
/* GPU draws' vertex programs: chunks of one 32 KiB block each, placed once per
 * program for the writer's life (a full set empties between frames). The
 * places hold a frame's reserve of two per draw (programs_room) beside the
 * programs placed before it. */
#define PROGRAM_CHUNKS 4u
#define PROGRAM_CHUNK 32768u
#define PROGRAMS 512u
_Static_assert(PROGRAMS > 2u * POULSBACK_FRAME_MAX_DRAWS, "a frame's reserve of places");
#define NOP_END_ID UINT64_MAX
#define STATE_EMIT_ID (UINT64_MAX - 1u) /* the TA state records' copy-and-emit program */
/* SUBMIT's BO list: the slot's two, the frame state's, the index buffer, the
 * arenas' chunks, the program chunks, the GPU draws' buffers, then depth and
 * target (both written). */
enum { JOB_SLOT_PDS, JOB_SLOT_VERTICES, JOB_FS, JOB_INDICES = JOB_FS + FS_BOS, JOB_ARENAS };
_Static_assert(JOB_ARENAS + ARENAS * ARENA_CHUNKS + PROGRAM_CHUNKS + POULSBACK_FRAME_EXTRA_BOS +
                       2u <=
                   POULSBACK_SUBMIT_MAX_BOS,
               "BO list");

#define STATES 4u
#define GROUPS 256u
#define EVENT_BYTES 256u
#define PBE_BYTES 16u

/* A relocation resolved per draw: the draw's vertices or vertex PDS. */
enum late_kind { LATE_VERTICES, LATE_VERTEX_PDS };
struct late {
    struct sgx535_task_packet_reloc r;
    uint32_t at, delta, kind;
};
#define LATES 4u
struct template
{
    bool built;
    uint8_t vertex_pds[VERTEX_PDS_DRAW], entry[TA_ENTRY];
    struct late vertex_pds_late[LATES], entry_late[LATES];
    uint32_t vertex_pds_lates, entry_lates, count_base;
};
struct group {
    poulsback_frame_shape shape; /* fragment and uniforms point at the copies */
    uint32_t key[64], key_words;
    struct sgx535_fragment_instruction fragment[SGX535_NIR_MAX_INSTRUCTIONS];
    uint32_t uniforms[SGX535_FRAGMENT_UNIFORM_WORDS];
    uint32_t stride; /* vertex bytes: 16 without inputs, else 32 */
    bool inputs, flat, normalized, clip;
    bool gpu_flat;        /* a GPU draw's flat colour: the fragment PDS iterates vertex 2 */
    struct template t[2]; /* the legacy (primary colour) and generic F32 families */
};
struct arena {
    struct poulsback_frame_bo bo[ARENA_CHUNKS];
    uint32_t count, current, used; /* chunks allocated; the one being filled */
};
struct state {
    uint32_t version, width, height, clear, no_depth, stride;
    uint64_t depth_bytes;
    struct group *groups[GROUPS];
    uint32_t group_count;
    bool built;
    struct poulsback_frame_bo bo[FS_BOS];
    struct arena arena[ARENAS];
    /* The bound single-draw frame: its source view names every section. */
    struct sgx535_depth_window_mesh_ordered *legacy;
    struct sgx535_depth_window_mesh_f32_ordered *f32;
    const struct sgx535_depth_window_mesh_ordered_view *view;
    uint8_t ta_header[TA_HEADER], ta_trailer[TA_TRAILER];
    uint8_t event[EVENT_BYTES], pbe[PBE_BYTES];
    uint32_t event_bytes;
    struct late event_pbe[LATES];
    uint32_t event_pbes;
    struct sgx535_task_packet_reloc pbe_target;
    uint32_t pbe_target_at;
    struct poulsback_submit_reg ta[POULSBACK_SUBMIT_MAX_REGS], raster[POULSBACK_SUBMIT_MAX_REGS];
    uint32_t ta_count, raster_count, ta_stream_reg, event_reg, depth_regs;
    uint64_t last_use;
};
struct draw {
    struct group *g;
    uint32_t offset, count;
    bool gpu, pixel; /* pixel: the draw's own TA state follows the group's */
    uint32_t tail[TA_GPU_TAIL], state[2];
};
struct program_place {
    uint64_t id;
    uint32_t chunk, offset;
};
/* A pixel state of the open frame by what makes it (pixel_state): a draw
 * that has an earlier draw's names that draw's state task, as EMGD sends a
 * state only when it changes. */
#define PIXEL_STATES 4u
#define MEMO_KEY                                                                                   \
    (10u + POULSBACK_FRAME_FRAGMENT_ITERATIONS + 6u + 128u + 2u +                                  \
     3u * POULSBACK_FRAME_FRAGMENT_ITERATIONS + POULSBACK_FRAME_FRAGMENT_TEXTURES + 7u)
struct pixel_memo {
    uint32_t key[MEMO_KEY], words, state[2];
};
struct poulsback_frame_writer {
    struct poulsback_frame_bo_ops ops;
    struct poulsback_frame_bo indices;
    struct state *states[STATES];
    uint64_t frames;
    struct poulsback_frame_bo program[PROGRAM_CHUNKS];
    uint32_t program_chunks, program_current, program_used, places;
    struct program_place place[PROGRAMS];
    /* The open frame. */
    bool open, f32, load;
    struct state *fs;
    struct poulsback_frame_bo pds, vertices;
    uint32_t draw_count, vertex_bytes, gpu_pds_bytes, extra_count;
    uint32_t extra[POULSBACK_FRAME_EXTRA_BOS];
    struct draw draws[POULSBACK_FRAME_MAX_DRAWS];
    struct pixel_memo memo[PIXEL_STATES];
    uint32_t memos, memo_next;
    /* the frame's last pixel state, its fragment's id and generation and its
     * outputs: a draw that has them all sends it again with no key built */
    const struct poulsback_frame_fragment *last_fragment;
    uint64_t last_id;
    uint32_t last_generation, last_outputs[2], last_state[2];
    bool last_outputs_set;
};

static inline uint32_t align(uint32_t n, uint32_t a)
{
    return (n + a - 1u) & ~(a - 1u);
}
static inline uint32_t code_bank(uint64_t va, uint32_t data_master)
{
    return 3u + 2u * (uint32_t)((va - CODE_VA) / 0x80000u) + data_master;
}
/* poulsback_frame.c: the frame state for SHAPE, and its group for SHAPE
 * (GPU_FLAT: a GPU draw's flat colour); NULL without memory or room. */
struct state *poulsback_frame_state_get(struct poulsback_frame_writer *w,
                                        const poulsback_frame_shape *s);
struct group *poulsback_frame_group_get(struct poulsback_frame_writer *w, struct state *fs,
                                        const poulsback_frame_shape *s, bool gpu_flat);
/* poulsback_frame_gpu.c: a CPU draw's own ISP state record (IN's ISP groups)
 * into X. */
int poulsback_frame_cpu_state(struct poulsback_frame_writer *w,
                              const struct poulsback_frame_vertices *in, struct draw *x);
#endif
