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
#include <stdlib.h>

#include "rendercheck.h"

/* Originally a bullseye, this pattern has been modified so that flipping
 * will result in errors as well.
 */
static int target_colors[5][5] = {
	{1, 1, 1, 1, 1},
	{1, 0, 0, 0, 1},
	{1, 1, 1, 0, 1},
	{1, 0, 0, 1, 1},
	{1, 1, 1, 1, 1},
};

static picture_info *create_target_picture(Display *dpy)
{
	picture_info *p;
	int i;

	p = malloc(sizeof(picture_info));

	p->d = XCreatePixmap(dpy, DefaultRootWindow(dpy), 5, 5, 32);
	p->format = XRenderFindStandardFormat(dpy, PictStandardARGB32);
	p->pict = XRenderCreatePicture(dpy, p->d, p->format, 0, NULL);
	p->name = (char *)"target picture";

	for (i = 0; i < 25; i++) {
		int x = i % 5;
		int y = i / 5;
		color4d *c = &colors[target_colors[x][y]];

		argb_fill(dpy, p, x, y, 1, 1, c->a, c->r, c->g, c->b);
	}

	return p;	
}

static void destroy_target_picture(Display *dpy, picture_info *p)
{
	XRenderFreePicture(dpy, p->pict);
	XFreePixmap(dpy, p->d);
	free(p);
}

/* Test source or mask coordinates by drawing from a 5x5 picture into the 0,0
 * pixel.
 * XXX: Something besides PictOpSrc should also be used, at least in the
 * !test_mask case, to avoid getting CopyArea acceleration (easy to implement)
 * rather than a more general Composite implementation.
 */
bool
srccoords_test(Display *dpy, picture_info *win, picture_info *white,
    bool test_mask)
{
	color4d expected, tested;
	int i;
	XRenderPictureAttributes pa;
	bool failed = false;
	int tested_colors[5][5];
	picture_info *src;

	src = create_target_picture(dpy);
	if (src == NULL) {
		fprintf(stderr, "couldn't allocate picture for test\n");
		return false;
	}

	for (i = 0; i < 25; i++) {
		int x = i % 5, y = i / 5;

		if (!test_mask)
			XRenderComposite(dpy, PictOpSrc, src->pict, 0,
			    win->pict, x, y, 0, 0, 0, 0, 1, 1);
		else {
			/* Using PictOpSrc, color 0 (white), and component
			 * alpha, the mask color should be written to the
			 * destination.
			 */
			pa.component_alpha = true;
			XRenderChangePicture(dpy, src->pict, CPComponentAlpha,
			    &pa);
			XRenderComposite(dpy, PictOpSrc, white->pict, src->pict,
			    win->pict, 0, 0, x, y, 0, 0, 1, 1);
			pa.component_alpha = false;
			XRenderChangePicture(dpy, src->pict, CPComponentAlpha,
			    &pa);
		}
		get_pixel(dpy, win, 0, 0, &tested);

		expected = colors[target_colors[x][y]];
		color_correct(win, &expected);
		if (tested.r == 1.0) {
			if (tested.g == 1.0 && tested.b == 1.0)
				tested_colors[x][y] = 0;
			else if (tested.g == 0.0 && tested.b == 0.0)
				tested_colors[x][y] = 1;
			else tested_colors[x][y] = 9;
		} else
			tested_colors[x][y] = 9;

		if (eval_diff(&win->format->direct, &expected, &tested) > 2.) {
		    print_fail(test_mask ? "mask coords" : "src coords",
			       &expected, &tested, x, y,
			       eval_diff(&win->format->direct, &expected, &tested));
		    failed = true;
		}
	}
	if (failed) {
		int j;

		printf("expected vs tested:\n");
		for (i = 0; i < 5; i++) {
			for (j = 0; j < 5; j++)
				printf("%d", target_colors[j][i]);
			printf(" ");
			for (j = 0; j < 5; j++)
				printf("%d", tested_colors[j][i]);
			printf("\n");
		}
	}

	destroy_target_picture(dpy, src);

	return !failed;
}
