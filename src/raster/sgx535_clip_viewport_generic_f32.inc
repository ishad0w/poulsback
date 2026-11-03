// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Typed generic-F32 front. Included after the private CLIP emit/bind mechanics;
 * no accepted primary-color image is changed into another source family. */
static int gf_args(const struct sgx535_generic_f32_clip_request *r, const void *out,
                   unsigned long out_bytes, const uint8_t *g, uint32_t bytes,
                   struct sgx535_generic_f32_clip_workspace *w, uint8_t *scratch, uint32_t cap,
                   const struct cv_span *extra, uint32_t nr)
{
    struct cv_span span[12];
    uint32_t i, j, n = 9;
    if (!cv_span_ok((struct cv_span){r, sizeof(*r)}) || r->source.vertex_count != 6 ||
        r->source.index_count != 6 || r->source.fragment_count < 5 ||
        r->source.fragment_count > SGX535_NIR_MAX_INSTRUCTIONS || bytes < 280 || cap < 280 ||
        nr > 3)
        return 0;
    span[0] = (struct cv_span){r, sizeof(*r)};
    span[1] = (struct cv_span){r->source.vertices, 6u * sizeof(*r->source.vertices)};
    span[2] = (struct cv_span){r->source.window_sidecar, 6u * sizeof(*r->source.window_sidecar)};
    span[3] = (struct cv_span){r->source.indices, 6u * sizeof(*r->source.indices)};
    span[4] = (struct cv_span){r->source.fragment, (unsigned long)r->source.fragment_count *
                                                       sizeof(*r->source.fragment)};
    span[5] = (struct cv_span){out, out_bytes};
    span[6] = (struct cv_span){g, bytes};
    span[7] = (struct cv_span){w, sizeof(*w)};
    span[8] = (struct cv_span){scratch, cap};
    for (i = 0; i < nr; i++)
        span[n++] = extra[i];
    for (i = 0; i < n; i++) {
        if (!cv_span_ok(span[i]))
            return 0;
        for (j = 0; j < i; j++)
            if (cv_overlap(span[i], span[j]))
                return 0;
    }
    return 1;
}
static int gf_same(const struct sgx535_generic_f32_clip_request *a,
                   const struct sgx535_generic_f32_clip_request *b)
{
    return a->version == b->version && a->packed_background_argb == b->packed_background_argb &&
           !memcmp(a->reserved, b->reserved, sizeof(a->reserved)) &&
           cv_same(&a->source, &b->source);
}
static struct sgx535_background_request
gf_background(const struct sgx535_generic_f32_clip_request *r)
{
    struct sgx535_background_request b = {0};
    b.version = SGX535_BACKGROUND_CLIP_VIEWPORT_VERSION;
    b.core_id = r->source.core_id;
    b.revision = r->source.revision;
    b.width = r->source.width;
    b.height = r->source.height;
    b.packed_argb = r->packed_background_argb;
    return b;
}
static enum sgx535_task_status gf_regenerate(const struct sgx535_generic_f32_clip_request *r,
                                             struct sgx535_generic_f32_clip_workspace *w,
                                             uint8_t *scratch)
{
    struct sgx535_generic_f32_clip_request f = *r;
    struct sgx535_background_request background;
    enum sgx535_task_status status;
    if (f.version != SGX535_GENERIC_F32_CLIP_VERSION || f.packed_background_argb != 0xff000000u ||
        f.reserved[0] || f.reserved[1] || f.source.interpolation != SGX535_VARYING_SMOOTH ||
        !cv_payload(&f.source, 0, 0, 0))
        return SGX535_TASK_UNSUPPORTED;
    status = cv_emit_profile(&f.source, &w->native, scratch, 1);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (!cv_payload(&f.source, scratch, &w->native.result.foreground.geometry,
                    w->native.fragment) ||
        !gf_same(r, &f))
        return SGX535_TASK_UNSUPPORTED;
    background = gf_background(&f);
    status = sgx535_background_clip_viewport_seed(&background, &w->native.result.background);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    w->native.result.version = SGX535_CLIP_VIEWPORT_MESH_BACKGROUND_VERSION;
    w->native.result.missing_background_contracts = SGX535_WINDOW_MESH_BACKGROUND_MISSING_ALL;
    memset(&w->result, 0, sizeof(w->result));
    w->result.version = SGX535_GENERIC_F32_CLIP_VERSION;
    w->result.missing_linkage_contracts = SGX535_GENERIC_F32_CLIP_MISSING_ALL;
    w->result.packet = w->native.result;
    return gf_same(r, &f) && cv_payload(&f.source, scratch, &w->native.result.foreground.geometry,
                                        w->native.fragment)
               ? SGX535_TASK_BLOCKED
               : SGX535_TASK_UNSUPPORTED;
}
enum sgx535_task_status
sgx535_generic_f32_clip_build(const struct sgx535_generic_f32_clip_request *r,
                              struct sgx535_generic_f32_clip_image *out, uint8_t *g, uint32_t bytes,
                              struct sgx535_generic_f32_clip_workspace *w, uint8_t *scratch,
                              uint32_t cap)
{
    enum sgx535_task_status status;
    if (!gf_args(r, out, sizeof(*out), g, bytes, w, scratch, cap, 0, 0))
        return SGX535_TASK_INVALID;
    status = gf_regenerate(r, w, scratch);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result;
        memcpy(g, scratch, 280);
    }
    return status;
}
static int gf_original(const struct sgx535_generic_f32_clip_image *p, const uint8_t *g,
                       struct sgx535_generic_f32_clip_workspace *w, const uint8_t *scratch)
{
    return !memcmp(&w->original, &w->result, sizeof(*p)) &&
           !memcmp(w->original_geometry, scratch, 280) && !memcmp(p, &w->original, sizeof(*p)) &&
           !memcmp(g, w->original_geometry, 280);
}
enum sgx535_task_status
sgx535_generic_f32_clip_validate(const struct sgx535_generic_f32_clip_request *r,
                                 const struct sgx535_generic_f32_clip_image *p, const uint8_t *g,
                                 uint32_t bytes, struct sgx535_generic_f32_clip_workspace *w,
                                 uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!gf_args(r, p, sizeof(*p), g, bytes, w, scratch, cap, 0, 0) || bytes != 280)
        return SGX535_TASK_INVALID;
    memcpy(&w->original, p, sizeof(*p));
    memcpy(w->original_geometry, g, 280);
    status = gf_regenerate(r, w, scratch);
    return status == SGX535_TASK_BLOCKED && !gf_original(p, g, w, scratch) ? SGX535_TASK_INVALID
                                                                           : status;
}
static enum sgx535_task_status gf_bind(const struct sgx535_generic_f32_clip_request *r,
                                       const struct sgx535_generic_f32_clip_image *p,
                                       const uint8_t *g, const struct sgx535_window_mesh_binding *b,
                                       const struct sgx535_background_binding *bg,
                                       struct sgx535_generic_f32_clip_workspace *w,
                                       uint8_t *scratch)
{
    struct sgx535_generic_f32_clip_request f = *r;
    struct sgx535_window_mesh_binding frozen_binding = *b;
    struct sgx535_background_binding frozen_background = *bg;
    struct sgx535_background_request request;
    enum sgx535_task_status status;
    memcpy(&w->original, p, sizeof(*p));
    memcpy(w->original_geometry, g, 280);
    status = gf_regenerate(r, w, scratch);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (!gf_original(p, g, w, scratch))
        return SGX535_TASK_INVALID;
    request = gf_background(&f);
    status = sgx535_clip_viewport_internal_bind_pair(&request, &frozen_binding, &frozen_background,
                                                     &w->native);
    if (status != SGX535_TASK_BLOCKED)
        return status;
    if (memcmp(b, &frozen_binding, sizeof(*b)) || memcmp(bg, &frozen_background, sizeof(*bg)) ||
        !gf_original(p, g, w, scratch) || !gf_same(r, &f) ||
        !cv_payload(&f.source, scratch, &w->native.result.foreground.geometry, w->native.fragment))
        return SGX535_TASK_INVALID;
    memset(&w->bound, 0, sizeof(w->bound));
    w->bound.version = SGX535_GENERIC_F32_CLIP_VERSION;
    w->bound.missing_linkage_contracts = SGX535_GENERIC_F32_CLIP_MISSING_ALL;
    w->bound.packet = w->native.bound;
    return SGX535_TASK_BLOCKED;
}
enum sgx535_task_status sgx535_generic_f32_clip_bind(
    const struct sgx535_generic_f32_clip_request *r, const struct sgx535_generic_f32_clip_image *p,
    const uint8_t *g, uint32_t bytes, const struct sgx535_window_mesh_binding *b,
    const struct sgx535_background_binding *bg, struct sgx535_generic_f32_clip_bound *out,
    struct sgx535_generic_f32_clip_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[3] = {{p, sizeof(*p)}, {b, sizeof(*b)}, {bg, sizeof(*bg)}};
    enum sgx535_task_status status;
    if (!gf_args(r, out, sizeof(*out), g, bytes, w, scratch, cap, extra, 3) || bytes != 280)
        return SGX535_TASK_INVALID;
    status = gf_bind(r, p, g, b, bg, w, scratch);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_generic_f32_clip_bound_validate(
    const struct sgx535_generic_f32_clip_request *r, const struct sgx535_generic_f32_clip_image *p,
    const uint8_t *g, uint32_t bytes, const struct sgx535_window_mesh_binding *b,
    const struct sgx535_background_binding *bg, const struct sgx535_generic_f32_clip_bound *out,
    struct sgx535_generic_f32_clip_workspace *w, uint8_t *scratch, uint32_t cap)
{
    const struct cv_span extra[3] = {{p, sizeof(*p)}, {b, sizeof(*b)}, {bg, sizeof(*bg)}};
    enum sgx535_task_status status;
    if (!gf_args(r, out, sizeof(*out), g, bytes, w, scratch, cap, extra, 3) || bytes != 280)
        return SGX535_TASK_INVALID;
    status = gf_bind(r, p, g, b, bg, w, scratch);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status
sgx535_generic_f32_clip_native_admission(const struct sgx535_generic_f32_clip_image *image)
{
    return image ? SGX535_TASK_UNSUPPORTED : SGX535_TASK_INVALID;
}
