// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_video.h"
#include "poulsback_mpeg2.h"
#include "poulsback_bo.h"
#include "poulsback_file.h"
#include "poulsback_vm.h"
#include "poulsback_vm_layout.h"
#include "psb_drv.h"
#include "sgx535_source_epoch.h"
#include "poulsback_errsite.h"
#include <asm/cacheflush.h>
#include <asm/unaligned.h>
#include <drm/drm_gem.h>
#include <linux/delay.h>
#include <linux/dma-resv.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define VIDEO_FIRMWARE "poulsback/msvdx-fw-0.30.bin"
#define V_ENABLE 0x0000
#define V_KICK 0x0088
#define V_REG_DATA 0x00f8
#define V_REG_REQUEST 0x00fc
#define V_RAM_DATA 0x0104
#define V_RAM_CONTROL 0x0108
#define V_RAM_STATUS 0x010c
#define V_MTX_RESET 0x0200
#define V_CONTROL 0x0600
#define V_IRQ_STATUS 0x0608
#define V_IRQ_CLEAR 0x060c
#define V_IRQ_ENABLE 0x0610
#define V_CLOCK 0x0620
#define V_MMU_CONTROL 0x0680
#define V_MMU_STATUS 0x068c
#define V_BANK 0x06f0
#define V_RENDEC_CONTROL0 0x0868
#define V_RENDEC_CONTROL1 0x086c
#define V_RENDEC_SIZE 0x0870
#define V_RENDEC_A 0x0874
#define V_RENDEC_B 0x0878
#define V_RENDEC_CONTEXT 0x0950
#define V_FW_STATUS 0x2cb0
#define V_COUNTER 0x2cbc
#define V_SIGNATURE 0x2cc0
#define V_REPLY_READ 0x2cc8
#define V_REPLY_WRITE 0x2ccc
#define V_COMMAND_READ 0x2cd4
#define V_FLAGS 0x2cd8
#define V_COMMAND_WRITE 0x2cdc
#define V_REPLY_RING 0x2ce0
#define V_COMMAND_RING 0x2e70
#define V_RING_WORDS 100U
#define V_RENDER_VLD 0x0800U
#define V_RENDER_HOST_INT 0x4000U
/* Use a nonzero PSB process/context tag for this engine's private context. */
#define V_CONTEXT 0x00010001U

struct video_memory {
	struct page **pages;
	u32 count;
	void *map;
	struct drm_mm_node node;
};

struct video_surface {
	struct drm_gem_object *bo;
	struct page **pages;
	u32 count;
	bool cached;
	struct drm_mm_node node;
};

struct video_job {
	struct poulsback_vm *owner_vm;
	struct video_memory bitstream;
	struct video_surface surfaces[3];
};

struct video_engine {
	struct drm_device *dev;
	void __iomem *regs;
	struct poulsback_vm *vm;
	struct video_memory rendec, command;
	const struct firmware *firmware;
	struct video_job *held;
	u32 sequence;
	bool claimed, ready, lost, parked, powered_down;
};

/* One Poulsbo device; synchronous submissions also serialize system sleep. */
static DEFINE_MUTEX(video_lock);
static struct video_engine *video;
static bool video_parked;

static u32 rd(struct video_engine *v, u32 offset)
{
	return readl(v->regs + offset);
}

static void wr(struct video_engine *v, u32 offset, u32 value)
{
	writel(value, v->regs + offset);
}

static int wait_reg(struct video_engine *v, u32 offset, u32 mask, u32 expected)
{
	u32 value;
	return readl_poll_timeout(v->regs + offset, value, (value & mask) == expected, 20, 100000);
}

static int hold(struct video_engine *v, int error, const char *where)
{
	error = POULSBACK_ERRSITE(error);
	v->lost = true;
	poulsback_vm_freeze();
	sgx535_epoch_lost(v->dev, SGX535_EPOCH_FAULT);
	drm_err(v->dev, "MSVDX %s failed (%d); resources retained until reboot\n", where, error);
	return error;
}

static void flush_pages(struct page **pages, u32 count)
{
	u32 i;
	for (i = 0; i < count; i++)
		clflush_cache_range(page_address(pages[i]), PAGE_SIZE);
}

static bool memory_free(struct video_engine *v, struct video_memory *m)
{
	u32 i;
	if (drm_mm_node_allocated(&m->node) && !poulsback_vm_unmap(v->vm, &m->node))
		return false;
	if (m->map)
		vunmap(m->map);
	for (i = 0; i < m->count; i++)
		__free_page(m->pages[i]);
	kfree(m->pages);
	memset(m, 0, sizeof(*m));
	return true;
}

static int memory_alloc(struct video_engine *v, struct video_memory *m, u32 bytes)
{
	u32 i, count = DIV_ROUND_UP(bytes, PAGE_SIZE);
	int ret = -ENOMEM;
	m->pages = kcalloc(count, sizeof(*m->pages), GFP_KERNEL);
	if (!m->pages)
		return ret;
	for (i = 0; i < count; i++) {
		m->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!m->pages[i])
			goto fail;
		m->count++;
	}
	m->map = vmap(m->pages, count, VM_MAP, PAGE_KERNEL);
	if (!m->map)
		goto fail;
	flush_pages(m->pages, count);
	ret = poulsback_vm_map(v->vm, POULSBACK_VM_GENERAL, PAGE_SIZE, m->pages,
			      count, false, &m->node);
	if (!ret)
		return 0;
fail:
	memory_free(v, m);
	return ret;
}

static int firmware_region(struct video_engine *v, u32 bank_bytes, u32 base,
			   u32 offset, const u8 *data, u32 words, bool verify)
{
	u32 saved = rd(v, V_RAM_CONTROL), bank = U32_MAX, i;
	int ret = wait_reg(v, V_RAM_STATUS, U32_MAX, 1);
	if (ret)
		return ret;
	for (i = 0; i < words; i++, offset += 4) {
		u32 next = base + offset / bank_bytes;
		if (next != bank) {
			wr(v, V_RAM_CONTROL, next << 20 | offset | (verify ? 3 : 2));
			bank = next;
		}
		if (!verify)
			wr(v, V_RAM_DATA, get_unaligned_le32(data + i * 4));
		ret = wait_reg(v, V_RAM_STATUS, U32_MAX, 1);
		if (ret)
			return ret;
		if (verify && rd(v, V_RAM_DATA) != get_unaligned_le32(data + i * 4))
			return -EIO;
	}
	wr(v, V_RAM_CONTROL, saved);
	return 0;
}

static int boot(struct video_engine *v)
{
	const u8 *fw = v->firmware->data;
	u32 bank, i;
	int ret;
	/* This sequence is for the pinned PSB 0.30 image and Poulsbo D0/D1. */
	wr(v, V_CLOCK, 0x7f);
	wr(v, V_IRQ_ENABLE, 0);
	wr(v, V_MMU_CONTROL, 0);
	wr(v, V_RENDEC_A, v->rendec.node.start);
	wr(v, V_RENDEC_B, v->rendec.node.start + (2U << 20));
	wr(v, V_RENDEC_SIZE, 512 | 128 << 16);
	wr(v, V_RENDEC_CONTROL1, 0x01050000);
	for (i = 0; i < 6; i++)
		wr(v, V_RENDEC_CONTEXT + 4 * i, 0x00101010);
	wr(v, V_RENDEC_CONTROL0, 1);
	wr(v, V_MTX_RESET, 1);
	wr(v, V_FLAGS, to_pci_dev(v->dev->dev)->revision >= 6 ? 0x220 : 0x222);
	wr(v, V_COUNTER, 0); wr(v, V_SIGNATURE, 0); wr(v, V_REPLY_READ, 0);
	wr(v, V_REPLY_WRITE, 0); wr(v, V_COMMAND_READ, 0); wr(v, V_COMMAND_WRITE, 0);
	wr(v, V_FW_STATUS, 0);
	bank = 1U << (((rd(v, V_BANK) >> 16) & 15) + 2);
	if (bank < 2048 || bank > 65536)
		return -EINVAL;
	ret = firmware_region(v, bank, 0x10, 0, fw + 16, 2841, false);
	if (!ret)
		ret = firmware_region(v, bank, 0x18, 0x2c80, fw + 11380, 1046, false);
	if (!ret)
		ret = firmware_region(v, bank, 0x10, 0, fw + 16, 2841, true);
	if (!ret)
		ret = firmware_region(v, bank, 0x18, 0x2c80, fw + 11380, 1046, true);
	if (ret)
		return ret;
	wr(v, V_REG_DATA, 0x80900000);
	wr(v, V_REG_REQUEST, 5);
	ret = wait_reg(v, V_REG_REQUEST, 0x80000000, 0x80000000);
	if (ret)
		return ret;
	wr(v, V_ENABLE, 1);
	ret = wait_reg(v, V_SIGNATURE, U32_MAX, 0xa5a5a5a5);
	if (ret)
		return ret;
	v->ready = true;
	v->powered_down = false;
	drm_info(v->dev, "MSVDX: PSB 0.30 firmware uploaded and read back; MTX ready\n");
	return 0;
}

static struct video_engine *engine_get(struct drm_device *dev)
{
	struct video_engine *v;
	struct pci_dev *pci = to_pci_dev(dev->dev);
	int ret;
	if (video)
		return video->dev != dev || video->lost ? ERR_PTR(-EIO) : video;
	if (!IS_PSB(dev) || pci_resource_len(pci, 0) < 0x58000)
		return ERR_PTR(-ENODEV);
	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);
	v->dev = dev;
	ret = request_firmware(&v->firmware, VIDEO_FIRMWARE, dev->dev);
	if (ret)
		goto free;
	if (v->firmware->size != 15564 || get_unaligned_le32(v->firmware->data) != 2 ||
	    get_unaligned_le32(v->firmware->data + 4) != 2841 ||
	    get_unaligned_le32(v->firmware->data + 8) != 1046 ||
	    get_unaligned_le32(v->firmware->data + 12) != 0x82882c80) {
		ret = -EINVAL;
		goto free;
	}
	v->vm = poulsback_vm_create();
	if (!v->vm) {
		ret = -ENOMEM;
		goto free;
	}
	ret = memory_alloc(v, &v->rendec, (2U << 20) + (512U << 10));
	if (!ret)
		ret = memory_alloc(v, &v->command, PAGE_SIZE);
	if (ret)
		goto free;
	poulsback_mpeg2_vlc(v->command.map);
	v->regs = ioremap(pci_resource_start(pci, 0) + 0x50000, 0x8000);
	if (!v->regs) {
		ret = -ENOMEM;
		goto free;
	}
	ret = sgx535_epoch_video_claim(dev);
	if (ret)
		goto free;
	__module_get(THIS_MODULE);
	v->claimed = true;
	video = v;
	return v;
free:
	if (v->regs)
		iounmap(v->regs);
	if (v->vm) {
		if (!memory_free(v, &v->command) || !memory_free(v, &v->rendec)) {
			__module_get(THIS_MODULE);
			v->claimed = true;
			video = v;
			hold(v, ret, "allocation cleanup");
			return ERR_PTR(ret);
		}
		poulsback_vm_put(v->vm);
	}
	release_firmware(v->firmware);
	kfree(v);
	return ERR_PTR(ret);
}

static int send_slice(struct video_engine *v, u32 bytes, u32 dma, u32 mode, u32 last_mb)
{
	u32 message[8], r, w, padding, free, i, seq = ++v->sequence;
	u32 reply[3], head, count, irq;
	unsigned long deadline;
	if (!seq)
		return -EOVERFLOW;
	wr(v, V_CLOCK, 0x7f);
	r = rd(v, V_COMMAND_READ); w = rd(v, V_COMMAND_WRITE);
	if (r >= V_RING_WORDS || w >= V_RING_WORDS)
		return -EPROTO;
	padding = w + 8 > V_RING_WORDS ? V_RING_WORDS - w : 0;
	free = (r + V_RING_WORDS - w - 1) % V_RING_WORDS;
	if (padding + 8 > free)
		return -EBUSY;
	message[0] = 0x8120 | bytes << 16;
	message[1] = poulsback_vm_directory(v->vm) | 1;
	message[2] = dma; message[3] = V_CONTEXT; message[4] = seq;
	message[5] = mode; message[6] = last_mb << 16;
	message[7] = V_RENDER_VLD | V_RENDER_HOST_INT;
	for (i = 0; i < padding; i++)
		wr(v, V_COMMAND_RING + 4 * w++, i ? 0 : padding * 4);
	w %= V_RING_WORDS;
	for (i = 0; i < 8; i++) {
		wr(v, V_COMMAND_RING + 4 * w, message[i]);
		w = (w + 1) % V_RING_WORDS;
	}
	wmb();
	wr(v, V_COMMAND_WRITE, w);
	wr(v, V_KICK, 1);
	deadline = jiffies + msecs_to_jiffies(2000);
	for (;;) {
		irq = rd(v, V_IRQ_STATUS);
		if (irq & 0xf00) {
			u32 status = rd(v, V_MMU_STATUS), pde, pte;
			poulsback_vm_peek(v->vm, status & PAGE_MASK, &pde, &pte);
			drm_err(v->dev, "MSVDX MMU fault: irq=%08x status=%08x fence=%u pde=%08x pte=%08x\n",
				irq, status, seq, pde, pte);
			return -EIO;
		}
		r = rd(v, V_REPLY_READ); w = rd(v, V_REPLY_WRITE);
		if (r >= V_RING_WORDS || w >= V_RING_WORDS)
			return -EPROTO;
		if (r != w) {
			head = rd(v, V_REPLY_RING + 4 * r);
			count = (head & 255) / 4;
			if ((head & 3) || count != 3 || (w + V_RING_WORDS - r) % V_RING_WORDS < count)
				return -EPROTO;
			for (i = 0; i < count; i++) {
				reply[i] = rd(v, V_REPLY_RING + 4 * r);
				r = (r + 1) % V_RING_WORDS;
			}
			wr(v, V_REPLY_READ, r);
			wr(v, V_IRQ_CLEAR, irq & 0x4000);
			if ((head >> 8 & 255) == 0xc5 || (head >> 8 & 255) == 0xc7) {
				drm_err(v->dev, "MSVDX firmware failure: id=%02x fence=%u irq=%08x context=%08x\n",
					head >> 8 & 255, reply[1], reply[2], V_CONTEXT);
				return -EIO;
			}
			if ((head >> 8 & 255) != 0xc0 || reply[1] != seq || !(reply[2] & V_RENDER_HOST_INT)) {
				drm_err(v->dev, "MSVDX reply %08x fence %u status %08x, expected %u\n",
					head, reply[1], reply[2], seq);
				return -EIO;
			}
			return wait_reg(v, V_FW_STATUS, 1, 1);
		}
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(20, 40);
	}
}

static bool job_free(struct video_engine *v, struct video_job *job)
{
	u32 i;
	if (poulsback_vm_held(v->vm))
		return false;
	for (i = 0; i < 3; i++) {
		struct video_surface *s = &job->surfaces[i];
		if (drm_mm_node_allocated(&s->node) && !poulsback_vm_unmap(v->vm, &s->node))
			return false;
		if (s->bo) {
			poulsback_bo_release(s->bo, job->owner_vm);
			s->bo = NULL;
		}
	}
	if (!memory_free(v, &job->bitstream))
		return false;
	poulsback_vm_put(job->owner_vm);
	kfree(job);
	return true;
}

int poulsback_mpeg2_decode_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	const struct poulsback_mpeg2_decode *p = data;
	struct poulsback_mpeg2_slice *slices;
	struct video_engine *v;
	struct video_job *job = NULL;
	struct drm_gem_object *bos[3];
	struct ww_acquire_ctx acquire;
	struct sgx535_epoch_snapshot source;
	u8 *stream;
	u32 handles[3] = {p->output, p->forward_reference, p->backward_reference};
	u32 addresses[3] = {0}, i, count = 0, previous = U32_MAX;
	int ret;
	bool locked = false;
	if (!p->slice_count || p->slice_count > POULSBACK_MPEG2_MAX_SLICES ||
	    !p->bitstream_bytes || p->bitstream_bytes > POULSBACK_MPEG2_MAX_BITSTREAM)
		return -EINVAL;
	slices = memdup_user(u64_to_user_ptr(p->slices), array_size(p->slice_count, sizeof(*slices)));
	if (IS_ERR(slices))
		return PTR_ERR(slices);
	stream = vmemdup_user(u64_to_user_ptr(p->bitstream), p->bitstream_bytes);
	if (IS_ERR(stream)) {
		ret = PTR_ERR(stream);
		goto free_slices;
	}
	ret = poulsback_mpeg2_validate(p, slices, stream);
	if (ret)
		goto free_stream;
	mutex_lock(&video_lock);
	sgx535_epoch_snapshot(&source);
	if (video_parked || !source.bound || source.loss) {
		ret = -EBUSY;
		goto unlock;
	}
	v = engine_get(dev);
	if (IS_ERR(v)) {
		ret = PTR_ERR(v);
		goto unlock;
	}
	if (v->parked || v->powered_down || poulsback_vm_held(v->vm)) {
		ret = -EBUSY;
		goto unlock;
	}
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto unlock;
	}
	job->owner_vm = poulsback_vm_get(poulsback_file_vm(file));
	for (i = 0; i < 3; i++) {
		struct video_surface *s = &job->surfaces[i];
		u64 size, va;
		u32 j;
		if (!handles[i])
			continue;
		ret = poulsback_bo_resolve(file, handles[i], &s->bo, &va, &size);
		if (ret)
			goto done;
		if (size < (u64)p->pitch * ALIGN(p->height, 32) * 3 / 2) {
			ret = -EINVAL;
			goto done;
		}
		for (j = 0; j < i; j++)
			if (job->surfaces[j].bo == s->bo) {
				ret = -EINVAL;
				goto done;
			}
		ret = poulsback_bo_video_pages(s->bo, &s->pages, &s->count, &s->cached);
		if (!ret)
			ret = poulsback_vm_map(v->vm, POULSBACK_VM_GENERAL, PAGE_SIZE,
					      s->pages, s->count, false, &s->node);
		if (ret)
			goto done;
		addresses[i] = s->node.start;
		bos[count++] = s->bo;
	}
	/* PSB maps unused addresses to a cleared page. Keep one zero tail page
	 * for reads past the input allocation, without mapping every unused VA. */
	ret = memory_alloc(v, &job->bitstream,
			   ALIGN(p->bitstream_bytes, 32U << 10) + PAGE_SIZE);
	if (ret)
		goto done;
	memcpy(job->bitstream.map, stream, p->bitstream_bytes);
	flush_pages(job->bitstream.pages, job->bitstream.count);
	ret = drm_gem_lock_reservations(bos, count, &acquire);
	if (ret)
		goto done;
	locked = true;
	for (i = 0; i < count; i++) {
		long waited = dma_resv_wait_timeout(bos[i]->resv,
				i ? DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ,
						 true, MAX_SCHEDULE_TIMEOUT);
		if (waited <= 0) {
			ret = waited ? waited : -ETIMEDOUT;
			goto done;
		}
	}
	for (i = 0; i < 3; i++)
		if (job->surfaces[i].bo && job->surfaces[i].cached)
			flush_pages(job->surfaces[i].pages, job->surfaces[i].count);
	if (poulsback_vm_held(v->vm)) {
		ret = -EIO;
		goto done;
	}
	if (!v->ready) {
		ret = boot(v);
		if (ret) {
			hold(v, ret, "firmware startup");
			goto done;
		}
	}
	for (i = 0; i < p->slice_count; i++) {
		u32 dma, bytes, rows;
		if (poulsback_vm_held(v->vm)) {
			ret = -EIO;
			goto done;
		}
		bytes = poulsback_mpeg2_commands(p, &slices[i], previous, addresses,
					       v->command.node.start, job->bitstream.node.start,
					       v->command.map, &dma);
		if (!bytes) {
			ret = -EINVAL;
			goto done;
		}
		flush_pages(v->command.pages, v->command.count);
		rows = ALIGN(p->height, p->flags & POULSBACK_MPEG2_PROGRESSIVE_SEQUENCE ? 16 : 32) / 16;
		ret = send_slice(v, bytes, dma, poulsback_mpeg2_mode(p),
				(rows - 1) << 8 | (DIV_ROUND_UP(p->width, 16) - 1));
		if (ret) {
			drm_err(dev, "MSVDX picture %ux%u type=%u flags=%x extension=%x fcode=%04x "
				"slice=%u/%u row=%u bytes=%u bit=%u quant=%u\n",
				p->width, p->height, p->coding_type, p->flags, p->coding_extension,
				p->f_code, i + 1, p->slice_count, slices[i].vertical_position,
				slices[i].bytes, slices[i].macroblock_offset, slices[i].quantiser_scale_code);
			drm_err(dev, "MSVDX mappings: stream=%08llx+%llx payload=%u slice_offset=%u surfaces=%08x,%08x,%08x bytes=%llu command=%08llx rendec=%08llx+%llx\n",
				job->bitstream.node.start, job->bitstream.node.size,
				p->bitstream_bytes, slices[i].offset,
				addresses[0], addresses[1], addresses[2], job->surfaces[0].node.size,
				v->command.node.start, v->rendec.node.start, v->rendec.node.size);
			hold(v, ret, "decode");
			goto done;
		}
		previous = slices[i].vertical_position;
	}
	if (job->surfaces[0].cached)
		flush_pages(job->surfaces[0].pages, job->surfaces[0].count);
done:
	if (locked)
		drm_gem_unlock_reservations(bos, count, &acquire);
	if (!job_free(v, job)) {
		v->held = job;
		if (!v->lost)
			hold(v, -EIO, "memory ownership");
		ret = -EIO;
	}
unlock:
	mutex_unlock(&video_lock);
free_stream:
	kvfree(stream);
free_slices:
	kfree(slices);
	return ret;
}

bool poulsback_video_retained(struct drm_device *dev)
{
	return video && video->dev == dev && video->claimed;
}

int poulsback_video_quiesce(struct drm_device *dev)
{
	int ret = 0;
	mutex_lock(&video_lock);
	if (video && video->dev == dev) {
		if (video->lost || video->held)
			ret = -EBUSY;
		else
			video->parked = true;
	}
	if (!ret)
		video_parked = true;
	mutex_unlock(&video_lock);
	return ret;
}

void poulsback_video_unquiesce(struct drm_device *dev)
{
	mutex_lock(&video_lock);
	video_parked = false;
	if (video && video->dev == dev && !video->lost && !video->powered_down)
		video->parked = false;
	mutex_unlock(&video_lock);
}

int poulsback_video_power_down(struct drm_device *dev)
{
	struct video_engine *v;
	int ret = 0;
	mutex_lock(&video_lock);
	v = video;
	if (!v || v->dev != dev || !v->ready)
		goto out;
	if (v->lost || !v->parked) {
		ret = -EBUSY;
		goto out;
	}
	ret = wait_reg(v, V_FW_STATUS, 1, 1);
	if (ret)
		goto failed;
	wr(v, V_CONTROL, 0x11110100);
	ret = wait_reg(v, V_CONTROL, 0x100, 0);
	if (ret)
		goto failed;
	wr(v, V_CLOCK, 0);
	v->powered_down = true;
	v->ready = false;
	goto out;
failed:
	hold(v, ret, "suspend");
out:
	mutex_unlock(&video_lock);
	return ret;
}

int poulsback_video_resume(struct drm_device *dev, int error)
{
	int ret = error;
	mutex_lock(&video_lock);
	if (video && video->dev == dev && video->powered_down) {
		if (!ret)
			ret = boot(video);
		if (ret)
			hold(video, ret, "resume");
	}
	mutex_unlock(&video_lock);
	return ret;
}

MODULE_FIRMWARE(VIDEO_FIRMWARE);
