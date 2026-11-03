// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Included only by the complete WINDOW producer; no arbitrary policy callback. */
enum sgx535_task_status
sgx535_window_mesh_gl_window_build(const struct sgx535_window_mesh_request *r,
                                   struct sgx535_window_mesh_task *out, uint8_t *geometry,
                                   uint32_t bytes, struct sgx535_window_mesh_workspace *w,
                                   uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, w, scratch, WINDOW_WIN7_GL, false);
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result.foreground;
        memcpy(geometry, scratch, out->geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_validate(
    const struct sgx535_window_mesh_request *r, const struct sgx535_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!arguments(r, p, sizeof(*p), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = regenerate(r, w, scratch, WINDOW_WIN7_GL, false);
    return status == SGX535_TASK_BLOCKED && (bytes != w->result.foreground.geometry.bytes ||
                                             memcmp(p, &w->result.foreground, sizeof(*p)) ||
                                             memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_bind(
    const struct sgx535_window_mesh_request *r, const struct sgx535_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_window_mesh_binding *b,
    struct sgx535_window_mesh_bound *out, struct sgx535_window_mesh_workspace *w, uint8_t *scratch,
    uint32_t cap)
{
    enum sgx535_task_status status;
    if (!bind_arguments(r, p, sizeof(*p), geometry, bytes, b, out, sizeof(*out), w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, p, geometry, bytes, b, w, scratch, WINDOW_WIN7_GL);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound.foreground;
    return status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_bound_validate(
    const struct sgx535_window_mesh_request *r, const struct sgx535_window_mesh_task *p,
    const uint8_t *geometry, uint32_t bytes, const struct sgx535_window_mesh_binding *b,
    const struct sgx535_window_mesh_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!bind_arguments(r, p, sizeof(*p), geometry, bytes, b, out, sizeof(*out), w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(r, p, geometry, bytes, b, w, scratch, WINDOW_WIN7_GL);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound.foreground, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
static int gl_background_arguments(const struct sgx535_window_mesh_gl_window_background_request *r,
                                   const void *out, unsigned long outbytes, const uint8_t *geometry,
                                   uint32_t bytes, struct sgx535_window_mesh_workspace *w,
                                   uint8_t *scratch, uint32_t cap)
{
    return span(r, sizeof(*r)) &&
           arguments(&r->foreground, out, outbytes, geometry, bytes, w, scratch, cap) &&
           !overlap(r, sizeof(*r), r->foreground.vertices,
                    (unsigned long)r->foreground.vertex_count * sizeof(*r->foreground.vertices)) &&
           !overlap(r, sizeof(*r), r->foreground.indices,
                    (unsigned long)r->foreground.index_count * sizeof(*r->foreground.indices)) &&
           !overlap(r, sizeof(*r), r->foreground.fragment,
                    (unsigned long)r->foreground.fragment_count *
                        sizeof(*r->foreground.fragment)) &&
           (!r->foreground.uniform_word_count ||
            !overlap(r, sizeof(*r), r->foreground.uniforms,
                     (unsigned long)r->foreground.uniform_word_count * 4u)) &&
           !overlap(r, sizeof(*r), out, outbytes) && !overlap(r, sizeof(*r), geometry, bytes) &&
           !overlap(r, sizeof(*r), w, sizeof(*w)) && !overlap(r, sizeof(*r), scratch, cap);
}
static int gl_background_same(const struct sgx535_window_mesh_gl_window_background_request *r,
                              const struct sgx535_window_mesh_gl_window_background_request *f)
{
    return r->version == f->version && r->packed_background_argb == f->packed_background_argb &&
           !memcmp(r->reserved, f->reserved, sizeof(r->reserved)) &&
           same_request(&r->foreground, &f->foreground);
}
static struct sgx535_window_mesh_background_request
gl_background_source(const struct sgx535_window_mesh_gl_window_background_request *r)
{
    struct sgx535_window_mesh_background_request q = {.version = r->version,
                                                      .packed_background_argb =
                                                          r->packed_background_argb,
                                                      .reserved = {r->reserved[0], r->reserved[1]},
                                                      .foreground = r->foreground};
    /* Explicit typed source copy, no cast or0/0 normalization. Private complete
     * construction checks version6 plus every retained1/1 source field. */
    return q;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_background_build(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    struct sgx535_window_mesh_background_image *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_window_mesh_gl_window_background_request frozen;
    struct sgx535_window_mesh_background_request source;
    enum sgx535_task_status status;
    if (!gl_background_arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    frozen = *r;
    source = gl_background_source(&frozen);
    status = background_construct(&source, w, scratch, WINDOW_WIN7_GL, false);
    if (status == SGX535_TASK_BLOCKED && !gl_background_same(r, &frozen))
        return SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->result;
        memcpy(geometry, scratch, out->foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_background_validate(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_window_mesh_gl_window_background_request frozen;
    struct sgx535_window_mesh_background_request source;
    enum sgx535_task_status status;
    if (!gl_background_arguments(r, p, sizeof(*p), geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    frozen = *r;
    source = gl_background_source(&frozen);
    status = background_construct(&source, w, scratch, WINDOW_WIN7_GL, false);
    if (status == SGX535_TASK_BLOCKED && !gl_background_same(r, &frozen))
        return SGX535_TASK_UNSUPPORTED;
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->result.foreground.geometry.bytes ||
                    memcmp(p, &w->result, sizeof(*p)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static int gl_background_bind_arguments(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    const struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    return gl_background_arguments(r, out, sizeof(*out), geometry, bytes, w, scratch, cap) &&
           !overlap(p, sizeof(*p), r, sizeof(*r)) && !overlap(b, sizeof(*b), r, sizeof(*r)) &&
           !overlap(bg, sizeof(*bg), r, sizeof(*r)) &&
           extra(p, sizeof(*p), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           extra(b, sizeof(*b), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           extra(bg, sizeof(*bg), &r->foreground, out, sizeof(*out), geometry, bytes, w, scratch,
                 cap) &&
           !overlap(p, sizeof(*p), b, sizeof(*b)) && !overlap(p, sizeof(*p), bg, sizeof(*bg)) &&
           !overlap(b, sizeof(*b), bg, sizeof(*bg));
}
static enum sgx535_task_status gl_background_bind_construct(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    struct sgx535_window_mesh_workspace *w, uint8_t *scratch)
{
    struct sgx535_window_mesh_gl_window_background_request frozen = *r;
    struct sgx535_window_mesh_background_request source = gl_background_source(&frozen);
    enum sgx535_task_status status =
        background_bind_construct(&source, p, geometry, bytes, b, bg, w, scratch, WINDOW_WIN7_GL);
    return status == SGX535_TASK_BLOCKED && !gl_background_same(r, &frozen)
               ? SGX535_TASK_UNSUPPORTED
               : status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_background_bind(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!gl_background_bind_arguments(r, p, geometry, bytes, b, bg, out, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = gl_background_bind_construct(r, p, geometry, bytes, b, bg, w, scratch);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_window_mesh_gl_window_background_bound_validate(
    const struct sgx535_window_mesh_gl_window_background_request *r,
    const struct sgx535_window_mesh_background_image *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_window_mesh_binding *b, const struct sgx535_background_binding *bg,
    const struct sgx535_window_mesh_background_bound *out, struct sgx535_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status;
    if (!gl_background_bind_arguments(r, p, geometry, bytes, b, bg, out, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = gl_background_bind_construct(r, p, geometry, bytes, b, bg, w, scratch);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
