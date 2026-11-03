/*
 * Copyright © 2006 Eric Anholt
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Eric Anholt not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Eric Anholt makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * ERIC ANHOLT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL ERIC ANHOLT BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rendercheck.h"

static int
expecting_error(Display *dpy, XErrorEvent *event)
{
    return true;
}

/**
 * Check SetPictureTransform on a source picture causing a crash.
 */
static bool
bug7366_test_set_picture_transform(Display *dpy)
{
    Picture source_pict;
    XRenderColor color;
    XTransform transform;

    memset(&color, 0, sizeof(color));
    source_pict = XRenderCreateSolidFill(dpy, &color);

    memset(&transform, 0, sizeof(transform));
    XRenderSetPictureTransform(dpy, source_pict, &transform);

    XSync(dpy, false);

    XRenderFreePicture(dpy, source_pict);

    return true;
}

/**
 * Check setting of AlphaMap to a source picture causing a crash.
 */
static bool
bug7366_test_set_alpha_map(Display *dpy)
{
    Picture source_pict, pict;
    XRenderColor color;
    Drawable pixmap;
    XRenderPictureAttributes pa;

    memset(&color, 0, sizeof(color));
    source_pict = XRenderCreateSolidFill(dpy, &color);

    pixmap = XCreatePixmap(dpy, DefaultRootWindow(dpy), 1, 1, 32);
    pict = XRenderCreatePicture(dpy, pixmap,
        XRenderFindStandardFormat(dpy, PictStandardARGB32), 0, NULL);

    XSetErrorHandler(expecting_error);
    pa.alpha_map = source_pict;
    XRenderChangePicture(dpy, pict, CPAlphaMap, &pa);
    XSync(dpy, false);
    XSetErrorHandler(NULL);

    XFreePixmap(dpy, pixmap);
    XRenderFreePicture(dpy, pict);
    XRenderFreePicture(dpy, source_pict);

    return true;
}

/**
 * Check SetPictureClipRectangles on a source potentially causing a crash.
 */
static bool
bug7366_test_set_picture_clip_rectangles(Display *dpy)
{
    Picture source_pict;
    XRenderColor color;
    XRectangle rectangle;

    memset(&color, 0, sizeof(color));
    source_pict = XRenderCreateSolidFill(dpy, &color);

    memset(&rectangle, 0, sizeof(rectangle));
    XSetErrorHandler(expecting_error);
    XRenderSetPictureClipRectangles(dpy, source_pict, 0, 0, &rectangle, 1);
    XSync(dpy, false);
    XSetErrorHandler(NULL);

    XRenderFreePicture(dpy, source_pict);

    return true;
}

/**
 * Check SetPictureFilter on a source potentially causing a crash.
 */
static bool
bug7366_test_set_picture_filter(Display *dpy)
{
    Picture source_pict;
    XRenderColor color;

    memset(&color, 0, sizeof(color));
    source_pict = XRenderCreateSolidFill(dpy, &color);

    XRenderSetPictureFilter(dpy, source_pict, "bilinear", NULL, 0);
    XSync(dpy, false);
    XSetErrorHandler(NULL);

    XRenderFreePicture(dpy, source_pict);

    return true;
}

bool
bug7366_test(Display *dpy)
{
    int maj, min;

    /* Make sure we actually have gradients available */
    XRenderQueryVersion(dpy, &maj, &min);
    if (maj != 0 || min < 10)
	return true;

    bug7366_test_set_picture_transform(dpy);
    bug7366_test_set_alpha_map(dpy);
    bug7366_test_set_picture_clip_rectangles(dpy);
    bug7366_test_set_picture_filter(dpy);

    /* If the server isn't gone, then we've succeeded. */
    return true;
}
