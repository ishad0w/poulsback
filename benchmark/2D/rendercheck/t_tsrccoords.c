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

#define TEST_WIDTH 40
#define TEST_HEIGHT 40

static int dot_colors[5][5] = {
	{7, 7, 7, 7, 7},
	{7, 7, 7, 7, 7},
	{7, 0, 7, 7, 7},
	{7, 7, 7, 7, 7},
	{7, 7, 7, 7, 7},
};

static picture_info *create_dot_picture(Display *dpy)
{
	picture_info *p;
	int i;

	p = malloc(sizeof(picture_info));

	p->d = XCreatePixmap(dpy, DefaultRootWindow(dpy), 5, 5, 32);
	p->format = XRenderFindStandardFormat(dpy, PictStandardARGB32);
	p->pict = XRenderCreatePicture(dpy, p->d, p->format, 0, NULL);
	p->name = (char *)"dot picture";

	for (i = 0; i < 25; i++) {
		int x = i % 5;
		int y = i / 5;
		color4d *c = &colors[dot_colors[y][x]];

		argb_fill(dpy, p, x, y, 1, 1, c->a, c->r, c->g, c->b);
	}

	return p;
}

static void destroy_dot_picture(Display *dpy, picture_info *p)
{
	XRenderFreePicture(dpy, p->pict);
	XFreePixmap(dpy, p->d);
	free(p);
}

static void init_transform (XTransform *t)
{
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			t->matrix[i][j] = XDoubleToFixed((i == j) ? 1 : 0);
}

/* Test drawing a 5x5 source image scaled 8x, as either a source or mask.
 */
bool
trans_coords_test(Display *dpy, picture_info *win, picture_info *white,
    bool test_mask)
{
	color4d tested;
	bool failed = false;
	int tested_colors[TEST_HEIGHT][TEST_WIDTH], expected_colors[TEST_HEIGHT][TEST_WIDTH];
	XTransform t;
	picture_info *src;
	XImage *image;
	int x, y;

	/* Skip this test when using indexed picture formats because
	 * indexed color comparisons are not implemented in rendercheck
	 * yet.
	 */
	if (win->format->type == PictTypeIndexed) {
		return true;
	}

	src = create_dot_picture(dpy);
	if (src == NULL) {
		fprintf(stderr, "couldn't allocate picture for test\n");
		return false;
	}

	init_transform(&t);
	t.matrix[2][2] = XDoubleToFixed(8);

	XRenderSetPictureTransform(dpy, src->pict, &t);

	if (!test_mask)
		XRenderComposite(dpy, PictOpSrc, src->pict, 0,
		    win->pict, 0, 0, 0, 0, 0, 0, TEST_WIDTH, TEST_HEIGHT);
	else {
		XRenderComposite(dpy, PictOpSrc, white->pict, src->pict,
		    win->pict, 0, 0, 0, 0, 0, 0, TEST_WIDTH, TEST_HEIGHT);
	}

	image = XGetImage(dpy, win->d,
			  0, 0, TEST_WIDTH, TEST_HEIGHT,
			  0xffffffff, ZPixmap);

	for (y = 0; y < TEST_HEIGHT; y++) {
		for (x = 0; x < TEST_WIDTH; x++) {
		int src_sample_x, src_sample_y;

		src_sample_x = x / 8;
		src_sample_y = y / 8;
		expected_colors[y][x] = dot_colors[src_sample_y][src_sample_x];

		get_pixel_from_image(image, win, x, y, &tested);

		if (tested.r == 1.0 && tested.g == 1.0 && tested.b == 1.0) {
			tested_colors[y][x] = 0;
		} else if (tested.r == 0.0 && tested.g == 0.0 &&
		    tested.b == 0.0) {
			tested_colors[y][x] = 7;
		} else {
			tested_colors[y][x] = 9;
		}
		if (tested_colors[y][x] != expected_colors[y][x])
			failed = true;
	    }
	}

	if (failed) {
		printf("%s transform coordinates test failed.\n",
		    test_mask ? "mask" : "src");
		printf("expected vs tested:\n");
		for (y = 0; y < TEST_HEIGHT; y++) {
			for (x = 0; x < TEST_WIDTH; x++)
				printf("%d", expected_colors[y][x]);
			printf(" ");
			for (x = 0; x < TEST_WIDTH; x++)
				printf("%d", tested_colors[y][x]);
			printf("\n");
		}
		printf(" vs tested (same)\n");
		for (y = 0; y < TEST_HEIGHT; y++) {
			for (x = 0; x < TEST_WIDTH; x++)
				printf("%d", tested_colors[y][x]);
			printf("\n");
		}
	}
	XDestroyImage(image);

	init_transform(&t);

	XRenderSetPictureTransform(dpy, src->pict, &t);

	destroy_dot_picture(dpy, src);

	return !failed;
}
