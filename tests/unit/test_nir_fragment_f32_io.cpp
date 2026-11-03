// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Real captured GLSL -> linked NIR -> explicit F32 IO transport. CPU only. */
#ifdef NDEBUG
#error "The actual Mesa NIR validator must remain enabled"
#endif
#include "compiler/glsl/standalone.h"
#include "compiler/glsl/gl_nir_linker.h"
#include "compiler/glsl/linker_util.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "main/mtypes.h"
#include "main/shader_types.h"
#include "main/shaderobj.h"
#include "program/prog_parameter.h"
extern "C" {
#include "sgx535_fragment_linkage.h"
#include "poulsback_nir.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

static unsigned checks, positive_cases, negative_cases;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition);                           \
            std::exit(EXIT_FAILURE);                                                               \
        }                                                                                          \
    } while (0)

struct result {
    sgx535_fragment_instruction ir[SGX535_NIR_MAX_INSTRUCTIONS];
    sgx535_fragment_linkage linkage;
    size_t count;
};
static const sgx535_fragment_nir_options strict_options = {SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                           SGX535_FRAGMENT_IO_STRICT};
static const sgx535_fragment_nir_options f32_options = {SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                        SGX535_FRAGMENT_IO_F32};

static char *printed(nir_shader *shader)
{
    char *raw = nullptr;
    size_t bytes = 0;
    FILE *stream = open_memstream(&raw, &bytes);
    CHECK(stream);
    nir_print_shader(shader, stream);
    CHECK(std::fclose(stream) == 0 && raw);
    return raw;
}
static nir_intrinsic_instr *intrinsic(nir_shader *shader, nir_intrinsic_op opcode)
{
    nir_intrinsic_instr *found = nullptr;
    nir_foreach_function_impl(impl, shader) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block)
    {
        if (instr->type != nir_instr_type_intrinsic)
            continue;
        auto *in = nir_instr_as_intrinsic(instr);
        if (in->intrinsic == opcode) {
            CHECK(!found);
            found = in;
        }
    }
    return found;
}
static void print_io(const char *phase, nir_shader *shader)
{
    auto *out = intrinsic(shader, nir_intrinsic_store_output);
    CHECK(out);
    auto sem = nir_intrinsic_io_semantics(out);
    std::printf("F32_IO_METADATA %s bits=%u type=%u medium=%u high16=%u location=%u slots=%u\n",
                phase, out->src[0].ssa->bit_size, nir_intrinsic_src_type(out), sem.medium_precision,
                sem.high_16bits, sem.location, sem.num_slots);
}
static result *run(nir_shader *shader, const sgx535_fragment_nir_options *options,
                   enum sgx535_nir_status expected, bool old_entry = false,
                   size_t capacity = SGX535_NIR_MAX_INSTRUCTIONS)
{
    auto *out = static_cast<result *>(std::malloc(sizeof(result)));
    auto *saved = static_cast<result *>(std::malloc(sizeof(result)));
    CHECK(out && saved);
    std::memset(out, 0xa5, sizeof(*out));
    std::memcpy(saved, out, sizeof(*saved));
    char *before = printed(shader);
    /* The old entry: the options entry with strict IO. */
    const sgx535_fragment_nir_options strict = {options->uniform_layout, SGX535_FRAGMENT_IO_STRICT};
    auto status = sgx535_nir_fragment_options_ir(shader, old_entry ? &strict : options, out->ir,
                                                 capacity, &out->count, &out->linkage);
    char *after = printed(shader);
    CHECK(status == expected && !std::strcmp(before, after));
    if (status == SGX535_NIR_OK) {
        CHECK(out->count > 0 && out->count <= capacity);
        const size_t used = out->count * sizeof(*out->ir);
        CHECK(!std::memcmp(reinterpret_cast<unsigned char *>(out->ir) + used,
                           reinterpret_cast<unsigned char *>(saved->ir) + used,
                           sizeof(out->ir) - used));
        positive_cases++;
    } else {
        CHECK(!std::memcmp(out, saved, sizeof(*out)));
        negative_cases++;
    }
    std::free(before);
    std::free(after);
    std::free(saved);
    return out;
}
static void refused(nir_shader *shader, const sgx535_fragment_nir_options *options,
                    enum sgx535_nir_status expected = SGX535_NIR_ERR_UNSUPPORTED,
                    bool old_entry = false, size_t capacity = SGX535_NIR_MAX_INSTRUCTIONS)
{
    std::free(run(shader, options, expected, old_entry, capacity));
}
static unsigned uniform_root(const result *ir, unsigned at)
{
    for (size_t n = 0; n < ir->count; n++) {
        CHECK(at < ir->count);
        auto &op = ir->ir[at];
        if (op.opcode == SGX535_FRAGMENT_UNIFORM)
            return op.index;
        CHECK(op.opcode == SGX535_FRAGMENT_MOV && op.source[0] < at);
        at = op.source[0];
    }
    CHECK(false);
    return 0;
}
static uint32_t solid_word(const poulsback_nir_solid_fragment *ir, unsigned at)
{
    for (unsigned n = 0; n < ir->instruction_count; n++) {
        CHECK(at < ir->instruction_count);
        const auto &op = ir->instruction[at];
        if (op.opcode == SGX535_FRAGMENT_CONSTANT)
            return op.immediate;
        CHECK(op.opcode == SGX535_FRAGMENT_MOV && op.source[0] < at);
        at = op.source[0];
    }
    CHECK(false);
    return 0;
}
static void solid_snapshot(nir_shader *source, bool packed, const uint32_t fg[4])
{
    const auto layout =
        packed ? SGX535_FRAGMENT_UNIFORM_DWORDS : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS;
    auto *out = static_cast<poulsback_nir_solid_fragment *>(
        std::malloc(sizeof(poulsback_nir_solid_fragment)));
    auto *saved = static_cast<poulsback_nir_solid_fragment *>(
        std::malloc(sizeof(poulsback_nir_solid_fragment)));
    CHECK(out && saved);
    std::memset(out, 0xa5, sizeof(*out));
    std::memcpy(saved, out, sizeof(*saved));
    CHECK(poulsback_nir_solid_fragment_ir(source, layout, out) == SGX535_NIR_ERR_UNSUPPORTED);
    CHECK(!std::memcmp(out, saved, sizeof(*out)));
    char *before = printed(source);
    uint32_t constants[4];
    std::memcpy(constants, fg, sizeof(constants));
    nir_shader *first = nullptr, *second = nullptr;
    CHECK(!poulsback_nir_specialize_solid_fragment(source, layout, constants, sizeof(constants),
                                                   &first));
    CHECK(first && !intrinsic(first, nir_intrinsic_load_uniform));
    CHECK(poulsback_nir_solid_fragment_ir(first, layout, out) == SGX535_NIR_OK);
    CHECK(!out->program.primary_register_count && !out->program.secondary_register_count &&
          !out->program.hardware_qualified && !out->program.numerical_qualified &&
          out->program.missing_contracts == SGX535_NORMALIZED_MISSING_ALL);
    unsigned outputs = 0;
    for (unsigned i = 0; i < out->instruction_count; i++)
        if (out->instruction[i].opcode == SGX535_FRAGMENT_OUTPUT) {
            CHECK(out->instruction[i].index == outputs);
            CHECK(solid_word(out, out->instruction[i].source[0]) == fg[outputs++]);
        }
    CHECK(outputs == 4);
    char *first_before = printed(first);
    constants[0] = 0x80000000;
    CHECK(!poulsback_nir_specialize_solid_fragment(source, layout, constants, sizeof(constants),
                                                   &second));
    char *first_after = printed(first);
    CHECK(!std::strcmp(first_before, first_after));
    CHECK(poulsback_nir_solid_fragment_ir(second, layout, out) == SGX535_NIR_OK);
    CHECK(solid_word(out, out->instruction[out->instruction_count - 4].source[0]) == 0x80000000);
    for (unsigned which = 0; which < 4; which++) {
        nir_shader *refused = source;
        const void *data = which == 0 ? nullptr : constants;
        const size_t bytes = which == 0 ? 0 : which == 1 ? 12 : which == 2 ? 15 : 16;
        constants[0] = which == 3 ? 0x7fc12345 : fg[0];
        CHECK(poulsback_nir_specialize_solid_fragment(source, layout, data, bytes, &refused) ==
              (which == 2 ? -EINVAL : -EOPNOTSUPP));
        CHECK(refused == source);
    }
    auto *bad = nir_shader_clone(nullptr, source);
    CHECK(bad);
    nir_builder b = nir_builder_create(nir_shader_get_entrypoint(bad));
    b.cursor = nir_before_instr(&intrinsic(bad, nir_intrinsic_store_output)->instr);
    (void)nir_load_uniform(&b, 1, 32, nir_imm_int(&b, 0), .base = packed ? 127 : 31, .range = 1,
                           .dest_type = nir_type_float32);
    nir_shader *unchanged = source;
    CHECK(poulsback_nir_specialize_solid_fragment(bad, layout, fg, 16, &unchanged) == -EOPNOTSUPP &&
          unchanged == source);
    ralloc_free(bad);
    char *after = printed(source);
    CHECK(!std::strcmp(before, after));
    std::free(before);
    std::free(after);
    std::free(first_before);
    std::free(first_after);
    std::free(out);
    std::free(saved);
    ralloc_free(first);
    ralloc_free(second);
}
static void check_fg_program(const result *ir)
{
    CHECK(!ir->linkage.count && ir->count >= 8);
    unsigned outputs = 0, uniforms = 0;
    for (size_t i = 0; i < ir->count; i++) {
        auto &op = ir->ir[i];
        if (op.opcode == SGX535_FRAGMENT_OUTPUT) {
            CHECK(op.index == outputs && uniform_root(ir, op.source[0]) == outputs);
            outputs++;
        } else if (op.opcode == SGX535_FRAGMENT_UNIFORM) {
            CHECK(op.index == uniforms && uniforms < 4);
            uniforms++;
        } else if (op.opcode == SGX535_FRAGMENT_CONSTANT)
            CHECK(op.immediate == 0);
        else
            CHECK(op.opcode == SGX535_FRAGMENT_MOV);
    }
    CHECK(outputs == 4 && uniforms == 4);
    auto *program =
        static_cast<sgx535_fragment_program *>(std::calloc(1, sizeof(sgx535_fragment_program)));
    CHECK(program && sgx535_fragment_compile(ir->ir, ir->count, SGX535_FRAGMENT_RAW_F32_WORDS,
                                             program) == SGX535_NIR_OK);
    CHECK(program->uniform_words_used[0] == 15 && !program->uniform_words_used[1] &&
          !program->uniform_words_used[2] && !program->uniform_words_used[3]);
    CHECK(program->secondary_register_count == 4 && !program->input_count &&
          !program->primary_register_count && program->encoding.word_count);
    CHECK(!program->arithmetic_qualified && !program->linkage_qualified &&
          !program->output_qualified);
    CHECK(sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED);
    std::printf("F32_IO_WORDS %u secondary=%u primary=%u native=closed\n",
                program->encoding.word_count, program->secondary_register_count,
                program->primary_register_count);
    for (unsigned i = 0; i < program->encoding.word_count; i++)
        std::printf("F32_IO_WORD %u %08x\n", i, program->encoding.words[i]);
    std::free(program);
}
static void output_controls(nir_shader *source)
{
    auto *shader = nir_shader_clone(nullptr, source);
    CHECK(shader);
    auto *out = intrinsic(shader, nir_intrinsic_store_output);
    auto *load = intrinsic(shader, nir_intrinsic_load_uniform);
    CHECK(out && load);
    auto sem = nir_intrinsic_io_semantics(out);
    CHECK(sem.medium_precision && !sem.high_16bits && out->src[0].ssa->bit_size == 32);
    auto *accepted = run(shader, &f32_options, SGX535_NIR_OK);
    check_fg_program(accepted);
    refused(shader, &strict_options);
    refused(shader, &strict_options, SGX535_NIR_ERR_UNSUPPORTED, true);
    auto changed_options = f32_options;
    changed_options.io_precision = static_cast<sgx535_fragment_io_precision_policy>(2);
    refused(shader, &changed_options);
    changed_options = f32_options;
    changed_options.uniform_layout = static_cast<sgx535_fragment_uniform_layout>(2);
    refused(shader, &changed_options);
    refused(shader, nullptr, SGX535_NIR_ERR_ARGUMENT);
    refused(shader, &f32_options, SGX535_NIR_ERR_LIMIT, false, accepted->count - 1);
    refused(shader, &f32_options, SGX535_NIR_ERR_LIMIT, false, 0);
    for (unsigned field = 0; field < 6; field++) {
        auto bad = sem;
        if (field == 0)
            bad.high_16bits = 1;
        else if (field == 1)
            bad.dual_source_blend_index = 1;
        else if (field == 2)
            bad.fb_fetch_output = 1;
        else if (field == 3)
            bad.num_slots = 2;
        else if (field == 4)
            bad.per_view = 1;
        else
            bad.location = FRAG_RESULT_DEPTH;
        nir_intrinsic_set_io_semantics(out, bad);
        refused(shader, &f32_options);
        nir_intrinsic_set_io_semantics(out, sem);
    }
    nir_intrinsic_set_src_type(out, nir_type_float16);
    refused(shader, &f32_options);
    nir_intrinsic_set_src_type(out, nir_type_float32);
    load->def.bit_size = 16;
    refused(shader, &f32_options);
    load->def.bit_size = 32;
    nir_intrinsic_set_dest_type(load, nir_type_float16);
    refused(shader, &f32_options);
    nir_intrinsic_set_dest_type(load, nir_type_float32);
    nir_intrinsic_set_write_mask(out, 0);
    refused(shader, &f32_options);
    nir_intrinsic_set_write_mask(out, 15);
    nir_intrinsic_set_base(load, SGX535_FRAGMENT_UNIFORM_WORDS);
    refused(shader, &f32_options);
    nir_intrinsic_set_base(load, 0);
    shader->info.float_controls_execution_mode = 1;
    refused(shader, &f32_options);
    shader->info.float_controls_execution_mode = 0;
    /* Independent test control: remove only the annotation on a private test
     * graph. The production F32 path never clears it. Strict IR must be exact. */
    auto highp = sem;
    highp.medium_precision = 0;
    nir_intrinsic_set_io_semantics(out, highp);
    auto *strict = run(shader, &strict_options, SGX535_NIR_OK);
    auto *legacy = run(shader, &strict_options, SGX535_NIR_OK, true);
    CHECK(strict->count == accepted->count && legacy->count == accepted->count);
    CHECK(!std::memcmp(strict->ir, accepted->ir, accepted->count * sizeof(*accepted->ir)));
    CHECK(!std::memcmp(legacy->ir, accepted->ir, accepted->count * sizeof(*accepted->ir)));
    CHECK(!std::memcmp(&strict->linkage, &accepted->linkage, sizeof(strict->linkage)));
    std::free(strict);
    std::free(legacy);
    std::free(accepted);
    ralloc_free(shader);
}
static void partial_output_controls(bool packed)
{
    static const nir_shader_compiler_options options = {};
    const sgx535_fragment_nir_options selected = {packed ? SGX535_FRAGMENT_UNIFORM_DWORDS
                                                         : SGX535_FRAGMENT_UNIFORM_VEC4_SLOTS,
                                                  SGX535_FRAGMENT_IO_F32};
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                   "single partial shader output");
    nir_def *u = nir_load_uniform(&b, 4, 32, nir_imm_int(&b, 0), .base = 0,
                                  .range = packed ? 4u : 1u, .dest_type = nir_type_float32);
    const unsigned lane[4] = {3, 0, 1, 2};
    nir_store_output(&b, nir_swizzle(&b, u, lane, 4), nir_imm_int(&b, 0), .write_mask = 15,
                     .src_type = nir_type_float32,
                     .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
    auto *store = intrinsic(b.shader, nir_intrinsic_store_output);
    CHECK(store);
    auto *full = run(b.shader, &selected, SGX535_NIR_OK);
    auto *strict = run(b.shader, &strict_options, SGX535_NIR_OK, true);
    CHECK(full->count == strict->count &&
          !std::memcmp(full->ir, strict->ir, full->count * sizeof(*full->ir)) &&
          !std::memcmp(&full->linkage, &strict->linkage, sizeof(full->linkage)));
    std::free(strict);
    for (unsigned mask = 1; mask < 15; mask++) {
        nir_intrinsic_set_write_mask(store, mask);
        nir_validate_shader(b.shader, "actual partial output write mask");
        auto *ir = run(b.shader, &selected, SGX535_NIR_OK);
        CHECK(ir->count == full->count + 1 && !ir->linkage.count);
        for (unsigned c = 0; c < 4; c++) {
            const auto &out = ir->ir[ir->count - 4 + c];
            CHECK(out.opcode == SGX535_FRAGMENT_OUTPUT && out.index == c);
            if (mask & (1u << c))
                CHECK(uniform_root(ir, out.source[0]) == lane[c]);
            else {
                const auto &value = ir->ir[out.source[0]];
                CHECK(value.opcode == SGX535_FRAGMENT_CONSTANT && value.immediate == 0);
            }
        }
        auto *program =
            static_cast<sgx535_fragment_program *>(std::calloc(1, sizeof(sgx535_fragment_program)));
        CHECK(program &&
              sgx535_fragment_compile(ir->ir, ir->count, SGX535_FRAGMENT_RAW_F32_WORDS, program) ==
                  SGX535_NIR_OK &&
              program->secondary_register_count == 4 && program->uniform_words_used[0] == 15 &&
              !program->primary_register_count &&
              sgx535_fragment_native_admission(program) == SGX535_NIR_ERR_UNSUPPORTED);
        std::free(program);
        refused(b.shader, &selected, SGX535_NIR_ERR_LIMIT, false, ir->count - 1);
        refused(b.shader, &strict_options);
        refused(b.shader, &strict_options, SGX535_NIR_ERR_UNSUPPORTED, true);
        std::free(ir);
    }
    nir_intrinsic_set_write_mask(store, 15);
    auto *again = run(b.shader, &selected, SGX535_NIR_OK);
    CHECK(again->count == full->count &&
          !std::memcmp(again->ir, full->ir, full->count * sizeof(*full->ir)));
    std::free(again);
    std::free(full);

    for (unsigned bad = 0; bad < 9; bad++) {
        auto *copy = nir_shader_clone(nullptr, b.shader);
        CHECK(copy);
        auto *out = intrinsic(copy, nir_intrinsic_store_output);
        nir_intrinsic_set_write_mask(out, 1);
        nir_builder edit = nir_builder_create(nir_shader_get_entrypoint(copy));
        edit.constant_fold_alu = false;
        edit.cursor = nir_before_instr(&out->instr);
        if (bad < 2)
            nir_intrinsic_set_write_mask(out, bad ? 16 : 0);
        else if (bad == 2)
            nir_intrinsic_set_component(out, 1);
        else if (bad == 3)
            out->num_components = 3;
        else if (bad == 4)
            nir_src_rewrite(&out->src[0], nir_imm_vec3(&edit, 1, 0, 0));
        else if (bad == 5)
            copy->info.float_controls_execution_mode = 1;
        else if (bad == 6)
            nir_store_output(&edit, nir_imm_vec4(&edit, 0, 0, 0, 1), nir_imm_int(&edit, 0),
                             .write_mask = 15, .src_type = nir_type_float32,
                             .io_semantics = {.location = FRAG_RESULT_DATA0, .num_slots = 1});
        else if (bad == 7) {
            nir_foreach_block(block, edit.impl)
                nir_foreach_instr(instr, block) if (instr->type == nir_instr_type_alu)
                    nir_instr_as_alu(instr)
                        ->fp_math_ctrl = nir_fp_preserve_inf;
        } else
            (void)nir_fsin(&edit, nir_imm_float(&edit, .5f));
        refused(copy, &selected);
        ralloc_free(copy);
    }
    /* Captured CB0 semantics remain exact, including the alpha-to-red source
     * swizzle. A masked lane does not hide an unavailable dead uniform load. */
    nir_intrinsic_set_write_mask(store, 1);
    const uint32_t cb[4] = {0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000};
    nir_shader *copy = nullptr;
    char *before = printed(b.shader);
    CHECK(!poulsback_nir_specialize_solid_fragment(b.shader, selected.uniform_layout, cb,
                                                   sizeof(cb), &copy));
    auto *literal = static_cast<poulsback_nir_solid_fragment *>(
        std::calloc(1, sizeof(poulsback_nir_solid_fragment)));
    CHECK(literal &&
          poulsback_nir_solid_fragment_ir(copy, selected.uniform_layout, literal) == SGX535_NIR_OK);
    for (unsigned c = 0; c < 4; c++) {
        const auto &out = literal->instruction[literal->instruction_count - 4 + c];
        CHECK(out.opcode == SGX535_FRAGMENT_OUTPUT && out.index == c &&
              solid_word(literal, out.source[0]) == (c ? 0 : cb[3]));
    }
    std::free(literal);
    ralloc_free(copy);
    char *after = printed(b.shader);
    CHECK(!std::strcmp(before, after));
    std::free(before);
    std::free(after);
    nir_builder edit = nir_builder_create(nir_shader_get_entrypoint(b.shader));
    edit.cursor = nir_before_instr(&store->instr);
    (void)nir_load_uniform(&edit, 1, 32, nir_imm_int(&edit, 0), .base = packed ? 127 : 31,
                           .range = 1, .dest_type = nir_type_float32);
    copy = b.shader;
    CHECK(poulsback_nir_specialize_solid_fragment(b.shader, selected.uniform_layout, cb, sizeof(cb),
                                                  &copy) == -EOPNOTSUPP &&
          copy == b.shader);
    ralloc_free(b.shader);
}
static unsigned io_slots(const glsl_type *type, bool bindless)
{
    return bindless ? 0 : glsl_count_attribute_slots(type, false);
}
static unsigned vec4_slots(const glsl_type *type, bool bindless)
{
    return bindless ? 0 : glsl_count_vec4_slots(type, false, false);
}
static unsigned dword_slots(const glsl_type *type, bool bindless)
{
    return glsl_count_dword_slots(type, bindless);
}
static void input_controls()
{
    static const nir_shader_compiler_options generated_options = {};
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &generated_options,
                                                   "F32-IO-annotation-controls");
    b.constant_fold_alu = false;
    auto *in = nir_intrinsic_instr_create(b.shader, nir_intrinsic_load_input);
    in->num_components = 4;
    in->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));
    nir_intrinsic_set_base(in, 0);
    nir_intrinsic_set_range(in, 1);
    nir_intrinsic_set_component(in, 0);
    nir_intrinsic_set_dest_type(in, nir_type_float32);
    nir_io_semantics is = {};
    is.location = VARYING_SLOT_VAR0;
    is.num_slots = 1;
    nir_intrinsic_set_io_semantics(in, is);
    nir_def_init(&in->instr, &in->def, 4, 32);
    nir_builder_instr_insert(&b, &in->instr);
    auto *out = nir_intrinsic_instr_create(b.shader, nir_intrinsic_store_output);
    out->num_components = 4;
    out->src[0] = nir_src_for_ssa(&in->def);
    out->src[1] = nir_src_for_ssa(nir_imm_int(&b, 0));
    nir_intrinsic_set_range(out, 1);
    nir_intrinsic_set_write_mask(out, 15);
    nir_intrinsic_set_src_type(out, nir_type_float32);
    nir_io_semantics os = {};
    os.location = FRAG_RESULT_DATA0;
    os.num_slots = 1;
    nir_intrinsic_set_io_semantics(out, os);
    nir_builder_instr_insert(&b, &out->instr);
    nir_validate_shader(b.shader, "generated metadata-only F32 controls");
    auto *reference = run(b.shader, &strict_options, SGX535_NIR_OK, true);
    for (unsigned input_medium = 0; input_medium < 2; input_medium++)
        for (unsigned output_medium = 0; output_medium < 2; output_medium++) {
            is.medium_precision = input_medium;
            os.medium_precision = output_medium;
            nir_intrinsic_set_io_semantics(in, is);
            nir_intrinsic_set_io_semantics(out, os);
            auto *accepted = run(b.shader, &f32_options, SGX535_NIR_OK);
            CHECK(
                accepted->count == reference->count &&
                !std::memcmp(accepted->ir, reference->ir,
                             reference->count * sizeof(*reference->ir)) &&
                !std::memcmp(&accepted->linkage, &reference->linkage, sizeof(reference->linkage)));
            CHECK(accepted->linkage.count == 1 &&
                  accepted->linkage.varying[0].semantic == SGX535_FRAGMENT_GENERIC &&
                  accepted->linkage.varying[0].index == 0 &&
                  accepted->linkage.varying[0].components == 4 &&
                  accepted->linkage.varying[0].interpolation == SGX535_FRAGMENT_FLAT);
            std::free(accepted);
            if (input_medium || output_medium) {
                refused(b.shader, &strict_options);
                refused(b.shader, &strict_options, SGX535_NIR_ERR_UNSUPPORTED, true);
            }
        }
    for (unsigned field = 0; field < 5; field++) {
        auto bad = is;
        if (field == 0)
            bad.high_16bits = 1;
        else if (field == 1)
            bad.num_slots = 2;
        else if (field == 2)
            bad.per_view = 1;
        else if (field == 3)
            bad.dual_source_blend_index = 1;
        else
            bad.high_dvec2 = 1;
        nir_intrinsic_set_io_semantics(in, bad);
        refused(b.shader, &f32_options);
        nir_intrinsic_set_io_semantics(in, is);
    }
    nir_intrinsic_set_dest_type(in, nir_type_float16);
    refused(b.shader, &f32_options);
    nir_intrinsic_set_dest_type(in, nir_type_float32);
    in->def.bit_size = 16;
    refused(b.shader, &f32_options);
    in->def.bit_size = 32;
    std::free(reference);
    ralloc_free(b.shader);
}
static int actual_pair(int argc, char **argv, bool packed)
{
    if (argc != 3)
        return EXIT_FAILURE;
    static gl_context context;
    standalone_options options = {};
    options.glsl_version = 100;
    options.do_link = 0;
    options.just_log = 1;
    auto *p = standalone_compile_shader(&options, 2, argv + 1, &context);
    CHECK(p && p->NumShaders == 2 && p->Shaders[0]->CompileStatus == COMPILE_SUCCESS &&
          p->Shaders[1]->CompileStatus == COMPILE_SUCCESS);
    /* Match the recorded standalone CPU case, never an API capability. */
    context.Const.PackedDriverUniformStorage = packed;
    context.Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents = 64;
    context.Const.Program[MESA_SHADER_FRAGMENT].MaxCombinedUniformComponents = 64;
    _mesa_clear_shader_program_data(&context, p);
    p->data->LinkStatus = LINKING_SUCCESS;
    link_shaders_init(&context, p);
    gl_nir_link_glsl(&context, p);
    CHECK(p->data->LinkStatus == LINKING_SUCCESS);
    auto *vs = p->_LinkedShaders[MESA_SHADER_VERTEX];
    auto *fs = p->_LinkedShaders[MESA_SHADER_FRAGMENT];
    CHECK(vs && vs->Program && vs->Program->nir && fs && fs->Program && fs->Program->nir);
    nir_validate_shader(vs->Program->nir, "actual captured glamor vertex source");
    nir_validate_shader(fs->Program->nir, "actual captured glamor fragment source");
    auto *params = fs->Program->Parameters;
    CHECK(params && params->NumParameters == 1 && params->NumParameterValues == 4);
    CHECK(_mesa_lookup_parameter_index(params, "fg") == 0 &&
          params->Parameters[0].ValueOffset == 0 && params->Parameters[0].Size == 4);
    const uint32_t fg[4] = {0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000};
    for (unsigned i = 0; i < 4; i++)
        params->ParameterValues[i].u = fg[i];
    std::printf("ACTUAL_FG_PARAMETER index=0 offset=0 size=4 bits=%08x,%08x,%08x,%08x\n", fg[0],
                fg[1], fg[2], fg[3]);
    auto *original = fs->Program->nir;
    char *original_before = printed(original);
    auto *copy = nir_shader_clone(nullptr, original);
    CHECK(copy);
    unsigned uniforms = 0;
    nir_foreach_uniform_variable(var, copy)
    {
        CHECK(!glsl_contains_opaque(var->type) && !var->data.bindless && !var->data.access);
        int index = _mesa_lookup_parameter_index(params, var->name);
        CHECK(index == 0);
        var->data.driver_location = packed ? params->Parameters[index].ValueOffset : index;
        uniforms++;
    }
    CHECK(uniforms == 1);
    print_io("linked", copy);
    nir_lower_global_vars_to_local(copy);
    nir_lower_vars_to_ssa(copy);
    nir_assign_io_var_locations(copy, nir_var_shader_in);
    nir_assign_io_var_locations(copy, nir_var_shader_out);
    nir_lower_io(copy, static_cast<nir_variable_mode>(nir_var_shader_in | nir_var_shader_out),
                 io_slots, nir_lower_io_use_interpolated_input_intrinsics);
    nir_lower_io(copy, nir_var_uniform, packed ? dword_slots : vec4_slots,
                 static_cast<nir_lower_io_options>(0));
    poulsback_nir_finalize_fragment(copy);
    nir_validate_shader(copy, "actual glamor standard nonpacked F32 IO");
    print_io("lowered-before", copy);
    std::puts("ACTUAL_F32_IO_NIR_BEGIN");
    nir_print_shader(copy, stdout);
    std::puts("ACTUAL_F32_IO_NIR_END");
    char *lowered_before = printed(copy);
    output_controls(copy);
    solid_snapshot(copy, packed, fg);
    input_controls();
    partial_output_controls(packed);
    char *lowered_after = printed(copy);
    CHECK(!std::strcmp(lowered_before, lowered_after));
    print_io("lowered-after", copy);
    char *original_after = printed(original);
    CHECK(!std::strcmp(original_before, original_after));
    for (unsigned i = 0; i < 4; i++)
        CHECK(params->ParameterValues[i].u == fg[i]);
    std::free(original_before);
    std::free(original_after);
    std::free(lowered_before);
    std::free(lowered_after);
    ralloc_free(copy);
    standalone_compiler_cleanup(p, &context);
    std::printf(
        "PASS actual captured GLSL F32 IO: %u positive, %u negative controls, %u checks; original metadata retained; CPU only/native closed\n",
        positive_cases, negative_cases, checks);
    return EXIT_SUCCESS;
}
int main(int argc, char **argv)
{
    if (actual_pair(argc, argv, false))
        return EXIT_FAILURE;
    return actual_pair(argc, argv, true);
}
