// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The frame writer (docs/graphics.md#frame-writer): a frame's GPU words written
 * per draw from state built once.
 *
 * - Frame state, per (size, clear colour, depth or none): the background
 *   object, the clear and terminate programs, the pixel event, the TA stream's
 *   header and trailer and the TA/raster register tables, built once by the
 *   window-mesh builders into the state's own buffers.
 * - A draw template, per draw state (fragment program, uniforms, interpolation,
 *   window and sample state) and frame family: its fragment program, fragment,
 *   state and MTE programs in the state's arenas, and the draw's vertex PDS and
 *   TA entry with every address but the per-draw ones resolved.
 * - Per draw: the post-VS vertices, written once into the frame's vertex
 *   buffer, the draw half of the vertex PDS and a 28-byte TA entry.
 * - Per GPU-shaded draw (poulsback_frame_draw_gpu): the vertex
 *   program, placed once in the writer's code chunks; the draw's vertex PDS
 *   (an attribute DMA per element from the buffers in place) and SECONDARY PDS
 *   (its constant block, written into the frame's stream, DMAed into SA); a
 *   44-byte TA entry: the template's STATE, a SECONDARY STATE per USE pipe and
 *   the INDEXLIST, which reads the draw's index buffer in place.
 *
 * The words equal those of the ordered constructor
 * (sgx535_depth_window_mesh_ordered_create/bind) for the same draws, after
 * relocation (test_frame.c compares them). The writer issues no
 * ioctl: buffers come from the caller's ops, and a frame ends in a SUBMIT
 * description. Single-threaded, like the context that owns it. */
#ifndef POULSBACK_FRAME_H
#define POULSBACK_FRAME_H
#include <stdbool.h>
#include <stdint.h>
#include "poulsback_exec_uapi.h"
#include "sgx535_depth_window_mesh_single_sample.h"

/* Limits of one frame: its draws (GPU draws and frame objects; each a vertex
 * PDS program and a TA entry in the PDS buffer's low region, below
 * POULSBACK_FRAME_GPU_PDS), and a CPU draw's vertices, the ordered
 * constructor's. A draw past them is refused; the frame is not split. */
#define POULSBACK_FRAME_MAX_DRAWS 160u
#define POULSBACK_FRAME_MAX_VERTICES 4096u
/* A frame's own buffers (one set per frame in flight): the PDS words (TA
 * stream, pixel event, per-draw programs; GPU draws' from
 * POULSBACK_FRAME_GPU_PDS on) and the stream (CPU vertices, client arrays,
 * converted indices, constant blocks). */
#define POULSBACK_FRAME_PDS_BYTES 65536u
#define POULSBACK_FRAME_GPU_PDS 32768u
#define POULSBACK_FRAME_VERTEX_BYTES (1u << 20)
/* glDrawArrays' indices: 0, 1, ... in the writer's u16 sequence. */
#define POULSBACK_FRAME_SEQUENCE 65536u

/* A CPU-mapped (write-combined) buffer at a fixed GPU address. */
struct poulsback_frame_bo {
    uint32_t handle;
    uint64_t gpu_va, size;
    uint8_t *map;
};
struct poulsback_frame_bo_ops {
    void *data;
    int (*alloc)(void *data, uint32_t bo_class, uint64_t bytes, struct poulsback_frame_bo *out);
    void (*free)(void *data, struct poulsback_frame_bo *);
    /* Waits until no submitted job reads the writer's buffers. */
    int (*idle)(void *data);
};

/* A draw's state: a window-mesh request without vertices, indices or counts.
 * Its fragment and uniform arrays are read during poulsback_frame_draw only. */
typedef struct sgx535_depth_window_mesh_single_sample_request poulsback_frame_shape;

/* A draw's post-VS vertices, strides in words: the position (window XYZ and
 * 1/W, or clip XYZW for SGX535_WINDOW_MESH_CLIP_VERSION; 4 words), clip W (one
 * word: the frame family) and, for a shape with inputs, RGBA. */
struct poulsback_frame_vertices {
    uint32_t count;
    const uint32_t *window, *clip_w, *rgba;
    uint32_t window_stride, clip_stride, rgba_stride;
    /* ISP words of the draw's own (a clear object), as
     * poulsback_frame_fragment's; 0: the group's */
    uint32_t isp_groups;
    const uint32_t *isp;
};

/* A target or depth buffer of a frame. */
struct poulsback_frame_buffer {
    uint32_t handle, stride;
    uint64_t gpu_va, size;
};

/* SUBMIT's variable part: the BO list and both register tables. */
struct poulsback_frame_job {
    uint32_t width, height, bo_count, ta_count, raster_count;
    struct poulsback_submit_bo bos[POULSBACK_SUBMIT_MAX_BOS];
    struct poulsback_submit_reg ta[POULSBACK_SUBMIT_MAX_REGS];
    struct poulsback_submit_reg raster[POULSBACK_SUBMIT_MAX_REGS];
};

struct poulsback_frame_writer;
int poulsback_frame_writer_create(const struct poulsback_frame_bo_ops *,
                                  struct poulsback_frame_writer **);
/* The caller has waited for every job of the writer. */
void poulsback_frame_writer_destroy(struct poulsback_frame_writer *);

/* Opens a frame into one frame slot's buffers (PDS class, POULSBACK_FRAME_PDS_BYTES;
 * GENERAL, POULSBACK_FRAME_VERTEX_BYTES), which no running job reads. */
void poulsback_frame_begin(struct poulsback_frame_writer *, const struct poulsback_frame_bo *pds,
                           const struct poulsback_frame_bo *vertices);
bool poulsback_frame_open(const struct poulsback_frame_writer *);
/* One draw of the open frame: finds or creates its template set and writes
 * its vertices. A refusal leaves the frame as it was. */
int poulsback_frame_draw(struct poulsback_frame_writer *, const poulsback_frame_shape *,
                         const struct poulsback_frame_vertices *);
/* Bytes of a 32-bit depth buffer: sgx535_depth_prepare's allocation, both
 * axes padded to 32. */
uint64_t poulsback_frame_depth_bytes(uint32_t width, uint32_t height);
/* The open frame's size and depth storage, once it has a draw. */
int poulsback_frame_extent(const struct poulsback_frame_writer *, uint32_t *width, uint32_t *height,
                           uint64_t *depth_bytes);
/* Writes the frame's TA stream and per-frame words and describes its SUBMIT
 * into TARGET with DEPTH. The frame is closed either way. */
int poulsback_frame_end(struct poulsback_frame_writer *,
                        const struct poulsback_frame_buffer *target,
                        const struct poulsback_frame_buffer *depth, struct poulsback_frame_job *);
/* A GPU-shaded draw. Addresses lie in BOs the caller names: the writer lists
 * them in the frame's SUBMIT (at most POULSBACK_FRAME_EXTRA_BOS per frame). */
#define POULSBACK_FRAME_GPU_ELEMENTS 8u
#define POULSBACK_FRAME_EXTRA_BOS 32u
struct poulsback_frame_gpu_program {
    uint64_t id; /* nonzero, unique per program for the writer's life */
    const uint32_t *words;
    uint32_t instruction_count, temps;
};
struct poulsback_frame_gpu_element {
    uint32_t handle;  /* the BO holding the data */
    uint64_t address; /* vertex 0's first dword: vertex I's at + I × stride */
    uint32_t dwords, stride;
};
/* A GPU draw's fragment program from the backend: its USE
 * words, linked for the draw's state; its iterations as DOUTI words in PA
 * order (the writer marks the last issue; a colour from TC0, F32: a frame
 * with such a draw is F32); PRIMARY iterated dwords; the
 * pixel SECONDARY block, sa0 first. The writer builds the draw's pixel PDS
 * program and SECONDARY program and sends them in a TA state record after
 * the group's, so the uniforms are the draw's data, not its group's. */
#define POULSBACK_FRAME_FRAGMENT_ITERATIONS 12u
#define POULSBACK_FRAME_FRAGMENT_TEXTURES 8u
#define POULSBACK_FRAME_TCS 10u
struct poulsback_frame_fragment {
    uint64_t id; /* nonzero, unique per program for the writer's life, apart from the vertex ids */
    const uint32_t *words;
    /* 0: no pixel program, for a draw that writes no colour (no tag), then
     * without iterations or constants */
    uint32_t instruction_count, temps, phase2, primary;
    uint32_t iteration_count;
    uint32_t iteration[POULSBACK_FRAME_FRAGMENT_ITERATIONS];
    const uint32_t *constants;
    uint32_t constant_count;
    /* the draw's ISP state groups, sent with its pixel state: bit n of
     * ISP_GROUPS is group n of the TA state record (ISPA, ISPB, ISPC, BF-ISPA,
     * BF-ISPB, BF-ISPC), ISP[n] its word; 0: the group's */
    uint32_t isp_groups;
    uint32_t isp[6];
    /* non-dependent texture reads: bit k of TEXTURE_MASK marks iteration k,
     * TEXTURE[k] its DOUTT0-2; TEXTURE_BO the BOs the draw samples (dependent
     * reads' too), put in the frame's list */
    uint32_t texture_mask;
    uint32_t texture[POULSBACK_FRAME_FRAGMENT_ITERATIONS][3];
    uint32_t texture_bo_count;
    uint32_t texture_bo[POULSBACK_FRAME_FRAGMENT_TEXTURES];
    /* HAS_VIEWPORT: the draw's viewport, the MTE's words (Tx, Sx, Ty, Sy, Tz,
     * Sz), sent with its pixel state after the group's STATE (the window's);
     * refused where the record would pass sixteen dwords (both faces' ISP
     * groups and the draw's own output selects) */
    bool has_viewport;
    uint32_t viewport[6];
    /* changes whenever the words above or the constants' values do: the
     * writer reuses the last draw's pixel state while it holds */
    uint32_t generation;
};
struct poulsback_frame_gpu_draw {
    const struct poulsback_frame_gpu_program *program;
    const struct poulsback_frame_fragment *fragment; /* NULL: the group's program */
    uint32_t element_count;
    struct poulsback_frame_gpu_element element[POULSBACK_FRAME_GPU_ELEMENTS];
    const uint32_t *constants; /* the SA block, sa0 first */
    uint32_t constant_count;
    uint32_t index_handle; /* the BO holding the indices */
    uint64_t index_address;
    uint32_t count; /* indices: a triangle list */
    bool index32, color, flat;
    /* The texture coordinates after POSITION when they are not the group's
     * (TC0 of four with COLOR, else none): TC[t] components (1-4) for each of
     * TC_COUNT, a backend program's varyings; its state record selects them. */
    uint32_t tc_count;
    uint8_t tc[POULSBACK_FRAME_TCS];
};
/* One GPU draw of the open frame (SHAPE as for poulsback_frame_draw, without
 * vertices). -ENOSPC: the writer's code chunks or BO list are full for this
 * frame; nothing changed. A refusal leaves the frame as it was. */
int poulsback_frame_draw_gpu(struct poulsback_frame_writer *, const poulsback_frame_shape *,
                             const struct poulsback_frame_gpu_draw *);
/* BYTES at ALIGNMENT in the open frame's stream, for a draw of this frame. */
int poulsback_frame_stream(struct poulsback_frame_writer *, uint32_t bytes, uint32_t alignment,
                           void **cpu, uint64_t *address, uint32_t *handle);
/* The u16 sequence 0, 1, ..., POULSBACK_FRAME_SEQUENCE - 1. */
void poulsback_frame_sequence(const struct poulsback_frame_writer *, uint64_t *address,
                              uint32_t *handle);
/* Drops the open frame's draws; it stays open (a full clear replaces them). */
void poulsback_frame_discard(struct poulsback_frame_writer *);
/* The open frame's render loads the depth and stencil its attachment stored:
 * it does not start with a full clear. A discard or the next frame ends it. */
void poulsback_frame_load(struct poulsback_frame_writer *);
/* The open frame's draws (GPU draws and frame objects), 0 when none is open. */
uint32_t poulsback_frame_draws(const struct poulsback_frame_writer *);
/* Drops the open frame. */
void poulsback_frame_abort(struct poulsback_frame_writer *);
#endif
