// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Fragment IR -> USSE words (sgx535_fs.h). Live ranges are computed first: a
 * value's temporary is free after its last use; a use inside a loop that does
 * not contain the definition keeps it to that loop's end; a VAR keeps its
 * temporary over every loop that names it. Then one pass emits. */
#include "sgx535_fs.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NO_REG 0xffu
#define NONE_AT UINT32_MAX
#define F32_ONE UINT32_C(0x3f800000)
#define F32_QUARTER UINT32_C(0x3e800000)
#define F32_MINUS_ZERO UINT32_C(0x80000000)

struct label {
    uint32_t pc, count;
    uint32_t pending[16]; /* forward branches to patch */
};
struct scope {
    uint8_t op;
    struct label other, exit, top; /* IF: ELSE target, end; LOOP: exit, top */
};

struct emitter {
    const struct sgx535_fs_instruction *ir;
    uint32_t count;
    const struct sgx535_fs_key *key;
    struct sgx535_fs_program *out;
    uint32_t last[SGX535_FS_MAX_INSTRUCTIONS];
    uint32_t loop_end[SGX535_FS_MAX_INSTRUCTIONS];    /* at a LOOP */
    uint32_t loop_parent[SGX535_FS_MAX_INSTRUCTIONS]; /* at a LOOP */
    uint32_t inner_loop[SGX535_FS_MAX_INSTRUCTIONS], outer_loop[SGX535_FS_MAX_INSTRUCTIONS];
    uint8_t nest[SGX535_FS_MAX_INSTRUCTIONS];
    uint32_t var_first[SGX535_FS_MAX_VARS], var_last[SGX535_FS_MAX_VARS];
    uint8_t var_reg[SGX535_FS_MAX_VARS];
    struct sgx535_use_operand value[SGX535_FS_MAX_INSTRUCTIONS];
    uint16_t owned[SGX535_FS_TEMPS]; /* values that own a temporary */
    uint32_t owned_count;
    bool busy[SGX535_FS_TEMPS];
    uint8_t varying_pa[SGX535_FS_MAX_VARYINGS], pos_pa;
    uint8_t texture_pa[SGX535_FS_MAX_INSTRUCTIONS];
    uint8_t sampler_sa[SGX535_FS_MAX_SAMPLERS][4]; /* per plane */
    uint8_t span[SGX535_FS_MAX_INSTRUCTIONS];      /* temporaries a value holds, 0: one */
    bool compared[SGX535_FS_MAX_INSTRUCTIONS];     /* a shadow read's 1.0 or 0.0 */
    uint32_t pending_drc;                          /* the TEX whose result is not waited for, + 1 */
    bool skipinv, output_top[5];
    struct scope scope[SGX535_FS_MAX_DEPTH];
    uint32_t depth;
    uint8_t output_reg[5];
    uint32_t kill_compare; /* the compare of EMGD's discard form, + 1 */
};

static enum sgx535_fs_status fail(struct emitter *e, uint32_t at, const char *reason,
                                  enum sgx535_fs_status status)
{
    e->out->failed_at = at;
    e->out->reason = reason;
    return status;
}

static struct sgx535_use_operand reg(uint8_t bank, uint32_t number)
{
    return (struct sgx535_use_operand){bank, (uint8_t)number, 0};
}
static const struct sgx535_use_operand none = {SGX535_USE_R, 0, 0};
#define ZERO reg(SGX535_USE_C, SGX535_USE_C_ZERO)
#define ONE reg(SGX535_USE_C, SGX535_USE_C_ONE)
#define TEMP(n) reg(SGX535_USE_R, (uint32_t)(n))

static bool emit(struct emitter *e, struct sgx535_use_instruction in)
{
    /* .skipinv where nothing needs the quad's invalid pixels; never on
     * gradients, samples or branches. */
    if (e->skipinv && in.opcode != SGX535_USE_SMP && in.opcode != SGX535_USE_FDSX &&
        in.opcode != SGX535_USE_FDSY && in.opcode != SGX535_USE_LIMM &&
        in.opcode <= SGX535_USE_UNPCKF32U8)
        in.skipinv = 1;
    return sgx535_use_append(e->out->words, &e->out->instruction_count, SGX535_FS_MAX_WORDS / 2u,
                             &in) == SGX535_USE_OK;
}

int sgx535_fs_temps(bool *busy, unsigned limit, uint32_t *high, unsigned n)
{
    for (unsigned t = 0; t + n <= limit; t++) {
        unsigned k = 0;
        while (k < n && !busy[t + k])
            k++;
        if (k < n)
            continue;
        for (k = 0; k < n; k++)
            busy[t + k] = true;
        if (t + n > *high)
            *high = t + n;
        return (int)t;
    }
    return -1;
}
static int temps(struct emitter *e, unsigned n)
{
    return sgx535_fs_temps(e->busy, SGX535_FS_TEMPS, &e->out->temps, n);
}
static int temp(struct emitter *e)
{
    return temps(e, 1);
}
static void release(struct emitter *e, uint32_t r)
{
    if (r < SGX535_FS_TEMPS)
        e->busy[r] = false;
}

/* SA words: sa0 reserved, then entries in the order they are asked for. */
int sgx535_fs_constant(struct sgx535_fs_constant *table, uint32_t *count, uint32_t kind,
                       uint32_t value)
{
    const struct sgx535_fs_constant key = {kind, value};
    uint32_t i = 1;
    if (!*count)
        *count = 1;
    while (i < *count && memcmp(&table[i], &key, sizeof(key)))
        i++;
    if (i == *count) {
        if (*count == SGX535_FS_MAX_CONSTANTS)
            return -1;
        table[(*count)++] = key;
    }
    return (int)i;
}
static int constant(struct emitter *e, uint32_t kind, uint32_t value)
{
    return sgx535_fs_constant(e->out->constant, &e->out->constant_count, kind, value);
}

struct sgx535_fs_iteration sgx535_fs_use_issue(uint32_t issue, uint32_t components, uint32_t pa)
{
    return (struct sgx535_fs_iteration){0xfu | issue << 12 | (components - 1u) << 22 |
                                            UINT32_C(1) << 24 | 3u << 26,
                                        0, 0, (uint8_t)pa, (uint8_t)components};
}

/* A literal: the hardware constants seen in EMGD's words (c48 0.0, c52 1.0,
 * c15 0.25), negated by a modifier; else an SA word. */
static bool literal(struct emitter *e, uint32_t bits, struct sgx535_use_operand *o)
{
    uint32_t magnitude = bits & ~F32_MINUS_ZERO;
    int sa;
    if (magnitude == 0 || magnitude == F32_ONE || magnitude == F32_QUARTER) {
        *o = magnitude == 0 ? ZERO : magnitude == F32_ONE ? ONE : reg(SGX535_USE_C, 15);
        o->mod = (uint8_t)(bits & F32_MINUS_ZERO ? SGX535_USE_NEG : 0);
        return true;
    }
    sa = constant(e, SGX535_FS_CONSTANT_LITERAL, bits);
    if (sa < 0)
        return false;
    *o = reg(SGX535_USE_SA, (uint32_t)sa);
    return true;
}

/* A source with the instruction's modifier applied on top of the value's. */
static struct sgx535_use_operand operand(const struct emitter *e, uint16_t v, uint8_t mod)
{
    struct sgx535_use_operand o = e->value[v];
    if (mod & SGX535_USE_ABS)
        o.mod = SGX535_USE_ABS;
    if (mod & SGX535_USE_NEG)
        o.mod ^= SGX535_USE_NEG;
    return o;
}

static bool r_or_pa(const struct sgx535_use_operand *o)
{
    return o->bank == SGX535_USE_R || o->bank == SGX535_USE_PA;
}

/* A copy; with modifiers, fmax(x, x) carries them on both sources. */
static bool copy(struct emitter *e, uint8_t pred, struct sgx535_use_operand dst,
                 struct sgx535_use_operand src)
{
    if (!src.mod)
        return emit(
            e, (struct sgx535_use_instruction){
                   .opcode = SGX535_USE_MOV, .pred = pred, .dst = dst, .src = {none, src, none}});
    return emit(e,
                (struct sgx535_use_instruction){
                    .opcode = SGX535_USE_FMAX, .pred = pred, .dst = dst, .src = {none, src, src}});
}

/* The operand copied into a scratch temporary when NEEDED: fmad's src0 must
 * be a temporary or PA, TEST and SMP take no modifiers. */
static bool materialize(struct emitter *e, struct sgx535_use_operand *o, int *scratch, bool needed)
{
    int t;
    if (!needed)
        return true;
    t = temp(e);
    if (t < 0 || !copy(e, 0, TEMP(t), *o))
        return false;
    *scratch = t;
    *o = TEMP(t);
    return true;
}
static bool to_r_or_pa(struct emitter *e, struct sgx535_use_operand *o, int *scratch)
{
    return materialize(e, o, scratch, !r_or_pa(o));
}
static bool plain(struct emitter *e, struct sgx535_use_operand *o, int *scratch)
{
    return materialize(e, o, scratch, o->mod != 0);
}

static bool wait(struct emitter *e)
{
    if (!e->pending_drc)
        return true;
    e->pending_drc = 0;
    return emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_WDF});
}

/* p0 = test(a - b). */
static bool compare(struct emitter *e, uint8_t test, struct sgx535_use_operand a,
                    struct sgx535_use_operand b)
{
    int sa = -1, sb = -1;
    bool ok = plain(e, &a, &sa) && plain(e, &b, &sb) &&
              emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_TEST,
                                                      .alu = SGX535_USE_ALU_FSUB,
                                                      .test = test,
                                                      .src = {none, a, b}});
    if (sa >= 0)
        release(e, (uint32_t)sa);
    if (sb >= 0)
        release(e, (uint32_t)sb);
    return ok;
}

static unsigned source_count(const struct sgx535_fs_instruction *in)
{
    switch (in->op) {
    case SGX535_FS_MAD:
    case SGX535_FS_SEL:
        return 3;
    case SGX535_FS_ADD:
    case SGX535_FS_MUL:
    case SGX535_FS_MIN:
    case SGX535_FS_MAX:
    case SGX535_FS_SLT:
    case SGX535_FS_SGE:
    case SGX535_FS_SEQ:
    case SGX535_FS_SNE:
        return 2;
    case SGX535_FS_TEX:
        return (unsigned)in->dim + (in->comp != SGX535_FS_TEX_IMPLICIT ? 1u : 0u) +
               (in->imm ? 1u : 0u);
    case SGX535_FS_MOV:
    case SGX535_FS_RCP:
    case SGX535_FS_RSQ:
    case SGX535_FS_LOG2:
    case SGX535_FS_EXP2:
    case SGX535_FS_FRACT:
    case SGX535_FS_DDX:
    case SGX535_FS_DDY:
    case SGX535_FS_TEXC:
    case SGX535_FS_KILL:
    case SGX535_FS_OUTPUT:
    case SGX535_FS_STORE:
    case SGX535_FS_IF:
        return 1;
    default:
        return 0;
    }
}

/* Scopes, loop nesting and last uses. */
static enum sgx535_fs_status analyse(struct emitter *e)
{
    uint32_t stack[SGX535_FS_MAX_DEPTH], depth = 0, loop = NONE_AT, outer = NONE_AT;
    bool has_else[SGX535_FS_MAX_DEPTH] = {false};
    uint32_t outputs[5] = {0};
    for (uint32_t k = 0; k < SGX535_FS_MAX_VARS; k++)
        e->var_first[k] = NONE_AT, e->var_last[k] = 0;
    for (uint32_t i = 0; i < e->count; i++) {
        const struct sgx535_fs_instruction *in = &e->ir[i];
        unsigned n = source_count(in);
        if (in->op > SGX535_FS_TEXSIZE)
            return fail(e, i, "opcode", SGX535_FS_ERR_ARGUMENT);
        if (in->op == SGX535_FS_TEXSIZE && (in->index >= SGX535_FS_MAX_SAMPLERS || in->comp > 2))
            return fail(e, i, "texture size", SGX535_FS_ERR_ARGUMENT);
        if (in->op == SGX535_FS_TEX && (!in->dim || in->dim > 3 || in->comp > SGX535_FS_TEX_LOD ||
                                        in->index >= SGX535_FS_MAX_SAMPLERS || in->imm > 1))
            return fail(e, i, "texture form", SGX535_FS_ERR_UNSUPPORTED);
        /* an instruction holds four sources: a form naming more (three
         * coordinates, a LOD and a comparator) is refused before any is read */
        if (n > sizeof(in->src) / sizeof(in->src[0]))
            return fail(e, i, "texture form", SGX535_FS_ERR_UNSUPPORTED);
        e->last[i] = i;
        e->inner_loop[i] = loop, e->outer_loop[i] = outer;
        e->nest[i] = (uint8_t)depth;
        for (unsigned s = 0; s < n; s++) {
            uint16_t v = in->src[s];
            uint8_t def;
            if (v >= i)
                return fail(e, i, "source not defined before its use", SGX535_FS_ERR_ARGUMENT);
            def = e->ir[v].op;
            if ((def >= SGX535_FS_KILL && def != SGX535_FS_LOAD && def != SGX535_FS_TEXSIZE) ||
                (def == SGX535_FS_TEX) != (in->op == SGX535_FS_TEXC && !s) || in->mod[s] > 3)
                return fail(e, i, "source is not a value of that kind", SGX535_FS_ERR_ARGUMENT);
            e->last[v] = i;
        }
        switch (in->op) {
        case SGX535_FS_STORE:
        case SGX535_FS_LOAD:
            if (in->index >= SGX535_FS_MAX_VARS)
                return fail(e, i, "VAR index", SGX535_FS_ERR_LIMIT);
            if (e->var_first[in->index] == NONE_AT)
                e->var_first[in->index] = i;
            e->var_last[in->index] = i;
            break;
        case SGX535_FS_IF:
        case SGX535_FS_LOOP:
            if (depth == SGX535_FS_MAX_DEPTH)
                return fail(e, i, "nesting", SGX535_FS_ERR_LIMIT);
            has_else[depth] = false;
            stack[depth++] = i;
            if (in->op == SGX535_FS_LOOP) {
                e->loop_parent[i] = loop;
                loop = i;
                if (outer == NONE_AT)
                    outer = i;
                e->inner_loop[i] = loop, e->outer_loop[i] = outer;
            }
            break;
        case SGX535_FS_ELSE:
            if (!depth || e->ir[stack[depth - 1]].op != SGX535_FS_IF || has_else[depth - 1])
                return fail(e, i, "ELSE", SGX535_FS_ERR_ARGUMENT);
            has_else[depth - 1] = true;
            break;
        case SGX535_FS_ENDIF:
        case SGX535_FS_ENDLOOP:
            if (!depth || e->ir[stack[depth - 1]].op !=
                              (in->op == SGX535_FS_ENDIF ? SGX535_FS_IF : SGX535_FS_LOOP))
                return fail(e, i, "unbalanced control flow", SGX535_FS_ERR_ARGUMENT);
            depth--;
            if (in->op == SGX535_FS_ENDLOOP) {
                e->loop_end[loop] = i;
                if (outer == loop)
                    outer = NONE_AT;
                loop = e->loop_parent[loop];
            }
            break;
        case SGX535_FS_BREAK:
        case SGX535_FS_CONTINUE:
            if (loop == NONE_AT)
                return fail(e, i, "BREAK outside a loop", SGX535_FS_ERR_ARGUMENT);
            break;
        case SGX535_FS_INPUT:
            if (in->index >= SGX535_FS_MAX_VARYINGS ||
                in->comp >= e->key->varying[in->index].components)
                return fail(e, i, "varying not in the key", SGX535_FS_ERR_ARGUMENT);
            break;
        case SGX535_FS_OUTPUT:
            if (in->index > SGX535_FS_DEPTH)
                return fail(e, i, "output", SGX535_FS_ERR_ARGUMENT);
            outputs[in->index]++;
            break;
        case SGX535_FS_TEXC:
            if (in->comp > 3)
                return fail(e, i, "texture component", SGX535_FS_ERR_ARGUMENT);
            break;
        case SGX535_FS_SYSTEM:
            if (in->index > SGX535_FS_FRONT_FACING)
                return fail(e, i, "system value", SGX535_FS_ERR_ARGUMENT);
            break;
        case SGX535_FS_UNIFORM:
            if (in->index > 255)
                return fail(e, i, "uniform dword", SGX535_FS_ERR_LIMIT);
            break;
        }
    }
    if (depth)
        return fail(e, e->count, "unbalanced control flow", SGX535_FS_ERR_ARGUMENT);
    /* A use inside loops that do not contain the definition: to the end of the
     * outermost of them. */
    for (uint32_t i = 0; i < e->count; i++) {
        unsigned n = source_count(&e->ir[i]);
        for (unsigned s = 0; s < n; s++) {
            uint16_t v = e->ir[i].src[s];
            uint32_t l = e->inner_loop[i], widest = NONE_AT;
            while (l != NONE_AT && !(l < v && v < e->loop_end[l])) {
                widest = l;
                l = e->loop_parent[l];
            }
            if (widest != NONE_AT && e->loop_end[widest] > e->last[v])
                e->last[v] = e->loop_end[widest];
        }
    }
    for (uint32_t k = 0; k < SGX535_FS_MAX_VARS; k++) {
        uint32_t f = e->var_first[k], l = e->var_last[k];
        if (f == NONE_AT)
            continue;
        if (e->outer_loop[f] != NONE_AT)
            f = e->outer_loop[f];
        if (e->outer_loop[l] != NONE_AT)
            l = e->loop_end[e->outer_loop[l]];
        e->var_first[k] = f, e->var_last[k] = l;
    }
    /* A sole KILL outside control flow of a compare used nowhere else is
     * EMGD's discard: the compare tests the passing condition into p1, which
     * predicates the epilogue's atst8. */
    {
        uint32_t kills = 0, at = 0, uses = 0;
        for (uint32_t i = 0; i < e->count; i++)
            if (e->ir[i].op == SGX535_FS_KILL)
                kills++, at = i;
        if (kills == 1 && !e->nest[at] && !e->ir[at].mod[0]) {
            uint16_t v = e->ir[at].src[0];
            for (uint32_t i = 0; i < e->count; i++)
                for (unsigned s = 0; s < source_count(&e->ir[i]); s++)
                    uses += e->ir[i].src[s] == v;
            if (uses == 1 && !e->nest[v] && e->ir[v].op >= SGX535_FS_SLT &&
                e->ir[v].op <= SGX535_FS_SNE)
                e->kill_compare = v + 1u;
        }
    }
    /* An output written once outside control flow keeps its operand to the
     * end; others go through a temporary. */
    for (uint32_t i = 0; i < e->count; i++)
        if (e->ir[i].op == SGX535_FS_OUTPUT && outputs[e->ir[i].index] == 1 && !e->nest[i]) {
            e->output_top[e->ir[i].index] = true;
            e->last[e->ir[i].src[0]] = e->count;
        }
    return SGX535_FS_OK;
}

/* The kill flag: zeroed before the first instruction, since a KILL may sit
 * in a branch or loop that does not run first (piglit glsl-fs-discard-04). */
static enum sgx535_fs_status kill_flag(struct emitter *e)
{
    int t;
    bool any = false;
    for (uint32_t i = 0; i < e->count; i++)
        any |= e->ir[i].op == SGX535_FS_KILL && e->kill_compare != e->ir[i].src[0] + 1u;
    if (!any)
        return SGX535_FS_OK;
    t = temp(e);
    if (t < 0)
        return fail(e, 0, "temporaries", SGX535_FS_ERR_LIMIT);
    e->out->kill = (uint8_t)t;
    return copy(e, 0, TEMP(t), ZERO) ? SGX535_FS_OK : fail(e, 0, "code size", SGX535_FS_ERR_LIMIT);
}

/* The planes of a read: one DOUTT or smp each (RGBA16F 2, RGBA32F 4). */
static unsigned planes(const struct sgx535_fs_sampler *s)
{
    return s->result == SGX535_FS_RESULT_F16X4 ? 2u : s->result == SGX535_FS_RESULT_F32X4 ? 4u : 1u;
}

/* A plain 2D read at a varying's first two components comes from the pixel
 * PDS (DOUTT) when it has one plane and no wrap in USE; every other read is
 * an smp. */
static bool direct_texture(const struct emitter *e, const struct sgx535_fs_instruction *in)
{
    const struct sgx535_fs_instruction *u, *v;
    if (in->op != SGX535_FS_TEX || in->dim != 2 || in->comp != SGX535_FS_TEX_IMPLICIT ||
        in->mod[0] || in->mod[1])
        return false;
    u = &e->ir[in->src[0]], v = &e->ir[in->src[1]];
    return u->op == SGX535_FS_INPUT && v->op == SGX535_FS_INPUT && u->index == v->index &&
           u->comp == 0 && v->comp == 1 && e->key->varying[u->index].interp == SGX535_FS_SMOOTH &&
           !in->imm && planes(&e->key->sampler[in->index]) == 1 &&
           !e->key->sampler[in->index].wrap[0] && !e->key->sampler[in->index].wrap[1];
}

/* The pixel PDS issues and the PA layout: varyings for the USE, position,
 * then the direct texture reads. Each issue starts at an even PA register
 * (a one-dword TC0 or packed V0 issue is followed by ZABS at pa2;
 * docs/re/sgx535/depth-contract.md#selecting-a-pre-d24-observation). */
static enum sgx535_fs_status iterations(struct emitter *e)
{
    struct sgx535_fs_program *p = e->out;
    bool read[SGX535_FS_MAX_VARYINGS] = {false}, position = false;
    uint32_t pa = 0;
    for (uint32_t i = 0; i < e->count; i++) {
        const struct sgx535_fs_instruction *in = &e->ir[i];
        unsigned n = source_count(in);
        bool direct = direct_texture(e, in);
        for (unsigned s = 0; s < n; s++)
            if (e->ir[in->src[s]].op == SGX535_FS_INPUT && !(direct && s < 2))
                read[e->ir[in->src[s]].index] = true;
        if (in->op == SGX535_FS_SYSTEM && in->index <= SGX535_FS_FRAG_W)
            position = true;
    }
    for (uint32_t v = 0; v < SGX535_FS_MAX_VARYINGS; v++) {
        const struct sgx535_fs_varying *var = &e->key->varying[v];
        if (!read[v])
            continue;
        if (var->interp > SGX535_FS_FLAT || p->iteration_count == SGX535_FS_MAX_ITERATIONS)
            return fail(e, 0, "varying", SGX535_FS_ERR_UNSUPPORTED);
        /* TEXISSUE none, USEISSUE TCv, USEDIM, USEPERSPECTIVE
         * (docs/re/sgx535/interpolation-pack-contract.md "Linux: compiled
         * inputs reach the fragment PDS program"); FLATSHADE Gouraud (3) or
         * vertex 2 (docs/re/emgd-linux/vertex-path.md#state-and-output-convention). */
        e->varying_pa[v] = (uint8_t)pa;
        p->iteration[p->iteration_count++] = (struct sgx535_fs_iteration){
            0xfu | v << 12 | (uint32_t)(var->components - 1u) << 22 | UINT32_C(1) << 24 |
                (var->interp == SGX535_FS_FLAT ? 2u : 3u) << 26,
            0, 0, (uint8_t)pa, var->components};
        pa = (pa + var->components + 1u) & ~1u;
    }
    if (position) {
        if (p->iteration_count == SGX535_FS_MAX_ITERATIONS)
            return fail(e, 0, "iterations", SGX535_FS_ERR_LIMIT);
        /* USEISSUE POSITION (13), four FP components (not yet qualified on the
         * device). */
        e->pos_pa = (uint8_t)pa;
        p->iteration[p->iteration_count++] = sgx535_fs_use_issue(13, 4, pa);
        pa = (pa + 4u + 1u) & ~1u;
    }
    for (uint32_t i = 0; i < e->count; i++) {
        const struct sgx535_fs_instruction *in = &e->ir[i];
        if (!direct_texture(e, in))
            continue;
        if (p->iteration_count == SGX535_FS_MAX_ITERATIONS)
            return fail(e, i, "iterations", SGX535_FS_ERR_LIMIT);
        /* TEXISSUE TCv, TEXPROJ RHW, USEISSUE none: EMGD's 0x0c00f100 form. */
        e->texture_pa[i] = (uint8_t)pa;
        p->iteration[p->iteration_count++] =
            (struct sgx535_fs_iteration){e->ir[in->src[0]].index | 1u << 8 | 15u << 12 | 3u << 26,
                                         (uint8_t)in->index, 1, (uint8_t)pa, 1};
        pa += 2;
    }
    if (pa > 128)
        return fail(e, 0, "primary attributes", SGX535_FS_ERR_LIMIT);
    p->primary = pa;
    return SGX535_FS_OK;
}

static void label_here(struct emitter *e, struct label *l)
{
    l->pc = e->out->instruction_count;
    for (uint32_t k = 0; k < l->count; k++)
        e->out->words[2u * l->pending[k]] = (l->pc - l->pending[k]) & 0xfffu;
    l->count = 0;
}
static bool branch(struct emitter *e, struct label *l, uint8_t pred, bool backward)
{
    uint32_t at = e->out->instruction_count;
    if (!backward && l->count == sizeof(l->pending) / sizeof(l->pending[0]))
        return false;
    if (!emit(e,
              (struct sgx535_use_instruction){.opcode = SGX535_USE_BR,
                                              .pred = pred,
                                              .immediate = backward ? (l->pc - at) & 0xfffu : 0}))
        return false;
    if (!backward)
        l->pending[l->count++] = at;
    return true;
}

/* Free the temporaries whose values or VARs are dead after i. */
static void retire(struct emitter *e, uint32_t i)
{
    for (uint32_t k = 0; k < e->owned_count;) {
        uint16_t v = e->owned[k];
        if (e->last[v] <= i) {
            for (unsigned r = 0; r < (e->span[v] ? e->span[v] : 1u); r++)
                release(e, e->value[v].number + r);
            e->owned[k] = e->owned[--e->owned_count];
        } else
            k++;
    }
    for (uint32_t k = 0; k < SGX535_FS_MAX_VARS; k++)
        if (e->var_reg[k] != NO_REG && e->var_last[k] <= i) {
            release(e, e->var_reg[k]);
            e->var_reg[k] = NO_REG;
        }
}

/* A value's own temporary. */
static bool define(struct emitter *e, uint32_t i, struct sgx535_use_operand *dst)
{
    int t = temp(e);
    if (t < 0)
        return false;
    *dst = TEMP(t);
    e->value[i] = *dst;
    if (e->last[i] > i)
        e->owned[e->owned_count++] = (uint16_t)i;
    else
        release(e, (uint32_t)t);
    return true;
}

static bool var_register(struct emitter *e, uint32_t k)
{
    int t;
    if (e->var_reg[k] != NO_REG)
        return true;
    t = temp(e);
    if (t < 0)
        return false;
    e->var_reg[k] = (uint8_t)t;
    return true;
}

/* NPOT REPEAT and MIRRORED_REPEAT: coordinate C into [0, 1] before a stride
 * read, fract(c) or 1 - |2 fract(c / 2) - 1| (nearest exact; docs/native-3d.md#texture-memory). */
static bool wrap(struct emitter *e, struct sgx535_use_operand c, uint8_t mode)
{
    const struct sgx535_use_operand minus_one = {SGX535_USE_C, SGX535_USE_C_ONE, SGX535_USE_NEG};
    struct sgx535_use_operand half, two;
    if (mode == SGX535_FS_WRAP_REPEAT)
        return emit(e, (struct sgx535_use_instruction){
                           .opcode = SGX535_USE_FFRC, .dst = c, .src = {none, c, c}});
    return literal(e, UINT32_C(0x3f000000), &half) && literal(e, UINT32_C(0x40000000), &two) &&
           emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_FMAD,
                                                   .dst = c,
                                                   .src = {c, half, ZERO}}) &&
           emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_FFRC,
                                                   .dst = c,
                                                   .src = {none, c, c}}) &&
           emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_FMAD,
                                                   .dst = c,
                                                   .src = {c, two, minus_one}}) &&
           copy(e, 0, c, (struct sgx535_use_operand){c.bank, c.number, SGX535_USE_ABS}) &&
           emit(e, (struct sgx535_use_instruction){
                       .opcode = SGX535_USE_FMAD, .dst = c, .src = {c, minus_one, ONE}});
}

/* Channel CH (0-3; 4 zero, 5 one) of read V into DST: a channel the format
 * lacks is 0 (G, B) or 1 (A); a shadow read's one channel is its 1.0 or
 * 0.0. */
static bool channel(struct emitter *e, struct sgx535_use_operand dst, struct sgx535_use_operand v,
                    uint8_t result, bool compared, unsigned ch)
{
    static const uint8_t bgra[4] = {2, 1, 0, 3}, count[] = {4, 1, 2, 1, 1, 4, 4};
    uint8_t format = 0, lane = 0;
    if (ch >= (compared ? 1u : count[result]))
        return copy(e, 0, dst, ch == 3 || ch == 5 ? ONE : ZERO);
    if (compared || result == SGX535_FS_RESULT_F32)
        return copy(e, 0, dst, v);
    if (result == SGX535_FS_RESULT_F32X4)
        return copy(e, 0, dst, TEMP(v.number + ch));
    if (result == SGX535_FS_RESULT_U8X4)
        lane = bgra[ch];
    else if (result == SGX535_FS_RESULT_U8 || result == SGX535_FS_RESULT_U88)
        lane = (uint8_t)ch;
    else if (result == SGX535_FS_RESULT_U16)
        format = 3;
    else
        format = 5, v.number = (uint8_t)(v.number + ch / 2u), lane = (uint8_t)(2u * (ch % 2u));
    return emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_UNPCKF32U8,
                                                   .scale = format != 5,
                                                   .format = format,
                                                   .mask = lane,
                                                   .dst = dst,
                                                   .src = {none, v, none}});
}

/* A shadow read: the reference against the depth in channel 0, GL's
 * function (table 3.27: 1.0 where ref OP depth), into the read's register. */
static bool shadow(struct emitter *e, uint32_t i, struct sgx535_use_operand v)
{
    static const uint8_t test[8] = {0,
                                    SGX535_USE_TEST_LT,
                                    SGX535_USE_TEST_EQ,
                                    SGX535_USE_TEST_LE,
                                    SGX535_USE_TEST_GT,
                                    SGX535_USE_TEST_NE,
                                    SGX535_USE_TEST_GE};
    const struct sgx535_fs_instruction *in = &e->ir[i];
    const struct sgx535_fs_sampler *s = &e->key->sampler[in->index];
    unsigned r = in->dim + (in->comp != SGX535_FS_TEX_IMPLICIT ? 1u : 0u), func = s->compare - 1u;
    if (!s->compare)
        return true;
    e->compared[i] = true;
    if (func == 0 || func == 7)
        return copy(e, 0, v, func ? ONE : ZERO);
    return (s->result == SGX535_FS_RESULT_F32 || channel(e, v, v, s->result, false, 0)) &&
           compare(e, test[func], operand(e, in->src[r], in->mod[r]), v) && copy(e, 0, v, ZERO) &&
           copy(e, 1, v, ONE);
}

static enum sgx535_fs_status texture(struct emitter *e, uint32_t i)
{
    const struct sgx535_fs_instruction *in = &e->ir[i];
    const struct sgx535_fs_sampler *s = &e->key->sampler[in->index];
    struct sgx535_use_operand dst, lod = none;
    unsigned n = planes(s);
    int sa, base, value, scratch = -1;
    if (s->result > SGX535_FS_RESULT_F32X4 || s->compare > 8 || s->wrap[0] > 2 || s->wrap[1] > 2)
        return fail(e, i, "texture result or sampler form", SGX535_FS_ERR_ARGUMENT);
    for (unsigned c = 0; c < 4; c++)
        if (s->swizzle[c] > SGX535_FS_SWIZZLE_ONE)
            return fail(e, i, "texture swizzle", SGX535_FS_ERR_ARGUMENT);
    if (direct_texture(e, in)) {
        e->value[i] = reg(SGX535_USE_PA, e->texture_pa[i]);
        return shadow(e, i, e->value[i]) ? SGX535_FS_OK
                                         : fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
    }
    for (unsigned p = 0; p < n; p++)
        if (e->sampler_sa[in->index][p] == NO_REG) {
            uint32_t w = (uint32_t)in->index << 4 | p << 2;
            sa = constant(e, SGX535_FS_CONSTANT_TEXTURE, w);
            if (sa < 0 || constant(e, SGX535_FS_CONSTANT_TEXTURE, w | 1u) != sa + 1 ||
                constant(e, SGX535_FS_CONSTANT_TEXTURE, w | 2u) != sa + 2)
                return fail(e, i, "constants", SGX535_FS_ERR_LIMIT);
            e->sampler_sa[in->index][p] = (uint8_t)sa;
        }
    base = temps(e, in->dim);
    if (base < 0)
        return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
    for (unsigned c = 0; c < in->dim; c++)
        if (!copy(e, 0, TEMP((uint32_t)base + c), operand(e, in->src[c], in->mod[c])) ||
            (c < 2 && s->wrap[c] && !wrap(e, TEMP((uint32_t)base + c), s->wrap[c])))
            return fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
    if (in->comp != SGX535_FS_TEX_IMPLICIT) {
        lod = operand(e, in->src[in->dim], in->mod[in->dim]);
        if (!plain(e, &lod, &scratch))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
    }
    if (n == 1) {
        if (!define(e, i, &dst))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
    } else {
        /* the planes in a row of temporaries the value holds */
        value = temps(e, n);
        if (value < 0)
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        dst = e->value[i] = TEMP(value);
        e->span[i] = (uint8_t)n;
        if (e->last[i] > i)
            e->owned[e->owned_count++] = (uint16_t)i;
        else
            for (unsigned p = 0; p < n; p++)
                release(e, (uint32_t)value + p);
    }
    /* One sample outstanding at a time, on drc0; no SYNCSTART (EMGD's
     * dependent smp2d carries none, implicit LOD or not). */
    for (unsigned p = 0; p < n; p++)
        if (!wait(e) ||
            !emit(e,
                  (struct sgx535_use_instruction){
                      .opcode = SGX535_USE_SMP,
                      .cdim = (uint8_t)(in->dim - 1u),
                      .lodm = in->comp == SGX535_FS_TEX_BIAS  ? 1
                              : in->comp == SGX535_FS_TEX_LOD ? 2
                                                              : 0,
                      .dst = TEMP(dst.number + p),
                      .src = {TEMP(base), reg(SGX535_USE_SA, e->sampler_sa[in->index][p]), lod}}))
            return fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
        else
            e->pending_drc = i + 1u;
    for (unsigned c = 0; c < in->dim; c++)
        release(e, (uint32_t)base + c);
    if (scratch >= 0)
        release(e, (uint32_t)scratch);
    if (in->imm && s->compare && (!wait(e) || !shadow(e, i, dst)))
        return fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
    return SGX535_FS_OK;
}

static enum sgx535_fs_status system_value(struct emitter *e, uint32_t i)
{
    const struct sgx535_fs_instruction *in = &e->ir[i];
    struct sgx535_use_operand dst;
    bool ok;
    if (in->index == SGX535_FS_FRAG_Y) {
        /* y = sign * y_window + height: GL's y up in the window, as is in an FBO. */
        int height = constant(e, SGX535_FS_CONSTANT_FRAG_Y, 0);
        int sign = constant(e, SGX535_FS_CONSTANT_FRAG_YSIGN, 0);
        if (height < 0 || sign < 0 || !define(e, i, &dst))
            return fail(e, i, "constants", SGX535_FS_ERR_LIMIT);
        ok =
            emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_FMAD,
                                                    .dst = dst,
                                                    .src = {reg(SGX535_USE_PA, e->pos_pa + 1u),
                                                            reg(SGX535_USE_SA, (uint32_t)sign),
                                                            reg(SGX535_USE_SA, (uint32_t)height)}});
    } else if (in->index == SGX535_FS_FRONT_FACING) {
        /* front = ((g16 ^ sense) & 1) == 0
         * (docs/re/emgd-linux/fragment-path.md#isp-state-and-epilogues). */
        int sense = constant(e, SGX535_FS_CONSTANT_FACING, 0), t;
        if (sense < 0 || !define(e, i, &dst) || (t = temp(e)) < 0)
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        ok =
            emit(e,
                 (struct sgx535_use_instruction){.opcode = SGX535_USE_XOR,
                                                 .dst = TEMP(t),
                                                 .src = {none,
                                                         reg(SGX535_USE_G, SGX535_USE_G_FACING),
                                                         reg(SGX535_USE_SA, (uint32_t)sense)}}) &&
            emit(e,
                 (struct sgx535_use_instruction){.opcode = SGX535_USE_TEST,
                                                 .alu = SGX535_USE_ALU_AND,
                                                 .test = SGX535_USE_TEST_EQ,
                                                 .src = {none, TEMP(t), reg(SGX535_USE_IMM, 1)}}) &&
            copy(e, 0, dst, ZERO) && copy(e, 1, dst, ONE);
        release(e, (uint32_t)t);
    } else {
        e->value[i] = reg(SGX535_USE_PA, (uint32_t)e->pos_pa + in->index);
        return SGX535_FS_OK;
    }
    return ok ? SGX535_FS_OK : fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
}

static enum sgx535_fs_status alu(struct emitter *e, uint32_t i)
{
    static const uint8_t test[] = {SGX535_USE_TEST_LT, SGX535_USE_TEST_GE, SGX535_USE_TEST_EQ,
                                   SGX535_USE_TEST_NE};
    const struct sgx535_fs_instruction *in = &e->ir[i];
    struct sgx535_use_operand dst, a, b, c, t;
    int scratch = -1;
    bool ok;
    uint8_t opcode;
    if (!define(e, i, &dst))
        return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
    a = operand(e, in->src[0], in->mod[0]);
    b = operand(e, in->src[1], in->mod[1]);
    c = operand(e, in->src[2], in->mod[2]);
    switch (in->op) {
    case SGX535_FS_MOV:
        ok = copy(e, 0, dst, a);
        break;
    case SGX535_FS_ADD:
    case SGX535_FS_MUL:
    case SGX535_FS_MAD:
        /* All three are fmad: a * 1.0 + b, a * b + 0.0, a * b + c; the factor
         * in src0 must be a temporary or PA. */
        if (in->op == SGX535_FS_ADD) {
            c = b, b = ONE;
            if (!r_or_pa(&a) && r_or_pa(&c))
                t = a, a = c, c = t;
        } else {
            if (in->op == SGX535_FS_MUL)
                c = ZERO;
            if (!r_or_pa(&a))
                t = a, a = b, b = t;
        }
        ok = to_r_or_pa(e, &a, &scratch) &&
             emit(e, (struct sgx535_use_instruction){
                         .opcode = SGX535_USE_FMAD, .dst = dst, .src = {a, b, c}});
        break;
    case SGX535_FS_MIN:
    case SGX535_FS_MAX:
        ok = emit(e, (struct sgx535_use_instruction){
                         .opcode = in->op == SGX535_FS_MIN ? SGX535_USE_FMIN : SGX535_USE_FMAX,
                         .dst = dst,
                         .src = {none, a, b}});
        break;
    case SGX535_FS_RCP:
    case SGX535_FS_RSQ:
    case SGX535_FS_LOG2:
    case SGX535_FS_EXP2:
        opcode = (uint8_t)(SGX535_USE_FRCP + (in->op - SGX535_FS_RCP));
        ok = emit(e, (struct sgx535_use_instruction){
                         .opcode = opcode, .dst = dst, .src = {none, a, none}});
        break;
    case SGX535_FS_FRACT:
        ok = emit(e, (struct sgx535_use_instruction){
                         .opcode = SGX535_USE_FFRC, .dst = dst, .src = {none, a, a}});
        break;
    case SGX535_FS_DDX:
    case SGX535_FS_DDY:
        /* Uniforms and constants have no gradient. */
        if (!r_or_pa(&a)) {
            ok = copy(e, 0, dst, ZERO);
            break;
        }
        ok = emit(e, (struct sgx535_use_instruction){
                         .opcode = in->op == SGX535_FS_DDX ? SGX535_USE_FDSX : SGX535_USE_FDSY,
                         .syncs = 1,
                         .dst = dst,
                         .src = {none, a, a}});
        /* The quad's rows run down the window: GL's dFdy takes the y sign of
         * the target's orientation (-1 in the window, 1 in an FBO). */
        if (ok && in->op == SGX535_FS_DDY) {
            int sign = constant(e, SGX535_FS_CONSTANT_FRAG_YSIGN, 0);
            ok = sign >= 0 && emit(e, (struct sgx535_use_instruction){
                                          .opcode = SGX535_USE_FMAD,
                                          .dst = dst,
                                          .src = {dst, reg(SGX535_USE_SA, (uint32_t)sign), ZERO}});
        }
        break;
    case SGX535_FS_SLT:
    case SGX535_FS_SGE:
    case SGX535_FS_SEQ:
    case SGX535_FS_SNE:
        if (e->kill_compare == i + 1u) {
            /* p1 = pass: the compare's negation (SLT kills, so GE passes) */
            static const uint8_t pass[] = {SGX535_USE_TEST_GE, SGX535_USE_TEST_LT,
                                           SGX535_USE_TEST_NE, SGX535_USE_TEST_EQ};
            int sa = -1, sb = -1;
            ok = plain(e, &a, &sa) && plain(e, &b, &sb) &&
                 emit(e, (struct sgx535_use_instruction){.opcode = SGX535_USE_TEST,
                                                         .alu = SGX535_USE_ALU_FSUB,
                                                         .test = pass[in->op - SGX535_FS_SLT],
                                                         .pdst = 1,
                                                         .src = {none, a, b}});
            if (sa >= 0)
                release(e, (uint32_t)sa);
            if (sb >= 0)
                release(e, (uint32_t)sb);
            e->out->kill_pred = 1;
            break;
        }
        ok = compare(e, test[in->op - SGX535_FS_SLT], a, b) && copy(e, 0, dst, ZERO) &&
             copy(e, 1, dst, ONE);
        break;
    case SGX535_FS_SEL:
        ok = compare(e, SGX535_USE_TEST_NE, a, ZERO) && copy(e, 0, dst, c) && copy(e, 1, dst, b);
        break;
    default:
        return fail(e, i, "opcode", SGX535_FS_ERR_ARGUMENT);
    }
    if (scratch >= 0)
        release(e, (uint32_t)scratch);
    return ok ? SGX535_FS_OK : fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
}

static enum sgx535_fs_status statement(struct emitter *e, uint32_t i)
{
    const struct sgx535_fs_instruction *in = &e->ir[i];
    struct scope *s = e->depth ? &e->scope[e->depth - 1u] : NULL;
    struct sgx535_use_operand a, dst;
    bool ok = true;
    int sa;
    switch (in->op) {
    case SGX535_FS_CONST:
        return literal(e, in->imm, &e->value[i]) ? SGX535_FS_OK
                                                 : fail(e, i, "constants", SGX535_FS_ERR_LIMIT);
    case SGX535_FS_INPUT:
        e->value[i] = reg(SGX535_USE_PA, (uint32_t)e->varying_pa[in->index] + in->comp);
        return SGX535_FS_OK;
    case SGX535_FS_UNIFORM:
        sa = constant(e,
                      in->comp == 2   ? SGX535_FS_CONSTANT_UNIFORM_BOOL
                      : in->comp == 1 ? SGX535_FS_CONSTANT_UNIFORM_INT
                                      : SGX535_FS_CONSTANT_UNIFORM,
                      in->index);
        if (sa < 0)
            return fail(e, i, "constants", SGX535_FS_ERR_LIMIT);
        e->value[i] = reg(SGX535_USE_SA, (uint32_t)sa);
        return SGX535_FS_OK;
    case SGX535_FS_TEXSIZE:
        sa = constant(e, SGX535_FS_CONSTANT_TEXSIZE, (uint32_t)in->index << 2 | in->comp);
        if (sa < 0)
            return fail(e, i, "constants", SGX535_FS_ERR_LIMIT);
        e->value[i] = reg(SGX535_USE_SA, (uint32_t)sa);
        return SGX535_FS_OK;
    case SGX535_FS_SYSTEM:
        return system_value(e, i);
    case SGX535_FS_TEX:
        return texture(e, i);
    case SGX535_FS_TEXC:
        if (e->pending_drc == in->src[0] + 1u && !wait(e))
            return fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
        if (!define(e, i, &dst))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        {
            const struct sgx535_fs_sampler *sampler = &e->key->sampler[e->ir[in->src[0]].index];
            unsigned sel = sampler->swizzle[in->comp];
            ok = channel(e, dst, e->value[in->src[0]], sampler->result, e->compared[in->src[0]],
                         sel ? sel - 1u : in->comp);
        }
        break;
    case SGX535_FS_LOAD: {
        /* The VAR's register stands for the value unless a STORE to the VAR
         * comes before the value's last use. */
        bool alias = true;
        for (uint32_t j = i + 1; j <= e->last[i] && j < e->count; j++)
            if (e->ir[j].op == SGX535_FS_STORE && e->ir[j].index == in->index)
                alias = false;
        if (!var_register(e, in->index))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        if (alias) {
            e->value[i] = TEMP(e->var_reg[in->index]);
            if (e->last[i] > e->var_last[in->index])
                e->var_last[in->index] = e->last[i];
            return SGX535_FS_OK;
        }
        if (!define(e, i, &dst))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        ok = copy(e, 0, dst, TEMP(e->var_reg[in->index]));
        break;
    }
    case SGX535_FS_KILL:
        if (e->kill_compare == in->src[0] + 1u)
            break; /* p1 already holds the passing condition */
        /* kill = 1.0 where src != 0, accumulated (zeroed at the start) */
        a = operand(e, in->src[0], in->mod[0]);
        ok = compare(e, SGX535_USE_TEST_NE, a, ZERO) && copy(e, 1, TEMP(e->out->kill), ONE);
        break;
    case SGX535_FS_OUTPUT:
        a = operand(e, in->src[0], in->mod[0]);
        if (e->output_top[in->index]) {
            if (in->index == SGX535_FS_DEPTH)
                e->out->depth = a, e->out->writes_depth = 1;
            else
                e->out->color[in->index] = a, e->out->has_color = 1;
            break;
        }
        if (e->output_reg[in->index] == NO_REG) {
            int t = temp(e);
            if (t < 0)
                return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
            e->output_reg[in->index] = (uint8_t)t;
        }
        dst = TEMP(e->output_reg[in->index]);
        if (in->index == SGX535_FS_DEPTH)
            e->out->depth = dst, e->out->writes_depth = 1;
        else
            e->out->color[in->index] = dst, e->out->has_color = 1;
        ok = copy(e, 0, dst, a);
        break;
    case SGX535_FS_STORE:
        if (!var_register(e, in->index))
            return fail(e, i, "temporaries", SGX535_FS_ERR_LIMIT);
        a = operand(e, in->src[0], in->mod[0]);
        if (a.bank != SGX535_USE_R || a.number != e->var_reg[in->index] || a.mod)
            ok = copy(e, 0, TEMP(e->var_reg[in->index]), a);
        break;
    case SGX535_FS_IF:
        a = operand(e, in->src[0], in->mod[0]);
        e->scope[e->depth++] = (struct scope){.op = SGX535_FS_IF};
        s = &e->scope[e->depth - 1u];
        ok = wait(e) && compare(e, SGX535_USE_TEST_NE, a, ZERO) && branch(e, &s->other, 5, false);
        break;
    case SGX535_FS_ELSE:
        ok = wait(e) && branch(e, &s->exit, 0, false);
        label_here(e, &s->other);
        break;
    case SGX535_FS_ENDIF:
        ok = wait(e);
        label_here(e, &s->other);
        label_here(e, &s->exit);
        e->depth--;
        break;
    case SGX535_FS_LOOP:
        ok = wait(e);
        e->scope[e->depth++] = (struct scope){.op = SGX535_FS_LOOP};
        label_here(e, &e->scope[e->depth - 1u].top);
        break;
    case SGX535_FS_BREAK:
    case SGX535_FS_CONTINUE: {
        uint32_t d = e->depth;
        while (e->scope[d - 1u].op != SGX535_FS_LOOP)
            d--;
        ok = wait(e) && (in->op == SGX535_FS_BREAK ? branch(e, &e->scope[d - 1u].exit, 0, false)
                                                   : branch(e, &e->scope[d - 1u].top, 0, true));
        break;
    }
    case SGX535_FS_ENDLOOP:
        ok = wait(e) && branch(e, &s->top, 0, true);
        label_here(e, &s->exit);
        e->depth--;
        break;
    default:
        return alu(e, i);
    }
    return ok ? SGX535_FS_OK : fail(e, i, "code size", SGX535_FS_ERR_LIMIT);
}

enum sgx535_fs_status sgx535_fs_compile(const struct sgx535_fs_instruction *ir, uint32_t count,
                                        const struct sgx535_fs_key *key,
                                        struct sgx535_fs_program *out)
{
    struct emitter *e;
    enum sgx535_fs_status status;
    /* an empty body is a program: a shader that writes no colour (GL leaves
     * it undefined; ours is zero) */
    if ((!ir && count) || !key || !out || count > SGX535_FS_MAX_INSTRUCTIONS)
        return SGX535_FS_ERR_ARGUMENT;
    e = calloc(1, sizeof(*e));
    if (!e)
        return SGX535_FS_ERR_LIMIT;
    memset(out, 0, sizeof(*out));
    e->ir = ir, e->count = count, e->key = key, e->out = out;
    memset(e->var_reg, NO_REG, sizeof(e->var_reg));
    memset(e->sampler_sa, NO_REG, sizeof(e->sampler_sa));
    memset(e->output_reg, NO_REG, sizeof(e->output_reg));
    for (unsigned c = 0; c < 4; c++)
        out->color[c] = ZERO;
    out->depth = ZERO;
    out->kill = NO_REG;
    out->constant_count = 1;
    status = analyse(e);
    /* Gradients and implicit-LOD samples need the quad's invalid pixels
     * computed, so no .skipinv anywhere then (FP, the FGRADIENT rule). */
    e->skipinv = true;
    for (uint32_t i = 0; i < count; i++)
        if (ir[i].op == SGX535_FS_DDX || ir[i].op == SGX535_FS_DDY ||
            (ir[i].op == SGX535_FS_TEX && ir[i].comp == SGX535_FS_TEX_IMPLICIT))
            e->skipinv = false;
    out->uses_derivatives = !e->skipinv;
    if (status == SGX535_FS_OK)
        status = iterations(e);
    if (status == SGX535_FS_OK)
        status = kill_flag(e);
    for (uint32_t i = 0; status == SGX535_FS_OK && i < count; i++) {
        status = statement(e, i);
        retire(e, i);
    }
    if (status == SGX535_FS_OK && !wait(e))
        status = fail(e, count, "code size", SGX535_FS_ERR_LIMIT);
    free(e);
    return status;
}
