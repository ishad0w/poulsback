// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* Distinct typed frontend; the internal source copy retains version4/BG6.
 * No conversion to version1/BG5 or convention normalization is permitted. */
static struct sgx535_depth_window_mesh_request
depth_gl_source(const struct sgx535_depth_window_mesh_gl_window_request *r)
{
    struct sgx535_depth_window_mesh_request source = {
        .version = r->version,
        .format = r->format,
        .no_depth = r->no_depth,
        .reserved = r->reserved,
        .scene = {.version = r->scene.version,
                  .packed_background_argb = r->scene.packed_background_argb,
                  .reserved = {r->scene.reserved[0], r->scene.reserved[1]},
                  .foreground = r->scene.foreground}};
    return source;
}
static int depth_gl_arguments(const struct sgx535_depth_window_mesh_gl_window_request *r,
                              struct sgx535_depth_window_mesh_gl_window_request *f,
                              struct sgx535_depth_window_mesh_request *source, const void *image,
                              unsigned long image_bytes, const void *binding,
                              unsigned long binding_bytes, const void *out, unsigned long out_bytes,
                              const uint8_t *geometry, uint32_t bytes,
                              struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch,
                              uint32_t cap)
{
    if (!span(r, sizeof(*r)))
        return 0;
    *f = *r;
    *source = depth_gl_source(f);
    return arguments(r, sizeof(*r), source, image, image_bytes, binding, binding_bytes, out,
                     out_bytes, geometry, bytes, w, scratch, cap);
}
static int depth_gl_same(const struct sgx535_depth_window_mesh_gl_window_request *r,
                         const struct sgx535_depth_window_mesh_gl_window_request *f)
{
    struct sgx535_depth_window_mesh_request a = depth_gl_source(r), b = depth_gl_source(f);
    return same_request(&a, &b);
}
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_build(
    const struct sgx535_depth_window_mesh_gl_window_request *r,
    struct sgx535_depth_window_mesh_task *out, uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_gl_window_request frozen;
    struct sgx535_depth_window_mesh_request source;
    enum sgx535_task_status status;
    if (!depth_gl_arguments(r, &frozen, &source, 0, 0, 0, 0, out, sizeof(*out), geometry, bytes, w,
                            scratch, cap))
        return SGX535_TASK_INVALID;
    status = regenerate(&source, &gl_depth, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED && !depth_gl_same(r, &frozen))
        return SGX535_TASK_UNSUPPORTED;
    if (status == SGX535_TASK_BLOCKED) {
        *out = w->image;
        memcpy(geometry, scratch, out->seed.foreground.geometry.bytes);
    }
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_validate(
    const struct sgx535_depth_window_mesh_gl_window_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch, uint32_t cap)
{
    struct sgx535_depth_window_mesh_gl_window_request frozen;
    struct sgx535_depth_window_mesh_request source;
    enum sgx535_task_status status;
    if (!depth_gl_arguments(r, &frozen, &source, 0, 0, 0, 0, p, sizeof(*p), geometry, bytes, w,
                            scratch, cap))
        return SGX535_TASK_INVALID;
    status = regenerate(&source, &gl_depth, w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED && !depth_gl_same(r, &frozen))
        return SGX535_TASK_UNSUPPORTED;
    return status == SGX535_TASK_BLOCKED &&
                   (bytes != w->image.seed.foreground.geometry.bytes ||
                    memcmp(p, &w->image, sizeof(*p)) || memcmp(geometry, scratch, bytes))
               ? SGX535_TASK_INVALID
               : status;
}
static enum sgx535_task_status
depth_gl_bind_construct(const struct sgx535_depth_window_mesh_gl_window_request *r,
                        const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry,
                        uint32_t bytes, const struct sgx535_depth_window_mesh_binding *binding,
                        const void *out, unsigned long out_bytes,
                        struct sgx535_depth_window_mesh_workspace *w, uint8_t *scratch,
                        uint32_t cap)
{
    struct sgx535_depth_window_mesh_gl_window_request frozen;
    struct sgx535_depth_window_mesh_request source;
    enum sgx535_task_status status;
    if (!p || !binding ||
        !depth_gl_arguments(r, &frozen, &source, p, sizeof(*p), binding, sizeof(*binding), out,
                            out_bytes, geometry, bytes, w, scratch, cap))
        return SGX535_TASK_INVALID;
    status = bind_construct(&source, &gl_depth, p, geometry, bytes, binding, w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && !depth_gl_same(r, &frozen) ? SGX535_TASK_UNSUPPORTED
                                                                       : status;
}
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_bind(
    const struct sgx535_depth_window_mesh_gl_window_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *binding,
    struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status =
        depth_gl_bind_construct(r, p, geometry, bytes, binding, out, sizeof(*out), w, scratch, cap);
    if (status == SGX535_TASK_BLOCKED)
        *out = w->bound;
    return status;
}
enum sgx535_task_status sgx535_depth_window_mesh_gl_window_bound_validate(
    const struct sgx535_depth_window_mesh_gl_window_request *r,
    const struct sgx535_depth_window_mesh_task *p, const uint8_t *geometry, uint32_t bytes,
    const struct sgx535_depth_window_mesh_binding *binding,
    const struct sgx535_depth_window_mesh_bound *out, struct sgx535_depth_window_mesh_workspace *w,
    uint8_t *scratch, uint32_t cap)
{
    enum sgx535_task_status status =
        depth_gl_bind_construct(r, p, geometry, bytes, binding, out, sizeof(*out), w, scratch, cap);
    return status == SGX535_TASK_BLOCKED && memcmp(out, &w->bound, sizeof(*out))
               ? SGX535_TASK_INVALID
               : status;
}
