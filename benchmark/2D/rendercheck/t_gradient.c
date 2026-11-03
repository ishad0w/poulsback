/*
 * Copyright © 2006 Lars Knoll
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  The copyright holders makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <assert.h>
#include "rendercheck.h"

typedef struct _stop {
    double x;
    color4d color;
} stop;

static const stop stop_list[][10] = {
    {
        { 0., {0, 0, 1.0, 1.0} },
        { 1., {1.0, 0, 0, 1.0} },
        { -1, {0, 0, 0, 0} }
    },
    {
        { 0., {0, 0, 1.0, 1.0} },
        { .5, {0, 1.0, 0, 1.0} },
        { 1., {1.0, 0, 0, 1.0} },
        { -1, {0, 0, 0, 0} }
    },
    {
        { 0., {0, 0, 1.0, 0} },
        { 1., {1.0, 0, 0, 1.0} },
        { -1, {0, 0, 0, 0} }
    },
    {
        { 0., {0, 0, 1.0, 0} },
        { .5, {0, 1.0, 0, .75} },
        { 1., {1.0, 0, 0, .5} },
        { -1, {0, 0, 0, 0} }
    }
};
static const int n_stop_list = sizeof(stop_list)/(10*sizeof(stop));

typedef struct _point {
    double x;
    double y;
} point;

static const point linear_gradient_points[] = {
    { -5, -5 },  { 5, 5 },
    { 0, 0 },  { 10, 10 },
};

typedef struct _pixel {
    int x;
    int y;
} pixel;

static const pixel test_pixels [] = {
    {0, 0},
    {0, 5},
    {5, 0},
    {5, 5},
    {3, 7},
    {7, 3},
    {20, 20},
    {30, 13},
    {39, 39},
    {-1, -1}
};

static const int n_linear_gradient_points = sizeof(linear_gradient_points)/(2*sizeof(point));

static bool got_bad_drawable;

static int expecting_bad_drawable(Display *dpy, XErrorEvent *event)
{
	if (event->error_code == BadDrawable)
		got_bad_drawable = true;

	return true;
}


/* Tests that rendering to a linear gradient returns an error as expected.
 */
bool
render_to_gradient_test(Display *dpy, picture_info *src)
{
	XLinearGradient g;
	Picture gradient;
	XFixed stops[10];
	XRenderColor colors[10];
	const stop *stps = &stop_list[0][0];
	int i, p = 0;

	g.p1.x = XDoubleToFixed(linear_gradient_points[p].x);
	g.p1.y = XDoubleToFixed(linear_gradient_points[p].y);
	g.p2.x = XDoubleToFixed(linear_gradient_points[p+1].x);
	g.p2.y = XDoubleToFixed(linear_gradient_points[p+1].y);
	for (i = 0; i < 10; ++i) {
                if (stps[i].x < 0)
			break;
                stops[i] = XDoubleToFixed(stps[i].x);
                colors[i].red = stps[i].color.r*65535;
                colors[i].green = stps[i].color.g*65535;
                colors[i].blue = stps[i].color.b*65535;
                colors[i].alpha = stps[i].color.a*65535;
	}
	gradient = XRenderCreateLinearGradient(dpy, &g, stops, colors, i);

	/* Clear out any failing requests before our expected to fail ones. */
	XSync(dpy, false);

	got_bad_drawable = false;
	XSetErrorHandler(expecting_bad_drawable);

	/* Try a real compositing path */
	XRenderComposite(dpy, PictOpOver, src->pict, 0, gradient,
			 0, 0, 0, 0, 0, 0, win_width, win_height);
	XSync(dpy, false);
	if (!got_bad_drawable) {
		printf("render_to_gradient: Failed to get BadDrawable with "
		       "Over\n");
		return false;
	} else {
		got_bad_drawable = false;
	}

	/* Try the copy path to catch bad short-circuiting to 2d. */
	XRenderComposite(dpy, PictOpSrc, src->pict, 0, gradient,
			 0, 0, 0, 0, 0, 0, win_width, win_height);
	XSync(dpy, false);
	if (!got_bad_drawable) {
		printf("render_to_gradient: Failed to get BadDrawable with "
		       "Src\n");
		return false;
	} else {
		got_bad_drawable = false;
	}
	XSetErrorHandler(NULL);

	XRenderFreePicture(dpy, gradient);

	return true;
}

static void gradientPixel(const stop *stops, double pos, unsigned int spread, color4d *result)
{
    const int PRECISION = 1<<16;
    int ipos = (pos + 0.05) * PRECISION - 1;
    int i;
    double frac;

    /* calculate the actual offset. */
    if (ipos < 0 || ipos >= PRECISION) {
        if (spread == RepeatNormal) {
            ipos = ipos % PRECISION;
            ipos = ipos < 0 ? PRECISION + ipos : ipos;

        } else if (spread == RepeatReflect) {
            const int limit = PRECISION * 2 - 1;
            ipos = ipos % limit;
            ipos = ipos < 0 ? limit + ipos : ipos;
            ipos = ipos >= PRECISION ? limit - ipos : ipos;

        } else if (spread == RepeatPad) {
            if (ipos < 0)
                ipos = 0;
            else if (ipos >= PRECISION)
                ipos = PRECISION-1;
        } else { /* RepeatNone */
            result->r = 0;
            result->g = 0;
            result->b = 0;
            result->a = 0;
            return;
        }
    }

    assert(ipos >= 0);
    assert(ipos < PRECISION);

    pos = ipos/(double)PRECISION;

    if (pos <= stops[0].x) {
        *result = stops[0].color;
        return;
    }

    for (i = 0; i < 10; ++i) {
        if (stops[i].x >= pos)
            break;
        if (stops[i].x < 0) {
            *result = stops[i-1].color;
            result->r *= result->a;
            result->g *= result->a;
            result->b *= result->a;
            return;
        }
    }

    frac = (pos - stops[i-1].x)/(stops[i].x - stops[i-1].x);

    result->r = (1.-frac)*stops[i-1].color.r + frac*stops[i].color.r;
    result->g = (1.-frac)*stops[i-1].color.g + frac*stops[i].color.g;
    result->b = (1.-frac)*stops[i-1].color.b + frac*stops[i].color.b;
    result->a = (1.-frac)*stops[i-1].color.a + frac*stops[i].color.a;

    result->r *= result->a;
    result->g *= result->a;
    result->b *= result->a;

    return;
}

static void calculate_linear_gradient_color(int x, int y,
                                            const point *points,
                                            const stop *stops,
                                            color4d *tgradient, int repeat)
{
    double dx, dy, l, xrel, yrel, pos;
    dx = points[1].x - points[0].x;
    dy = points[1].y - points[0].y;
    l = dx*dx + dy*dy;

    xrel = x - points[0].x;
    yrel = y - points[0].y;

    pos = (dx*xrel + dy*yrel)/l;
    gradientPixel(stops, pos, repeat, tgradient);
}



bool linear_gradient_test(Display *dpy, picture_info *win,
                          picture_info *dst, int op, picture_info *dst_color)
{
    color4d expected, tested, tdst, tgradient;
    int i, s, p, repeat;
    Picture gradient;
    char testname[40];
    bool success = true;

    for (s = 0; s < n_stop_list; ++s) {
        for (p = 0; p < n_linear_gradient_points; p += 2) {
            XLinearGradient g;
            XFixed stops[10];
            XRenderColor colors[10];
            const stop *stps = &stop_list[s][0];
            g.p1.x = XDoubleToFixed(linear_gradient_points[p].x);
            g.p1.y = XDoubleToFixed(linear_gradient_points[p].y);
            g.p2.x = XDoubleToFixed(linear_gradient_points[p+1].x);
            g.p2.y = XDoubleToFixed(linear_gradient_points[p+1].y);
            for (i = 0; i < 10; ++i) {
                if (stps[i].x < 0)
                    break;
                stops[i] = XDoubleToFixed(stps[i].x);
                colors[i].red = stps[i].color.r*65535;
                colors[i].green = stps[i].color.g*65535;
                colors[i].blue = stps[i].color.b*65535;
                colors[i].alpha = stps[i].color.a*65535;
            }
            gradient = XRenderCreateLinearGradient(dpy, &g, stops, colors, i);

            for (repeat = 1; repeat < 4; ++repeat) {
                const pixel *pix;
                XRenderPictureAttributes pa;
		pa.repeat = repeat;
		XRenderChangePicture(dpy, gradient, CPRepeat, &pa);

		assert (dst_color->pict > 100);
                XRenderComposite(dpy, PictOpSrc, dst_color->pict, 0, dst->pict, 0, 0,
                                 0, 0, 0, 0, win_width, win_height);
                XRenderComposite(dpy, ops[op].op, gradient, 0,
                                 dst->pict, 0, 0, 0, 0, 0, 0, win_width, win_height);

		copy_pict_to_win(dpy, dst, win, win_width, win_height);

                pix = test_pixels;
                while (pix->x >= 0) {
                    XRenderDirectFormat acc;
                    get_pixel(dpy, dst, pix->x, pix->y, &tested);

                    calculate_linear_gradient_color(pix->x, pix->y, &linear_gradient_points[p],
                                                    stps, &tgradient, repeat);

                    tdst = dst_color->color;
                    color_correct(dst, &tdst);
                    do_composite(ops[op].op, &tgradient, NULL, &tdst,
                                 &expected, False);
                    color_correct(dst, &expected);

                    accuracy(&acc, &dst->format->direct, &dst_color->format->direct);

                    if (eval_diff(&acc, &expected, &tested) > 3.) {
			snprintf(testname, 40,
				 "%s linear gradient", ops[op].name);
			print_fail(testname, &expected, &tested, 0, 0,
				   eval_diff(&dst->format->direct, &expected, &tested));
                        printf("gradient: %d stops: %d repeat: %d pos: %d/%d\n"
                               "src color: %.2f %.2f %.2f %.2f\n"
                               "dst color: %.2f %.2f %.2f %.2f\n",
                               p/2, s,
                               repeat, pix->x, pix->y,
                               tgradient.r, tgradient.g,
                               tgradient.b, tgradient.a,
                               dst_color->color.r, dst_color->color.g,
                               dst_color->color.b, dst_color->color.a);
                        success = false;
                    } else if (is_verbose) {
                        printf("src: %d/%d, dst: %s\n", s, p, dst->name);
                    }
                    ++pix;
                }
            }
            XRenderFreePicture(dpy, gradient);
        }
    }
    return success;
}


