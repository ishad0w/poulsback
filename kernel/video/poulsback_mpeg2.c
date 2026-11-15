// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include "poulsback_mpeg2.h"
#include "poulsback_mpeg2_vlc.h"

int poulsback_mpeg2_validate(const struct poulsback_mpeg2_decode *p,
			   const struct poulsback_mpeg2_slice *s, const u8 *data)
{
	u32 i, end = 0;
	u32 rows;
	if (p->abi != POULSBACK_VIDEO_ABI || (p->flags & ~POULSBACK_MPEG2_PROGRESSIVE_SEQUENCE) || p->reserved[0] ||
	    p->reserved[1] || p->reserved[2] || !p->output ||
	    !p->width || !p->height || ((p->width | p->height) & 1) ||
	    p->width > POULSBACK_MPEG2_MAX_DIMENSION ||
	    p->height > POULSBACK_MPEG2_MAX_DIMENSION ||
	    p->coding_type < 1 || p->coding_type > 3 || p->f_code >> 16 ||
	    p->coding_extension & ~0x1fffU ||
	    (p->coding_extension & 0x100c) != 0x100c ||
	    !p->slice_count || p->slice_count > POULSBACK_MPEG2_MAX_SLICES ||
	    !p->bitstream_bytes || p->bitstream_bytes > POULSBACK_MPEG2_MAX_BITSTREAM)
		return -EINVAL;
	if ((p->pitch != 512 && p->pitch != 1024 && p->pitch != 1280 && p->pitch != 2048) ||
	    p->pitch < ALIGN(p->width, 16))
		return -EINVAL;
	if ((p->coding_type == 1 && (p->forward_reference || p->backward_reference)) ||
	    (p->coding_type >= 2 && !p->forward_reference) ||
	    (p->coding_type == 2 && p->backward_reference) ||
	    (p->coding_type == 3 && !p->backward_reference) ||
	    p->output == p->forward_reference || p->output == p->backward_reference)
		return -EINVAL;
	for (i = 0; i < (p->coding_type == 3 ? 4 : p->coding_type == 2 ? 2 : 0); i++) {
		u32 code = (p->f_code >> (12 - 4 * i)) & 15;
		if (!code || code > 9)
			return -EINVAL;
	}
	for (i = 0; i < 64; i++)
		if (!p->intra_matrix[i] || !p->non_intra_matrix[i])
			return -EINVAL;
	rows = ALIGN(p->height, p->flags & POULSBACK_MPEG2_PROGRESSIVE_SEQUENCE ? 16 : 32) / 16;
	for (i = 0; i < p->slice_count; i++) {
		const struct poulsback_mpeg2_slice *v = &s[i];
		u32 bit = 37;
		if (v->reserved || v->bytes < 5 || v->bytes > 29U * 65535U || v->offset < end ||
		    v->offset > p->bitstream_bytes || v->bytes > p->bitstream_bytes - v->offset ||
		    v->vertical_position >= rows ||
		    v->vertical_position >= 128 ||
		    v->macroblock_offset < 38 || (u64)v->macroblock_offset >= (u64)v->bytes * 8 ||
		    v->quantiser_scale_code < 1 || v->quantiser_scale_code > 31)
			return -EINVAL;
		if (get_unaligned_be32(data + v->offset) != 0x100 + v->vertical_position + 1 ||
		    data[v->offset + 4] >> 3 != v->quantiser_scale_code)
			return -EINVAL;
		while (bit < v->bytes * 8 &&
		       (data[v->offset + bit / 8] & (128U >> (bit % 8))))
			bit += 9;
		if (++bit != v->macroblock_offset || bit >= v->bytes * 8)
			return -EINVAL;
		end = v->offset + v->bytes;
	}
	return 0;
}

u32 poulsback_mpeg2_mode(const struct poulsback_mpeg2_decode *p)
{
	u32 stride = p->pitch == 512 ? 4 : p->pitch == 1024 ? 5 : p->pitch == 1280 ? 2 : 6;
	return stride << 24 | 0x00133000;
}

struct command {
	u32 *words, count, dma_count, va;
};

static void word(struct command *c, u32 value)
{
	c->words[c->count++] = cpu_to_le32(value);
}

static void reg(struct command *c, u32 address, u32 value)
{
	word(c, 0x04800000 | address);
	word(c, value);
}

static void rendec(struct command *c, u32 address, const u32 *values, u32 count)
{
	u32 i;
	word(c, 0x20000000 | (count + 4));
	word(c, 0x0001000b);
	word(c, 0);
	word(c, 3 | (address << 2) | ((2 * count - 1) << 16));
	for (i = 0; i < count; i++)
		word(c, values[i]);
	word(c, 7);
}

/* PSB's DMA count uses peripheral units; its next pointer uses 16-byte units. */
static u32 dma(struct command *c, u32 source, u32 bytes, u32 profile)
{
	u32 first = c->va + PB_VIDEO_DMA_OFFSET + c->dma_count * 32;
	u32 width = profile == 0 ? 2 : profile == 1 ? 1 : 4;
	u32 dest = profile == 0 ? 0x3000 : profile == 1 ? 0x820 : 0x350;
	u32 mode = profile == 0 ? 1 : profile == 1 ? 2 : 0;
	u32 burst = profile == 0 ? 2 : profile == 1 ? 4 : 1;
	while (bytes) {
		u32 count = min(bytes / width, 65535U);
		u32 *d = c->words + PB_VIDEO_DMA_OFFSET / 4 + c->dma_count++ * 8;
		bool last = count * width == bytes;
		d[0] = cpu_to_le32(mode << 28);
		d[1] = cpu_to_le32(count | (profile == 0 ? 0x10000 : 0x40000) |
				    (last ? 0x80000000 : 0));
		d[2] = cpu_to_le32(dest);
		d[3] = cpu_to_le32(burst << 26);
		d[4] = d[5] = 0;
		d[6] = cpu_to_le32(source);
		d[7] = last ? 0 : cpu_to_le32((c->va + PB_VIDEO_DMA_OFFSET + c->dma_count * 32) >> 4);
		source += count * width;
		bytes -= count * width;
	}
	return first;
}

void poulsback_mpeg2_vlc(void *page)
{
	u32 i;
	BUILD_BUG_ON(sizeof(poulsback_mpeg2_vlc_words) != PB_VIDEO_VLC_BYTES);
	for (i = 0; i < ARRAY_SIZE(poulsback_mpeg2_vlc_words); i++)
		put_unaligned_le16(poulsback_mpeg2_vlc_words[i], page + PB_VIDEO_VLC_OFFSET + i * 2);
}

u32 poulsback_mpeg2_commands(const struct poulsback_mpeg2_decode *p,
			   const struct poulsback_mpeg2_slice *s, u32 previous_row,
			   const u32 surfaces[3], u32 command_va, u32 bitstream_va,
			   void *page, u32 *render_dma)
{
	struct command c = { .words = page, .va = command_va };
	u32 x = p->coding_extension, values[32], i, size, fe, be;
	u32 rows = ALIGN(p->height, p->flags & POULSBACK_MPEG2_PROGRESSIVE_SEQUENCE ? 16 : 32) / 16;
	u32 columns = DIV_ROUND_UP(p->width, 16);
	u32 uv = p->pitch * ALIGN(p->height, 32);
	u32 first = previous_row != s->vertical_position;
	memset(page, 0, PB_VIDEO_VLC_OFFSET);
	word(&c, 0x10000006);
	reg(&c, 0x8ec, 0x6000); reg(&c, 0x8f0, 0x3f017);
	reg(&c, 0x8f4, 0xab0e5); reg(&c, 0x8f8, 0x16e);
	reg(&c, 0x92c, 0x16c6ed); reg(&c, 0x93c, 0x2805);
	word(&c, 0xc0000000 | (dma(&c, command_va + PB_VIDEO_VLC_OFFSET, PB_VIDEO_VLC_BYTES, 0) >> 4));
	word(&c, 0xb0000000); word(&c, s->macroblock_offset); word(&c, s->bytes);
	word(&c, 0xa0000000 | (dma(&c, bitstream_va + s->offset, s->bytes, 1) >> 4));
	size = ((rows * 16 - 1) << 12) | (columns * 16 - 1);
	values[0] = values[1] = size;
	values[2] = poulsback_mpeg2_mode(p);
	values[3] = surfaces[0]; values[4] = surfaces[0] + uv;
	rendec(&c, 0x1000, values, 5);
	memset(values, 0, 4 * sizeof(u32));
	if (p->coding_type >= 2) {
		u32 recent = surfaces[p->coding_type == 3 ? 2 : 1];
		values[0] = recent; values[1] = recent + uv;
	}
	if (p->coding_type == 3) {
		values[2] = surfaces[1]; values[3] = surfaces[1] + uv;
	}
	rendec(&c, 0x1100, values, 4);
	fe = ((x >> 9 & 1) << 9) | ((x >> 7 & 1) << 8) | ((x & 3) << 6) |
	     ((x >> 4 & 1) << 5) | ((x >> 6 & 1) << 4) | ((x >> 5 & 1) << 3) |
	     6 | (x >> 8 & 1);
	word(&c, 0x10000003);
	reg(&c, 0xe0c, fe); reg(&c, 0xe10, p->coding_type << 16 | p->f_code);
	reg(&c, 0xe14, first << 28 | s->vertical_position << 21 | s->quantiser_scale_code << 16);
	be = ((x >> 5 & 1) << 9) | ((x >> 7 & 1) << 8) | ((x & 3) << 6) | ((x >> 9 & 1) << 3);
	values[0] = columns - 1; values[1] = rows - 1;
	values[2] = be; values[3] = p->f_code;
	values[4] = p->coding_type << 4 | ((x >> 4 & 1) << 3) | ((x >> 6 & 1) << 2) | 3;
	values[5] = first << 13 | s->vertical_position << 5 | s->quantiser_scale_code;
	rendec(&c, 0xe50, values, 6);
	values[0] = 8 | (p->coding_type - 1);
	rendec(&c, 0x1400, values, 1);
	for (i = 0; i < 16; i++) {
		values[i] = get_unaligned_le32(p->non_intra_matrix + 4 * i);
		values[i + 16] = get_unaligned_le32(p->intra_matrix + 4 * i);
	}
	rendec(&c, 0x700, values, 32);
	word(&c, 0x10000001); reg(&c, 0x80c, 11);
	values[0] = 11; rendec(&c, 0x810, values, 1);
	word(&c, 0x60000000);
	size = c.count * 4;
	*render_dma = dma(&c, command_va, size, 4);
	return size;
}
