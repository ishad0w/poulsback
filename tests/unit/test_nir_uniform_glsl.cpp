// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Real GLSL linker parameter values -> explicit standard load_uniform units.
 * No GL context rendering or CPU fragment evaluator. */
#ifdef NDEBUG
#error "Actual Mesa validation must remain enabled"
#endif
#include "compiler/glsl/standalone.h"
#include "compiler/glsl/gl_nir_linker.h"
#include "compiler/glsl/linker_util.h"
#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "main/mtypes.h"
#include "main/shader_types.h"
#include "main/shaderobj.h"
#include "program/prog_parameter.h"
extern "C" {
#include "sgx535_fragment_linkage.h"
#include "poulsback_program.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned slots(const glsl_type *t, bool bindless)
{
    return bindless ? 0 : glsl_count_attribute_slots(t, false);
}
static unsigned dwords(const glsl_type *t, bool bindless)
{
    return bindless ? 0 : glsl_count_dword_slots(t, false);
}
static unsigned vec4s(const glsl_type *t, bool bindless)
{
    return bindless ? 0 : glsl_count_vec4_slots(t, false, false);
}
static char *printed(nir_shader *s)
{
    char *raw = nullptr;
    size_t bytes = 0;
    FILE *f = open_memstream(&raw, &bytes);
    if (!f)
        return nullptr;
    nir_print_shader(s, f);
    if (fclose(f)) {
        free(raw);
        return nullptr;
    }
    return raw;
}
static int parameter(gl_program_parameter_list *params, const char *name)
{
    return _mesa_lookup_parameter_index(params, name);
}
static bool root_uniform(const sgx535_fragment_instruction *ir, size_t count, uint32_t source,
                         uint32_t *word)
{
    for (size_t n = 0; n < count; n++) {
        if (source >= count)
            return false;
        if (ir[source].opcode == SGX535_FRAGMENT_UNIFORM) {
            *word = ir[source].index;
            return true;
        }
        if (ir[source].opcode != SGX535_FRAGMENT_MOV || ir[source].source[0] >= source)
            return false;
        source = ir[source].source[0];
    }
    return false;
}
int main(int argc, char **argv)
{
    static gl_context context;
    standalone_options options = {};
    gl_shader_program *p;
    int result = EXIT_FAILURE;
    if (argc != 5 || (std::strcmp(argv[3], "packed") && std::strcmp(argv[3], "vec4")) ||
        (std::strcmp(argv[4], "mixed") && std::strcmp(argv[4], "aggregate")))
        return result;
    bool packed = !std::strcmp(argv[3], "packed");
    bool aggregate = !std::strcmp(argv[4], "aggregate");
    options.glsl_version = 120;
    options.do_link = 0;
    options.just_log = 1;
    p = standalone_compile_shader(&options, 2, argv + 1, &context);
    if (!p)
        return result;
    context.Const.PackedDriverUniformStorage = packed;
    if (p->NumShaders == 2 && p->Shaders[0]->CompileStatus == COMPILE_SUCCESS &&
        p->Shaders[1]->CompileStatus == COMPILE_SUCCESS) {
        _mesa_clear_shader_program_data(&context, p);
        p->data->LinkStatus = LINKING_SUCCESS;
        link_shaders_init(&context, p);
        gl_nir_link_glsl(&context, p);
    }
    if (p->data->InfoLog)
        std::fputs(p->data->InfoLog, stderr);
    auto *fragment = p->_LinkedShaders[MESA_SHADER_FRAGMENT];
    if (p->data->LinkStatus == LINKING_SUCCESS && fragment && fragment->Program &&
        fragment->Program->nir) {
        nir_shader *nir = fragment->Program->nir;
        auto *params = fragment->Program->Parameters;
        bool valid = params && params->NumParameterValues <= 128;
        uint32_t expected[4] = {}, output[128] = {};
        const char *names[4] = {aggregate ? "palette" : "opacity", aggregate ? "basis" : "bias",
                                aggregate ? "palette" : "rgb", aggregate ? "basis" : "tint"};
        const unsigned columns[4] = {aggregate ? 1u : 0u, aggregate ? 1u : 0u, 0, 0};
        const unsigned components[4] = {0, 1, 2, aggregate ? 0u : 3u};
        for (unsigned i = 0; valid && i < 4; i++) {
            int at = parameter(params, names[i]);
            valid = at >= 0 && static_cast<unsigned>(at) + columns[i] < params->NumParameters;
            if (valid)
                expected[i] = params->Parameters[at + columns[i]].ValueOffset + components[i];
        }
        for (unsigned i = 0; valid && i < params->NumParameterValues; i++)
            params->ParameterValues[i].u = 0x81234567u + i;
        if (valid && params->NumParameterValues >= 4) {
            params->ParameterValues[0].u = 0x80000000;
            params->ParameterValues[1].u = 0x7fc12345;
            params->ParameterValues[2].u = 0x7f800000;
            params->ParameterValues[3].u = 0xff800000;
        }
        for (unsigned i = 0; valid && i < params->NumParameters; i++)
            std::printf("ACTUAL_PARAMETER %u %s offset=%u size=%u\n", i, params->Parameters[i].Name,
                        params->Parameters[i].ValueOffset, params->Parameters[i].Size);
        nir_validate_shader(nir, "actual GLSL uniform source");
        std::puts("ACTUAL_UNIFORM_NIR_BEFORE_NORMALIZATION");
        nir_print_shader(nir, stdout);
        if (valid)
            nir_foreach_variable_in_shader(var, nir)
            {
                if (var->data.mode == nir_var_uniform) {
                    int at = parameter(params, var->name);
                    if (at < 0 || var->data.bindless || var->data.access) {
                        valid = false;
                        break;
                    }
                    /* Exact standard state-tracker contract, using actual linked
                 * parameter offsets, never custom sequential fixture slots. */
                    var->data.driver_location =
                        packed ? static_cast<int>(params->Parameters[at].ValueOffset) : at;
                } else if (var->data.mode == nir_var_shader_out)
                    var->data.driver_location = 0;
            }
        if (valid) {
            nir_lower_global_vars_to_local(nir);
            nir_lower_vars_to_ssa(nir);
            nir_lower_io(nir, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), slots,
                         nir_lower_io_use_interpolated_input_intrinsics);
            nir_lower_io(nir, nir_var_uniform, packed ? dwords : vec4s, (nir_lower_io_options)0);
            nir_opt_dce(nir);
            nir_opt_constant_folding(nir);
            nir_opt_dce(nir);
            nir_remove_dead_variables(nir,
                                      (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out |
                                                          nir_var_uniform | nir_var_function_temp |
                                                          nir_var_shader_temp),
                                      nullptr);
            nir_validate_shader(nir, "actual standard uniform units");
            auto *ir = static_cast<sgx535_fragment_instruction *>(
                std::calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(sgx535_fragment_instruction)));
            auto *program = static_cast<sgx535_fragment_program *>(
                std::calloc(1, sizeof(sgx535_fragment_program)));
            sgx535_fragment_linkage link = {};
            size_t count = 0, words = 0;
            char *before = printed(nir);
            const sgx535_fragment_nir_options strict = {packed ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                               : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                        SGX535_FRAGMENT_IO_STRICT};
            auto status = ir && program
                              ? sgx535_nir_fragment_options_ir(
                                    nir, &strict, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &link)
                              : SGX535_NIR_ERR_NOMEM;
            char *after = printed(nir);
            std::puts("ACTUAL_UNIFORM_NIR_AFTER_NORMALIZATION");
            nir_print_shader(nir, stdout);
            valid = status == SGX535_NIR_OK && before && after && !std::strcmp(before, after) &&
                    !link.count;
            unsigned outputs = 0;
            for (size_t i = 0; valid && i < count; i++)
                if (ir[i].opcode == SGX535_FRAGMENT_OUTPUT) {
                    uint32_t word = UINT32_MAX;
                    valid = outputs < 4 && ir[i].index == outputs &&
                            root_uniform(ir, count, ir[i].source[0], &word) &&
                            word == expected[outputs];
                    outputs++;
                }
            valid = valid && outputs == 4 &&
                    sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
                        SGX535_NIR_OK;
            if (valid)
                valid = poulsback_fragment_uniform_snapshot(program, params->ParameterValues,
                                                            params->NumParameterValues *
                                                                sizeof(*params->ParameterValues),
                                                            output, 128, &words) == SGX535_NIR_OK &&
                        words == program->secondary_register_count &&
                        !std::memcmp(output, params->ParameterValues, words * 4) &&
                        !program->arithmetic_qualified && !program->linkage_qualified &&
                        !program->output_qualified &&
                        sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED;
            if (valid) {
                uint32_t saved[128];
                std::memcpy(saved, output, sizeof(saved));
                size_t refused_words = SIZE_MAX;
                valid = words &&
                        poulsback_fragment_uniform_snapshot(
                            program, params->ParameterValues, (words - 1) * sizeof(*output), output,
                            128, &refused_words) == SGX535_NIR_ERR_LIMIT &&
                        refused_words == SIZE_MAX && !std::memcmp(saved, output, sizeof(saved));
            }
            std::printf(
                "UNIFORM_GLSL_METRICS {\"packed\":%u,\"aggregate\":%u,\"status\":%d,\"parameter_words\":%u,\"snapshot_words\":%zu,\"native_qualification\":0}\n",
                packed, aggregate, status, params->NumParameterValues, words);
            if (valid) {
                std::puts(
                    "PASS actual GLSL120 linked parameter offsets, standard uniform units and exact-bit snapshots; native admission refused");
                result = EXIT_SUCCESS;
            }
            free(before);
            free(after);
            free(ir);
            free(program);
        }
    }
    standalone_compiler_cleanup(p, &context);
    return result;
}
