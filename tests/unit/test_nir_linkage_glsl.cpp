// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Actual GLSL120 compatibility semantic frontend, not a GL/GPU context. */
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
extern "C" {
#include "sgx535_fragment_linkage.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned slots(const glsl_type *t, bool bindless)
{
    return bindless ? 0 : glsl_count_attribute_slots(t, false);
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
int main(int argc, char **argv)
{
    static gl_context context;
    standalone_options options = {};
    gl_shader_program *p;
    int result = EXIT_FAILURE;
    if (argc != 4 || (std::strcmp(argv[3], "1") && std::strcmp(argv[3], "3")))
        return result;
    options.glsl_version = 120;
    options.do_link = 0;
    options.just_log = 1;
    p = standalone_compile_shader(&options, 2, argv + 1, &context);
    if (!p)
        return result;
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
        nir_validate_shader(nir, "actual compatibility GLSL before normalization");
        std::puts("ACTUAL_COMPATIBILITY_NIR_BEFORE_NORMALIZATION");
        nir_print_shader(nir, stdout);
        nir_lower_global_vars_to_local(nir);
        nir_lower_vars_to_ssa(nir);
        nir_lower_io(nir, (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out), slots,
                     nir_lower_io_use_interpolated_input_intrinsics);
        nir_opt_dce(nir);
        nir_remove_dead_variables(nir,
                                  (nir_variable_mode)(nir_var_shader_in | nir_var_shader_out |
                                                      nir_var_function_temp | nir_var_shader_temp),
                                  nullptr);
        nir_opt_dce(nir);
        nir_opt_constant_folding(nir);
        nir_opt_dce(nir);
        nir_validate_shader(nir, "actual compatibility GLSL normalized fragment");
        auto *ir = static_cast<sgx535_fragment_instruction *>(
            std::calloc(SGX535_NIR_MAX_INSTRUCTIONS, sizeof(sgx535_fragment_instruction)));
        auto *program =
            static_cast<sgx535_fragment_program *>(std::calloc(1, sizeof(sgx535_fragment_program)));
        sgx535_fragment_linkage link = {};
        size_t count = 0;
        char *before = printed(nir);
        const sgx535_fragment_nir_options dwords_strict = {SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                           SGX535_FRAGMENT_IO_STRICT};
        sgx535_nir_status status =
            ir && program ? sgx535_nir_fragment_options_ir(
                                nir, &dwords_strict, ir, SGX535_NIR_MAX_INSTRUCTIONS, &count, &link)
                          : SGX535_NIR_ERR_NOMEM;
        char *after = printed(nir);
        std::puts("ACTUAL_COMPATIBILITY_NIR_AFTER_NORMALIZATION");
        nir_print_shader(nir, stdout);
        bool valid = status == SGX535_NIR_OK && before && after && !std::strcmp(before, after) &&
                     link.count == static_cast<unsigned>(std::atoi(argv[3])) &&
                     link.varying[0].semantic == SGX535_FRAGMENT_COLOR &&
                     link.varying[0].index == 0;
        if (valid && link.count == 3)
            valid =
                link.varying[1].semantic == SGX535_FRAGMENT_COLOR && link.varying[1].index == 1 &&
                link.varying[2].semantic == SGX535_FRAGMENT_GENERIC && link.varying[2].index == 0;
        if (valid)
            valid = sgx535_fragment_compile(ir, count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
                        SGX535_NIR_OK &&
                    program->input_count == link.count && !program->linkage_qualified &&
                    !program->arithmetic_qualified && !program->output_qualified &&
                    sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED;
        if (valid) {
            sgx535_fragment_dri_nir_program dri = {}, saved = {};
            auto *symbolic = static_cast<sgx535_fragment_dri_nir_ir *>(
                std::malloc(sizeof(sgx535_fragment_dri_nir_ir)));
            auto *symbolic_saved = static_cast<sgx535_fragment_dri_nir_ir *>(
                std::malloc(sizeof(sgx535_fragment_dri_nir_ir)));
            if (!symbolic || !symbolic_saved) {
                free(symbolic);
                free(symbolic_saved);
                free(before);
                free(after);
                free(ir);
                free(program);
                standalone_compiler_cleanup(p, &context);
                return EXIT_FAILURE;
            }
            std::memset(symbolic, 0xa5, sizeof(*symbolic));
            *symbolic_saved = *symbolic;
            std::memset(&dri, 0xa5, sizeof(dri));
            saved = dri;
            auto native = sgx535_nir_project_dri_color0(nir, SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                        SGX535_FRAGMENT_SMOOTH,
                                                        SGX535_FRAGMENT_U8_WORD_PROJECTION, &dri);
            auto exported = sgx535_nir_dri_color0_ir(nir, SGX535_FRAGMENT_UNIFORM_DWORDS,
                                                     SGX535_FRAGMENT_SMOOTH, symbolic);
            if (link.count == 1) {
                static const uint32_t literal[16] = {
                    0xa0000100u, 0x28a10000u, 0xa0200080u, 0x28a10000u, 0xa0400000u, 0x28a10000u,
                    0xa0600180u, 0x28a10000u, 0x20040000u, 0x40a10c11u, 0x20040080u, 0x40a10c09u,
                    0x20040100u, 0x40a10c05u, 0x20040180u, 0x40a50c21u};
                valid = native == SGX535_NIR_OK &&
                        !std::memcmp(&dri.source_linkage, &link, sizeof(link)) &&
                        dri.program.encoding.word_count == 16 &&
                        !std::memcmp(dri.program.encoding.words, literal, sizeof(literal)) &&
                        sgx535_fragment_dri_color0_native_admission(&dri.program) ==
                            SGX535_NIR_ERR_UNSUPPORTED;
                valid = valid && exported == SGX535_NIR_OK && symbolic->instruction_count == 8 &&
                        !std::memcmp(&symbolic->source_linkage, &link, sizeof(link)) &&
                        symbolic->resolved_interpolation == SGX535_FRAGMENT_SMOOTH;
                if (valid)
                    std::puts(
                        "PASS actual GLSL120 primary COLOR -> matched DRI16-word program; no native admission");
            } else
                valid = native == SGX535_NIR_ERR_UNSUPPORTED &&
                        !std::memcmp(&dri, &saved, sizeof(dri)) &&
                        exported == SGX535_NIR_ERR_UNSUPPORTED &&
                        !std::memcmp(symbolic, symbolic_saved, sizeof(*symbolic));
            char *bridge_after = printed(nir);
            valid = valid && bridge_after && before && !std::strcmp(before, bridge_after);
            free(bridge_after);
            free(symbolic_saved);
            free(symbolic);
        }
        std::printf(
            "LINKAGE_GLSL_METRICS {\"status\":%d,\"inputs\":%u,\"ir_instructions\":%zu,\"native_qualification\":0}\n",
            status, link.count, count);
        if (valid) {
            std::puts(
                "PASS actual GLSL120 COLOR/GENERIC semantic linkage; native admission refused");
            result = EXIT_SUCCESS;
        }
        free(before);
        free(after);
        free(ir);
        free(program);
    }
    standalone_compiler_cleanup(p, &context);
    return result;
}
