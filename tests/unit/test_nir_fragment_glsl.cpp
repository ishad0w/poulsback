// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual pinned Mesa GLSL frontend -> explicit NIR lowering -> raw native
 * instruction construction. No pipe_screen, rendering or numerical oracle. */
#ifdef NDEBUG
#error "Actual NIR validation must remain enabled"
#endif
#include "compiler/glsl/standalone.h"
#include "compiler/glsl/gl_nir_linker.h"
#include "compiler/glsl/linker_util.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "main/mtypes.h"
#include "main/shader_types.h"
#include "main/shaderobj.h"
extern "C" {
#include "sgx535_fragment.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned io_slots(const glsl_type *type, bool bindless)
{
    return bindless ? 0 : glsl_count_attribute_slots(type, false);
}
static unsigned uniform_words(const glsl_type *type, bool bindless)
{
    return bindless ? 0 : glsl_count_dword_slots(type, false);
}
static bool lower(nir_shader *nir)
{
    unsigned uniforms = 0, inputs = 0, outputs = 0;
    nir_foreach_variable_in_shader(var, nir)
    {
        /* The fixture's public contract uses only simple F32 vec4 resources.
         * Reject unsupported resources rather than dropping their metadata. */
        if (var->data.mode == nir_var_uniform) {
            if (var->type != glsl_vec4_type() || var->data.bindless || var->data.access)
                return false;
            var->data.driver_location = uniforms;
            uniforms += 4;
        } else if (var->data.mode == nir_var_shader_in) {
            if (var->type != glsl_vec4_type() || var->data.location < VARYING_SLOT_VAR0 ||
                var->data.location >= VARYING_SLOT_VAR0 + 10 || var->data.sample ||
                var->data.centroid || var->data.interpolation == INTERP_MODE_FLAT ||
                var->data.interpolation == INTERP_MODE_NOPERSPECTIVE)
                return false;
            var->data.driver_location = var->data.location - VARYING_SLOT_VAR0;
            inputs++;
        } else if (var->data.mode == nir_var_shader_out) {
            if (var->type != glsl_vec4_type() || var->data.index ||
                (var->data.location != FRAG_RESULT_COLOR &&
                 var->data.location != FRAG_RESULT_DATA0))
                return false;
            var->data.driver_location = 0;
            outputs++;
        } else if (var->data.mode != nir_var_function_temp && var->data.mode != nir_var_shader_temp)
            return false;
    }
    if (!inputs || uniforms != 8 || outputs != 1)
        return false;
    nir_lower_global_vars_to_local(nir);
    nir_lower_vars_to_ssa(nir);
    nir_lower_io(nir, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), io_slots,
                 nir_lower_io_use_interpolated_input_intrinsics);
    nir_lower_io(nir, nir_var_uniform, uniform_words, (nir_lower_io_options)0);
    nir_opt_dce(nir);
    nir_remove_dead_variables(nir,
                              (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out |
                                                  nir_var_uniform | nir_var_function_temp |
                                                  nir_var_shader_temp),
                              nullptr);
    nir_opt_dce(nir);
    nir_opt_constant_folding(nir);
    nir_opt_dce(nir);
    nir_validate_shader(nir, "actual GLSL lowered fragment profile");
    return true;
}
static char *printed(nir_shader *shader)
{
    char *result = nullptr;
    size_t size = 0;
    FILE *file = open_memstream(&result, &size);
    if (!file)
        return nullptr;
    nir_print_shader(shader, file);
    if (fclose(file)) {
        free(result);
        return nullptr;
    }
    return result;
}
int main(int argc, char **argv)
{
    static gl_context context;
    standalone_options options = {};
    gl_shader_program *program;
    int result = EXIT_FAILURE;
    bool expect_reject = argc == 4 && !std::strcmp(argv[3], "--expect-reject");
    if (argc != 3 && !expect_reject)
        return EXIT_FAILURE;
    options.glsl_version = 120;
    options.do_link = 0;
    options.just_log = 1;
    program = standalone_compile_shader(&options, 2, argv + 1, &context);
    if (!program)
        return EXIT_FAILURE;
    if (program->NumShaders == 2 && program->Shaders[0]->CompileStatus == COMPILE_SUCCESS &&
        program->Shaders[1]->CompileStatus == COMPILE_SUCCESS) {
        _mesa_clear_shader_program_data(&context, program);
        program->data->LinkStatus = LINKING_SUCCESS;
        link_shaders_init(&context, program);
        gl_nir_link_glsl(&context, program);
    }
    if (program->data->InfoLog)
        std::fputs(program->data->InfoLog, stderr);
    gl_linked_shader *fragment = program->_LinkedShaders[MESA_SHADER_FRAGMENT];
    if (program->data->LinkStatus == LINKING_SUCCESS && fragment && fragment->Program &&
        fragment->Program->nir) {
        nir_shader *nir = fragment->Program->nir;
        nir_validate_shader(nir, "actual GLSL fragment before normalization");
        std::puts("ACTUAL_GLSL_FRAGMENT_NIR_BEFORE_LOWERING");
        nir_print_shader(nir, stdout);
        if (lower(nir)) {
            char *before = printed(nir);
            sgx535_fragment_program raw = {}, packed = {};
            sgx535_nir_status raw_status =
                sgx535_nir_project_fragment(nir, SGX535_FRAGMENT_RAW_F32_WORDS, &raw);
            sgx535_nir_status packed_status =
                sgx535_nir_project_fragment(nir, SGX535_FRAGMENT_U8_WORD_PROJECTION, &packed);
            char *after = printed(nir);
            std::puts("ACTUAL_GLSL_FRAGMENT_NIR_AFTER_EXPLICIT_LOWERING");
            nir_print_shader(nir, stdout);
            std::printf(
                "FRAGMENT_TRANSPORT_METRICS {\"raw_status\":%d,\"packed_status\":%d,\"raw_words\":%u,\"packed_words\":%u,\"primary_words\":%u,\"secondary_words\":%u,\"temp_registers\":%u,\"arithmetic_qualified\":%u,\"linkage_qualified\":%u,\"output_qualified\":%u}\n",
                raw_status, packed_status, raw.encoding.word_count, packed.encoding.word_count,
                raw.primary_register_count, raw.secondary_register_count,
                raw.encoding.temp_register_count, raw.arithmetic_qualified, raw.linkage_qualified,
                raw.output_qualified);
            if (expect_reject && before && after && !std::strcmp(before, after) &&
                raw_status == SGX535_NIR_ERR_UNSUPPORTED &&
                packed_status == SGX535_NIR_ERR_UNSUPPORTED && !raw.encoding.word_count &&
                !packed.encoding.word_count) {
                std::puts(
                    "PASS actual Mesa GLSL/NIR constrained floating instructions explicitly rejected; flags retained");
                result = EXIT_SUCCESS;
            } else if (!expect_reject && before && after && !std::strcmp(before, after) &&
                       raw_status == SGX535_NIR_OK && packed_status == SGX535_NIR_OK &&
                       raw.input_count == 1 && raw.primary_register_count == 4 &&
                       raw.secondary_register_count == 8 && raw.uniform_words_used[0] == 255 &&
                       !raw.arithmetic_qualified && !raw.linkage_qualified &&
                       !raw.output_qualified &&
                       sgx535_fragment_native_admission(&raw) == SGX535_NIR_ERR_UNSUPPORTED &&
                       sgx535_fragment_native_admission(&packed) == SGX535_NIR_ERR_UNSUPPORTED) {
                std::puts(
                    "PASS actual Mesa GLSL/NIR varying vec4 + two uniform vec4 native word projections; native admission refused");
                result = EXIT_SUCCESS;
            }
            free(before);
            free(after);
        }
    }
    standalone_compiler_cleanup(program, &context);
    return result;
}
