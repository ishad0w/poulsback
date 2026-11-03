// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Included by task_launch.c; keep its checked canonical projection private. */
#include "sgx535_normalized_color0_task.h"
static int normalized_launch_arguments(const struct sgx535_normalized_color0_request *r,
                                       const struct sgx535_normalized_color0_task *p,
                                       const struct sgx535_task_launch_binding *b, const void *out,
                                       struct sgx535_normalized_color0_launch_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !b || !out || !w || !r->fragment || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    if (bytes > ~(unsigned long)0 - (unsigned long)r->fragment)
        return 0;
    return !overlap(w, sizeof(*w), r, sizeof(*r)) && !overlap(w, sizeof(*w), p, sizeof(*p)) &&
           !overlap(w, sizeof(*w), b, sizeof(*b)) && !overlap(w, sizeof(*w), r->fragment, bytes) &&
           !overlap(w, sizeof(*w), out, sizeof(struct sgx535_normalized_color0_launch)) &&
           !overlap(out, sizeof(struct sgx535_normalized_color0_launch), r, sizeof(*r)) &&
           !overlap(out, sizeof(struct sgx535_normalized_color0_launch), p, sizeof(*p)) &&
           !overlap(out, sizeof(struct sgx535_normalized_color0_launch), b, sizeof(*b)) &&
           !overlap(out, sizeof(struct sgx535_normalized_color0_launch), r->fragment, bytes);
}
static enum sgx535_task_status
normalized_launch_project(const struct sgx535_normalized_color0_task *p,
                          const struct sgx535_task_launch_binding *b,
                          struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status = construct_validated_packet(&p->packet, b, &w->projection);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    memset(&w->result, 0, sizeof(w->result));
    w->result.launch = w->projection.result;
    w->result.fragment_missing_contracts = w->packet.result.fragment.missing_contracts;
    w->result.reference_policy = w->packet.result.fragment.reference_policy;
    return status;
}
static enum sgx535_task_status normalized_launch_construct(
    const struct sgx535_normalized_color0_request *r, const struct sgx535_normalized_color0_task *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status = sgx535_normalized_color0_task_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return normalized_launch_project(p, b, w);
}
enum sgx535_task_status sgx535_normalized_color0_launch_build(
    const struct sgx535_normalized_color0_request *r, const struct sgx535_normalized_color0_task *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_launch_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_launch_validate(
    const struct sgx535_normalized_color0_request *r, const struct sgx535_normalized_color0_task *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_launch_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}
static int normalized_tile16_span(const void *p, unsigned long bytes)
{
    return p && bytes <= ~(unsigned long)0 - (unsigned long)p;
}
static int
normalized_tile16_launch_arguments(const struct sgx535_normalized_color0_tile16_request *r,
                                   const struct sgx535_normalized_color0_task *p,
                                   const struct sgx535_task_launch_binding *b,
                                   const struct sgx535_normalized_color0_launch *out,
                                   struct sgx535_normalized_color0_launch_workspace *w)
{
    unsigned long bytes;
    if (!r || !p || !b || !out || !w || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS)
        return 0;
    bytes = (unsigned long)r->fragment_count * sizeof(*r->fragment);
    return normalized_tile16_span(r, sizeof(*r)) && normalized_tile16_span(p, sizeof(*p)) &&
           normalized_tile16_span(b, sizeof(*b)) && normalized_tile16_span(out, sizeof(*out)) &&
           normalized_tile16_span(w, sizeof(*w)) && normalized_tile16_span(r->fragment, bytes) &&
           !overlap(w, sizeof(*w), r, sizeof(*r)) && !overlap(w, sizeof(*w), p, sizeof(*p)) &&
           !overlap(w, sizeof(*w), b, sizeof(*b)) && !overlap(w, sizeof(*w), r->fragment, bytes) &&
           !overlap(w, sizeof(*w), out, sizeof(*out)) &&
           !overlap(out, sizeof(*out), r, sizeof(*r)) &&
           !overlap(out, sizeof(*out), p, sizeof(*p)) &&
           !overlap(out, sizeof(*out), b, sizeof(*b)) &&
           !overlap(out, sizeof(*out), r->fragment, bytes);
}
static enum sgx535_task_status
normalized_tile16_launch_construct(const struct sgx535_normalized_color0_tile16_request *r,
                                   const struct sgx535_normalized_color0_task *p,
                                   const struct sgx535_task_launch_binding *b,
                                   struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status =
        sgx535_normalized_color0_tile16_task_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return normalized_launch_project(p, b, w);
}
enum sgx535_task_status sgx535_normalized_color0_tile16_launch_build(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *p, const struct sgx535_task_launch_binding *b,
    struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_tile16_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_tile16_launch_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_tile16_launch_validate(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *p, const struct sgx535_task_launch_binding *b,
    const struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_tile16_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_tile16_launch_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}
static enum sgx535_task_status
normalized_mask_tile16_launch_construct(const struct sgx535_normalized_color0_tile16_request *r,
                                        const struct sgx535_normalized_color0_task *p,
                                        const struct sgx535_task_launch_binding *b,
                                        struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status =
        sgx535_normalized_color0_mask_tile16_task_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return normalized_launch_project(p, b, w);
}
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_launch_build(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *p, const struct sgx535_task_launch_binding *b,
    struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_tile16_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_mask_tile16_launch_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_color0_mask_tile16_launch_validate(
    const struct sgx535_normalized_color0_tile16_request *r,
    const struct sgx535_normalized_color0_task *p, const struct sgx535_task_launch_binding *b,
    const struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_color0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!normalized_tile16_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = normalized_mask_tile16_launch_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}

static int pa0_launch_arguments(const struct sgx535_normalized_pa0_request *r,
                                const struct sgx535_normalized_pa0_task *p,
                                const struct sgx535_task_launch_binding *b,
                                const struct sgx535_normalized_color0_launch *out,
                                struct sgx535_normalized_pa0_launch_workspace *w)
{
    size_t ir_bytes, uniform_bytes;
    if (!r || !p || !b || !out || !w || r->fragment_count < 5 ||
        r->fragment_count > SGX535_NIR_MAX_INSTRUCTIONS ||
        r->uniform_word_count > SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!r->uniforms != !!r->uniform_word_count))
        return 0;
    ir_bytes = (size_t)r->fragment_count * sizeof(*r->fragment);
    uniform_bytes = (size_t)r->uniform_word_count * sizeof(*r->uniforms);
    if (!normalized_tile16_span(r, sizeof(*r)) || !normalized_tile16_span(p, sizeof(*p)) ||
        !normalized_tile16_span(b, sizeof(*b)) || !normalized_tile16_span(out, sizeof(*out)) ||
        !normalized_tile16_span(w, sizeof(*w)) || !normalized_tile16_span(r->fragment, ir_bytes) ||
        (uniform_bytes && !normalized_tile16_span(r->uniforms, uniform_bytes)))
        return 0;
    return !overlap(w, sizeof(*w), r, sizeof(*r)) && !overlap(w, sizeof(*w), p, sizeof(*p)) &&
           !overlap(w, sizeof(*w), b, sizeof(*b)) && !overlap(w, sizeof(*w), out, sizeof(*out)) &&
           !overlap(w, sizeof(*w), r->fragment, ir_bytes) &&
           !overlap(out, sizeof(*out), r, sizeof(*r)) &&
           !overlap(out, sizeof(*out), p, sizeof(*p)) &&
           !overlap(out, sizeof(*out), b, sizeof(*b)) &&
           !overlap(out, sizeof(*out), r->fragment, ir_bytes) &&
           (!uniform_bytes || (!overlap(w, sizeof(*w), r->uniforms, uniform_bytes) &&
                               !overlap(out, sizeof(*out), r->uniforms, uniform_bytes)));
}
static enum sgx535_task_status pa0_launch_construct(
    const struct sgx535_normalized_pa0_request *r, const struct sgx535_normalized_pa0_task *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_normalized_pa0_launch_workspace *w)
{
    enum sgx535_task_status status = sgx535_normalized_pa0_task_validate(r, p, &w->packet);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    status = construct_validated_packet(&p->task.packet, b, &w->projection);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    memset(&w->result, 0, sizeof(w->result));
    w->result.launch = w->projection.result;
    w->result.fragment_missing_contracts = p->task.fragment.missing_contracts;
    w->result.reference_policy = p->task.fragment.reference_policy;
    return status;
}
enum sgx535_task_status sgx535_normalized_pa0_launch_build(
    const struct sgx535_normalized_pa0_request *r, const struct sgx535_normalized_pa0_task *p,
    const struct sgx535_task_launch_binding *b, struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_pa0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!pa0_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = pa0_launch_construct(r, p, b, w);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->result;
    return status;
}
enum sgx535_task_status sgx535_normalized_pa0_launch_validate(
    const struct sgx535_normalized_pa0_request *r, const struct sgx535_normalized_pa0_task *p,
    const struct sgx535_task_launch_binding *b, const struct sgx535_normalized_color0_launch *out,
    struct sgx535_normalized_pa0_launch_workspace *w)
{
    enum sgx535_task_status status;
    if (!pa0_launch_arguments(r, p, b, out, w))
        return SGX535_TASK_INVALID;
    status = pa0_launch_construct(r, p, b, w);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    return memcmp(out, &w->result, sizeof(*out)) ? SGX535_TASK_INVALID : status;
}
