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
#include <string.h>

#include "rendercheck.h"

/* Test that filling of the 1x1 repeating pictures worked as expected.  This is
 * pretty basic to most of the tests.
 */
bool
fill_test(Display *dpy, picture_info *win, picture_info *src)
{
	color4d tested;
	char *name;

	get_pixel(dpy, src, 0, 0, &tested);
	copy_pict_to_win(dpy, src, win, win_width, win_height);

	if (eval_diff(&src->format->direct, &src->color, &tested) > 2.) {
	    describe_format(&name, "fill ", src->format);
	    print_fail(name, &src->color, &tested, 0, 0,
		       eval_diff(&src->format->direct, &src->color, &tested));
	    free(name);
	    return false;
	}

	return true;
}
