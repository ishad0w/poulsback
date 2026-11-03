// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual Mesa GLSL/NIR CPU frontend checker; no screen, GL renderer or GPU. */
#ifdef NDEBUG
#error "The actual Mesa NIR validator must remain enabled"
#endif
#include "compiler/glsl/standalone.h"
#include "compiler/glsl/gl_nir_linker.h"
#include "compiler/glsl/linker_util.h"
#include "compiler/nir/nir.h"
#include "main/mtypes.h"
#include "main/shader_types.h"
#include "main/shaderobj.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

int main(int argc, char **argv)
{
    static gl_context context;
    standalone_options options = {};
    gl_shader_program *program;
    int result = EXIT_FAILURE;

    if (argc != 5 || (std::atoi(argv[1]) != 100 && std::atoi(argv[1]) != 120) ||
        (std::atoi(argv[2]) != 64 && std::atoi(argv[2]) != 256))
        return EXIT_FAILURE;
    options.glsl_version = std::atoi(argv[1]);
    options.do_link = 0;
    options.just_log = 1;
    program = standalone_compile_shader(&options, 2, argv + 3, &context);
    if (!program)
        return EXIT_FAILURE;

    /* Upstream standalone main does not propagate its internal status.
     * Validate real objects, then validate/dump the actual linked NIR. */
    if (program->NumShaders == 2 && program->Shaders[0]->CompileStatus == COMPILE_SUCCESS &&
        program->Shaders[1]->CompileStatus == COMPILE_SUCCESS) {
        /* These explicit limits belong only to this CPU frontend fixture.
         * They are never supplied to a native pipe_screen or GL context.
         * Retain the upstream minimum failure, then isolate the real corpus
         * frontend with a labelled 256-component fixture. */
        context.Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = std::atoi(argv[2]);
        context.Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents =
            std::atoi(argv[2]);
        _mesa_clear_shader_program_data(&context, program);
        program->data->LinkStatus = LINKING_SUCCESS;
        link_shaders_init(&context, program);
        gl_nir_link_glsl(&context, program);
        if (program->data->InfoLog)
            std::fputs(program->data->InfoLog, stderr);
    }
    unsigned measured = 0, nir_stages = 0;
    if (program->_LinkedShaders[MESA_SHADER_FRAGMENT])
        measured = program->_LinkedShaders[MESA_SHADER_FRAGMENT]->num_uniform_components;
    if (program->data->LinkStatus == LINKING_SUCCESS) {
        bool complete = true;
        for (mesa_shader_stage stage : {MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT}) {
            gl_linked_shader *linked = program->_LinkedShaders[stage];
            if (!linked || !linked->Program || !linked->Program->nir) {
                complete = false;
                break;
            }
            nir_validate_shader(linked->Program->nir, "actual glamor startup frontend");
            nir_print_shader(linked->Program->nir, stdout);
            nir_stages++;
        }
        if (complete) {
            std::puts("PASS actual Mesa shader compile/link and validated vertex/fragment NIR");
            result = EXIT_SUCCESS;
        }
    }
    std::printf(
        "POULSBACK_FRONTEND_METRICS {\"cpu_fragment_uniform_components\":%d,\"vertex_compile\":%d,\"fragment_compile\":%d,\"link_status\":%d,\"measured_fragment_uniform_components\":%u,\"validated_nir_stages\":%u}\n",
        std::atoi(argv[2]), program->NumShaders >= 1 ? program->Shaders[0]->CompileStatus : 0,
        program->NumShaders >= 2 ? program->Shaders[1]->CompileStatus : 0,
        program->data->LinkStatus, measured, nir_stages);
    standalone_compiler_cleanup(program, &context);
    return result;
}
