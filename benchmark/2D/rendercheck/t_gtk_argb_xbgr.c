/* Copyright © 2014 Keith Packard <keithp@keithp.com>
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

/* Test Render's ability to flip bytes around when the source and mask are the
 * same pixmap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include "rendercheck.h"

#define WIDTH	1
#define HEIGHT	1

#define PIXEL_ABGR	0xff886644
#define PIXEL_RGB	0x446688

static struct rendercheck_test_result
test_gtk_argb_xbgr(Display *dpy)
{
	int x, y;
	Pixmap	pix_32;
	Pixmap	pix_24;
	Picture	pic_24;
	Picture	pic_32_xbgr;
	Picture	pic_32_argb;
	XRenderPictFormat	templ;
	XRenderPictFormat	*pic_xbgr_format;
	XRenderPictFormat	*pic_argb_format;
	XRenderPictFormat	*pic_rgb_format;
	GC	gc_32;
	XImage	*image_24, *image_32;
	struct rendercheck_test_result result = {};

	templ.type = PictTypeDirect;
	templ.depth = 32;
	templ.direct.alphaMask = 0;
	templ.direct.red = 0;
	templ.direct.green = 8;
	templ.direct.blue = 16;

	pic_xbgr_format = XRenderFindFormat(dpy,
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

	if (!pic_argb_format || !pic_xbgr_format || !pic_rgb_format) {
		printf("Couldn't find xBGR and ARGB formats\n");
		record_result(&result, false);
		return result;
	}

	pix_32 = XCreatePixmap(dpy, RootWindow(dpy, DefaultScreen(dpy)),
	    WIDTH, HEIGHT, 32);

	pic_32_xbgr = XRenderCreatePicture(dpy, pix_32, pic_xbgr_format, 0,
	    NULL);
	pic_32_argb = XRenderCreatePicture(dpy, pix_32, pic_argb_format, 0,
	    NULL);

	image_32 = XCreateImage(dpy,
	    NULL,
	    32,
	    ZPixmap,
	    0,
	    NULL,
	    WIDTH, HEIGHT, 32, 0);

	image_32->data = malloc(HEIGHT * image_32->bytes_per_line);

	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			XPutPixel(image_32, x, y, PIXEL_ABGR);

	gc_32 = XCreateGC(dpy, pix_32, 0, NULL);

	XPutImage(dpy, pix_32, gc_32, image_32, 0, 0, 0, 0, WIDTH, HEIGHT);

	pix_24 = XCreatePixmap(dpy, RootWindow(dpy, DefaultScreen(dpy)),
	    WIDTH, HEIGHT, 24);

	pic_24 = XRenderCreatePicture(dpy, pix_24, pic_rgb_format, 0, NULL);

	XRenderComposite(dpy, PictOpOver, pic_32_xbgr, pic_32_argb, pic_24,
	    0, 0, 0, 0, 0, 0, WIDTH, HEIGHT);

	image_24 = XGetImage(dpy, pix_24, 0, 0, WIDTH, HEIGHT, 0xffffffff,
	    ZPixmap);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned long pixel = XGetPixel(image_24, x, y);
			if (pixel != PIXEL_RGB) {
				printf("fail: pixel value is %08lx "
				    "should be %08x\n",
				    pixel, PIXEL_RGB);
				record_result(&result, false);
				return result;
			}
		}
	}

	free(image_32->data);
	XFree(image_32);
	XFree(image_24->data);
	XFree(image_24);
	XFreeGC(dpy, gc_32);

	record_result(&result, true);
	return result;
}

DECLARE_RENDERCHECK_ARG_TEST(gtk_argb_xbgr, "GTK xRGB/ABGR same picture",
			     test_gtk_argb_xbgr);
