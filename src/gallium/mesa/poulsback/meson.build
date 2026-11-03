# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

# The poulsback Gallium driver (docs/graphics.md) and its host fixtures.
libpoulsback = static_library(
  'poulsback', ['poulsback_winsys.c', 'poulsback_screen.c', 'poulsback_image.c',
               'poulsback_draw.c', 'poulsback_color0_task.c', 'poulsback_scene_samples.c',
               'poulsback_engine.c',
               'poulsback_frame.c',
               'poulsback_program.c', 'poulsback_context.c', 'poulsback_context_state.c',
               'poulsback_nir.c',
               'poulsback_surface.c', 'poulsback_sampler.c',
               'poulsback_resource.c',
               'poulsback_slab.c', 'poulsback_vertex.c',
               'poulsback_fence.c', 'poulsback_lost.c',
               'poulsback_fragment.c',
               'poulsback_context_objects.c',
               'poulsback_context_textures.c',
               'poulsback_frame_gpu.c',
               'compiler/usse.c', 'compiler/iterator.c', 'compiler/pack.c',
               'compiler/texture2d.c',
               'compiler/pds.c', 'compiler/pds_fragment.c', 'compiler/pds_secondary.c', 'compiler/bitwise.c', 'compiler/fragment.c',
               'compiler/fragment_nir_adapter.c',
               'compiler/vertex.c', 'compiler/vertex_nir_adapter.c',
               'compiler/bool32_nir.c',
               'compiler/compare_nir.c',
               'compiler/fragment_raw_sign_normalized_nir.c',
               'raster/sgx535_task_packet.c',
               'raster/sgx535_window_vertex.c',
               'raster/sgx535_draw_mask.c', 'raster/sgx535_zls.c',
               'raster/sgx535_task_launch.c', 'raster/sgx535_background_object.c', 'raster/sgx535_background_pair.c',
               'raster/sgx535_clip_w1.c',
               'raster/sgx535_engine121.c',
               'raster/sgx535_task_image.c',
               'raster/sgx535_pbe_state.c',
               'raster/sgx535_pixel_event.c',
               'raster/sgx535_region_clip.c',
               'raster/sgx535_heap_layout.c',
               'compiler/sgx535_vertex_dma.c',
               'compiler/sgx535_vertex_pds.c',
               'compiler/sgx535_vertex_use.c',
               'compiler/sgx535_pds_use_link.c',
               'compiler/sgx535_use_code_bank.c',
               'raster/sgx535_varying_packet.c',
               'raster/sgx535_compiled_color0_packet.c',
               'compiler/fragment_normalized.c',
               'raster/sgx535_normalized_color0_task.c',
               'raster/sgx535_normalized_color0_mask_list.c',
               'raster/sgx535_window_mesh.c',
               'raster/sgx535_depth_state.c', 'raster/sgx535_depth_clear.c',
               'raster/sgx535_isp_words.c',
               'raster/sgx535_depth_window_mesh.c',
               'raster/sgx535_texture_layout.c',
               'compiler/use_encode.c', 'compiler/fs_emit.c', 'compiler/fs_epilogue.c',
               'compiler/fs_nir.c',
               'raster/sgx535_texture_words.c',
               'raster/sgx535_pixel_pds.c',
               'raster/sgx535_mte_state.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux,
                         include_directories('compiler'), include_directories('raster')],
  # The product driver: no re-verification of its own data (raster/poulsback_selfcheck.h).
  c_args : ['-DPOULSBACK_SELFCHECK=0'],
  dependencies : [dep_libdrm, dep_thread, idep_nir_headers],
)
driver_poulsback = declare_dependency(
  compile_args : '-DGALLIUM_POULSBACK',
  link_with : libpoulsback,
  dependencies : [dep_libdrm, dep_thread, idep_nir],
)
poulsback_loader_test = executable(
  'poulsback-loader-refusal', 'test_loader.c',
  include_directories : [inc_include, inc_src, inc_gallium],
  link_with : [libpoulsback, libgallium],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
)
test('poulsback-loader-refusal', poulsback_loader_test)
poulsback_fence_test = executable(
  'poulsback-fence-callbacks', ['test_fence.c', 'poulsback_fence.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_thread, idep_mesautil],
)
test('poulsback-fence-callbacks', poulsback_fence_test)
poulsback_lost_test = executable(
  'poulsback-lost', ['test_lost.c', 'poulsback_lost.c'],
  include_directories : [inc_include, inc_src, inc_gallium],
)
test('poulsback-lost', poulsback_lost_test)
poulsback_frame_test = executable(
  'poulsback-frame-writer', 'test_frame.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, include_directories('compiler'),
                         include_directories('raster')],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
  link_with : [libpoulsback, libgallium],
)
# Supply native-format frame files in POULSBACK_FRAME_SCENES=DIR; see test_frame.c.
test('poulsback-frame-writer', poulsback_frame_test)
poulsback_slab_test = executable(
  'poulsback-slab', ['test_slab.c', 'poulsback_slab.c'],
)
test('poulsback-slab', poulsback_slab_test)
poulsback_surface_sampler_test = executable(
  'poulsback-surface-sampler-callbacks',
  ['test_surface_sampler.c', 'poulsback_surface.c', 'poulsback_sampler.c'],
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_thread, idep_mesautil],
  link_with : libgallium,
)
test('poulsback-surface-sampler-callbacks', poulsback_surface_sampler_test)
poulsback_window_test = executable(
  'poulsback-window-transform', 'test_window.c',
  include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [dep_thread, idep_nir, idep_mesautil],
  link_with : libgallium,
)
test('poulsback-window-transform', poulsback_window_test)
poulsback_glsl_nir_check = executable(
  'poulsback-glsl-nir-check', 'test_glsl.cpp',
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  dependencies : [dep_clock, dep_thread, idep_getopt, idep_mesautil, idep_nir_headers],
  link_with : libglsl_standalone,
)
poulsback_dril_entry = executable(
  'poulsback-dril-entry', 'test_dril.c',
  include_directories : inc_include,
  dependencies : cc.find_library('dl'),
)
poulsback_fragment_glsl_nir_check = executable(
  'poulsback-fragment-glsl-nir-check', 'test_nir_fragment_glsl.cpp',
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium,
                         inc_gallium_aux, include_directories('compiler')],
  dependencies : [dep_clock, dep_thread, dep_libdrm, idep_getopt,
                  idep_mesautil, idep_nir_headers],
  link_with : [libglsl_standalone, libpoulsback],
)
poulsback_nir_linkage_test = executable(
  'poulsback-nir-linkage', 'test_nir_linkage.c',
  include_directories : [inc_include, inc_src, inc_gallium, include_directories('compiler')],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
  link_with : libpoulsback,
)
test('poulsback-nir-linkage', poulsback_nir_linkage_test)
poulsback_linkage_glsl_nir_check = executable(
  'poulsback-linkage-glsl-nir-check', 'test_nir_linkage_glsl.cpp',
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium,
                         inc_gallium_aux, include_directories('compiler')],
  dependencies : [dep_clock, dep_thread, dep_libdrm, idep_getopt,
                  idep_mesautil, idep_nir_headers],
  link_with : [libglsl_standalone, libpoulsback],
)
poulsback_nir_uniform_test = executable(
  'poulsback-nir-uniform', 'test_nir_uniform.c',
  include_directories : [inc_include, inc_src, inc_gallium, include_directories('compiler')],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
  link_with : libpoulsback,
)
test('poulsback-nir-uniform', poulsback_nir_uniform_test)
poulsback_nir_raw_sign_test = executable(
  'poulsback-nir-raw-sign', 'test_nir_fragment_raw_sign.c',
  include_directories : [inc_include, inc_src, inc_gallium, include_directories('compiler')],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
  link_with : libpoulsback,
)
test('poulsback-nir-raw-sign', poulsback_nir_raw_sign_test)
poulsback_nir_raw_sign_normalized_test = executable(
  'poulsback-nir-raw-sign-normalized', 'test_nir_fragment_raw_sign_normalized.c',
  include_directories : [inc_include, inc_src, inc_gallium, include_directories('compiler')],
  dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
  link_with : libpoulsback,
  link_args : ['-Wl,--wrap=calloc'],
)
test('poulsback-nir-raw-sign-normalized', poulsback_nir_raw_sign_normalized_test)
foreach variant : ['bool32', 'bool32_inot', 'bool32_ieq', 'bool32_ior', 'bool32_fneu', 'bool32_bcsel', 'ige_const', 'flt_const']
  bool32_test_name = 'poulsback-nir-' + variant.replace('_', '-')
  bool32_test = executable(
    bool32_test_name, 'test_nir_fragment_' + variant + '.c',
    c_args : ['bool32_fneu', 'bool32_bcsel', 'ige_const', 'flt_const'].contains(variant) ? ['-fno-fast-math'] : [],
    include_directories : [inc_include, inc_src, inc_gallium, include_directories('compiler')],
    dependencies : [dep_libdrm, dep_thread, idep_nir, idep_mesautil],
    link_with : libpoulsback,
  )
  test(bool32_test_name, bool32_test)
endforeach
poulsback_uniform_glsl_nir_check = executable(
  'poulsback-uniform-glsl-nir-check', 'test_nir_uniform_glsl.cpp',
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium,
                         inc_gallium_aux, include_directories('compiler')],
  dependencies : [dep_clock, dep_thread, dep_libdrm, idep_getopt,
                  idep_mesautil, idep_nir_headers],
  link_with : [libglsl_standalone, libpoulsback],
)
# Takes a .vert/.frag pair on its command line (tests/compiler).
poulsback_f32_io_glsl_nir_check = executable(
  'poulsback-f32-io-glsl-nir-check', 'test_nir_fragment_f32_io.cpp',
  cpp_args : ['-fno-fast-math'],
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium,
                         inc_gallium_aux, include_directories('compiler')],
  dependencies : [dep_clock, dep_thread, dep_libdrm, idep_getopt,
                  idep_mesautil, idep_nir_headers],
  link_with : [libglsl_standalone, libpoulsback, libgallium],
)

# The fragment backend against NIR on one supplied GLSL pair.
# Fixture contract and arguments: tests/unit/test_fs_nir.cpp.
poulsback_fs_nir_check = executable(
  'poulsback-fs-nir', ['test_fs_nir.cpp', 'sgx535_use_sim.c'],
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium,
                         inc_gallium_aux, include_directories('compiler')],
  dependencies : [dep_clock, dep_thread, dep_libdrm, idep_getopt,
                  idep_mesautil, idep_nir_headers],
  link_with : [libglsl_standalone, libpoulsback],
)
