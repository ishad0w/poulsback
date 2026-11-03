/*
 * Copyright © 2004 Eric Anholt
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

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#if HAVE_ERR_H
# include <err.h>
#else
# include <stdarg.h>
# include <stdlib.h>
__attribute__((format(printf, 2, 3)))
static inline void errx(int eval, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "Fatal Error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(eval);
}
#endif

#define min(a, b) (a < b ? a : b)
#define max(a, b) (a > b ? a : b)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef struct _color4d
{
	double r, g, b, a;
} color4d;

typedef struct _picture_info {
	Drawable d;
	Picture pict;
	XRenderPictFormat *format;
	char *name;		/* Possibly, some descriptive name. */
	color4d color;		/* If a 1x1R pict, the (corrected) color.*/
} picture_info;

struct op_info {
	int op;
	const char *name;
	bool disabled;
};

struct rendercheck_test_result {
	int tests;
	int passed;
};

static inline void
record_result(struct rendercheck_test_result *result, bool success)
{
	result->tests++;
	if (success)
		result->passed++;
}

#define TEST_FILL		0x0001
#define TEST_DSTCOORDS		0x0002
#define TEST_SRCCOORDS		0x0004
#define TEST_MASKCOORDS		0x0008
#define TEST_TSRCCOORDS		0x0010
#define TEST_TMASKCOORDS	0x0020
#define TEST_BLEND		0x0040
#define TEST_COMPOSITE		0x0080
#define TEST_CACOMPOSITE	0x0100
#define TEST_GRADIENTS  	0x0200
#define TEST_REPEAT	  	0x0400
#define TEST_TRIANGLES  	0x0800
#define TEST_BUG7366		0x1000
#define TEST_gtk_argb_xbgr	0x2000
#define TEST_libreoffice_xrgb	0x4000
#define TEST_shmblend		0x8000

struct rendercheck_test {
	int bit;
	const char *arg_name;
	const char *long_name;
	struct rendercheck_test_result (*func)(Display *dpy);
};

#ifdef __MACH__
#define SECTION(X) section("__DATA,__" X )
#define SECTION_START(X) __asm("section$start$__DATA$__" X)
#define SECTION_END(X) __asm("section$end$__DATA$__" X)
#else
#define SECTION(X) section(X)
#define SECTION_START(X)
#define SECTION_END(X)
#endif

#ifdef __clang__
#define ATTRIBUTE_NO_ASAN __attribute__((no_sanitize("address")))
#else
#define ATTRIBUTE_NO_ASAN /**/
#endif

#define DECLARE_RENDERCHECK_TEST(name)             \
	const struct rendercheck_test test_desc_##name \
	ATTRIBUTE_NO_ASAN \
	__attribute__ ((used, SECTION ("test_section")))

#define DECLARE_RENDERCHECK_ARG_TEST(arg_name_, long_name_, func_)		\
	DECLARE_RENDERCHECK_TEST(arg_name_) = {				\
		.bit = TEST_##arg_name_,				\
		.arg_name = #arg_name_,					\
		.long_name = long_name_,				\
		.func = func_,						\
	}

/* Storage that will point at the start and end of the ELF/MACH-O section for test
 * structs.  These are automatically set up by the linker when placing things
 * in their sections.
 */
extern struct rendercheck_test __start_test_section SECTION_START("test_section");
extern struct rendercheck_test __stop_test_section SECTION_END("test_section");

struct render_format {
	XRenderPictFormat *format;
	char *name;
};

extern struct render_format *formats;
extern int nformats;

extern int pixmap_move_iter;
extern int win_width, win_height;
extern struct op_info ops[];
extern bool is_verbose, minimalrendering;
extern color4d colors[];
extern int enabled_tests;
extern int format_whitelist_len;
extern char **format_whitelist;
extern picture_info *argb32white, *argb32red, *argb32green, *argb32blue;
extern int num_ops;
extern int num_colors;

/* main.c */
void
describe_format(char **desc, const char *prefix, XRenderPictFormat *format);

int
bit_count(int i);

void
print_tests(FILE *file, int tests);

/* tests.c */
void
color_correct(picture_info *pi, color4d *color);

void
get_pixel(Display *dpy,
	  const picture_info *pi,
	  int x, int y,
	  color4d *color);

void
get_pixel_from_image(XImage *image,
		     const picture_info *pi,
		     int x, int y,
		     color4d *color);

void
accuracy(XRenderDirectFormat *result,
	 const XRenderDirectFormat *a,
	 const XRenderDirectFormat *b);

double
eval_diff(const XRenderDirectFormat *accuracy,
	  const color4d *expected,
	  const color4d *test);

void print_fail(const char *name,
		const color4d *expected,
		const color4d *test,
		int x, int y,
		double d);

void print_pass(const char *name,
		const color4d *expected,
		int x, int y,
		double d);

void
argb_fill(Display *dpy, picture_info *p, int x, int y, int w, int h, float a,
    float r, float g, float b);

bool
do_tests(Display *dpy, picture_info *win);

void
copy_pict_to_win(Display *dpy, picture_info *pict, picture_info *win,
    int width, int height);

/* ops.c */
void
do_composite(int op,
	     const color4d *src,
	     const color4d *mask,
	     const color4d *dst,
	     color4d *result,
	     bool componentAlpha);

/* The tests */
bool
blend_test(Display *dpy, picture_info *win, picture_info *dst,
	   const int *op, int num_op,
	   const picture_info **src_color, int num_src,
	   const picture_info **dst_color, int num_dst);

bool
composite_test(Display *dpy, picture_info *win, picture_info *dst,
	       const int *op, int num_op,
	       const picture_info **src_color, int num_src,
	       const picture_info **mask_color, int num_mask,
	       const picture_info **dst_color, int num_dst,
	       bool componentAlpha);

bool
dstcoords_test(Display *dpy, picture_info *win, int op, picture_info *dst,
    picture_info *bg, picture_info *fg);

bool
fill_test(Display *dpy, picture_info *win, picture_info *src);

bool
srccoords_test(Display *dpy, picture_info *win, picture_info *white,
    bool test_mask);

bool
trans_coords_test(Display *dpy, picture_info *win, picture_info *white,
    bool test_mask);

bool
trans_srccoords_test_2(Display *dpy, picture_info *win, picture_info *white,
    bool test_mask);

bool render_to_gradient_test(Display *dpy, picture_info *src);

bool linear_gradient_test(Display *dpy, picture_info *win,
                          picture_info *dst, int op, picture_info *dst_color);

bool
repeat_test(Display *dpy, picture_info *win, picture_info *dst, int op,
    picture_info *dst_color, picture_info *c1, picture_info *c2,
    bool test_mask);

bool
triangles_test(Display *dpy, picture_info *win, picture_info *dst, int op,
    picture_info *src_color, picture_info *dst_color);

bool
tristrip_test(Display *dpy, picture_info *win, picture_info *dst, int op,
    picture_info *src_color, picture_info *dst_color);

bool
trifan_test(Display *dpy, picture_info *win, picture_info *dst, int op,
    picture_info *src_color, picture_info *dst_color);

bool
bug7366_test(Display *dpy);

#define for_each_test(test)						\
	for (struct rendercheck_test *test = &__start_test_section;	\
	     test < &__stop_test_section;				\
	     test++)
