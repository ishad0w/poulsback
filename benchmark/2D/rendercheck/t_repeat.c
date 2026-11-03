/*
 * Copyright © 2006 Eric Anholt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <anholt@FreeBSD.org>
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include "rendercheck.h"

#define TEST_WIDTH 40
#define TEST_HEIGHT 40

/* We choose some sizes larger than width/height because AAs like to turn
 * off repeating when it's unnecessary and we want to make sure that those paths
 * are sane.
 */
static const int sizes[] = {1, 2, 4, 8, 10, 16, 20, 32, 64, 100};

/* Sets up a repeating picture at various sizes, with the upper-left corner
 * filled with a different color than the rest.  It tiles this over the whole
 * destination, then samples the result to see if it tiled appropriately.  If
 * test_mask is set, the repeating picture is used as a component-alpha mask,
 * with argb32white as the source.
 */
bool
repeat_test(Display *dpy, picture_info *win, picture_info *dst, int op,
    picture_info *dst_color, picture_info *c1, picture_info *c2, bool test_mask)
{
	unsigned int wi, hi;

	for (wi = 0; wi < sizeof(sizes) / sizeof(int); wi++) {
	    int w = sizes[wi];
	    for (hi = 0; hi < sizeof(sizes) / sizeof(int); hi++) {
		picture_info src;
		int h = sizes[hi];
		int c2w = w / 2;
		int c2h = h / 2;
		int x, y, i;
		char name[40];
		color4d tdst, c1expected, c2expected;
		XRenderPictureAttributes pa;
		XRenderDirectFormat acc;
		XImage *image;
		bool failed = false;

		pa.component_alpha = test_mask;
		pa.repeat = true;

		src.d = XCreatePixmap(dpy, DefaultRootWindow(dpy), w, h, 32);
		src.format = XRenderFindStandardFormat(dpy, PictStandardARGB32);
		src.pict = XRenderCreatePicture(dpy, src.d, src.format,
		    CPComponentAlpha | CPRepeat, &pa);
		src.name = (char *)"repeat picture";

		/* Fill to the first color */
		XRenderComposite(dpy, PictOpSrc, c1->pict, None, src.pict,
				 0, 0, 0, 0, 0, 0, w, h);
		/* And set the upper-left to the second color */
		XRenderComposite(dpy, PictOpSrc, c2->pict, None, src.pict,
				 0, 0, 0, 0, 0, 0, c2w, c2h);

		for (i = 0; i < pixmap_move_iter; i++) {
			/* Fill to dst_color */
			XRenderComposite(dpy, PictOpSrc,
					 dst_color->pict, None, dst->pict,
					 0, 0, 0, 0, 0, 0,
					 TEST_WIDTH, TEST_HEIGHT);
			/* Composite the repeat picture in. */
			if (!test_mask) {
				XRenderComposite(dpy, ops[op].op,
						 src.pict, None, dst->pict,
						 0, 0, 0, 0, 0, 0,
						 TEST_WIDTH, TEST_HEIGHT);
			} else {
				/* Using PictOpSrc, color 0 (white), and
				 * component alpha, the mask color should be
				 * written to the destination.
				 */
				XRenderComposite(dpy, ops[op].op,
						 argb32white->pict, src.pict, dst->pict,
						 0, 0, 0, 0, 0, 0,
						 TEST_WIDTH, TEST_HEIGHT);
			}
		}

		copy_pict_to_win(dpy, dst, win, win_width, win_height);
		tdst = dst_color->color;
		color_correct(dst, &tdst);

		accuracy(&acc,
			 &dst->format->direct,
			 &dst_color->format->direct);
		accuracy(&acc, &acc, &c1->format->direct);
		accuracy(&acc, &acc, &c2->format->direct);

		if (!test_mask) {
			do_composite(ops[op].op, &c1->color, NULL, &tdst,
			    &c1expected, false);
			do_composite(ops[op].op, &c2->color, NULL, &tdst,
			    &c2expected, false);
		} else {
			do_composite(ops[op].op, &argb32white->color,
			    &c1->color, &tdst, &c1expected, true);
			do_composite(ops[op].op, &argb32white->color,
			    &c2->color, &tdst, &c2expected, true);
		}
		color_correct(dst, &c1expected);
		color_correct(dst, &c2expected);

		image = XGetImage(dpy, dst->d,
				  0, 0, TEST_WIDTH, TEST_HEIGHT,
				  ~0U, ZPixmap);

		for (y = 0; y < TEST_HEIGHT; y++) {
		    for (x = 0; x < TEST_WIDTH; x++) {
			int samplex = x % w;
			int sampley = y % h;
			color4d *expected, tested;

			if (samplex < c2w && sampley < c2h) {
				expected = &c2expected;
			} else {
				expected = &c1expected;
			}
			get_pixel_from_image(image, dst, x, y, &tested);

			if (eval_diff(&acc, expected, &tested) > 3.) {
			    snprintf(name, 40, "%dx%d %s %s-repeat", w, h,
				     ops[op].name, test_mask ? "mask" : "src");

			    print_fail(name, expected, &tested, x, y,
				       eval_diff(&acc, expected, &tested));

			    failed = true;
			    goto out;
			}
		    }
		}
out:
		XDestroyImage(image);
		XRenderFreePicture(dpy, src.pict);
		XFreePixmap(dpy, src.d);

		if (failed)
		    return false;
	    }
	}
	return true;
}
