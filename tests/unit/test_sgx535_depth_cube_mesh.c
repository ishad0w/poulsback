// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#define POULSBACK_DEPTH_WINDOW_MESH_MAIN depth_cube_legacy_main
#include "test_sgx535_depth_window_mesh.c"
#undef POULSBACK_DEPTH_WINDOW_MESH_MAIN
#include "depth_cube_reference.h"

static void cube_vertices(struct sgx535_window_mesh_vertex *v, uint32_t *indices,
                          uint32_t clip_words[36][4], unsigned order, unsigned width,
                          unsigned height)
{
    for (unsigned i = 0; i < DEPTH_CUBE_VERTICES; i++) {
        unsigned triangle = order == 1 ? 11u - i / 3u : i / 3u;
        unsigned corner = i % 3u;
        if (order == 2 && corner)
            corner = 3u - corner;
        float clip[4], color[4], window_words[4];
        depth_cube_input(triangle * 3u + corner, clip, color);
        window_words[0] = clip[0] * ((float)width * .5f) + (float)width * .5f;
        window_words[1] = clip[1] * ((float)height * -.5f) + (float)height * .5f;
        window_words[2] = clip[2] * .5f + .5f;
        window_words[3] = 1.f;
        memcpy(clip_words[i], clip, 16);
        memcpy(v[i].window_xyz_reciprocal_w, window_words, 16);
        memcpy(v[i].rgba, color, 16);
        indices[i] = i;
    }
}
static void cube_save(const char *directory, unsigned format, unsigned order, unsigned width,
                      unsigned height, const char *suffix, const void *data, size_t bytes)
{
    char name[96];
    int n =
        width == 16 && height == 16
            ? snprintf(name, sizeof(name), "cube-f%u-o%u.%s.original.bin", format, order, suffix)
            : snprintf(name, sizeof(name), "cube-w%u-h%u-f%u-o%u.%s.original.bin", width, height,
                       format, order, suffix);
    CHECK(n > 0 && (size_t)n < sizeof(name));
    save(directory, name, data, bytes);
}
int main(int argc, char **argv)
{
    const unsigned capacity = 4096, scratch_capacity = 8192;
    CHECK(argc == 1 || argc == 2);
    CHECK(depth_cube_legacy_main(1, argv) == 0);
    struct sgx535_window_mesh_vertex vertices[36];
    uint32_t indices[36], clip[36][4];
    struct sgx535_fragment_instruction *ir = calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(*ir));
    struct sgx535_depth_window_mesh_workspace *work = calloc(1, sizeof(*work));
    struct sgx535_depth_window_mesh_task *task = malloc(sizeof(*task)), *old = malloc(sizeof(*old));
    struct sgx535_depth_window_mesh_bound *bound = malloc(sizeof(*bound)),
                                          *saved = malloc(sizeof(*saved));
    struct sgx535_window_mesh_task *literal = malloc(sizeof(*literal));
    uint8_t *geometry = malloc(capacity), *scratch = malloc(scratch_capacity),
            *expected = malloc(capacity), *old_geometry = malloc(capacity);
    CHECK(ir && work && task && old && bound && saved && literal && geometry && scratch &&
          expected && old_geometry);
    static const unsigned dimensions[][2] = {{16, 16}, {64, 64}, {1280, 800}};
    for (unsigned size = 0; size < 3; size++)
        for (unsigned format = 1; format <= 2; format++)
            for (unsigned order = 0; order < (size == 2 ? 1u : 3u); order++) {
                unsigned width = dimensions[size][0], height = dimensions[size][1];
                struct sgx535_depth_window_mesh_request r =
                    depth_request(vertices, indices, ir, 36, 36, width, height, format);
                struct sgx535_depth_window_mesh_binding bind;
                r.scene.packed_background_argb = 0xff000000u;
                r.scene.foreground.order = SGX535_WINDOW_MESH_DEINDEXED;
                r.scene.foreground.window.min_samples = 1;
                r.scene.foreground.window.viewport_scale_f32[0] =
                    r.scene.foreground.window.viewport_translate_f32[0] = f32(width / 2u);
                r.scene.foreground.window.viewport_scale_f32[1] = f32(height / 2u) | 0x80000000u;
                r.scene.foreground.window.viewport_translate_f32[1] = f32(height / 2u);
                cube_vertices(vertices, indices, clip, order, width, height);
                memset(geometry, 0xa5, capacity);
                CHECK(sgx535_depth_window_mesh_build(&r, task, geometry, capacity, work, scratch,
                                                     scratch_capacity) == SGX535_TASK_BLOCKED);
                depth_binding(task, &bind);
                uint32_t bytes = task->seed.foreground.geometry.bytes;
                CHECK(bytes == 1300 && task->depth.allocation_bytes ==
                                           (uint64_t)((width + 31u) & ~31u) *
                                               ((height + 31u) & ~31u) * (format == 1 ? 2u : 4u));
                CHECK(sgx535_depth_window_mesh_bind(&r, task, geometry, bytes, &bind, bound, work,
                                                    scratch,
                                                    scratch_capacity) == SGX535_TASK_BLOCKED);
                complete_oracle(&r, task, geometry, literal, expected, bound);
                CHECK(task->seed.foreground.vertex_count == 36 &&
                      task->seed.foreground.index_count == 36);
                CHECK(task->seed.foreground.fragment_code_bytes == 144);
                for (unsigned i = bytes; i < capacity; i++)
                    CHECK(geometry[i] == 0xa5);
                if (argc == 2) {
                    cube_save(argv[1], format, order, width, height, "clip", clip, sizeof(clip));
                    cube_save(argv[1], format, order, width, height, "vertices", vertices,
                              sizeof(vertices));
                    cube_save(argv[1], format, order, width, height, "indices", indices,
                              sizeof(indices));
                    cube_save(argv[1], format, order, width, height, "geometry", geometry, bytes);
                    cube_save(argv[1], format, order, width, height, "task", task, sizeof(*task));
                    cube_save(argv[1], format, order, width, height, "bound", bound,
                              sizeof(*bound));
                    cube_save(argv[1], format, order, width, height, "pds", bound->image.pds,
                              sizeof(bound->image.pds));
                }
                *old = *task;
                *saved = *bound;
                memcpy(old_geometry, geometry, capacity);
                for (unsigned i = 0; i < 36; i++) {
                    indices[i] = 36;
                    CHECK(sgx535_depth_window_mesh_build(&r, task, geometry, capacity, work,
                                                         scratch,
                                                         scratch_capacity) != SGX535_TASK_BLOCKED);
                    CHECK(!memcmp(task, old, sizeof(*task)) &&
                          !memcmp(geometry, old_geometry, capacity));
                    indices[i] = i;
                }
                /* The last face participates in every sleeping source recheck. */
                uint32_t value = vertices[35].rgba[2];
                drift_at = &vertices[35].rgba[2];
                CHECK(sgx535_depth_window_mesh_build(&r, task, geometry, capacity, work, scratch,
                                                     scratch_capacity) == SGX535_TASK_UNSUPPORTED);
                CHECK(!drift_at && !memcmp(task, old, sizeof(*task)) &&
                      !memcmp(geometry, old_geometry, capacity));
                vertices[35].rgba[2] = value;
                for (unsigned i = 0; i < 36; i++) {
                    geometry[i * 32u + 31u] ^= 1u;
                    CHECK(sgx535_depth_window_mesh_validate(&r, task, geometry, bytes, work,
                                                            scratch, scratch_capacity) ==
                          SGX535_TASK_INVALID);
                    geometry[i * 32u + 31u] ^= 1u;
                }
                fail_alloc = 1;
                CHECK(sgx535_depth_window_mesh_bind(&r, task, geometry, bytes, &bind, bound, work,
                                                    scratch,
                                                    scratch_capacity) == SGX535_TASK_NOMEM);
                fail_alloc = 0;
                CHECK(!memcmp(bound, saved, sizeof(*bound)));
                CHECK(sgx535_depth_window_mesh_build(&r, task, geometry, bytes - 1u, work, scratch,
                                                     scratch_capacity) == SGX535_TASK_INVALID);
                CHECK(!memcmp(task, old, sizeof(*task)) &&
                      !memcmp(geometry, old_geometry, capacity));
                CHECK(sgx535_depth_window_mesh_native_admission(bound) == SGX535_TASK_UNSUPPORTED);
            }
    free(old_geometry);
    free(expected);
    free(scratch);
    free(geometry);
    free(literal);
    free(saved);
    free(bound);
    free(old);
    free(task);
    free(work);
    free(ir);
    printf(
        "cube depth mesh: %u checks; 16x16/64x64 plus 1280x800 construction; 12 triangles/36 vertices; D16/D24S8; CPU only\n",
        checks);
    return 0;
}
