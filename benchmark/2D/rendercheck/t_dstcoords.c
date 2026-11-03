/*
 * Copyright © 2005 Eric Anholt
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

#include "rendercheck.h"

#define TEST_WIDTH	10
#define TEST_HEIGHT	10

/* Test destination coordinates by drawing a 3x3 picture offset one pixel.
 *
 * Note: fg must have solid 1.0 alpha, but still have an alpha channel.
 * Otherwise, we're likely to hit a path that maps PictOpOver -> PictOpSrc,
 * for example.
 */
bool
dstcoords_test(Display *dpy, picture_info *win, int op, picture_info *dst,
    picture_info *bg, picture_info *fg)
{
	color4d expected, tested;
	XImage *image;
	int x, y, i;
	bool failed = false;

	for (i = 0; i < pixmap_move_iter; i++) {
		XRenderComposite(dpy, PictOpSrc, bg->pict, 0, dst->pict, 0, 0,
		    0, 0, 0, 0, TEST_WIDTH, TEST_HEIGHT);
		XRenderComposite(dpy, op, fg->pict, 0, dst->pict, 0, 0,
		    0, 0, 1, 1, 3, 3);
	}

	copy_pict_to_win(dpy, dst, win, TEST_WIDTH, TEST_HEIGHT);

	image = XGetImage(dpy, dst->d,
			  0, 0, 5, 5,
			  ~0U, ZPixmap);

	for (x = 0; x < 5; x++) {
		for (y = 0; y < 5; y++) {
			get_pixel_from_image(image, dst, x, y, &tested);
			if ((x >= 1 && x <= 3) && (y >= 1 && y <= 3))
				expected = fg->color;
			else
				expected = bg->color;

			color_correct(dst, &expected);
			if (eval_diff(&dst->format->direct, &expected, &tested) > 2.0) {
			    print_fail("dst coords",
				       &expected, &tested, x, y,
				       eval_diff(&dst->format->direct, &expected, &tested));
				failed = true;
			}
		}
	}

	XDestroyImage(image);

	return !failed;
}
