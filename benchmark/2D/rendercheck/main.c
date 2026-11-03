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

#include "rendercheck.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include "version.h"

bool is_verbose = false, minimalrendering = false;
int enabled_tests = ~0;		/* Enable all tests by default */

int format_whitelist_len = 0;
char **format_whitelist = NULL;

/* Number of times to repeat operations so that pixmaps will tend to get moved
 * into offscreen memory and allow hardware acceleration.
 */
int pixmap_move_iter = 1;

int win_width = 40;
int win_height = 200;

int
bit_count(int i)
{
	int count;

	count = (i >> 1) & 033333333333;
	count = i - count - ((count >> 1) & 033333333333);
	count = (((count + (count >> 3)) & 030707070707) % 077);
	/* HAKMEM 169 */
	return count;
}

/* This is not complete, but decent enough for now.*/
void
describe_format(char **desc, const char *prefix, XRenderPictFormat *format)
{
	char ad[4] = "", rd[4] = "", gd[4] = "", bd[4] = "";
	int ac, rc, gc, bc;
	int ashift;

	if (!prefix)
	    prefix = "";

	ac = bit_count(format->direct.alphaMask);
	rc = bit_count(format->direct.redMask);
	gc = bit_count(format->direct.greenMask);
	bc = bit_count(format->direct.blueMask);

	if (ac != 0) {
		snprintf(ad, 4, "a%d", ac);
		ashift = format->direct.alpha;
	} else if (rc + bc + gc < format->depth) {
		/* There are bits that are not part of A,R,G,B. Mark them with
		 * an x.
		 */
		snprintf(ad, 4, "x%d", format->depth - rc - gc - bc);
		if (format->direct.red == 0 || format->direct.blue == 0)
			ashift = format->depth;
		else
			ashift = 0;
	} else
		ashift = 0;

	if (rc  != 0)
		snprintf(rd, 4, "r%d", rc);
	if (gc  != 0)
		snprintf(gd, 4, "g%d", gc);
	if (bc  != 0)
		snprintf(bd, 4, "b%d", bc);

	if (ashift > format->direct.red) {
		if (format->direct.red > format->direct.blue)
			asprintf(desc, "%s%s%s%s%s", prefix, ad, rd, gd, bd);
		else
			asprintf(desc, "%s%s%s%s%s", prefix, ad, bd, gd, rd);
	} else {
		if (format->direct.red > format->direct.blue)
			asprintf(desc, "%s%s%s%s%s", prefix, rd, gd, bd, ad);
		else
			asprintf(desc, "%s%s%s%s%s", prefix, bd, gd, rd, ad);
	}
}

struct {
    int flag;
    const char *name;
} available_tests[] = {
    {TEST_FILL, "fill"},
    {TEST_DSTCOORDS, "dcoords"},
    {TEST_SRCCOORDS, "scoords"},
    {TEST_MASKCOORDS, "mcoords"},
    {TEST_TSRCCOORDS, "tscoords"},
    {TEST_TMASKCOORDS, "tmcoords"},
    {TEST_BLEND, "blend"},
    {TEST_COMPOSITE, "composite"},
    {TEST_CACOMPOSITE, "cacomposite"},
    {TEST_GRADIENTS, "gradients"},
    {TEST_REPEAT, "repeat"},
    {TEST_TRIANGLES, "triangles"},
    {TEST_BUG7366, "bug7366"},
    {0, NULL}
};

static void
print_test_separator(int i)
{
        if (i % 5 == 0) {
            if(i != 0)
                putc('\n', stderr);
            putc('\t', stderr);
        } else {
            fprintf(stderr, ", ");
        }
}

void print_tests(FILE *file, int tests) {
    int i, j;
    
    for (i=0, j=0; available_tests[i].name; i++) {
        if (!(available_tests[i].flag & tests))
            continue;
	print_test_separator(j++);
        fprintf(stderr, "%s", available_tests[i].name);
        j++;
    }
    for_each_test(test) {
	if (!(test->bit & tests))
	    continue;
	print_test_separator(j++);
        fprintf(stderr, "%s", test->arg_name);
        j++;
    }
    if (j)
        fprintf(file, "\n");
}

_X_NORETURN
static void
usage (char *program)
{
    fprintf(stderr, "usage: %s [-d|--display display] [-v|--verbose]\n"
	"\t[-t test1,test2,...] [-o op1,op2,...] [-f format1,format2,...]\n"
	"\t[--sync] [--minimalrendering] [--version]\n"
	"Available tests:\n", program);
    print_tests(stderr, ~0);
    exit(1);
}

int main(int argc, char **argv)
{
	Display *dpy;
	XEvent ev;
	int i, o, maj, min, ret = 1;
	static int is_sync = false, print_version = false;
	static int longopt_minimalrendering = 0;
	XWindowAttributes a;
	XSetWindowAttributes as;
	picture_info window;
	char *display = NULL;
	char *test_name, *format, *opname, *nextname;

	static struct option longopts[] = {
		{ "display",	required_argument,	NULL,	'd' },
		{ "iterations",	required_argument,	NULL,	'i' },
		{ "formats",	required_argument,	NULL,	'f' },
		{ "tests",	required_argument,	NULL,	't' },
		{ "ops",	required_argument,	NULL,	'o' },
		{ "verbose",	no_argument,		NULL,	'v' },
		{ "sync",	no_argument,		&is_sync, true},
		{ "minimalrendering", no_argument,
		  &longopt_minimalrendering, true},
		{ "version",	no_argument,		&print_version, true },
		{ NULL,		0,			NULL,	0 }
	};

	while ((o = getopt_long(argc, argv, "d:i:f:t:o:v", longopts, NULL)) != -1) {
		switch (o) {
		case 'd':
			display = optarg;
			break;
		case 'i':
			pixmap_move_iter = atoi(optarg);
			break;
		case 'o':
			for (i = 0; i < num_ops; i++)
				ops[i].disabled = true;

			nextname = optarg;
			while ((opname = strsep(&nextname, ",")) != NULL) {
				for (i = 0; i < num_ops; i++) {
					if (strcasecmp(ops[i].name, opname) !=
					    0)
						continue;
					ops[i].disabled = false;
					break;
				}
				if (i == num_ops)
					usage(argv[0]);
			}
			break;
		case 'f':
			nextname = optarg;
			for (format_whitelist_len = 0;;format_whitelist_len++)
			{
				if ((format = strsep(&nextname, ",")) == NULL)
					break;
			}

			format_whitelist = malloc(sizeof(char *) *
			    format_whitelist_len);
			if (format_whitelist == NULL)
				errx(1, "malloc");

			/* Now the list is separated by \0s, so use strlen to
			 * step between entries.
			 */
			format = optarg;
			for (i = 0; i < format_whitelist_len; i++) {
				format_whitelist[i] = strdup(format);
				format += strlen(format) + 1;
			}

			break;
		case 't':
			nextname = optarg;

			/* disable all tests */
			enabled_tests = 0;

			while ((test_name = strsep(&nextname, ",")) != NULL) {
				int i;
				bool found = false;
				for(i=0; available_tests[i].name; i++) {
					if(strcmp(test_name, available_tests[i].name) == 0) {
						enabled_tests |= available_tests[i].flag;
						found = true;
						break;
					}
				}
				for_each_test(test) {
					if (strcmp(test_name,
						   test->arg_name) == 0) {
						enabled_tests |= test->bit;
						found = true;
						break;
					}
				}
				if (!found)
					usage(argv[0]);
			}

			break;
		case 'v':
			is_verbose = true;
			break;
		case 0:
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	minimalrendering = longopt_minimalrendering;

	/* Print the version string.  Bail out if --version was requested and
	 * continue otherwise.
	 */
	printf("rendercheck %s\n", VERSION);
	if (print_version)
		return 0;

	dpy = XOpenDisplay(display);
	if (dpy == NULL)
		errx(1, "Couldn't open display.");
	if (is_sync)
		XSynchronize(dpy, 1);

	if (!XRenderQueryExtension(dpy, &i, &i))
		errx(1, "Render extension missing.");

	XRenderQueryVersion(dpy, &maj, &min);
	if (maj != 0 || min < 1)
		errx(1, "Render extension version too low (%d.%d).", maj, min);

	printf("Render extension version %d.%d\n", maj, min);

	/* Conjoint/Disjoint were added in version 0.2, so disable those ops if
	 * the server doesn't support them.
	 */
	if (min < 2) {
		printf("Server doesn't support conjoint/disjoint ops, disabling.\n");
		num_ops = PictOpSaturate;
	}

	window.d = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0,
	    win_width, win_height, 0, 0, WhitePixel(dpy, 0));

	as.override_redirect = True;
	XChangeWindowAttributes(dpy, window.d, CWOverrideRedirect, &as);

	XGetWindowAttributes(dpy, window.d, &a);
	window.format = XRenderFindVisualFormat(dpy, a.visual);
	window.pict = XRenderCreatePicture(dpy, window.d,
	    window.format, 0, NULL);
	describe_format(&format, NULL, window.format);
	printf("Window format: %s\n", format);
	asprintf(&window.name, "%s window", format);
	free(format);
	XSelectInput(dpy, window.d, ExposureMask);
	XMapWindow(dpy, window.d);

	/* We have to premultiply the alpha into the r, g, b values of the
	 * sample colors.  Render colors are premultiplied with alpha, so r,g,b
	 * can never be greater than alpha.
	 */
	for (i = 0; i < num_colors; i++) {
		colors[i].r *= colors[i].a;
		colors[i].g *= colors[i].a;
		colors[i].b *= colors[i].a;
	}

	while (XNextEvent(dpy, &ev) == 0) {
		if (ev.type == Expose && !ev.xexpose.count) {
			if (do_tests(dpy, &window))
				ret = 0;
			else
				ret = 1;
			break;
		}
	}

	free(window.name);
	for (i = 0; i < format_whitelist_len; i++)
		free(format_whitelist[i]);
	free(format_whitelist);

        XCloseDisplay(dpy);
	return ret;
}
