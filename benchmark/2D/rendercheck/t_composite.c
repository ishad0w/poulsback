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

/* Test a composite of a given operation, source, mask, and destination picture.
 * Fills the window, and samples from the 0,0 pixel corner.
 */
bool
composite_test(Display *dpy, picture_info *win, picture_info *dst,
	       const int *op, int num_op,
	       const picture_info **src_color, int num_src,
	       const picture_info **mask_color, int num_mask,
	       const picture_info **dst_color, int num_dst,
	       bool componentAlpha)
{
	color4d expected, tested, tdst, tmsk;
	char testname[40];
	int i, s, m, d, iter;
	int page, num_pages;

	/* If the window is smaller than the number of sources to test,
	 * we need to break the sources up into pages.
	 *
	 * We however assume that the window will always be wider than num_ops.
	 */
	num_pages = num_src / win_height + 1;

	for (d = 0; d < num_dst; d++) {
	    tdst = dst_color[d]->color;
	    color_correct(dst, &tdst);

	    for (m = 0; m < num_mask; m++) {
		XRenderDirectFormat mask_acc;
		int this_src, rem_src;
		XImage *image;

		if (componentAlpha) {
		    XRenderPictureAttributes pa;

		    pa.component_alpha = true;
		    XRenderChangePicture(dpy, mask_color[m]->pict,
					 CPComponentAlpha, &pa);
		}

		rem_src = num_src;
		for (page = 0; page < num_pages; page++) {
		    this_src = rem_src / (num_pages - page);
		    for (iter = 0; iter < pixmap_move_iter; iter++) {
			XRenderComposite(dpy, PictOpSrc,
					 dst_color[d]->pict, 0, dst->pict,
					 0, 0,
					 0, 0,
					 0, 0,
					 num_op, this_src);
			for (s = 0; s < this_src; s++) {
			    for (i = 0; i < num_op; i++)
				XRenderComposite(dpy, ops[op[i]].op,
						 src_color[s]->pict,
						 mask_color[m]->pict,
						 dst->pict,
						 0, 0,
						 0, 0,
						 i, s,
						 1, 1);
			}
		    }

		    if (componentAlpha) {
			XRenderPictureAttributes pa;

			pa.component_alpha = false;
			XRenderChangePicture(dpy, mask_color[m]->pict,
					     CPComponentAlpha, &pa);
		    }

		    image = XGetImage(dpy, dst->d,
				      0, 0, num_op, this_src,
				      0xffffffff, ZPixmap);
		    copy_pict_to_win(dpy, dst, win, win_width, win_height);

		    if (componentAlpha &&
			mask_color[m]->format->direct.redMask == 0) {
			/* Ax component-alpha masks expand alpha into
			 * all color channels.
			 * XXX: This should be located somewhere generic.
			 */
			tmsk.a = mask_color[m]->color.a;
			tmsk.r = mask_color[m]->color.a;
			tmsk.g = mask_color[m]->color.a;
			tmsk.b = mask_color[m]->color.a;
		    } else
			tmsk = mask_color[m]->color;

		    accuracy(&mask_acc,
			     &mask_color[m]->format->direct,
			     &dst_color[d]->format->direct);
		    accuracy(&mask_acc, &mask_acc, &dst->format->direct);

		    for (s = 0; s < this_src; s++) {
			XRenderDirectFormat acc;

			accuracy(&acc, &mask_acc, &src_color[s]->format->direct);

			for (i = 0; i < num_op; i++) {
			    get_pixel_from_image(image, dst, i, s, &tested);

			    do_composite(ops[op[i]].op,
					 &src_color[s]->color, &tmsk, &tdst,
					 &expected, componentAlpha);
			    color_correct(dst, &expected);

			    if (eval_diff(&acc, &expected, &tested) > 3.) {
				snprintf(testname, 40,
					 "%s %scomposite", ops[op[i]].name,
					 componentAlpha ? "CA " : "");
				print_fail(testname, &expected, &tested, 0, 0,
					   eval_diff(&acc, &expected, &tested));
				printf("src color: %.2f %.2f %.2f %.2f\n"
				       "msk color: %.2f %.2f %.2f %.2f\n"
				       "dst color: %.2f %.2f %.2f %.2f\n",
				       src_color[s]->color.r,
				       src_color[s]->color.g,
				       src_color[s]->color.b,
				       src_color[s]->color.a,
				       mask_color[m]->color.r,
				       mask_color[m]->color.g,
				       mask_color[m]->color.b,
				       mask_color[m]->color.a,
				       dst_color[d]->color.r,
				       dst_color[d]->color.g,
				       dst_color[d]->color.b,
				       dst_color[d]->color.a);
				printf("src: %s, mask: %s, dst: %s\n",
				       src_color[s]->name,
				       mask_color[m]->name,
				       dst->name);
				XDestroyImage(image);
				return false;
			    }
			}
		    }
		    XDestroyImage(image);
		    rem_src -= this_src;
		}
	    }
	}

	return true;
}
