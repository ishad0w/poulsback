// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "sgx535_zls.h"
#include <string.h>

/* ZLSCTL: SLOADEN 1, ZLOADEN 2, FORCEZLOAD 5, LOADMASK 21 to load;
 * ZSTOREEN 4, FORCEZSTORE 6, SSTOREEN 7, STOREMASK 22 to store; ZLSEXTENT
 * 15:8; ZLOADFORMAT 17:16 and ZSTOREFORMAT 19:18 (D24S8 1, D16 2). */
#define ZLS_LOAD UINT32_C(0x00200026)
#define ZLS_STORE UINT32_C(0x004000d0)
#define BGOBJ_MASK (UINT32_C(1) << 8)
#define BGOBJ_ENABLEBGTAG (UINT32_C(1) << 9)

bool sgx535_zls_words(const struct sgx535_zls_request *r, struct sgx535_zls_words *w)
{
    uint32_t extent, format;
    if (!r || !w || !r->width || r->width > 4096u || r->format > SGX535_ZLS_D24S8 ||
        ((r->load || r->store) && r->format == SGX535_ZLS_NONE))
        return false;
    if (r->load || r->store) {
        format = r->format == SGX535_ZLS_D16 ? 2u : 1u;
        extent = ((r->width + 31u) & ~31u) / 16u - 1u;
        w->zlsctl = format << 16 | format << 18 | extent << 8 | (r->load ? ZLS_LOAD : 0u) |
                    (r->store ? ZLS_STORE : 0u);
    } else {
        w->zlsctl = ((r->width + 15u) / 16u - 1u) << 8;
    }
    memcpy(&w->bgobjdepth, &r->clear_depth, sizeof(w->bgobjdepth));
    w->bgobj = r->clear_stencil | BGOBJ_MASK | BGOBJ_ENABLEBGTAG;
    return true;
}
