// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Complete request4/BG6 raw0 entry. The common emitter never sees a rewritten
 * convention; fixed source validation precedes every regeneration. */
static int depth_single_arguments(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    struct sgx535_depth_window_mesh_single_sample_request *f,
    struct sgx535_depth_window_mesh_request *source, const void *image, unsigned long image_bytes,
    const void *binding, unsigned long binding_bytes, const void *out, unsigned long out_bytes,
    const struct sgx535_single_sample_lowering *receipt, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct sgx535_window_mesh_request *m;
    struct cpu_span ranges[11];
    uint32_t i, n = 8;
    if (!span(r, sizeof(*r)) || !span(receipt, sizeof(*receipt)))
        return 0;
    *f = *r;
    *source = depth_gl_source(&f->mesh);
    m = &source->scene.foreground;
    if (!arguments(r, sizeof(*r), source, image, image_bytes, binding, binding_bytes, out,
                   out_bytes, geometry, bytes, w, scratch, cap))
        return 0;
    ranges[0] = (struct cpu_span){r, sizeof(*r)};
    ranges[1] =
        (struct cpu_span){m->vertices, (unsigned long)m->vertex_count * sizeof(*m->vertices)};
    ranges[2] = (struct cpu_span){m->indices, (unsigned long)m->index_count * sizeof(*m->indices)};
    ranges[3] =
        (struct cpu_span){m->fragment, (unsigned long)m->fragment_count * sizeof(*m->fragment)};
    ranges[4] = (struct cpu_span){out, out_bytes};
    ranges[5] = (struct cpu_span){geometry, bytes};
    ranges[6] = (struct cpu_span){w, sizeof(*w)};
    ranges[7] = (struct cpu_span){scratch, cap};
    if (m->uniform_word_count)
        ranges[n++] = (struct cpu_span){m->uniforms, (unsigned long)m->uniform_word_count * 4u};
    if (image)
        ranges[n++] = (struct cpu_span){image, image_bytes};
    if (binding)
        ranges[n++] = (struct cpu_span){binding, binding_bytes};
    for (i = 0; i < n; i++)
        if (overlap(receipt, sizeof(*receipt), ranges[i].p, ranges[i].bytes))
            return 0;
    return 1;
}
static int depth_single_same(const struct sgx535_depth_window_mesh_single_sample_request *r,
                             const struct sgx535_depth_window_mesh_single_sample_request *f)
{
    return depth_gl_same(&r->mesh, &f->mesh) &&
           !memcmp(&r->samples, &f->samples, sizeof(r->samples));
}
static int depth_single_policy(const struct sgx535_depth_window_mesh_single_sample_request *f,
                               struct depth_policy *policy,
                               struct sgx535_single_sample_lowering *receipt, bool raw_mask)
{
    *policy = gl_depth;
    policy->raw_mask = raw_mask;
    policy->single_sample = &f->samples;
    /* No depth attachment: the window mesh's own ISP test and write state
     * (ALWAYS, write disabled: 0x01d00000) for every fragment. */
    if (f->mesh.no_depth) {
        policy->compare = SGX535_DEPTH_ALWAYS;
        policy->write_enable = 0;
    }
    return sgx535_single_sample_project(&f->samples, &f->mesh.scene.foreground.window,
                                        f->mesh.scene.foreground.fragment_count, receipt);
}
/* Keep conversion/alias validation off the nested compiler stack. The local
 * source is copied into private workspace only after every public span passes. */
static __attribute__((__noinline__)) int
depth_single_build_snapshot(const struct sgx535_depth_window_mesh_single_sample_request *r,
                            struct sgx535_depth_window_mesh_single_sample_request *f,
                            struct sgx535_depth_window_mesh_task *out,
                            struct sgx535_single_sample_lowering *receipt, uint8_t *geometry,
                            uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
                            uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_request source;
    if (!depth_single_arguments(r, f, &source, 0, 0, 0, 0, out, sizeof(*out), receipt, geometry,
                                bytes, w, scratch, cap))
        return 0;
    w->single_source_request = source;
    return 1;
}
static enum sgx535_task_status depth_single_build_io(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    struct sgx535_depth_window_mesh_task *out, struct sgx535_single_sample_lowering *receipt,
    uint8_t *geometry, uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap, bool raw_mask, enum sgx535_window_fragment_io io)
{
    struct sgx535_depth_window_mesh_single_sample_request f;
    struct sgx535_single_sample_lowering projected;
    struct depth_policy policy;
    enum sgx535_task_status st;
    if (!depth_single_build_snapshot(r, &f, out, receipt, geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    if (!depth_single_policy(&f, &policy, &projected, raw_mask))
        return SGX535_TASK_UNSUPPORTED;
    policy.io = io;
    st = regenerate(&w->single_source_request, &policy, w, scratch, cap);
    if (POULSBACK_SELFCHECK && st == SGX535_TASK_BLOCKED && !depth_single_same(r, &f))
        return SGX535_TASK_UNSUPPORTED;
    if (st == SGX535_TASK_BLOCKED) {
        *out = w->image;
        *receipt = projected;
        memcpy(geometry, scratch, out->seed.foreground.geometry.bytes);
    }
    return st;
}
static enum sgx535_task_status
depth_single_build(const struct sgx535_depth_window_mesh_single_sample_request *r,
                   struct sgx535_depth_window_mesh_task *out,
                   struct sgx535_single_sample_lowering *receipt, uint8_t *geometry, uint32_t bytes,
                   struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap,
                   bool raw_mask)
{
    return depth_single_build_io(r, out, receipt, geometry, bytes, w, scratch, cap, raw_mask,
                                 SGX535_WINDOW_PRIMARY_COLOR);
}
static enum sgx535_task_status
depth_single_validate(const struct sgx535_depth_window_mesh_single_sample_request *r,
                      const struct sgx535_depth_window_mesh_task *p,
                      const struct sgx535_single_sample_lowering *receipt, const uint8_t *geometry,
                      uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
                      uint8_t *scratch, uint32_t cap, bool raw_mask)
{
    struct sgx535_depth_window_mesh_single_sample_request f;
    struct sgx535_depth_window_mesh_request source;
    struct sgx535_single_sample_lowering projected;
    struct depth_policy policy;
    enum sgx535_task_status st;
    if (!depth_single_arguments(r, &f, &source, 0, 0, 0, 0, p, sizeof(*p), receipt, geometry, bytes,
                                w, scratch, cap))
        return SGX535_TASK_INVALID;
    if (!depth_single_policy(&f, &policy, &projected, raw_mask))
        return SGX535_TASK_UNSUPPORTED;
    st = regenerate(&source, &policy, w, scratch, cap);
    if (st == SGX535_TASK_BLOCKED && !depth_single_same(r, &f))
        return SGX535_TASK_UNSUPPORTED;
    return st == SGX535_TASK_BLOCKED && (bytes != w->image.seed.foreground.geometry.bytes ||
                                         memcmp(p, &w->image, sizeof(*p)) ||
                                         memcmp(receipt, &projected, sizeof(projected)) ||
                                         memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : st;
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_build(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    struct sgx535_depth_window_mesh_task *p, struct sgx535_single_sample_lowering *receipt,
    uint8_t *geometry, uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    return depth_single_build(r, p, receipt, geometry, bytes, w, scratch, cap, false);
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    const struct sgx535_depth_window_mesh_task *p,
    const struct sgx535_single_sample_lowering *receipt, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return depth_single_validate(r, p, receipt, geometry, bytes, w, scratch, cap, false);
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_raw_mask_build_v1(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    struct sgx535_depth_window_mesh_task *p, struct sgx535_single_sample_lowering *receipt,
    uint8_t *geometry, uint32_t bytes, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    return depth_single_build(r, p, receipt, geometry, bytes, w, scratch, cap, true);
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_raw_mask_validate_v1(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    const struct sgx535_depth_window_mesh_task *p,
    const struct sgx535_single_sample_lowering *receipt, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return depth_single_validate(r, p, receipt, geometry, bytes, w, scratch, cap, true);
}
static enum sgx535_task_status
depth_single_bind(const struct sgx535_depth_window_mesh_single_sample_request *r,
                  const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry,
                  uint32_t bytes, const struct sgx535_depth_window_mesh_binding *binding,
                  const void *out, unsigned long out_bytes,
                  const struct sgx535_single_sample_lowering *receipt,
                  struct sgx535_single_sample_lowering *projected,
                  struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_single_sample_request f;
    struct sgx535_depth_window_mesh_request source;
    struct depth_policy policy;
    enum sgx535_task_status st;
    if (!p || !binding ||
        !depth_single_arguments(r, &f, &source, p, sizeof(*p), binding, sizeof(*binding), out,
                                out_bytes, receipt, geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    if (!depth_single_policy(&f, &policy, projected, false))
        return SGX535_TASK_UNSUPPORTED;
    st = bind_construct(&source, &policy, p, geometry, bytes, binding, w, scratch, cap);
    return st == SGX535_TASK_BLOCKED && !depth_single_same(r, &f) ? SGX535_TASK_UNSUPPORTED : st;
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_bind(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *binding,
    struct sgx535_depth_window_mesh_bound *out, struct sgx535_single_sample_lowering *receipt,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_single_sample_lowering projected;
    enum sgx535_task_status st = depth_single_bind(
        r, p, geometry, bytes, binding, out, sizeof(*out), receipt, &projected, w, scratch, cap);
    if (st == SGX535_TASK_BLOCKED) {
        *out = w->bound;
        *receipt = projected;
    }
    return st;
}
enum sgx535_task_status sgx535_depth_window_mesh_single_sample_bound_validate(
    const struct sgx535_depth_window_mesh_single_sample_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *binding,
    const struct sgx535_depth_window_mesh_bound *out,
    const struct sgx535_single_sample_lowering *receipt,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_single_sample_lowering projected;
    enum sgx535_task_status st = depth_single_bind(
        r, p, geometry, bytes, binding, out, sizeof(*out), receipt, &projected, w, scratch, cap);
    return st == SGX535_TASK_BLOCKED && (memcmp(out, &w->bound, sizeof(*out)) ||
                                         memcmp(receipt, &projected, sizeof(projected)))
               ? SGX535_TASK_INVALID
               : st;
}
