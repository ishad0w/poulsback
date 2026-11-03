/*
 * Copyright © 2016 Broadcom
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "rendercheck.h"
#include <X11/extensions/XShm.h>

bool had_x_error;
static int (*orig_x_error_handler)(Display *, XErrorEvent *);

static int
shmerrorhandler(Display *d, XErrorEvent *e)
{
	had_x_error = true;
	if (e->error_code == BadAccess) {
		fprintf(stderr,"failed to attach shared memory\n");
		return 0;
	} else {
		return (*orig_x_error_handler)(d,e);
	}
}

static XShmSegmentInfo *
get_x_shm_info(Display *dpy, size_t size)
{
	XShmSegmentInfo *shm_info = calloc(1, sizeof(*shm_info));

	shm_info->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT|0777);
	if (shm_info->shmid < 0) {
		free(shm_info);
		return NULL;
	}

	shm_info->shmaddr = shmat(shm_info->shmid, NULL, 0);
	if (shm_info->shmaddr == (void *)-1) {
		free(shm_info);
		return NULL;
	}

	shm_info->readOnly = false;

	XSync(dpy, true);
	had_x_error = false;
	orig_x_error_handler = XSetErrorHandler(shmerrorhandler);
	XShmAttach(dpy, shm_info);
	XSync(dpy, true);
	XSetErrorHandler(orig_x_error_handler);

	return shm_info;
}

static void
fill_shm_with_formatted_color(Display *dpy, XShmSegmentInfo *shm_info,
			      XRenderPictFormat *format,
			      int w, int h, color4d *color)
{
	XImage *image;
	XRenderDirectFormat *layout = &format->direct;
	unsigned long r = layout->redMask * color->r;
	unsigned long g = layout->greenMask * color->g;
	unsigned long b = layout->blueMask * color->b;
	unsigned long a = layout->alphaMask * color->a;
	unsigned long pix;

	r <<= layout->red;
	g <<= layout->green;
	b <<= layout->blue;
	a <<= layout->alpha;

	pix = r | g | b | a;

	image = XShmCreateImage(dpy, NULL, format->depth, ZPixmap,
				shm_info->shmaddr, shm_info, w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			XPutPixel(image, x, y, pix);
		}
	}
	XDestroyImage(image);
}

static bool
test_format(Display *dpy, int op, int w, int h, struct render_format *format)
{
	XRenderPictFormat *argb32_format;
	XShmSegmentInfo *shm_info;
	Pixmap src_pix = 0, dst_pix;
	bool pass = true;
	size_t size = w * h * 4;
	color4d dst_color = {.25, .25, .25, .25};
	picture_info src, dst;

	shm_info = get_x_shm_info(dpy, size);
	if (!shm_info) {
		pass = false;
		goto fail;
	}

	/* Create the SHM pixmap for the source. */
	src_pix = XShmCreatePixmap(dpy, DefaultRootWindow(dpy),
				   shm_info->shmaddr, shm_info, w, h,
				   format->format->depth);

	src.pict = XRenderCreatePicture(dpy, src_pix, format->format, 0, NULL);
	src.format = format->format;

	/* Make a plain a8r8g8b8 picture for the dst. */
	argb32_format = XRenderFindStandardFormat(dpy, PictStandardARGB32);
	dst_pix = XCreatePixmap(dpy, DefaultRootWindow(dpy), w, h,
				argb32_format->depth);
	dst.pict = XRenderCreatePicture(dpy, dst_pix, argb32_format, 0, NULL);
	dst.format = XRenderFindStandardFormat(dpy, PictStandardARGB32);

	for (int i = 0; i < num_colors; i++) {
		color4d src_color = colors[i];
		color4d expected;
		XImage *image;
		XRenderDirectFormat acc;

		fill_shm_with_formatted_color(dpy, shm_info, format->format,
					      w, h, &colors[i]);

		argb_fill(dpy, &dst, 0, 0, w, h,
			  dst_color.a, dst_color.r,
			  dst_color.g, dst_color.b);

		XRenderComposite(dpy, op,
				 src.pict, 0, dst.pict,
				 0, 0,
				 0, 0,
				 0, 0,
				 w, h);

		image = XGetImage(dpy, dst_pix,
				  0, 0, w, h,
				  0xffffffff, ZPixmap);

		color_correct(&src, &src_color);

		accuracy(&acc,
			 &argb32_format->direct,
			 &format->format->direct);

		do_composite(op, &src_color, NULL, &dst_color, &expected, false);

		for (int j = 0; j < w * h; j++) {
			int x = j % w;
			int y = j / h;
			color4d tested;

			get_pixel_from_image(image, &dst, x, y, &tested);

			if (eval_diff(&acc, &expected, &tested) > 3.) {
				char testname[30];

				pass = false;

				snprintf(testname, ARRAY_SIZE(testname),
					 "%s %s SHM blend", ops[op].name,
					format->name);

				print_fail(testname, &expected, &tested, x, y,
					   eval_diff(&acc, &expected, &tested));
				printf("src color: %.2f %.2f %.2f %.2f\n",
				       src_color.r,
				       src_color.g,
				       src_color.b,
				       src_color.a);
				printf("dst color: %.2f %.2f %.2f %.2f\n",
				       dst_color.r,
				       dst_color.g,
				       dst_color.b,
				       dst_color.a);

				break;
			}
		}

		XDestroyImage(image);
	}

	XRenderFreePicture(dpy, src.pict);
	XRenderFreePicture(dpy, dst.pict);
	XFreePixmap(dpy, dst_pix);
fail:
	if (shm_info) {
		XFreePixmap(dpy, src_pix);
		XShmDetach(dpy, shm_info);
		/* Wait for server to fully detach before removing. */
		XSync(dpy, False);
		shmdt(shm_info->shmaddr);
		shmctl(shm_info->shmid, IPC_RMID, NULL);
		free(shm_info);
	}

	return pass;
}

static struct rendercheck_test_result
test_shmblend(Display *dpy)
{
	struct rendercheck_test_result result = {};
	int major, minor, i;
	Bool pixmaps_supported;

	if (!XShmQueryExtension(dpy) ||
	    !XShmQueryVersion(dpy, &major, &minor, &pixmaps_supported) ||
	    !pixmaps_supported) {
		printf("SHM blend test: skipped\n");
		record_result(&result, true);
		return result;
	}

	for (i = 0; i < nformats; i++) {
		struct render_format *format = &formats[i];

		printf("Beginning SHM blend test from %s\n", format->name);

		record_result(&result, test_format(dpy, PictOpSrc, 8, 8,
						   format));
		record_result(&result, test_format(dpy, PictOpOver, 8, 8,
						   format));
	}

	return result;
}

DECLARE_RENDERCHECK_ARG_TEST(shmblend, "SHM Pixmap blending",
			     test_shmblend);
