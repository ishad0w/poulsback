// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* The fragment backend (src/compiler/fs_emit.c, fs_epilogue.c) on the host:
 * IR programs, hand-written and random, are compiled and linked; the words run
 * on the USSE model (sgx535_use_sim.c) and must give the bytes an IR
 * interpreter here gives, pixel for pixel of a quad; the epilogue variants
 * against GL's blend, mask, logic-op and alpha rules. --dump prints every
 * linked word with the model's rev121 text for independent decoding
 * (docs/re/sgx535/usse-instruction-contracts.md).
 * CPU only: no claim about the hardware's arithmetic. */
#include "sgx535_fs.h"
#include "sgx535_use_sim.c" /* the model, compiled with the test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, programs, words_total;
static bool dump;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)

static int rnd(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (int)(*s >> 16 & 0x7fffu);
}

/* ---- a deterministic texture -------------------------------------------------- */
/* The variant's samplers (formats) that the model's reads and the interpreter
 * share. */
static const struct sgx535_fs_key *texture_key;

/* GL's R, G, B, A of SAMPLER at C, in [0, 1]. */
static void content(unsigned sampler, const float c[3], unsigned dim, float rgba[4])
{
    float u = c[0], v = dim > 1 ? c[1] : 0.0f, w = dim > 2 ? c[2] : 0.0f;
    rgba[0] = u * 3.7f + (float)sampler * 0.13f - floorf(u * 3.7f + (float)sampler * 0.13f);
    rgba[1] = v * 2.3f - floorf(v * 2.3f);
    rgba[2] = (u + v + w) * 1.9f - floorf((u + v + w) * 1.9f);
    rgba[3] = (float)(64u + sampler * 7u) / 255.0f;
}

/* An F32 as IEEE half bits, round to nearest even (normal range only). */
static uint32_t half_bits(float f)
{
    uint32_t x;
    int e;
    float m;
    if (f == 0.0f)
        return 0;
    m = frexpf(fabsf(f), &e);          /* f = m 2^e, m in [0.5, 1) */
    x = (uint32_t)lrintf(m * 2048.0f); /* 11 bits with the implicit one */
    if (x == 2048u)
        x = 1024u, e++;
    return (f < 0 ? 0x8000u : 0u) | (uint32_t)(e + 14) << 10 | (x & 1023u);
}
static float half_value(uint32_t h)
{
    return ldexpf((float)((h & 1023u) | 1024u), (int)(h >> 10 & 31u) - 25) *
           (h & 0x8000u ? -1.0f : 1.0f) * (h & 0x7fffu ? 1.0f : 0.0f);
}

/* The dword the texture unit returns for plane PLANE of SAMPLER's format. */
static uint32_t texel(void *data, unsigned sampler, unsigned plane, const float c[3], unsigned dim)
{
    uint8_t result = texture_key ? texture_key->sampler[sampler].result : 0;
    float x[4];
    uint32_t b[4];
    (void)data;
    content(sampler, c, dim, x);
    for (unsigned k = 0; k < 4; k++)
        b[k] = (uint32_t)lrintf(255.0f * x[k]);
    switch (result) {
    case SGX535_FS_RESULT_U8:
        return b[0] * 0x01010101u; /* CHANREPLICATE */
    case SGX535_FS_RESULT_U88:
        return b[0] | b[1] << 8;
    case SGX535_FS_RESULT_U16:
        return (uint32_t)lrintf(65535.0f * x[0]);
    case SGX535_FS_RESULT_F32:
        return sgx535_sim_bits(x[0]);
    case SGX535_FS_RESULT_F16X4:
        return half_bits(x[2 * plane]) | half_bits(x[2 * plane + 1]) << 16;
    case SGX535_FS_RESULT_F32X4:
        return sgx535_sim_bits(x[plane]);
    default:
        return b[2] | b[1] << 8 | b[0] << 16 | b[3] << 24; /* B8G8R8A8: R in byte 2 */
    }
}

/* GL's value of a read's channels (0-3), as the format stores them: the
 * channels it lacks are 0 (G, B) and 1 (A). */
static void stored(unsigned sampler, const float c[3], unsigned dim, float ch[4])
{
    uint8_t result = texture_key->sampler[sampler].result;
    float x[4];
    content(sampler, c, dim, x);
    for (unsigned k = 0; k < 4; k++) {
        float byte = (float)lrintf(255.0f * x[k]) / 255.0f;
        unsigned have = result == SGX535_FS_RESULT_U8X4 || result >= SGX535_FS_RESULT_F16X4 ? 4u
                        : result == SGX535_FS_RESULT_U88                                    ? 2u
                                                                                            : 1u;
        ch[k] = k >= have                          ? (k == 3 ? 1.0f : 0.0f)
                : result == SGX535_FS_RESULT_U16   ? (float)lrintf(65535.0f * x[k]) / 65535.0f
                : result == SGX535_FS_RESULT_F32   ? x[k]
                : result == SGX535_FS_RESULT_F32X4 ? x[k]
                : result == SGX535_FS_RESULT_F16X4 ? half_value(half_bits(x[k]))
                                                   : byte;
    }
}

static float wrapped(float c, uint8_t mode)
{
    float f = c - floorf(c), m;
    if (mode == SGX535_FS_WRAP_REPEAT)
        return f;
    m = 2.0f * (c * 0.5f - floorf(c * 0.5f)) - 1.0f;
    return mode == SGX535_FS_WRAP_MIRRORED ? 1.0f - fabsf(m) : c;
}

/* GL 2.1 table 3.27: 1.0 where REF FUNC DEPTH. */
static float shadow_test(unsigned func, float ref, float depth)
{
    bool r[8] = {false, ref<depth, ref == depth, ref <= depth, ref> depth, ref != depth,
                 ref >= depth, true};
    return r[func & 7u] ? 1.0f : 0.0f;
}

/* ---- the IR interpreter ---------------------------------------------------------- */
struct inputs {
    float varying[4][SGX535_FS_MAX_VARYINGS][4]; /* per pixel */
    uint32_t uniform[64];
    float frag[4][4]; /* window x, y, z, w per pixel */
    float height, ysign;
    unsigned back[4];
    unsigned facing_sense;
};

static float f32(uint32_t bits)
{
    return sgx535_sim_f32(bits);
}
static float modded(float x, uint8_t mod)
{
    if (mod & SGX535_USE_ABS)
        x = fabsf(x);
    if (mod & SGX535_USE_NEG)
        x = -x;
    return x;
}

/* Bytes B, G, R, A of COLOR0 per pixel, as the opaque epilogue packs them. */
static bool interpret(const struct sgx535_fs_instruction *ir, uint32_t n, const struct inputs *in,
                      uint32_t out[4], bool killed[4])
{
    static float v[SGX535_FS_MAX_INSTRUCTIONS][4];
    static float tex[SGX535_FS_MAX_INSTRUCTIONS][4][4];
    float var[SGX535_FS_MAX_VARS][4] = {{0}}, color[4][4] = {{0}};
    uint32_t stack[SGX535_FS_MAX_DEPTH], depth = 0, steps = 0;
    for (uint32_t i = 0; i < n; i++, steps++) {
        const struct sgx535_fs_instruction *x = &ir[i];
        if (steps > 200000)
            return false;
        for (unsigned p = 0; p < 4; p++) {
            float a = 0, b = 0, c = 0;
            if (x->op >= SGX535_FS_MOV && x->op <= SGX535_FS_SEL) {
                a = modded(v[x->src[0]][p], x->mod[0]);
                b = modded(v[x->src[1]][p], x->mod[1]);
                c = modded(v[x->src[2]][p], x->mod[2]);
            }
            switch (x->op) {
            case SGX535_FS_CONST:
                v[i][p] = f32(x->imm);
                break;
            case SGX535_FS_INPUT:
                v[i][p] = in->varying[p][x->index][x->comp];
                break;
            case SGX535_FS_UNIFORM:
                v[i][p] = x->comp == 2   ? (in->uniform[x->index] ? 1.0f : 0.0f)
                          : x->comp == 1 ? (float)(int32_t)in->uniform[x->index]
                                         : f32(in->uniform[x->index]);
                break;
            case SGX535_FS_SYSTEM:
                if (x->index == SGX535_FS_FRAG_Y)
                    v[i][p] = in->ysign * in->frag[p][1] + in->height;
                else if (x->index == SGX535_FS_FRONT_FACING)
                    v[i][p] = ((in->back[p] ^ in->facing_sense) & 1u) ? 0.0f : 1.0f;
                else
                    v[i][p] = in->frag[p][x->index];
                break;
            case SGX535_FS_MOV:
                v[i][p] = a;
                break;
            case SGX535_FS_ADD:
                v[i][p] = a + b;
                break;
            case SGX535_FS_MUL:
                v[i][p] = a * b;
                break;
            case SGX535_FS_MAD:
                v[i][p] = a * b + c;
                break;
            case SGX535_FS_MIN:
                v[i][p] = fminf(a, b);
                break;
            case SGX535_FS_MAX:
                v[i][p] = fmaxf(a, b);
                break;
            case SGX535_FS_RCP:
                v[i][p] = 1.0f / a;
                break;
            case SGX535_FS_RSQ:
                v[i][p] = 1.0f / sqrtf(a);
                break;
            case SGX535_FS_LOG2:
                v[i][p] = log2f(a);
                break;
            case SGX535_FS_EXP2:
                v[i][p] = exp2f(a);
                break;
            case SGX535_FS_FRACT:
                v[i][p] = a - floorf(a);
                break;
            case SGX535_FS_DDX: {
                unsigned l = p & 2u;
                v[i][p] =
                    modded(v[x->src[0]][l + 1], x->mod[0]) - modded(v[x->src[0]][l], x->mod[0]);
                break;
            }
            case SGX535_FS_DDY: {
                unsigned t = p & 1u;
                /* GL's y: the window's rows run down. */
                v[i][p] = in->ysign * (modded(v[x->src[0]][t + 2], x->mod[0]) -
                                       modded(v[x->src[0]][t], x->mod[0]));
                break;
            }
            case SGX535_FS_SLT:
                v[i][p] = a < b ? 1.0f : 0.0f;
                break;
            case SGX535_FS_SGE:
                v[i][p] = a >= b ? 1.0f : 0.0f;
                break;
            case SGX535_FS_SEQ:
                v[i][p] = a == b ? 1.0f : 0.0f;
                break;
            case SGX535_FS_SNE:
                v[i][p] = a != b ? 1.0f : 0.0f;
                break;
            case SGX535_FS_SEL:
                v[i][p] = a != 0.0f ? b : c;
                break;
            case SGX535_FS_TEX: {
                const struct sgx535_fs_sampler *s = &texture_key->sampler[x->index];
                float coord[3] = {0, 0, 0};
                unsigned r = x->dim + (x->comp != SGX535_FS_TEX_IMPLICIT ? 1u : 0u);
                for (unsigned k = 0; k < x->dim; k++) {
                    coord[k] = modded(v[x->src[k]][p], x->mod[k]);
                    if (k < 2 && s->wrap[k])
                        coord[k] = wrapped(coord[k], s->wrap[k]);
                }
                stored(x->index, coord, x->dim, tex[i][p]);
                if (x->imm && s->compare) {
                    tex[i][p][0] = shadow_test(s->compare - 1u, modded(v[x->src[r]][p], x->mod[r]),
                                               tex[i][p][0]);
                    tex[i][p][1] = tex[i][p][2] = 0.0f, tex[i][p][3] = 1.0f;
                }
                break;
            }
            case SGX535_FS_TEXC: {
                unsigned sel = texture_key->sampler[ir[x->src[0]].index].swizzle[x->comp];
                unsigned ch = sel ? sel - 1u : x->comp;
                v[i][p] = ch < 4 ? tex[x->src[0]][p][ch] : ch == 4 ? 0.0f : 1.0f;
                break;
            }
            case SGX535_FS_OUTPUT:
                if (x->index < 4)
                    color[p][x->index] = modded(v[x->src[0]][p], x->mod[0]);
                break;
            case SGX535_FS_STORE:
                var[x->index][p] = modded(v[x->src[0]][p], x->mod[0]);
                break;
            case SGX535_FS_KILL:
                if (modded(v[x->src[0]][p], x->mod[0]) != 0.0f)
                    killed[p] = true;
                break;
            case SGX535_FS_LOAD:
                v[i][p] = var[x->index][p];
                break;
            default:
                break;
            }
        }
        switch (x->op) {
        case SGX535_FS_IF:
            if (modded(v[x->src[0]][0], x->mod[0]) == 0.0f) {
                /* to the matching ELSE (and past it) or ENDIF */
                unsigned d = 0;
                for (i++; i < n; i++) {
                    if (ir[i].op == SGX535_FS_IF || ir[i].op == SGX535_FS_LOOP)
                        d++;
                    else if ((ir[i].op == SGX535_FS_ENDIF || ir[i].op == SGX535_FS_ENDLOOP) && d)
                        d--;
                    else if (!d && (ir[i].op == SGX535_FS_ELSE || ir[i].op == SGX535_FS_ENDIF))
                        break;
                }
            }
            break;
        case SGX535_FS_ELSE: {
            unsigned d = 0;
            for (i++; i < n; i++) {
                if (ir[i].op == SGX535_FS_IF || ir[i].op == SGX535_FS_LOOP)
                    d++;
                else if ((ir[i].op == SGX535_FS_ENDIF || ir[i].op == SGX535_FS_ENDLOOP) && d)
                    d--;
                else if (!d && ir[i].op == SGX535_FS_ENDIF)
                    break;
            }
            break;
        }
        case SGX535_FS_LOOP:
            stack[depth++] = i;
            break;
        case SGX535_FS_ENDLOOP:
            i = stack[depth - 1];
            break;
        case SGX535_FS_CONTINUE: {
            /* the innermost loop's top */
            unsigned d = 0;
            uint32_t j = i;
            while (j--) {
                if (ir[j].op == SGX535_FS_ENDLOOP)
                    d++;
                else if (ir[j].op == SGX535_FS_LOOP && !d--)
                    break;
            }
            while (depth && stack[depth - 1] != j)
                depth--;
            i = j;
            break;
        }
        case SGX535_FS_BREAK: {
            unsigned d = 0;
            for (i++; i < n; i++) {
                if (ir[i].op == SGX535_FS_LOOP)
                    d++;
                else if (ir[i].op == SGX535_FS_ENDLOOP && !d--)
                    break;
            }
            depth--;
            break;
        }
        default:
            break;
        }
    }
    for (unsigned p = 0; p < 4; p++) {
        uint32_t w = 0;
        for (unsigned c = 0; c < 4; c++) {
            float f = color[p][c];
            f = f != f ? 0.0f : f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
            w |= (uint32_t)lrintf(f * 255.0f) << (8u * (unsigned)"\2\1\0\3"[c]);
        }
        out[p] = w;
    }
    return true;
}

/* ---- running a program --------------------------------------------------------------- */
static struct sgx535_fs_program body;
static struct sgx535_fs_variant variant;

static void load(struct sgx535_sim *sim, const struct sgx535_fs_key *key, const struct inputs *in)
{
    memset(sim, 0, sizeof(*sim));
    sim->sample = texel;
    for (uint32_t c = 1; c < body.constant_count; c++) {
        const struct sgx535_fs_constant *k = &body.constant[c];
        uint32_t w = 0;
        switch (k->kind) {
        case SGX535_FS_CONSTANT_UNIFORM:
            w = in->uniform[k->value];
            break;
        case SGX535_FS_CONSTANT_UNIFORM_INT:
            w = sgx535_sim_bits((float)(int32_t)in->uniform[k->value]);
            break;
        case SGX535_FS_CONSTANT_UNIFORM_BOOL:
            w = sgx535_sim_bits(in->uniform[k->value] ? 1.0f : 0.0f);
            break;
        case SGX535_FS_CONSTANT_TEXSIZE:
            w = sgx535_sim_bits(64.0f);
            break;
        case SGX535_FS_CONSTANT_LITERAL:
            w = k->value;
            break;
        case SGX535_FS_CONSTANT_TEXTURE:
            w = 0x7e000000u | (k->value >> 2) << 2;
            break;
        case SGX535_FS_CONSTANT_FRAG_Y:
            w = sgx535_sim_bits(in->height);
            break;
        case SGX535_FS_CONSTANT_FRAG_YSIGN:
            w = sgx535_sim_bits(in->ysign);
            break;
        case SGX535_FS_CONSTANT_FACING:
            w = in->facing_sense;
            break;
        }
        sim->sa[c] = w;
    }
    /* the epilogue's words as a draw without tests gives them: everything passes */
    {
        static const struct sgx535_fs_feedback none = {
            .depth_func = 7, .stencil_func = 7, .back_func = 7, .alpha_func = 7};
        uint32_t feedback[2];
        sgx535_fs_feedback_words(&none, feedback);
        for (uint32_t c = body.constant_count; c < variant.constant_count; c++)
            if (variant.constant[c].kind == SGX535_FS_CONSTANT_EPILOGUE &&
                variant.constant[c].value != SGX535_FS_BLEND_COLOR)
                sim->sa[c] = feedback[variant.constant[c].value - 1u];
    }
    for (unsigned p = 0; p < 4; p++) {
        sim->pixel[p].g16 = in->back[p];
        for (uint32_t t = 0; t < body.iteration_count; t++) {
            const struct sgx535_fs_iteration *it = &body.iteration[t];
            unsigned issue = it->douti >> 12 & 15u;
            for (unsigned c = 0; c < it->dwords; c++) {
                uint32_t value;
                if (it->texture) {
                    float coord[3] = {in->varying[p][it->douti & 15u][0],
                                      in->varying[p][it->douti & 15u][1], 0};
                    value = texel(NULL, it->sampler, 0, coord, 2);
                } else if (issue == 13)
                    value = sgx535_sim_bits(in->frag[p][c]);
                else
                    value = sgx535_sim_bits(in->varying[p][issue][c]);
                sim->pixel[p].pa[it->pa + c] = value;
            }
        }
    }
    (void)key;
}

static void print_words(void)
{
    for (uint32_t i = 0; i < variant.instruction_count; i++) {
        char text[160];
        uint32_t w0 = variant.words[2 * i], w1 = variant.words[2 * i + 1];
        if (!sgx535_sim_text(w0, w1, text, sizeof(text)))
            snprintf(text, sizeof(text), "?");
        printf("%08x %08x %s\n", w0, w1, text);
    }
}

/* Compile, link and compare with the interpreter on ROUNDS random inputs. */
static bool run(const char *name, const struct sgx535_fs_instruction *ir, uint32_t n,
                const struct sgx535_fs_key *key, unsigned rounds, unsigned *seed)
{
    static const struct sgx535_fs_epilogue_key opaque = {.target = SGX535_FS_TARGET_B8G8R8A8,
                                                         .colormask = 15};
    enum sgx535_fs_status status;
    texture_key = key;
    status = sgx535_fs_compile(ir, n, key, &body);
    if (status != SGX535_FS_OK) {
        fprintf(stderr, "%s: compile %d at %u: %s\n", name, status, body.failed_at,
                body.reason ? body.reason : "");
        return false;
    }
    status = sgx535_fs_link(&body, &opaque, &variant);
    if (status != SGX535_FS_OK) {
        fprintf(stderr, "%s: link %d\n", name, status);
        return false;
    }
    programs++;
    words_total += variant.instruction_count;
    if (dump) {
        print_words();
        return true;
    }
    for (unsigned r = 0; r < rounds; r++) {
        struct inputs in;
        struct sgx535_sim sim;
        uint32_t want[4];
        memset(&in, 0, sizeof(in));
        for (unsigned p = 0; p < 4; p++) {
            for (unsigned v = 0; v < SGX535_FS_MAX_VARYINGS; v++)
                for (unsigned c = 0; c < 4; c++)
                    in.varying[p][v][c] = (float)(rnd(seed) % 2001 - 1000) / 400.0f;
            in.frag[p][0] = 10.5f + (float)(p & 1u);
            in.frag[p][1] = 20.5f + (float)(p >> 1);
            in.frag[p][2] = (float)(rnd(seed) % 1000) / 1000.0f;
            in.frag[p][3] = 1.0f;
            in.back[p] = (unsigned)(rnd(seed) & 1);
        }
        for (unsigned u = 0; u < 64; u++)
            in.uniform[u] = sgx535_sim_bits((float)(rnd(seed) % 2001 - 1000) / 300.0f);
        in.uniform[60] = sgx535_sim_bits((float)(rnd(seed) % 5)); /* loop counts */
        in.uniform[61] = (rnd(seed) & 1) ? ~0u : 0u; /* a bool, Mesa's native-integer true */
        in.uniform[62] = (uint32_t)(rnd(seed) % 6);  /* an int */
        in.height = 64.0f;
        in.ysign = (rnd(seed) & 1) ? -1.0f : 1.0f;
        in.facing_sense = (unsigned)(rnd(seed) & 1);
        bool killed[4] = {false, false, false, false};
        if (!interpret(ir, n, &in, want, killed)) {
            fprintf(stderr, "%s: interpreter ran away\n", name);
            return false;
        }
        load(&sim, key, &in);
        /* a discard links as punch-through: phase 2 writes the colour */
        if (!sgx535_sim_run(&sim, variant.words,
                            variant.phase2 ? variant.phase2 : variant.instruction_count) ||
            (variant.phase2 && !sgx535_sim_run(&sim, variant.words + 2u * variant.phase2,
                                               variant.instruction_count - variant.phase2))) {
            fprintf(stderr, "%s: model: %s\n", name, sim.error);
            return false;
        }
        for (unsigned p = 0; p < 4; p++) {
            uint32_t got = sim.pixel[p].o[0];
            if (sim.pixel[p].discarded != killed[p]) {
                fprintf(stderr, "%s: round %u pixel %u: discard %d, interpreter %d\n", name, r, p,
                        sim.pixel[p].discarded, killed[p]);
                return false;
            }
            if (killed[p])
                continue;
            for (unsigned c = 0; c < 4; c++) {
                int d = (int)(got >> (8 * c) & 0xffu) - (int)(want[p] >> (8 * c) & 0xffu);
                if (d < -1 || d > 1) {
                    fprintf(stderr, "%s: round %u pixel %u: %08x, interpreter %08x\n", name, r, p,
                            got, want[p]);
                    return false;
                }
            }
        }
    }
    return true;
}

/* ---- programs -------------------------------------------------------------------------- */
#define I(...) ((struct sgx535_fs_instruction){__VA_ARGS__})
struct builder {
    struct sgx535_fs_instruction ir[SGX535_FS_MAX_INSTRUCTIONS];
    uint32_t n;
};
static uint16_t add(struct builder *b, struct sgx535_fs_instruction x)
{
    b->ir[b->n] = x;
    return (uint16_t)b->n++;
}
static uint16_t op1(struct builder *b, uint8_t op, uint16_t a)
{
    return add(b, I(.op = op, .src = {a}));
}
static uint16_t op2(struct builder *b, uint8_t op, uint16_t a, uint16_t c)
{
    return add(b, I(.op = op, .src = {a, c}));
}
static uint16_t constf(struct builder *b, float f)
{
    return add(b, I(.op = SGX535_FS_CONST, .imm = sgx535_sim_bits(f)));
}
static void outputs(struct builder *b, uint16_t r, uint16_t g, uint16_t bl, uint16_t a)
{
    uint16_t c[4] = {r, g, bl, a};
    for (unsigned k = 0; k < 4; k++)
        add(b, I(.op = SGX535_FS_OUTPUT, .index = (uint16_t)k, .src = {c[k]}));
}

static struct sgx535_fs_key key4(void)
{
    struct sgx535_fs_key k;
    memset(&k, 0, sizeof(k));
    for (unsigned v = 0; v < 4; v++)
        k.varying[v] = (struct sgx535_fs_varying){SGX535_FS_SMOOTH, 4, 0};
    k.varying[3].interp = SGX535_FS_FLAT;
    return k;
}

static void fixed_programs(unsigned *seed)
{
    struct sgx535_fs_key key = key4();
    struct builder *b = calloc(1, sizeof(*b));
    uint16_t x, y, z, w, t, u, i, cond, acc;

    /* gl_FragColor = varying 0 */
    for (unsigned c = 0; c < 4; c++)
        add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = (uint8_t)c));
    outputs(b, 0, 1, 2, 3);
    CHECK(run("copy", b->ir, b->n, &key, 8, seed));
    CHECK(body.primary == 4 && body.iteration_count == 1 && body.iteration[0].douti == 0x0dc0000fu);
    CHECK(variant.instruction_count == 2); /* two packs, EMGD's */

    /* arithmetic, modifiers, uniforms, literals */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 0));
    y = add(b, I(.op = SGX535_FS_UNIFORM, .index = 3));
    z = add(b, I(.op = SGX535_FS_ADD, .src = {x, y}, .mod = {SGX535_USE_NEG, SGX535_USE_ABS}));
    w = add(b, I(.op = SGX535_FS_MAD, .src = {y, z, constf(b, 0.375f)}));
    t = op1(b, SGX535_FS_FRACT, w);
    u = op1(b, SGX535_FS_RCP,
            add(b, I(.op = SGX535_FS_MAX, .src = {z, constf(b, 0.5f)}, .mod = {SGX535_USE_ABS})));
    outputs(b, t, op1(b, SGX535_FS_FRACT, u), op2(b, SGX535_FS_MUL, y, y), constf(b, 1.0f));
    CHECK(run("alu", b->ir, b->n, &key, 64, seed));

    /* compare and select, exp/log/rsq */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 1));
    y = add(b, I(.op = SGX535_FS_INPUT, .index = 2, .comp = 3));
    cond = op2(b, SGX535_FS_SLT, x, y);
    z = add(b, I(.op = SGX535_FS_SEL, .src = {cond, x, y}, .mod = {0, SGX535_USE_NEG}));
    w = op1(b, SGX535_FS_EXP2, op2(b, SGX535_FS_MIN, z, constf(b, 3.0f)));
    t = op1(b, SGX535_FS_LOG2,
            op2(b, SGX535_FS_MAX,
                add(b, I(.op = SGX535_FS_MOV, .src = {y}, .mod = {SGX535_USE_ABS})),
                constf(b, 0.25f)));
    u = op1(b, SGX535_FS_RSQ,
            op2(b, SGX535_FS_ADD,
                add(b, I(.op = SGX535_FS_MOV, .src = {x}, .mod = {SGX535_USE_ABS})),
                constf(b, 1.0f)));
    outputs(b, op1(b, SGX535_FS_FRACT, w), op1(b, SGX535_FS_FRACT, t), u,
            op2(b, SGX535_FS_SEQ, op2(b, SGX535_FS_SGE, x, y),
                op2(b, SGX535_FS_SNE, cond, constf(b, 1.0f))));
    CHECK(run("compare", b->ir, b->n, &key, 64, seed));

    /* derivatives of a varying, of a uniform, and of a product */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 0));
    y = add(b, I(.op = SGX535_FS_UNIFORM, .index = 1));
    z = op2(b, SGX535_FS_MUL, x, x);
    outputs(b, op1(b, SGX535_FS_FRACT, op1(b, SGX535_FS_DDX, x)),
            op1(b, SGX535_FS_FRACT, op1(b, SGX535_FS_DDY, z)), op1(b, SGX535_FS_DDX, y),
            op1(b, SGX535_FS_FRACT,
                add(b, I(.op = SGX535_FS_DDY, .src = {x}, .mod = {SGX535_USE_NEG}))));
    CHECK(run("derivatives", b->ir, b->n, &key, 32, seed));
    CHECK(body.uses_derivatives);
    for (uint32_t k = 0; k < body.instruction_count; k++)
        CHECK(!(body.words[2 * k + 1] >> 23 & 1) || (body.words[2 * k + 1] >> 27) == 9);

    /* textures: a direct read, a dependent read with bias, a 3D read */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 0));
    y = add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 1));
    t = add(b, I(.op = SGX535_FS_TEX, .index = 2, .dim = 2, .src = {x, y}));
    z = op2(b, SGX535_FS_MUL, x, add(b, I(.op = SGX535_FS_UNIFORM, .index = 0)));
    u = add(b, I(.op = SGX535_FS_TEX, .index = 5, .dim = 2, .comp = SGX535_FS_TEX_BIAS,
                 .src = {z, y, constf(b, 1.0f)}));
    w = add(b, I(.op = SGX535_FS_TEX, .index = 7, .dim = 3, .comp = SGX535_FS_TEX_LOD,
                 .src = {y, z, x, constf(b, 2.0f)}));
    outputs(b, add(b, I(.op = SGX535_FS_TEXC, .comp = 0, .src = {t})),
            add(b, I(.op = SGX535_FS_TEXC, .comp = 1, .src = {u})),
            add(b, I(.op = SGX535_FS_TEXC, .comp = 2, .src = {w})),
            add(b, I(.op = SGX535_FS_TEXC, .comp = 3, .src = {t})));
    CHECK(run("textures", b->ir, b->n, &key, 32, seed));
    CHECK(body.iteration_count >= 1 && body.iteration[body.iteration_count - 1].texture == 1 &&
          body.iteration[body.iteration_count - 1].douti == 0x0c00f101u);

    /* uniform IF/ELSE with a VAR merge, nested in a loop counted by a uniform */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 0));
    y = add(b, I(.op = SGX535_FS_UNIFORM, .index = 60));
    add(b, I(.op = SGX535_FS_STORE, .index = 0, .src = {constf(b, 0.0f)}));
    add(b, I(.op = SGX535_FS_STORE, .index = 1, .src = {x}));
    add(b, I(.op = SGX535_FS_LOOP));
    i = add(b, I(.op = SGX535_FS_LOAD, .index = 0));
    cond = op2(b, SGX535_FS_SGE, i, y);
    add(b, I(.op = SGX535_FS_IF, .src = {cond}));
    add(b, I(.op = SGX535_FS_BREAK));
    add(b, I(.op = SGX535_FS_ENDIF));
    acc = add(b, I(.op = SGX535_FS_LOAD, .index = 1));
    add(b, I(.op = SGX535_FS_IF, .src = {op2(b, SGX535_FS_SLT, i, constf(b, 2.0f))}));
    add(b, I(.op = SGX535_FS_STORE, .index = 1,
             .src = {
                 add(b, I(.op = SGX535_FS_MAD, .src = {acc, constf(b, 1.25f), constf(b, 0.5f)}))}));
    add(b, I(.op = SGX535_FS_ELSE));
    add(b, I(.op = SGX535_FS_STORE, .index = 1, .src = {op2(b, SGX535_FS_MUL, acc, acc)}));
    add(b, I(.op = SGX535_FS_ENDIF));
    add(b,
        I(.op = SGX535_FS_STORE, .index = 0, .src = {op2(b, SGX535_FS_ADD, i, constf(b, 1.0f))}));
    add(b, I(.op = SGX535_FS_ENDLOOP));
    acc = add(b, I(.op = SGX535_FS_LOAD, .index = 1));
    outputs(b, op1(b, SGX535_FS_FRACT, acc), op1(b, SGX535_FS_FRACT, x),
            add(b, I(.op = SGX535_FS_LOAD, .index = 0)), constf(b, 0.5f));
    CHECK(run("loop", b->ir, b->n, &key, 64, seed));

    /* gl_FragCoord, gl_FrontFacing, a flat varying */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_SYSTEM, .index = SGX535_FS_FRAG_X));
    y = add(b, I(.op = SGX535_FS_SYSTEM, .index = SGX535_FS_FRAG_Y));
    z = add(b, I(.op = SGX535_FS_SYSTEM, .index = SGX535_FS_FRONT_FACING));
    w = add(b, I(.op = SGX535_FS_INPUT, .index = 3, .comp = 2));
    outputs(b, op2(b, SGX535_FS_MUL, x, constf(b, 1.0f / 32.0f)),
            op2(b, SGX535_FS_MUL, y, constf(b, 1.0f / 64.0f)), z, op1(b, SGX535_FS_FRACT, w));
    CHECK(run("system", b->ir, b->n, &key, 32, seed));
    CHECK(body.iteration_count == 2 && body.iteration[0].douti == 0x09c0300fu &&
          body.iteration[1].douti == 0x0dc0d00fu);

    /* refusals */
    b->n = 0;
    add(b, I(.op = SGX535_FS_IF, .src = {0}));
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_ARGUMENT);
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 0));
    add(b, I(.op = SGX535_FS_IF, .src = {x}));
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_ARGUMENT);
    b->n = 0;
    add(b, I(.op = SGX535_FS_INPUT, .index = 7, .comp = 0));
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_ARGUMENT);
    b->n = 0;
    add(b, I(.op = SGX535_FS_BREAK));
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_ARGUMENT);
    /* three coordinates, a LOD and a comparator: five sources of four */
    b->n = 0;
    add(b, I(.op = SGX535_FS_CONST));
    add(b, I(.op = SGX535_FS_TEX, .dim = 3, .comp = SGX535_FS_TEX_LOD, .imm = 1));
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_UNSUPPORTED && body.reason &&
          !strcmp(body.reason, "texture form"));
    /* 70 values alive at once exceed the 64 temporaries */
    b->n = 0;
    x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 0));
    for (unsigned k = 0; k < 70; k++)
        op2(b, SGX535_FS_MUL, x, constf(b, (float)k + 2.0f));
    t = constf(b, 0.0f);
    for (unsigned k = 0; k < 70; k++)
        t = op2(b, SGX535_FS_ADD, t, (uint16_t)(2 + 2 * k));
    outputs(b, t, t, t, t);
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_ERR_LIMIT);
    free(b);
}

/* Random straight-line and uniform-flow programs. */
static void random_programs(unsigned count, unsigned *seed)
{
    struct sgx535_fs_key key = key4();
    struct builder *b = calloc(1, sizeof(*b));
    for (unsigned k = 0; k < count; k++) {
        uint16_t vals[256], nv = 0;
        char name[32];
        unsigned length = 8u + (unsigned)rnd(seed) % 40u;
        b->n = 0;
        for (unsigned v = 0; v < 3; v++)
            vals[nv++] = add(b, I(.op = SGX535_FS_INPUT, .index = (uint16_t)v,
                                  .comp = (uint8_t)(rnd(seed) % 4)));
        vals[nv++] = add(b, I(.op = SGX535_FS_UNIFORM, .index = (uint16_t)(rnd(seed) % 16)));
        vals[nv++] = constf(b, (float)(rnd(seed) % 100) / 37.0f);
        for (unsigned s = 0; s < length && nv < 250; s++) {
            uint16_t a = vals[rnd(seed) % nv], c = vals[rnd(seed) % nv], d = vals[rnd(seed) % nv];
            uint8_t m0 = (uint8_t)(rnd(seed) % 4 == 0 ? rnd(seed) % 4 : 0);
            uint16_t r;
            switch (rnd(seed) % 14) {
            case 0:
                r = add(b, I(.op = SGX535_FS_ADD, .src = {a, c}, .mod = {m0}));
                break;
            case 1:
                r = add(b, I(.op = SGX535_FS_MUL, .src = {a, c}, .mod = {0, m0}));
                break;
            case 2:
                r = add(b, I(.op = SGX535_FS_MAD, .src = {a, c, d}, .mod = {m0, 0, m0}));
                break;
            case 3:
                r = op2(b, rnd(seed) & 1 ? SGX535_FS_MIN : SGX535_FS_MAX, a, c);
                break;
            case 4:
                r = op1(b, SGX535_FS_RCP,
                        add(b, I(.op = SGX535_FS_MAX, .src = {a, constf(b, 0.25f)},
                                 .mod = {SGX535_USE_ABS})));
                break;
            case 5:
                r = op1(b, SGX535_FS_RSQ,
                        add(b, I(.op = SGX535_FS_MAX, .src = {a, constf(b, 0.25f)},
                                 .mod = {SGX535_USE_ABS})));
                break;
            case 6:
                r = op1(b, SGX535_FS_LOG2,
                        add(b, I(.op = SGX535_FS_MAX, .src = {a, constf(b, 0.25f)},
                                 .mod = {SGX535_USE_ABS})));
                break;
            case 7:
                r = op1(b, SGX535_FS_EXP2, op2(b, SGX535_FS_MIN, a, constf(b, 4.0f)));
                break;
            case 8:
                r = add(b, I(.op = SGX535_FS_FRACT, .src = {a}, .mod = {m0}));
                break;
            case 9:
                r = op2(b, (uint8_t)(SGX535_FS_SLT + rnd(seed) % 4), a, c);
                break;
            case 10:
                r = add(b, I(.op = SGX535_FS_SEL, .src = {op2(b, SGX535_FS_SLT, a, c), c, d},
                             .mod = {0, m0, 0}));
                break;
            case 11:
                r = op1(b, rnd(seed) & 1 ? SGX535_FS_DDX : SGX535_FS_DDY, a);
                break;
            case 12: {
                uint16_t t = add(b, I(.op = SGX535_FS_TEX, .index = (uint16_t)(rnd(seed) % 4),
                                      .dim = 2, .src = {a, c}));
                r = add(b, I(.op = SGX535_FS_TEXC, .comp = (uint8_t)(rnd(seed) % 4), .src = {t}));
                break;
            }
            default:
                r = add(b, I(.op = SGX535_FS_UNIFORM, .index = (uint16_t)(rnd(seed) % 32)));
                break;
            }
            vals[nv++] = r;
        }
        outputs(b, op1(b, SGX535_FS_FRACT, vals[nv - 1]), op1(b, SGX535_FS_FRACT, vals[nv - 2]),
                op1(b, SGX535_FS_FRACT, vals[rnd(seed) % nv]), vals[rnd(seed) % nv]);
        snprintf(name, sizeof(name), "random %u", k);
        CHECK(run(name, b->ir, b->n, &key, 16, seed));
    }
    free(b);
}

/* ---- the epilogue against GL's per-fragment operations -------------------------------- */

static unsigned epilogue_variants, epilogue_cases, epilogue_punchthrough;

/* GL 2.1's blend factor (table 4.1) for component C of source S, destination
 * D and constant K, all in [0, 1]. */
static double gl_factor(uint8_t f, unsigned c, const double s[4], const double d[4],
                        const double k[4])
{
    switch (f) {
    case SGX535_FS_ZERO:
        return 0;
    case SGX535_FS_ONE:
        return 1;
    case SGX535_FS_SRC_COLOR:
        return s[c];
    case SGX535_FS_ONE_MINUS_SRC_COLOR:
        return 1 - s[c];
    case SGX535_FS_DST_COLOR:
        return d[c];
    case SGX535_FS_ONE_MINUS_DST_COLOR:
        return 1 - d[c];
    case SGX535_FS_SRC_ALPHA:
        return s[3];
    case SGX535_FS_ONE_MINUS_SRC_ALPHA:
        return 1 - s[3];
    case SGX535_FS_DST_ALPHA:
        return d[3];
    case SGX535_FS_ONE_MINUS_DST_ALPHA:
        return 1 - d[3];
    case SGX535_FS_CONSTANT_COLOR:
        return k[c];
    case SGX535_FS_ONE_MINUS_CONSTANT_COLOR:
        return 1 - k[c];
    case SGX535_FS_CONSTANT_ALPHA:
        return k[3];
    case SGX535_FS_ONE_MINUS_CONSTANT_ALPHA:
        return 1 - k[3];
    default: /* SRC_ALPHA_SATURATE */
        return c == 3 ? 1 : fmin(s[3], 1 - d[3]);
    }
}

/* The body: COLOR0 from four uniforms; DEPTH and KILL as asked. */
static void epilogue_body(struct builder *b, bool kill, bool depth)
{
    b->n = 0;
    for (unsigned c = 0; c < 4; c++)
        add(b, I(.op = SGX535_FS_UNIFORM, .index = (uint16_t)c));
    if (kill)
        add(b, I(.op = SGX535_FS_KILL, .src = {add(b, I(.op = SGX535_FS_UNIFORM, .index = 4))}));
    outputs(b, 0, 1, 2, 3);
    if (depth)
        add(b, I(.op = SGX535_FS_OUTPUT, .index = SGX535_FS_DEPTH,
                 .src = {add(b, I(.op = SGX535_FS_UNIFORM, .index = 5))}));
}

/* Run the variant on one quad: uniforms, the target's bytes, the blend colour
 * and feedback words; returns o0 of pixel 0 (the target where it was
 * discarded) and its discard and depth. Phase 2 runs after phase 1, on the
 * same PA (the model runs it for every pixel). */
static bool epilogue_run(const float src[4], uint32_t target, uint32_t blend_color,
                         const uint32_t feedback[2], float kill, float depth, uint32_t *out,
                         bool *discarded, float *got_depth)
{
    struct sgx535_sim sim;
    if (dump)
        print_words();
    memset(&sim, 0, sizeof(sim));
    sim.sample = texel;
    for (uint32_t c = 1; c < variant.constant_count; c++) {
        const struct sgx535_fs_constant *k = &variant.constant[c];
        if (k->kind == SGX535_FS_CONSTANT_UNIFORM)
            sim.sa[c] = sgx535_sim_bits(k->value < 4    ? src[k->value]
                                        : k->value == 4 ? kill
                                                        : depth);
        else if (k->kind == SGX535_FS_CONSTANT_LITERAL)
            sim.sa[c] = k->value;
        else if (k->kind == SGX535_FS_CONSTANT_EPILOGUE)
            sim.sa[c] = k->value == SGX535_FS_BLEND_COLOR ? blend_color : feedback[k->value - 1u];
    }
    for (unsigned p = 0; p < 4; p++)
        sim.pixel[p].o[0] = target;
    if (!sgx535_sim_run(&sim, variant.words,
                        variant.phase2 ? variant.phase2 : variant.instruction_count) ||
        (variant.phase2 && !sgx535_sim_run(&sim, variant.words + 2u * variant.phase2,
                                           variant.instruction_count - variant.phase2))) {
        fprintf(stderr, "epilogue: model: %s\n", sim.error);
        return false;
    }
    *out = sim.pixel[0].discarded ? target : sim.pixel[0].o[0];
    *discarded = sim.pixel[0].discarded;
    *got_depth = sim.pixel[0].depth;
    return true;
}

static uint32_t bgra(const unsigned char rgba[4])
{
    return rgba[2] | (uint32_t)rgba[1] << 8 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[3] << 24;
}

static void epilogue_tests(unsigned *seed)
{
    static const uint8_t factors[] = {SGX535_FS_ZERO,
                                      SGX535_FS_ONE,
                                      SGX535_FS_SRC_COLOR,
                                      SGX535_FS_ONE_MINUS_SRC_COLOR,
                                      SGX535_FS_DST_COLOR,
                                      SGX535_FS_ONE_MINUS_DST_COLOR,
                                      SGX535_FS_SRC_ALPHA,
                                      SGX535_FS_ONE_MINUS_SRC_ALPHA,
                                      SGX535_FS_DST_ALPHA,
                                      SGX535_FS_ONE_MINUS_DST_ALPHA,
                                      SGX535_FS_CONSTANT_COLOR,
                                      SGX535_FS_ONE_MINUS_CONSTANT_COLOR,
                                      SGX535_FS_CONSTANT_ALPHA,
                                      SGX535_FS_ONE_MINUS_CONSTANT_ALPHA,
                                      SGX535_FS_SRC_ALPHA_SATURATE};
    struct sgx535_fs_key key = key4();
    struct builder *b = calloc(1, sizeof(*b));
    const uint32_t nofeedback[2] = {0, 0};
    unsigned refused = 0;
    epilogue_body(b, false, false);
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_OK);
    /* Blending: every factor pair with every equation, colour and alpha alike,
     * and a sample of separate alpha halves; GL within 1/255. */
    for (unsigned e = 0; e < 5; e++)
        for (unsigned i = 0; i < sizeof(factors); i++)
            for (unsigned j = 0; j < sizeof(factors); j++)
                for (unsigned sep = 0; sep < 2; sep++) {
                    struct sgx535_fs_epilogue_key k = {.target = SGX535_FS_TARGET_B8G8R8A8,
                                                       .colormask = 15,
                                                       .blend = 1,
                                                       .rgb_src = factors[i],
                                                       .rgb_dst = factors[j],
                                                       .rgb_eq = (uint8_t)e};
                    unsigned ea = sep ? (unsigned)rnd(seed) % 5u : e;
                    k.alpha_src = sep ? factors[(unsigned)rnd(seed) % sizeof(factors)] : factors[i];
                    k.alpha_dst = sep ? factors[(unsigned)rnd(seed) % sizeof(factors)] : factors[j];
                    k.alpha_eq = (uint8_t)ea;
                    if (sgx535_fs_link(&body, &k, &variant) != SGX535_FS_OK) {
                        /* only GL's invalid states: SRC_ALPHA_SATURATE as a destination */
                        CHECK(k.rgb_dst == SGX535_FS_SRC_ALPHA_SATURATE ||
                              k.alpha_dst == SGX535_FS_SRC_ALPHA_SATURATE);
                        refused++;
                        continue;
                    }
                    /* the body iterates nothing; a translucent epilogue's pa0
                     * must be the task's */
                    CHECK(body.primary == 0 && (variant.pass_type != 1 || variant.primary >= 1));
                    epilogue_variants++;
                    for (unsigned r = 0; r < 8; r++) {
                        unsigned char sb[4], db[4], kb[4];
                        double s[4], d[4], kk[4];
                        float src[4];
                        uint32_t got;
                        bool gone;
                        float z;
                        for (unsigned c = 0; c < 4; c++) {
                            sb[c] = (unsigned char)rnd(seed), db[c] = (unsigned char)rnd(seed);
                            kb[c] = (unsigned char)rnd(seed);
                            src[c] = (float)sb[c] / 255.0f;
                            s[c] = sb[c] / 255.0, d[c] = db[c] / 255.0, kk[c] = kb[c] / 255.0;
                        }
                        if (variant.no_color) {
                            CHECK(k.rgb_src == SGX535_FS_ZERO && k.rgb_dst == SGX535_FS_ONE);
                            continue;
                        }
                        if (!epilogue_run(src, bgra(db), bgra(kb), nofeedback, 0, 0, &got, &gone,
                                          &z)) {
                            CHECK(0);
                            break;
                        }
                        for (unsigned c = 0; c < 4; c++) {
                            unsigned eq = c == 3 ? k.alpha_eq : k.rgb_eq;
                            uint8_t fs = c == 3 ? k.alpha_src : k.rgb_src;
                            uint8_t fd = c == 3 ? k.alpha_dst : k.rgb_dst;
                            double x = gl_factor(fs, c, s, d, kk) * s[c],
                                   y = gl_factor(fd, c, s, d, kk) * d[c], v;
                            v = eq == SGX535_FS_EQ_ADD                ? x + y
                                : eq == SGX535_FS_EQ_SUBTRACT         ? x - y
                                : eq == SGX535_FS_EQ_REVERSE_SUBTRACT ? y - x
                                : eq == SGX535_FS_EQ_MIN              ? fmin(s[c], d[c])
                                                                      : fmax(s[c], d[c]);
                            v = v < 0 ? 0 : v > 1 ? 1 : v;
                            {
                                int want = (int)lrint(v * 255),
                                    have = (int)(got >> (8u * (unsigned)"\2\1\0\3"[c]) & 0xffu);
                                epilogue_cases++;
                                if (abs(want - have) > 1) {
                                    fprintf(
                                        stderr,
                                        "blend %u/%u eq %u (alpha %u/%u eq %u) c%u: %d, GL %d\n",
                                        k.rgb_src, k.rgb_dst, k.rgb_eq, k.alpha_src, k.alpha_dst,
                                        k.alpha_eq, c, have, want);
                                    CHECK(0);
                                }
                            }
                        }
                    }
                }
    printf("epilogue: %u blend variants, %u refused (GL-invalid), ", epilogue_variants, refused);
    /* Colour masks and logic ops: exact bytes. */
    for (unsigned mask = 0; mask < 16; mask++)
        for (unsigned op = 0; op < 17; op++) {
            struct sgx535_fs_epilogue_key k = {.target = SGX535_FS_TARGET_B8G8R8A8,
                                               .colormask = (uint8_t)mask,
                                               .logic = op < 16,
                                               .logic_op = (uint8_t)(op & 15u)};
            unsigned char sb[4], db[4];
            float src[4];
            uint32_t got, s32, d32, want;
            bool gone;
            float z;
            CHECK(sgx535_fs_link(&body, &k, &variant) == SGX535_FS_OK);
            /* a partial mask or a logic op goes through pa0 */
            CHECK(variant.no_color || (mask == 15 && (op == 16 || op == 3)) ||
                  variant.primary >= 1);
            for (unsigned c = 0; c < 4; c++) {
                sb[c] = (unsigned char)rnd(seed), db[c] = (unsigned char)rnd(seed);
                src[c] = (float)sb[c] / 255.0f;
            }
            s32 = bgra(sb), d32 = bgra(db);
            switch (op) {
            case 0:
                want = 0;
                break;
            case 1:
                want = s32 & d32;
                break;
            case 2:
                want = s32 & ~d32;
                break;
            case 4:
                want = ~s32 & d32;
                break;
            case 5:
                want = d32;
                break;
            case 6:
                want = s32 ^ d32;
                break;
            case 7:
                want = s32 | d32;
                break;
            case 8:
                want = ~(s32 | d32);
                break;
            case 9:
                want = ~(s32 ^ d32);
                break;
            case 10:
                want = ~d32;
                break;
            case 11:
                want = s32 | ~d32;
                break;
            case 12:
                want = ~s32;
                break;
            case 13:
                want = ~s32 | d32;
                break;
            case 14:
                want = ~(s32 & d32);
                break;
            case 15:
                want = ~0u;
                break;
            default:
                want = s32;
                break; /* COPY, and no logic op */
            }
            {
                /* the mask keeps the target's bytes: R 2, G 1, B 0, A 3 */
                uint32_t keep = 0;
                for (unsigned c = 0; c < 4; c++)
                    if (!(mask >> c & 1u))
                        keep |= 0xffu << (8u * (unsigned)"\2\1\0\3"[c]);
                want = (want & ~keep) | (d32 & keep);
            }
            if (variant.no_color) {
                CHECK(want == d32);
                continue;
            }
            if (!epilogue_run(src, d32, 0, nofeedback, 0, 0, &got, &gone, &z))
                CHECK(0);
            else if (got != want) {
                fprintf(stderr, "mask %x logic %u: %08x, GL %08x\n", mask, op, got, want);
                CHECK(0);
            }
            epilogue_cases++;
        }
    /* Alpha test (8 functions), discard, depth output. */
    for (unsigned func = 0; func < 8; func++)
        for (unsigned r = 0; r < 16; r++) {
            struct sgx535_fs_epilogue_key k = {
                .target = SGX535_FS_TARGET_B8G8R8A8, .colormask = 15, .alpha_test = 1};
            struct sgx535_fs_feedback f = {.depth_func = 1,
                                           .stencil_func = 7,
                                           .alpha_func = (uint8_t)func,
                                           .alpha_ref = (uint8_t)rnd(seed)};
            uint32_t fb[2], got;
            unsigned char sb[4] = {10, 20, 30, (unsigned char)(r < 2 ? f.alpha_ref : rnd(seed))};
            float src[4] = {10 / 255.0f, 20 / 255.0f, 30 / 255.0f, (float)sb[3] / 255.0f}, z;
            bool gone, pass;
            unsigned a = sb[3], ref = f.alpha_ref;
            sgx535_fs_feedback_words(&f, fb);
            CHECK(sgx535_fs_link(&body, &k, &variant) == SGX535_FS_OK && variant.pass_type == 4);
            pass = func == 0   ? false
                   : func == 1 ? a < ref
                   : func == 2 ? a == ref
                   : func == 3 ? a <= ref
                   : func == 4 ? a > ref
                   : func == 5 ? a != ref
                   : func == 6 ? a >= ref
                               : true;
            if (!epilogue_run(src, 0, 0, fb, 0, 0, &got, &gone, &z))
                CHECK(0);
            else
                CHECK(gone == !pass && (!pass || got == bgra(sb)));
            epilogue_cases++;
        }
    /* The alpha test with blending, masks and logic ops (pass type 2): where
     * the pixel passes, phase 2 writes what the one-phase variant writes. */
    for (unsigned i = 0; i < 400; i++) {
        const uint32_t always[2] = {0x0000e001u, 0x0000ee00u}; /* stencil and alpha ALWAYS */
        struct sgx535_fs_epilogue_key k = {.target = SGX535_FS_TARGET_B8G8R8A8,
                                           .colormask = (uint8_t)(1u + (unsigned)rnd(seed) % 15u)};
        unsigned char sb[4], db[4], kb[4];
        float src[4], z;
        uint32_t one, two;
        bool gone, opaque;
        if (i % 3u == 0) {
            k.logic = 1, k.logic_op = (uint8_t)((unsigned)rnd(seed) % 16u);
        } else if (i % 3u == 1) {
            k.blend = 1;
            k.rgb_src = k.alpha_src = factors[(unsigned)rnd(seed) % (sizeof(factors) - 1u)];
            k.rgb_dst = k.alpha_dst = factors[(unsigned)rnd(seed) % (sizeof(factors) - 1u)];
            k.rgb_eq = k.alpha_eq = (uint8_t)((unsigned)rnd(seed) % 5u);
        }
        for (unsigned c = 0; c < 4; c++) {
            sb[c] = (unsigned char)rnd(seed), db[c] = (unsigned char)rnd(seed);
            kb[c] = (unsigned char)rnd(seed);
            src[c] = (float)sb[c] / 255.0f;
        }
        if (sgx535_fs_link(&body, &k, &variant) != SGX535_FS_OK || variant.no_color)
            continue;
        CHECK(epilogue_run(src, bgra(db), bgra(kb), nofeedback, 0, 0, &one, &gone, &z));
        opaque = (!k.logic || k.logic_op == 3) &&
                 (!k.blend || (k.rgb_src == SGX535_FS_ONE && k.rgb_dst == SGX535_FS_ZERO &&
                               k.rgb_eq == SGX535_FS_EQ_ADD));
        k.alpha_test = 1;
        CHECK(sgx535_fs_link(&body, &k, &variant) == SGX535_FS_OK && variant.phase2 % 2u == 0 &&
              variant.punchthrough && variant.pass_type == (opaque ? 4 : 2));
        CHECK(epilogue_run(src, bgra(db), bgra(kb), always, 0, 0, &two, &gone, &z) && !gone &&
              one == two);
        epilogue_cases++;
        epilogue_punchthrough++;
    }
    epilogue_body(b, true, false);
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_OK);
    for (unsigned kill = 0; kill < 2; kill++) {
        struct sgx535_fs_epilogue_key k = {.target = SGX535_FS_TARGET_B8G8R8A8, .colormask = 15};
        const uint32_t fb[2] = {0x0000e001u, 0x0000ee00u}; /* stencil and alpha ALWAYS */
        float src[4] = {0.5f, 0.25f, 0.75f, 1.0f}, z;
        uint32_t got;
        bool gone;
        CHECK(sgx535_fs_link(&body, &k, &variant) == SGX535_FS_OK && variant.pass_type == 4);
        CHECK(epilogue_run(src, 0, 0, fb, kill ? 1.0f : 0.0f, 0, &got, &gone, &z) &&
              gone == (kill != 0));
        epilogue_cases++;
    }
    epilogue_body(b, false, true);
    CHECK(sgx535_fs_compile(b->ir, b->n, &key, &body) == SGX535_FS_OK);
    {
        struct sgx535_fs_epilogue_key k = {.target = SGX535_FS_TARGET_B8G8R8A8, .colormask = 15};
        const uint32_t fb[2] = {0x0000e001u, 0x0000ee00u};
        float src[4] = {0.5f, 0.25f, 0.75f, 1.0f}, z = 0;
        uint32_t got;
        bool gone;
        CHECK(sgx535_fs_link(&body, &k, &variant) == SGX535_FS_OK && variant.pass_type == 5);
        CHECK(epilogue_run(src, 0, 0, fb, 0, 0.625f, &got, &gone, &z) && z == 0.625f);
        epilogue_cases++;
    }
    printf("%u cases (%u with a colour phase after the alpha test)\n", epilogue_cases,
           epilogue_punchthrough);
    free(b);
}

/* Texture reads: every result class through several swizzles, direct and
 * dependent; shadow reads with GL's eight functions; NPOT wrap in USE. */
static void texture_programs(unsigned *seed)
{
    static const uint8_t swizzles[5][4] = {
        {0, 0, 0, 0}, /* as stored */
        {SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R,
         SGX535_FS_SWIZZLE_ONE}, /* L */
        {SGX535_FS_SWIZZLE_ZERO, SGX535_FS_SWIZZLE_ZERO, SGX535_FS_SWIZZLE_ZERO,
         SGX535_FS_SWIZZLE_R}, /* A */
        {SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R,
         SGX535_FS_SWIZZLE_R}, /* I */
        {SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R, SGX535_FS_SWIZZLE_R,
         SGX535_FS_SWIZZLE_G}, /* LA */
    };
    struct builder *b = calloc(1, sizeof(*b));
    char name[64];
    for (unsigned result = 0; result <= SGX535_FS_RESULT_F32X4; result++)
        for (unsigned z = 0; z < 5; z++)
            for (unsigned dependent = 0; dependent < 2; dependent++) {
                struct sgx535_fs_key key = key4();
                uint16_t x, y, tex;
                key.sampler[1].result = (uint8_t)result;
                memcpy(key.sampler[1].swizzle, swizzles[z], 4);
                b->n = 0;
                x = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 0));
                y = add(b, I(.op = SGX535_FS_INPUT, .index = 0, .comp = 1));
                if (dependent)
                    y = add(b, I(.op = SGX535_FS_MUL, .src = {y, x}));
                tex = add(b, I(.op = SGX535_FS_TEX, .index = 1, .dim = 2, .src = {x, y}));
                outputs(b, add(b, I(.op = SGX535_FS_TEXC, .comp = 0, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 1, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 2, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 3, .src = {tex})));
                snprintf(name, sizeof(name), "read-%u-%u-%u", result, z, dependent);
                CHECK(run(name, b->ir, b->n, &key, 8, seed));
            }
    /* shadow: depth in F32, U16 or a U8X4 R; ref from a varying; depth modes */
    for (unsigned result = 0; result <= SGX535_FS_RESULT_F32; result += 3)
        for (unsigned compare = 0; compare <= 8; compare++)
            for (unsigned z = 0; z < 4; z += 1) {
                struct sgx535_fs_key key = key4();
                uint16_t x, y, ref, tex;
                key.sampler[2].result = (uint8_t)(result == 0 ? 0 : result == 3 ? 3 : 4);
                key.sampler[2].compare = (uint8_t)compare;
                memcpy(key.sampler[2].swizzle, swizzles[z], 4);
                b->n = 0;
                x = add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 0));
                y = add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 1));
                ref = add(b, I(.op = SGX535_FS_FRACT,
                               .src = {add(b, I(.op = SGX535_FS_INPUT, .index = 1, .comp = 2))}));
                tex = add(
                    b, I(.op = SGX535_FS_TEX, .index = 2, .dim = 2, .imm = 1, .src = {x, y, ref}));
                outputs(b, add(b, I(.op = SGX535_FS_TEXC, .comp = 0, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 1, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 2, .src = {tex})),
                        add(b, I(.op = SGX535_FS_TEXC, .comp = 3, .src = {tex})));
                snprintf(name, sizeof(name), "shadow-%u-%u-%u", result, compare, z);
                CHECK(run(name, b->ir, b->n, &key, 8, seed));
            }
    /* NPOT REPEAT and MIRRORED_REPEAT in USE, both axes, coordinates past [0, 1] */
    for (unsigned s = 0; s < 3; s++)
        for (unsigned u = 0; u < 3; u++) {
            struct sgx535_fs_key key = key4();
            uint16_t x, y, tex;
            key.sampler[3].wrap[0] = (uint8_t)s, key.sampler[3].wrap[1] = (uint8_t)u;
            b->n = 0;
            x = add(b, I(.op = SGX535_FS_INPUT, .index = 2, .comp = 0));
            y = add(b, I(.op = SGX535_FS_INPUT, .index = 2, .comp = 1));
            tex = add(b, I(.op = SGX535_FS_TEX, .index = 3, .dim = 2, .src = {x, y}));
            outputs(b, add(b, I(.op = SGX535_FS_TEXC, .comp = 0, .src = {tex})),
                    add(b, I(.op = SGX535_FS_TEXC, .comp = 1, .src = {tex})),
                    add(b, I(.op = SGX535_FS_TEXC, .comp = 2, .src = {tex})),
                    add(b, I(.op = SGX535_FS_TEXC, .comp = 3, .src = {tex})));
            snprintf(name, sizeof(name), "wrap-%u-%u", s, u);
            CHECK(run(name, b->ir, b->n, &key, 8, seed));
        }
    /* no instructions: a shader that writes no colour (piglit glsl-fs-multiple-builtins) */
    {
        struct sgx535_fs_key key = key4();
        CHECK(run("empty", b->ir, 0, &key, 2, seed));
    }
    /* an int and a bool uniform as Mesa stores them (piglit glsl-fs-uniform-bool-1) */
    {
        struct sgx535_fs_key key = key4();
        uint16_t flag, count;
        b->n = 0;
        flag = add(b, I(.op = SGX535_FS_UNIFORM, .comp = 2, .index = 61));
        count = add(b, I(.op = SGX535_FS_UNIFORM, .comp = 1, .index = 62));
        outputs(b, flag, op2(b, SGX535_FS_MUL, count, constf(b, 0.125f)),
                add(b, I(.op = SGX535_FS_SEL, .src = {flag, constf(b, 0.25f), constf(b, 0.75f)})),
                constf(b, 1.0f));
        CHECK(run("uniform-kinds", b->ir, b->n, &key, 16, seed));
    }
    /* piglit glsl-fs-discard-04: for (i < j) { if (i > k) continue; discard; },
     * j and k uniform: the kill flag must hold from the program's start */
    for (unsigned round = 0; round < 2; round++) {
        struct sgx535_fs_key key = key4();
        uint16_t j, k, i, c;
        b->n = 0;
        j = add(b, I(.op = SGX535_FS_UNIFORM, .index = (uint16_t)(round ? 5 : 6)));
        k = add(b, I(.op = SGX535_FS_UNIFORM, .index = 7));
        add(b, I(.op = SGX535_FS_STORE, .index = 0, .src = {add(b, I(.op = SGX535_FS_CONST))}));
        add(b, I(.op = SGX535_FS_LOOP));
        i = add(b, I(.op = SGX535_FS_LOAD, .index = 0));
        c = add(b, I(.op = SGX535_FS_SGE, .src = {i, j}));
        add(b, I(.op = SGX535_FS_IF, .src = {c}));
        add(b, I(.op = SGX535_FS_BREAK));
        add(b, I(.op = SGX535_FS_ENDIF));
        add(b, I(.op = SGX535_FS_STORE, .index = 0,
                 .src = {
                     add(b, I(.op = SGX535_FS_ADD,
                              .src = {i, add(b, I(.op = SGX535_FS_CONST, .imm = 0x3f800000u))}))}));
        c = add(b, I(.op = SGX535_FS_SLT, .src = {k, i}));
        add(b, I(.op = SGX535_FS_IF, .src = {c}));
        add(b, I(.op = SGX535_FS_CONTINUE));
        add(b, I(.op = SGX535_FS_ENDIF));
        add(b,
            I(.op = SGX535_FS_KILL, .src = {add(b, I(.op = SGX535_FS_CONST, .imm = 0x3f800000u))}));
        add(b, I(.op = SGX535_FS_ENDLOOP));
        outputs(b, add(b, I(.op = SGX535_FS_CONST)), j, k, add(b, I(.op = SGX535_FS_CONST)));
        CHECK(run("loop-discard", b->ir, b->n, &key, 16, seed));
    }
    free(b);
}

int main(int argc, char **argv)
{
    unsigned seed = 1;
    dump = argc == 2 && !strcmp(argv[1], "--dump");
    fixed_programs(&seed);
    random_programs(400, &seed);
    texture_programs(&seed);
    epilogue_tests(&seed);
    if (!dump)
        printf("fragment backend: %u programs (%u words), %u checks, %u failures\n", programs,
               words_total, checks, failures);
    return failures ? 1 : 0;
}
