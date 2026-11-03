// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Fixed raw0 source route into the existing complete WINDOW/BG emitter. */
static int single_sample_seed_arguments(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples, const void *out, unsigned long outbytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct sgx535_window_mesh_request *m;
    if (!span(samples, sizeof(*samples)) ||
        !gl_background_arguments(r, out, outbytes, geometry, bytes, w, scratch, cap))
        return 0;
    m = &r->foreground;
    if (overlap(samples, sizeof(*samples), r, sizeof(*r)) ||
        overlap(samples, sizeof(*samples), m->vertices,
                (unsigned long)m->vertex_count * sizeof(*m->vertices)) ||
        overlap(samples, sizeof(*samples), m->indices,
                (unsigned long)m->index_count * sizeof(*m->indices)) ||
        overlap(samples, sizeof(*samples), m->fragment,
                (unsigned long)m->fragment_count * sizeof(*m->fragment)) ||
        (m->uniform_word_count && overlap(samples, sizeof(*samples), m->uniforms,
                                          (unsigned long)m->uniform_word_count * 4u)) ||
        overlap(samples, sizeof(*samples), out, outbytes) ||
        overlap(samples, sizeof(*samples), geometry, bytes) ||
        overlap(samples, sizeof(*samples), w, sizeof(*w)) ||
        overlap(samples, sizeof(*samples), scratch, cap))
        return 0;
    return (!binding || (!overlap(samples, sizeof(*samples), binding, sizeof(*binding)) &&
                         !overlap(samples, sizeof(*samples), background, sizeof(*background)))) !=
           0;
}
static int single_uniform_arguments(const struct sgx535_window_mesh_uniform_source *u,
                                    const struct sgx535_window_mesh_gl_window_background_request *r,
                                    const struct sgx535_single_sample_source *samples,
                                    const void *out, unsigned long outbytes,
                                    const struct sgx535_window_mesh_background_image *image,
                                    const struct sgx535_window_mesh_binding *binding,
                                    const struct sgx535_background_binding *background,
                                    const uint8_t *geometry, uint32_t bytes,
                                    struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                                    uint32_t cap)
{
    struct uniform_span {
        const void *p;
        unsigned long bytes;
    } ranges[13];
    const struct sgx535_window_mesh_request *m = &r->foreground;
    uint32_t n = 0, i, j, uniform_ranges;
    if (!span(u, sizeof(*u)) || u->word_count > SGX535_FRAGMENT_UNIFORM_WORDS ||
        (!!u->words != !!u->word_count))
        return 0;
    ranges[n++] = (struct uniform_span){u, sizeof(*u)};
    if (u->word_count)
        ranges[n++] = (struct uniform_span){u->words, (unsigned long)u->word_count * 4u};
    uniform_ranges = n;
    ranges[n++] = (struct uniform_span){r, sizeof(*r)};
    ranges[n++] = (struct uniform_span){samples, sizeof(*samples)};
    ranges[n++] = (struct uniform_span){out, outbytes};
    ranges[n++] = (struct uniform_span){geometry, bytes};
    ranges[n++] = (struct uniform_span){w, sizeof(*w)};
    ranges[n++] = (struct uniform_span){scratch, cap};
    ranges[n++] =
        (struct uniform_span){m->vertices, (unsigned long)m->vertex_count * sizeof(*m->vertices)};
    ranges[n++] =
        (struct uniform_span){m->indices, (unsigned long)m->index_count * sizeof(*m->indices)};
    ranges[n++] =
        (struct uniform_span){m->fragment, (unsigned long)m->fragment_count * sizeof(*m->fragment)};
    if (image)
        ranges[n++] = (struct uniform_span){image, sizeof(*image)};
    for (i = 0; i < uniform_ranges; i++) {
        if (!span(ranges[i].p, ranges[i].bytes))
            return 0;
        for (j = i + 1; j < n; j++)
            if (overlap(ranges[i].p, ranges[i].bytes, ranges[j].p, ranges[j].bytes))
                return 0;
        if (binding && (overlap(ranges[i].p, ranges[i].bytes, binding, sizeof(*binding)) ||
                        overlap(ranges[i].p, ranges[i].bytes, background, sizeof(*background))))
            return 0;
    }
    return 1;
}
static int single_uniform_same(const struct sgx535_window_mesh_uniform_source *u,
                               const struct sgx535_window_mesh_uniform_source *frozen,
                               const struct sgx535_window_mesh_workspace *w)
{
    return !u || (!memcmp(u, frozen, sizeof(*u)) &&
                  (!frozen->word_count ||
                   !memcmp(frozen->words, w->uniforms, (unsigned long)frozen->word_count * 4u)));
}
static void single_uniform_copy(const struct sgx535_window_mesh_uniform_source *u,
                                struct sgx535_window_mesh_workspace *w)
{
    memset(w->uniforms, 0, sizeof(w->uniforms));
    w->uniform_word_count = u->word_count;
    if (u->word_count)
        memcpy(w->uniforms, u->words, (unsigned long)u->word_count * 4u);
}
static enum sgx535_task_status
single_background_build(const struct sgx535_window_mesh_gl_window_background_request *r,
                        const struct sgx535_single_sample_source *samples,
                        struct sgx535_window_mesh_background_image *out, uint8_t *geometry,
                        uint32_t bytes, struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
                        uint32_t cap, bool raw_mask, enum sgx535_window_fragment_io io,
                        const struct sgx535_window_mesh_uniform_source *uniforms)
{
    struct sgx535_window_mesh_gl_window_background_request frozen;
    struct sgx535_window_mesh_background_request source;
    struct sgx535_single_sample_source facts;
    struct sgx535_single_sample_lowering receipt;
    struct sgx535_window_mesh_uniform_source uniform = {0};
    enum sgx535_task_status status;
    if (!single_sample_seed_arguments(r, samples, out, sizeof(*out), 0, 0, geometry, bytes, w,
                                      scratch, cap) ||
        (uniforms && !single_uniform_arguments(uniforms, r, samples, out, sizeof(*out), 0, 0, 0,
                                               geometry, bytes, w, scratch, cap)))
        return SGX535_TASK_INVALID;
    if (uniforms) {
        uniform = *uniforms;
        if (uniform.constant_flags || uniform.reserved[0] || uniform.reserved[1])
            return SGX535_TASK_UNSUPPORTED;
        single_uniform_copy(&uniform, w);
    }
    frozen = *r;
    facts = *samples;
    source = gl_background_source(&frozen);
    if (!sgx535_single_sample_project(&facts, &frozen.foreground.window,
                                      frozen.foreground.fragment_count, &receipt))
        return SGX535_TASK_UNSUPPORTED;
    status =
        background_construct_io(&source, w, scratch, WINDOW_WIN7_GL_SINGLE_SAMPLE, raw_mask, io);
    if (status == SGX535_TASK_BLOCKED &&
        (!gl_background_same(r, &frozen) || memcmp(samples, &facts, sizeof(facts)) ||
         !single_uniform_same(uniforms, &uniform, w)))
        return SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result;
        memcpy(geometry, scratch, out->foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_f32_build(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return single_background_build(r, samples, out, geometry, bytes, w, scratch, cap, false,
                                   SGX535_WINDOW_GENERIC0_F32, NULL);
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_build(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return single_background_build(r, samples, out, geometry, bytes, w, scratch, cap, false,
                                   SGX535_WINDOW_PRIMARY_COLOR, NULL);
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_raw_mask_build_v1(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    return single_background_build(r, samples, out, geometry, bytes, w, scratch, cap, true,
                                   SGX535_WINDOW_PRIMARY_COLOR, NULL);
}
static enum sgx535_task_status single_background_bind_owner_io(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap, bool full_owner, bool raw_mask,
    enum sgx535_window_fragment_io io, const struct sgx535_window_mesh_uniform_source *uniforms)
{
    struct sgx535_window_mesh_gl_window_background_request frozen;
    struct sgx535_window_mesh_background_request source;
    struct sgx535_single_sample_source facts;
    struct sgx535_single_sample_lowering receipt;
    struct sgx535_window_mesh_uniform_source uniform = {0};
    enum sgx535_task_status status;
    if (!gl_background_bind_arguments(r, p, geometry, bytes, binding, background, out, w, scratch,
                                      cap) ||
        !single_sample_seed_arguments(r, samples, out, sizeof(*out), binding, background, geometry,
                                      bytes, w, scratch, cap) ||
        overlap(samples, sizeof(*samples), p, sizeof(*p)) ||
        (uniforms && !single_uniform_arguments(uniforms, r, samples, out, sizeof(*out), p, binding,
                                               background, geometry, bytes, w, scratch, cap)))
        return SGX535_TASK_INVALID;
    if (uniforms) {
        uniform = *uniforms;
        if (uniform.constant_flags || uniform.reserved[0] || uniform.reserved[1])
            return SGX535_TASK_UNSUPPORTED;
        single_uniform_copy(&uniform, w);
    }
    frozen = *r;
    facts = *samples;
    source = gl_background_source(&frozen);
    if (!sgx535_single_sample_project(&facts, &frozen.foreground.window,
                                      frozen.foreground.fragment_count, &receipt))
        return SGX535_TASK_UNSUPPORTED;
    status = background_bind_construct_owner_io(&source, p, geometry, bytes, binding, background, w,
                                                scratch, WINDOW_WIN7_GL_SINGLE_SAMPLE, full_owner,
                                                raw_mask, io);
    if (status == SGX535_TASK_BLOCKED &&
        (!gl_background_same(r, &frozen) || memcmp(samples, &facts, sizeof(facts)) ||
         !single_uniform_same(uniforms, &uniform, w)))
        return SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_bind(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    return single_background_bind_owner_io(r, samples, p, geometry, bytes, binding, background, out,
                                           w, scratch, cap, false, false,
                                           SGX535_WINDOW_PRIMARY_COLOR, NULL);
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_bind_full_owner(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap, bool raw_mask)
{
    return single_background_bind_owner_io(r, samples, p, geometry, bytes, binding, background, out,
                                           w, scratch, cap, true, raw_mask,
                                           SGX535_WINDOW_PRIMARY_COLOR, NULL);
}

enum sgx535_task_status sgx535_window_mesh_single_sample_background_f32_bind_full_owner(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    return single_background_bind_owner_io(r, samples, p, geometry, bytes, binding, background, out,
                                           w, scratch, cap, true, false, SGX535_WINDOW_GENERIC0_F32,
                                           NULL);
}

enum sgx535_task_status sgx535_window_mesh_single_sample_background_secondary_build(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_uniform_source *uniforms,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    if (!uniforms)
        return SGX535_TASK_INVALID;
    return single_background_build(r, samples, out, geometry, bytes, w, scratch, cap, false,
                                   SGX535_WINDOW_GENERIC0_F32_SECONDARY, uniforms);
}
enum sgx535_task_status sgx535_window_mesh_single_sample_background_secondary_bind(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_single_sample_source *samples,
    const struct sgx535_window_mesh_uniform_source *uniforms,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *binding,
    const struct sgx535_background_binding *background,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    if (!uniforms)
        return SGX535_TASK_INVALID;
    return single_background_bind_owner_io(r, samples, p, geometry, bytes, binding, background, out,
                                           w, scratch, cap, true, false,
                                           SGX535_WINDOW_GENERIC0_F32_SECONDARY, uniforms);
}
