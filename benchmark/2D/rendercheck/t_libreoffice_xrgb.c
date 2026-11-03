/* Copyright © 2014 Keith Packard <keithp@keithp.com>
 * Copyright © 2014 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file t_libreoffice_xrgb.c
 *
 * Test Render's XRGB behavior when the pixmap is initialized with PutImage.
 * Catches a bug in glamor where the Render acceleration was relying on the
 * high bits of the pixmap having been set to 0xff, which is certainly not
 * guaranteed.  The rest of rendercheck didn't catch the bug, because it uses
 * XRenderFillRectangle() to initialize the common XRGB pictures, and
 * apparently that was setting the unused bits to 1 at the time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "rendercheck.h"

#define WIDTH	1
#define HEIGHT	1

#define PIXEL_XRGB		0x11886644
#define PIXEL_ARGB		0xff886644
#define INVERT_PIXEL_ARGB	0xff7799bb

static bool
libreoffice_xrgb_test_one(Display *dpy, bool invert)
{
	int x, y;
	Pixmap	src_pix, dst_pix;
	Picture	src_pict, dst_pict;
	XRenderPictFormat templ;
	XRenderPictFormat *pic_xrgb_format;
	XRenderPictFormat *pic_argb_format;
	XRenderPictFormat *pic_rgb_format;
	GC gc;
	XImage *image;

	templ.type = PictTypeDirect;
	templ.depth = 32;
	templ.direct.alphaMask = 0;
	templ.direct.red = 16;
	templ.direct.green = 8;
	templ.direct.blue = 0;

	pic_xrgb_format = XRenderFindFormat(dpy,
	    PictFormatType |
	    PictFormatDepth |
	    PictFormatAlphaMask |
	    PictFormatRed |
	    PictFormatGreen |
	    PictFormatBlue,
	    &templ, 0);

	templ.type = PictTypeDirect;
	templ.depth = 32;
	templ.direct.alpha = 24;
	templ.direct.red = 16;
	templ.direct.green = 8;
	templ.direct.blue = 0;

	pic_argb_format = XRenderFindFormat(dpy,
	    PictFormatType |
	    PictFormatDepth |
	    PictFormatAlpha |
	    PictFormatRed |
	    PictFormatGreen |
	    PictFormatBlue,
	    &templ, 0);

	templ.type = PictTypeDirect;
	templ.depth = 24;
	templ.direct.red = 16;
	templ.direct.green = 8;
	templ.direct.blue = 0;

	pic_rgb_format = XRenderFindFormat(dpy,
	    PictFormatType |
	    PictFormatDepth |
	    PictFormatRed |
	    PictFormatGreen |
	    PictFormatBlue,
	    &templ, 0);

	if (!pic_argb_format || !pic_xrgb_format || !pic_rgb_format) {
		printf("Couldn't find xRGB and ARGB formats\n");
		return false;
	}

	src_pix = XCreatePixmap(dpy, RootWindow(dpy, DefaultScreen(dpy)),
	    WIDTH, HEIGHT, 32);

	dst_pix = XCreatePixmap(dpy, RootWindow(dpy, DefaultScreen(dpy)),
	    WIDTH, HEIGHT, 32);

	src_pict = XRenderCreatePicture(dpy, src_pix, pic_xrgb_format, 0,
	    NULL);
	dst_pict = XRenderCreatePicture(dpy, dst_pix, pic_argb_format, 0,
	    NULL);

	image = XCreateImage(dpy,
	    NULL,
	    32,
	    ZPixmap,
	    0,
	    NULL,
	    WIDTH, HEIGHT, 32, 0);

	image->data = malloc(HEIGHT * image->bytes_per_line);

	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			XPutPixel(image, x, y, PIXEL_XRGB);

	gc = XCreateGC(dpy, src_pix, 0, NULL);

	XPutImage(dpy, src_pix, gc, image, 0, 0, 0, 0, WIDTH, HEIGHT);

	if (invert) {
		XSetFunction(dpy, gc, GXinvert);

		XFillRectangle(dpy, src_pix, gc, 0, 0, WIDTH, HEIGHT);
	}

	XDestroyImage(image);

	XRenderComposite(dpy, PictOpOver, src_pict, None, dst_pict,
	    0, 0, 0, 0, 0, 0, WIDTH, HEIGHT);

	image = XGetImage(dpy, dst_pix, 0, 0, WIDTH, HEIGHT, 0xffffffff,
			  ZPixmap);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned long expected = (invert ? INVERT_PIXEL_ARGB :
						  PIXEL_ARGB);
			unsigned long pixel = XGetPixel(image, x, y);

			if (pixel != expected) {
				printf("fail: pixel value is %08lx, "
				       "should be %08lx\n",
				       pixel, expected);
				return false;
			}
		}
	}
	XDestroyImage(image);
	XFreeGC(dpy, gc);

	return true;
}

static struct rendercheck_test_result
test_libreoffice_xrgb(Display *dpy)
{
	struct rendercheck_test_result result = { };

	record_result(&result, libreoffice_xrgb_test_one(dpy, false));
	record_result(&result, libreoffice_xrgb_test_one(dpy, true));

	return result;
}

DECLARE_RENDERCHECK_ARG_TEST(libreoffice_xrgb, "LibreOffice xRGB",
			     test_libreoffice_xrgb);
