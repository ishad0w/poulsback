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

#include <stdlib.h>

#include "rendercheck.h"

#define mult_chan(src, dst, Fa, Fb) \
	min((double)(src) * (double)(Fa) + (double)(dst) * (double)(Fb), 1.0)

static double
calc_op(int op, double src, double dst, double srca, double dsta)
{
	double Fa, Fb;

	switch (op) {
		case PictOpClear:
		case PictOpDisjointClear:
		case PictOpConjointClear:
			return mult_chan(src, dst, 0.0, 0.0);
		case PictOpSrc:
		case PictOpDisjointSrc:
		case PictOpConjointSrc:
			return mult_chan(src, dst, 1.0, 0.0);
		case PictOpDst:
		case PictOpDisjointDst:
		case PictOpConjointDst:
			return mult_chan(src, dst, 0.0, 1.0);
		case PictOpOver:
			return mult_chan(src, dst, 1.0, 1.0 - srca);
		case PictOpOverReverse:
			return mult_chan(src, dst, 1.0 - dsta, 1.0);
		case PictOpIn:
			return mult_chan(src, dst, dsta, 0.0);
		case PictOpInReverse:
			return mult_chan(src, dst, 0.0, srca);
		case PictOpOut:
			return mult_chan(src, dst, 1.0 - dsta, 0.0);
		case PictOpOutReverse:
			return mult_chan(src, dst, 0.0, 1.0 - srca);
		case PictOpAtop:
			return mult_chan(src, dst, dsta, 1.0 - srca);
		case PictOpAtopReverse:
			return mult_chan(src, dst, 1.0 - dsta,  srca);
		case PictOpXor:
			return mult_chan(src, dst, 1.0 - dsta, 1.0 - srca);
		case PictOpAdd:
			return mult_chan(src, dst, 1.0, 1.0);
			break;
		case PictOpSaturate:
		case PictOpDisjointOverReverse:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, (1.0 - dsta) / srca);
			return mult_chan(src, dst, Fa, 1.0);
		case PictOpDisjointOver:
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, (1.0 - srca) / dsta);
			return mult_chan(src, dst, 1.0, Fb);
		case PictOpDisjointIn:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - (1.0 - dsta) / srca);
			return mult_chan(src, dst, Fa, 0.0);
		case PictOpDisjointInReverse:
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - (1.0 - srca) / dsta);
			return mult_chan(src, dst, 0.0, Fb);
		case PictOpDisjointOut:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, (1.0 - dsta) / srca);
			return mult_chan(src, dst, Fa, 0.0);
		case PictOpDisjointOutReverse:
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, (1.0 - srca) / dsta);
			return mult_chan(src, dst, 0.0, Fb);
		case PictOpDisjointAtop:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - (1.0 - dsta) / srca);
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, (1.0 - srca) / dsta);
			return mult_chan(src, dst, Fa, Fb);
		case PictOpDisjointAtopReverse:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, (1.0 - dsta) / srca);
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - (1.0 - srca) / dsta);
			return mult_chan(src, dst, Fa, Fb);
		case PictOpDisjointXor:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, (1.0 - dsta) / srca);
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, (1.0 - srca) / dsta);
			return mult_chan(src, dst, Fa, Fb);
		case PictOpConjointOver:
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - srca / dsta);
			return mult_chan(src, dst, 1.0, Fb);
		case PictOpConjointOverReverse:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - dsta / srca);
			return mult_chan(src, dst, Fa, 1.0);
		case PictOpConjointIn:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, dsta / srca);
			return mult_chan(src, dst, Fa, 0.0);
		case PictOpConjointInReverse:
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, srca / dsta);
			return mult_chan(src, dst, 0.0, Fb);
		case PictOpConjointOut:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - dsta / srca);
			return mult_chan(src, dst, Fa, 0.0);
		case PictOpConjointOutReverse:
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - srca / dsta);
			return mult_chan(src, dst, 0.0, Fb);
		case PictOpConjointAtop:
			if (srca == 0.0)
				Fa = 1.0;
			else
				Fa = min(1.0, dsta / srca);
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - srca / dsta);
			return mult_chan(src, dst, Fa, Fb);
		case PictOpConjointAtopReverse:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - dsta / srca);
			if (dsta == 0.0)
				Fb = 1.0;
			else
				Fb = min(1.0, srca / dsta);
			return mult_chan(src, dst, Fa, Fb);
		case PictOpConjointXor:
			if (srca == 0.0)
				Fa = 0.0;
			else
				Fa = max(0.0, 1.0 - dsta / srca);
			if (dsta == 0.0)
				Fb = 0.0;
			else
				Fb = max(0.0, 1.0 - srca / dsta);
			return mult_chan(src, dst, Fa, Fb);
		default:
			abort();
	}
}

void
do_composite(int op,
	     const color4d *src,
	     const color4d *mask,
	     const color4d *dst,
	     color4d *result,
	     bool componentAlpha)
{
	color4d srcval, srcalpha;

	if (mask != NULL && componentAlpha) {
		srcval.r = src->r * mask->r;
		srcval.g = src->g * mask->g;
		srcval.b = src->b * mask->b;
		srcval.a = src->a * mask->a;
		srcalpha.r = src->a * mask->r;
		srcalpha.g = src->a * mask->g;
		srcalpha.b = src->a * mask->b;
		srcalpha.a = src->a * mask->a;
	} else if (mask != NULL) {
		srcval.r = src->r * mask->a;
		srcval.g = src->g * mask->a;
		srcval.b = src->b * mask->a;
		srcval.a = src->a * mask->a;
		srcalpha.r = src->a * mask->a;
		srcalpha.g = src->a * mask->a;
		srcalpha.b = src->a * mask->a;
		srcalpha.a = src->a * mask->a;
	} else {
		srcval = *src;
		srcalpha.r = src->a;
		srcalpha.g = src->a;
		srcalpha.b = src->a;
		srcalpha.a = src->a;
	}

	result->r = calc_op(op, srcval.r, dst->r, srcalpha.r, dst->a);
	result->g = calc_op(op, srcval.g, dst->g, srcalpha.g, dst->a);
	result->b = calc_op(op, srcval.b, dst->b, srcalpha.b, dst->a);
	result->a = calc_op(op, srcval.a, dst->a, srcalpha.a, dst->a);
}
