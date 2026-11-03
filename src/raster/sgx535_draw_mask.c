// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_draw_mask.h"

/* Pass type 3 (viewport), depth ALWAYS, no tag or depth write, ISPB and ISPC
 * present (EMGD's 0x07f00300); ISPB's UPASSCTL 15. */
#define MASK_ISPA UINT32_C(0x07f00300)
#define MASK_ISPB UINT32_C(0x000000f0)
#define MASK_SET (UINT32_C(1) << 25)

bool sgx535_scissor_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                         uint32_t target_height, bool bottom_up, struct sgx535_rect *rect)
{
    if (!rect || width < 0 || height < 0 || target_height > 4096u)
        return false;
    rect->x0 = x, rect->x1 = x + width;
    if (bottom_up)
        rect->y0 = y, rect->y1 = y + height;
    else
        rect->y0 = (int32_t)target_height - y - height, rect->y1 = (int32_t)target_height - y;
    return true;
}

static bool empty(const struct sgx535_rect *r)
{
    return r->x0 >= r->x1 || r->y0 >= r->y1;
}

static void object(struct sgx535_mask_object *o, const struct sgx535_rect *r, bool set)
{
    *o = (struct sgx535_mask_object){MASK_ISPA, MASK_ISPB, set ? MASK_SET : 0u, *r};
}

int sgx535_draw_mask_objects(const struct sgx535_rect *from, const struct sgx535_rect *to,
                             uint32_t width, uint32_t height, struct sgx535_mask_object out[2])
{
    const struct sgx535_rect whole = {0, 0, (int32_t)width, (int32_t)height};
    int n = 0;
    if (!out || !width || !height || width > 4096u || height > 4096u)
        return -1;
    from = from ? from : &whole;
    if (!to) {
        /* the whole target again: one set */
        if (from == &whole)
            return 0;
        object(&out[n++], &whole, true);
        return n;
    }
    if (from->x0 == to->x0 && from->y0 == to->y0 && from->x1 == to->x1 && from->y1 == to->y1)
        return 0;
    if (!empty(from))
        object(&out[n++], from, false);
    if (!empty(to))
        object(&out[n++], to, true);
    return n;
}
