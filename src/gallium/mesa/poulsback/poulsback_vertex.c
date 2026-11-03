// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "poulsback_vertex.h"
#include "poulsback_refuse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layouts a shader meets: usually one or two. */
#define VARIANTS 4u
struct variant {
    bool used;
    uint8_t components[SGX535_VERTEX_INPUTS];
    uint32_t tc_count;
    struct sgx535_vertex_tc tc[SGX535_VERTEX_TCS];
    struct poulsback_frame_gpu_program program;
    struct sgx535_vertex_code code;
};
struct poulsback_vertex_shader {
    struct sgx535_vertex_nir_program ir;
    struct variant variant[VARIANTS];
    unsigned next; /* the variant replaced next */
};

/* Program identities for the frame writer's placement: unique per process. */
static uint64_t next_id(void)
{
    static uint64_t id;
    return __atomic_add_fetch(&id, 1, __ATOMIC_RELAXED);
}

struct poulsback_vertex_shader *poulsback_vertex_shader_create(const struct nir_shader *nir,
                                                               const char **refusal)
{
    struct poulsback_vertex_shader *vs = calloc(1, sizeof(*vs));
    *refusal = NULL;
    if (!vs) {
        *refusal = "memory";
        return NULL;
    }
    if (sgx535_nir_vertex_ir(nir, &vs->ir) != SGX535_VERTEX_OK) {
        *refusal = vs->ir.refusal ? vs->ir.refusal : "shader";
        free(vs);
        return NULL;
    }
    return vs;
}

void poulsback_vertex_shader_destroy(struct poulsback_vertex_shader *vs)
{
    free(vs);
}

uint32_t poulsback_vertex_shader_inputs(const struct poulsback_vertex_shader *vs)
{
    return vs->ir.input_count;
}

static bool same_tcs(const struct variant *v, const struct sgx535_vertex_tc *tc, uint32_t n)
{
    if (v->tc_count != n)
        return false;
    for (uint32_t t = 0; t < n; t++)
        if (v->tc[t].location != tc[t].location || v->tc[t].components != tc[t].components)
            return false;
    return true;
}

const struct poulsback_frame_gpu_program *
poulsback_vertex_shader_program(struct poulsback_vertex_shader *vs,
                                const uint8_t components[SGX535_VERTEX_INPUTS],
                                const struct sgx535_vertex_tc *tc, uint32_t tc_count)
{
    struct variant *v;
    if (tc_count > SGX535_VERTEX_TCS)
        return NULL;
    for (unsigned i = 0; i < VARIANTS; i++)
        if (vs->variant[i].used && same_tcs(&vs->variant[i], tc, tc_count) &&
            !memcmp(vs->variant[i].components, components, SGX535_VERTEX_INPUTS))
            return &vs->variant[i].program;
    v = &vs->variant[vs->next];
    v->used = false;
    if (sgx535_vertex_nir_link(&vs->ir, components, tc, tc_count, &v->code) != SGX535_VERTEX_OK)
        return NULL;
    /* A replaced variant's program stays placed under its old identity; the
     * new one gets its own. */
    v->program = (struct poulsback_frame_gpu_program){next_id(), v->code.words,
                                                      v->code.instruction_count, v->code.temps};
    v->used = true;
    v->tc_count = tc_count;
    if (tc_count)
        memcpy(v->tc, tc, tc_count * sizeof(*tc));
    memcpy(v->components, components, SGX535_VERTEX_INPUTS);
    vs->next = (vs->next + 1u) % VARIANTS;
    if (poulsback_trace_on()) {
        fprintf(stderr,
                "poulsback trace: vs: program %llu: %u instructions, %u temporaries, "
                "%u constants, %u texture coordinates:",
                (unsigned long long)v->program.id, v->code.instruction_count, v->code.temps,
                vs->ir.constant_count, tc_count);
        for (uint32_t i = 0; i < v->code.instruction_count; i++)
            fprintf(stderr, " %08x:%08x", v->code.words[2u * i], v->code.words[2u * i + 1u]);
        fprintf(stderr, "\n");
    }
    return &v->program;
}

uint32_t poulsback_vertex_shader_constants(const struct poulsback_vertex_shader *vs, const void *cb0,
                                           uint32_t bytes, uint32_t *out)
{
    const struct sgx535_vertex_nir_program *p = &vs->ir;
    for (uint32_t k = 0; k < p->constant_count; k++) {
        const struct sgx535_vertex_constant *c = &p->constant[k];
        if (c->kind == SGX535_VERTEX_WORD) {
            if (c->value >= bytes / 4u)
                return 0;
            memcpy(&out[k], (const uint8_t *)cb0 + 4u * c->value, 4);
        } else
            out[k] = c->kind == SGX535_VERTEX_LITERAL ? c->value : 0;
    }
    return p->constant_count;
}
