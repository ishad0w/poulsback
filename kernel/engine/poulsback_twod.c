// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_twod.h"
#include "poulsback_vm_layout.h"

/* The UAPI's stated surface limits are the encoder's (HW-2D-002). */
_Static_assert(POULSBACK_2D_MAX_STRIDE ==
		       (SGX535_2D_RASTER_MAX_STRIDE & ~(POULSBACK_2D_STRIDE_ALIGN - 1U)),
	       "SUBMIT_2D's stride limit");

/* Surface i of the job as the encoder sees it: the BO's identity (its list
 * index: SUBMIT_2D refuses an object listed twice, and different objects never
 * share VAs) and its address relative to TWOD_REQ_BASE. */
static enum poulsback_twod_status surface(const struct poulsback_twod_bo *bos,
					  sgx535_u32 bo_count,
					  const struct poulsback_2d_surface *s,
					  sgx535_u64 base,
					  struct sgx535_2d_raster_surface *out)
{
	static const enum sgx535_2d_raster_format formats[] = {
		[POULSBACK_2D_ARGB8888] = SGX535_2D_RASTER_ARGB8888,
		[POULSBACK_2D_XRGB8888] = SGX535_2D_RASTER_XRGB8888,
		[POULSBACK_2D_RGB565] = SGX535_2D_RASTER_RGB565,
	};
	if (s->bo >= bo_count || s->reserved[0] || s->reserved[1] || !s->format ||
	    s->format > POULSBACK_2D_RGB565 || s->offset >= bos[s->bo].size)
		return POULSBACK_TWOD_INVALID;
	*out = (struct sgx535_2d_raster_surface){
		.storage_id = s->bo + 1ULL,
		.offset_bytes = bos[s->bo].va - base + s->offset,
		.allocation_bytes = bos[s->bo].size - s->offset,
		.width = s->width,
		.height = s->height,
		.stride_bytes = s->stride,
		.format = formats[s->format],
	};
	return POULSBACK_TWOD_OK;
}

static enum poulsback_twod_status status(enum sgx535_2d_raster_status s)
{
	if (s == SGX535_2D_RASTER_OK)
		return POULSBACK_TWOD_OK;
	return s == SGX535_2D_RASTER_UNSUPPORTED ? POULSBACK_TWOD_UNSUPPORTED
						 : POULSBACK_TWOD_INVALID;
}

static struct sgx535_2d_raster_rect rect(const struct poulsback_2d_rect *r)
{
	return (struct sgx535_2d_raster_rect){r->x, r->y, r->width, r->height};
}

/* Encodes one checked operation, limited to clip (NULL: unclipped), at the
 * end of the plan and counts its pixels against the job's cap. */
static enum poulsback_twod_status encode(const struct sgx535_2d_raster_surface *views,
					 const struct poulsback_2d_op *op,
					 const struct sgx535_2d_raster_rect *clip, sgx535_u32 *words,
					 sgx535_u32 capacity, struct poulsback_twod_plan *p,
					 sgx535_u64 *pixels)
{
	struct sgx535_2d_raster_rect dst = rect(&op->rect), src;
	enum poulsback_twod_status ret;
	sgx535_u32 size;
	size_t n;
	if (op->kind == POULSBACK_2D_FILL) {
		ret = status(sgx535_2d_raster_fill(&views[op->dst], &dst, clip, op->pixel,
						   (enum sgx535_2d_raster_rop)op->rop,
						   words + p->count, capacity - p->count, &n));
	} else {
		src = (struct sgx535_2d_raster_rect){op->src_x, op->src_y, op->rect.width,
						     op->rect.height};
		ret = status(sgx535_2d_raster_copy(&views[op->src], &src, &views[op->dst], &dst,
						   clip, (enum sgx535_2d_raster_rop)op->rop,
						   words + p->count, capacity - p->count, &n));
	}
	if (ret || !n)
		return ret;
	/* The command's size word precedes its FLUSH (x << 12 | y). */
	size = words[p->count + n - 2];
	*pixels += (sgx535_u64)(size >> 12 & 4095U) * (size & 4095U);
	if (*pixels > POULSBACK_2D_MAX_PIXELS)
		return POULSBACK_TWOD_INVALID;
	p->count += (sgx535_u32)n;
	p->expected += POULSBACK_TWOD_COUNT_PER_OP;
	return POULSBACK_TWOD_OK;
}

/* Keeps [*lo, *hi) inside [first, end). */
static void keep(int64_t *lo, int64_t *hi, int64_t first, int64_t end)
{
	if (first > *lo)
		*lo = first;
	if (end < *hi)
		*hi = end;
}

/* HW-2D-006: the 2D core writes a destination that starts off a 64-byte
 * boundary about half as fast (fills a quarter). An operation that keeps at
 * least POULSBACK_TWOD_SPLIT_BYTES of each row and POULSBACK_TWOD_SPLIT_ROWS
 * rows after clipping runs as two commands: a head up to the next boundary and
 * an aligned body (smaller ones lose more to the second command than they
 * gain). Rows share the start's phase: strides are multiples of 128. Inside
 * one surface, a copy toward larger x runs the body first, so that neither
 * command overwrites what the other still reads. */
static enum poulsback_twod_status encode_op(const struct sgx535_2d_raster_surface *views,
					    const struct poulsback_2d_op *op, sgx535_u32 *words,
					    sgx535_u32 capacity, struct poulsback_twod_plan *p,
					    sgx535_u64 *pixels)
{
	const struct sgx535_2d_raster_surface *d = &views[op->dst], *s = &views[op->src];
	const int copy = op->kind == POULSBACK_2D_COPY, clipped = op->flags & POULSBACK_2D_CLIP;
	const sgx535_u32 bpp = d->format == SGX535_2D_RASTER_RGB565 ? 2 : 4;
	int64_t lo = op->rect.x, hi = lo + op->rect.width, top = op->rect.y,
		bottom = top + op->rect.height;
	struct sgx535_2d_raster_rect clip = rect(&op->clip), parts[2];
	enum poulsback_twod_status ret;
	sgx535_u32 phase, head;
	int body_first;
	/* The destination rectangle the encoder's clipping keeps. */
	keep(&lo, &hi, 0, d->width);
	keep(&top, &bottom, 0, d->height);
	if (clipped) {
		keep(&lo, &hi, op->clip.x, (int64_t)op->clip.x + op->clip.width);
		keep(&top, &bottom, op->clip.y, (int64_t)op->clip.y + op->clip.height);
	}
	if (copy) {
		keep(&lo, &hi, (int64_t)op->rect.x - op->src_x,
		     (int64_t)op->rect.x - op->src_x + s->width);
		keep(&top, &bottom, (int64_t)op->rect.y - op->src_y,
		     (int64_t)op->rect.y - op->src_y + s->height);
	}
	phase = (sgx535_u32)(d->offset_bytes + (sgx535_u64)lo * bpp) & 63U;
	head = phase ? (64 - phase) / bpp : 0;
	if (!head || (hi - lo) * bpp < POULSBACK_TWOD_SPLIT_BYTES ||
	    bottom - top < POULSBACK_TWOD_SPLIT_ROWS)
		return encode(views, op, clipped ? &clip : NULL, words, capacity, p, pixels);
	/* The body keeps at least SPLIT_BYTES - 64 of each row. */
	parts[0] = (struct sgx535_2d_raster_rect){(int32_t)lo, (int32_t)top, head,
						  (sgx535_u32)(bottom - top)};
	parts[1] = (struct sgx535_2d_raster_rect){(int32_t)(lo + head), (int32_t)top,
						  (sgx535_u32)(hi - lo - head),
						  (sgx535_u32)(bottom - top)};
	body_first = copy && s->storage_id == d->storage_id && op->src_x < op->rect.x;
	ret = encode(views, op, &parts[body_first], words, capacity, p, pixels);
	if (!ret)
		ret = encode(views, op, &parts[!body_first], words, capacity, p, pixels);
	return ret;
}

enum poulsback_twod_status
poulsback_twod_plan(const struct poulsback_twod_bo *bos, sgx535_u32 bo_count,
		    const struct poulsback_2d_surface *surfaces, sgx535_u32 surface_count,
		    const struct poulsback_2d_op *ops, sgx535_u32 op_count, sgx535_u32 *words,
		    sgx535_u32 capacity, struct poulsback_twod_plan *plan)
{
	/* The scratch page as a surface of its own (BO surfaces are 1-16). */
	static const struct sgx535_2d_raster_surface scratch = {
		.storage_id = ~0ULL,
		.offset_bytes = POULSBACK_VM_2D_SCRATCH_VA - POULSBACK_VM_TWOD_REQ_BASE,
		.allocation_bytes = POULSBACK_VM_PAGE,
		.width = 1,
		.height = 1,
		.stride_bytes = 128,
		.format = SGX535_2D_RASTER_XRGB8888,
	};
	static const struct sgx535_2d_raster_rect pixel = {0, 0, 1, 1};
	struct sgx535_2d_raster_surface views[POULSBACK_2D_MAX_SURFACES];
	struct poulsback_twod_plan p = {0};
	sgx535_u64 base = POULSBACK_VM_TWOD_REQ_BASE, pixels = 0;
	enum poulsback_twod_status ret;
	sgx535_u32 i;
	size_t n;
	if (!bo_count || bo_count > POULSBACK_2D_MAX_BOS || !surface_count ||
	    surface_count > POULSBACK_2D_MAX_SURFACES || !op_count ||
	    op_count > POULSBACK_2D_MAX_OPS || capacity < POULSBACK_TWOD_WORDS(op_count))
		return POULSBACK_TWOD_INVALID;
	for (i = 0; i < bo_count; i++)
		if (!poulsback_vm_span_ok(POULSBACK_VM_2D, bos[i].va, bos[i].size))
			return POULSBACK_TWOD_INVALID;
	/* Every surface, used or not, meets the encoder's layout rules: an empty
	 * fill checks only its destination. */
	for (i = 0; i < surface_count; i++) {
		static const struct sgx535_2d_raster_rect empty = {0, 0, 0, 0};
		ret = surface(bos, bo_count, &surfaces[i], base, &views[i]);
		if (ret)
			return ret;
		ret = status(sgx535_2d_raster_fill(&views[i], &empty, NULL, 0,
						   SGX535_2D_RASTER_COPY, words, 0, &n));
		if (ret)
			return ret;
	}
	for (i = 0; i < op_count; i++) {
		const struct poulsback_2d_op *op = &ops[i];
		if ((op->flags & ~POULSBACK_2D_CLIP) || op->rop > SGX535_2D_RASTER_SET ||
		    op->dst >= surface_count)
			return POULSBACK_TWOD_INVALID;
		if (!(op->flags & POULSBACK_2D_CLIP) &&
		    (op->clip.x || op->clip.y || op->clip.width || op->clip.height))
			return POULSBACK_TWOD_INVALID;
		if (op->kind == POULSBACK_2D_FILL) {
			if (op->src || op->src_x || op->src_y)
				return POULSBACK_TWOD_INVALID;
		} else if (op->kind == POULSBACK_2D_COPY) {
			if (op->pixel || op->src >= surface_count)
				return POULSBACK_TWOD_INVALID;
		} else {
			return POULSBACK_TWOD_INVALID;
		}
		ret = encode_op(views, op, words, capacity, &p, &pixels);
		if (ret)
			return ret;
		p.writes |= 1U << surfaces[op->dst].bo;
	}
	if (p.count) {
		ret = status(sgx535_2d_raster_fill(&scratch, &pixel, NULL, 0, SGX535_2D_RASTER_COPY,
						   words + p.count, capacity - p.count, &n));
		if (ret || !n)
			return POULSBACK_TWOD_INVALID;
		p.count += (sgx535_u32)n;
		p.expected += POULSBACK_TWOD_COUNT_PER_OP;
	}
	*plan = p;
	return POULSBACK_TWOD_OK;
}
