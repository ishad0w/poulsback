// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_bo.h"
#include "poulsback_engine.h"
#include "poulsback_errsite.h"
#include "poulsback_job.h"
#include "poulsback_overlap.h"
#include "poulsback_teardown.h"
#include "poulsback_vm.h"
#include "poulsback_vm_layout.h"
#include "psb_drv.h"
#include "sgx535_engine121.h"
#include "sgx535_engine_irq.h"
#include "sgx535_source_epoch.h"
#include "sgx535_use_code_bank.h"

#include <asm/cacheflush.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>
#include <linux/debugfs.h>
#include <linux/dma-fence.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define ENGINE_DPM_PAGES 8192U
#define ENGINE_TABLES_PER_AREA (ENGINE_DPM_PAGES / 1024U)
#define ENGINE_DEVICE_TABLES (2U * ENGINE_TABLES_PER_AREA)

#define REG_CORE_ID 0x0010U
#define REG_CORE_REVISION 0x0014U
#define REG_SOFT_RESET 0x0080U
#define REG_HOST_ENABLE2 0x0110U
#define REG_HOST_CLEAR2 0x0114U
#define REG_EVENT_STATUS2 0x0118U
#define REG_EVENT_STATUS 0x012cU
#define REG_HOST_ENABLE 0x0130U
#define REG_HOST_CLEAR 0x0134U
#define REG_PDS_EXEC_BASE 0x0ab8U
#define REG_BIF_CTRL 0x0c00U
#define REG_BIF_INT_STAT 0x0c04U
#define REG_BIF_FAULT 0x0c08U
#define REG_BIF_BASE1 0x0c38U
/* DPM_STATE_CONTEXT_ID: ALLOC (the TA's context) bit 2, DALLOC (the 3D's) bit
 * 1, LS (the target of the TA program's state and control CLEARs) bit 0. */
#define REG_DPM_CONTEXT_ID 0x0630U
/* DPM_PAGE_STATUS: the TA's pages in bits 31:16 (sgx535defs.h). */
#define REG_DPM_PAGE_STATUS 0x0724U
#define DPM_CONTEXT_ALLOC_LS(c) ((c) << 2 | (c))
#define DPM_CONTEXT_DALLOC(c) ((c) << 1)
/* USE cache invalidate and the USSE's task queues and slots. */
#define REG_USE_CACHE 0x0a08U
#define REG_DMS_CTRL 0x0a74U
#define DMS_DISABLE_VERTEX_PIXEL_EVENT 0x7U
#define REG_USE0_DM_SLOT 0x0aa4U
#define REG_USE1_DM_SLOT 0x0aa8U
#define USE_DM_SLOTS_IDLE 0xaaaaaaaaU
#define REG_USE0_SERV_PIXEL 0x0b08U
#define REG_USE1_SERV_PIXEL 0x0b14U
#define USE_SERV_EMPTY 0x00010000U
#define REG_BIF_BASE15 0x0c70U
#define REG_BIF_BANK0 0x0c78U
#define REG_BIF_BASE0 0x0c84U
#define REG_TWOD_REQ_BASE 0x0c88U
#define REG_TA_REQ_BASE 0x0c90U
#define REG_3D_REQ_BASE 0x0cacU
#define REG_TWOD_BLIT_STATUS 0x0e04U
#define REG_TWOD_SOCIF 0x0e18U
#define REG_TWOD_SLAVE_PORT 0x4000U

/* Every requester routes to the kernel directory (slot 15) while idle; a job
 * points its requesters at its file: a 3D job's at slot 0 (BASE0), a 2D job's
 * at slot 1 (BASE1), so that a 2D job runs during a raster pass
 * (poulsback_overlap.h). */
#define ENGINE_BANK0_IDLE POULSBACK_OVERLAP_BANK0_IDLE
#define ENGINE_HOST_ENABLE 0x1a04300fU
#define ENGINE_HOST_ENABLE2 0x10U
#define ENGINE_STATUS2_BIF_FAULT 0x10U
#define ENGINE_WAIT_NS 100000000ULL
#define ENGINE_JOB_TIMEOUT_MS 2000U
/* Quiesce drain: two job timeouts and 100 ms (docs/kernel/execution.md). */
#define ENGINE_QUIESCE_MS (2U * ENGINE_JOB_TIMEOUT_MS + 100U)

/* A 2D job routes the TA and 2D requesters to its slot: the 2D core writes
 * through BANK0.TA (HW-ADDR-004) and reads its sources through BANK0.2D.
 * The serial 3D path allows a 2D job to hold TA during raster; concurrent
 * TA/raster rerouting has a separate late-request limit (docs/hardware.md). */

/* EVENT_STATUS bits (sgx535defs.h:135-197). */
#define EV_DPM_3D_MEM_FREE 0x1U
#define EV_OOM_MT 0x2U
#define EV_OOM_GBL 0x4U
#define EV_TA_TERMINATE 0x1000U
#define EV_TA_FINISHED 0x2000U
#define EV_PIXELBE_END 0x40000U
#define EV_OOM_ZLS 0x02000000U
#define EV_TA_DPM_FAULT 0x10000000U
#define EV_TWOD_COMPLETE 0x08000000U
#define EV_DONE (EV_PIXELBE_END | EV_DPM_3D_MEM_FREE)
#define EV_FATAL (EV_TA_TERMINATE | EV_OOM_ZLS | EV_TA_DPM_FAULT)

/* The 2D core: SOCIF bits 7:0 count the slave port's free
 * slots, 0x80 when empty; BLIT_STATUS bits 23:0 count completed commands, bit
 * 24 is busy. Words go in bursts of at most 0x60 from the port base, as PSB's
 * (psb_sgx.c psb_2d_submit). */
/* While jobs run, and briefly after, the CPUs stay out of C2 and deeper: this
 * Atom's LAPIC timer stops there (no ARAT), so engine work avoids relying on
 * HPET broadcast wakeups (HW-IRQ-003, docs/hardware.md). C1E's
 * exit latency is 10 us, C2's 20 us. */
#define ENGINE_CPU_LATENCY_US 10
#define ENGINE_QOS_IDLE_MS 100

#define TWOD_SOCIF_EMPTY 0x80U
#define TWOD_SOCIF_FREE 0xffU
#define TWOD_BURST 0x60U
#define TWOD_BUSY 0x01000000U
#define TWOD_COUNT 0x00ffffffU

/* The ISP reset between the TA and the raster pass. */
static const struct sgx535_engine121_action engine_isp_reset[2] = {
	{SGX535_ENGINE121_WRITE, 0x080, 0x20, 0},
	{SGX535_ENGINE121_WRITE, 0x080, 0, 0},
};

/* TE_TPCCONTROL FLUSH and its event (EVENT_STATUS bit 10, not a host IRQ). */
#define REG_TE_TPCCONTROL 0x0224U
#define TPC_FLUSH 0x40000000U
#define EV_TPC_FLUSH 0x00000400U
#define ENGINE_TPC_FLUSH_US 1000U

enum engine_state { ENGINE_DOWN, ENGINE_READY, ENGINE_WEDGED };

struct poulsback_engine {
	struct drm_device *dev;
	void __iomem *regs;
	enum engine_state state;
	int error;
	u64 bringup_ns;
	struct page *directory, *tables[ENGINE_DEVICE_TABLES];
	u32 table_index[ENGINE_DEVICE_TABLES], table_pde[ENGINE_DEVICE_TABLES];
	struct page **dpm[2];
	/* 3D jobs (three credits, poulsback_overlap.h) and 2D jobs (one). */
	struct drm_gpu_scheduler sched, sched2d;
	/* wq: TA starts (start_work), raster starts (raster_work), OOM; wq2d: 2D
	 * starts and feeding the 2D core (twod_work). */
	struct workqueue_struct *wq, *wq2d;
	struct work_struct raster_work, oom_work, twod_work, start_work;
	/* The CPU latency limit held while jobs run (qos_lock); relaxed by
	 * qos_relax once the engine has been idle for ENGINE_QOS_IDLE_MS. */
	struct pm_qos_request cpu_qos;
	struct delayed_work qos_relax;
	struct mutex qos_lock;
	bool qos_held;
	/* Under the engine IRQ lock: when jobs may run (poulsback_overlap.h); the
	 * 3D job in its TA pass, the one whose TA finished and whose raster
	 * waits, the one in its raster pass and the 2D job; those that wait for
	 * their turn (3D in order) and those due to start. */
	struct poulsback_overlap ov;
	struct poulsback_job *ta_job, *ready_job, *raster_job, *job2d;
	struct poulsback_job *wait3d[POULSBACK_OVERLAP_CREDITS_3D], *wait2d;
	struct poulsback_job *start_ta, *start_raster, *start2d;
	u32 overlapped;	  /* 2D jobs started during a raster pass */
	u32 ta_in_raster; /* TA passes started beside a job that holds a DPM context */
	bool oom_deferred; /* a TA out of memory restarts at the raster's end */
	/* The DPM context selector as written, and the context the next TA
	 * takes: TAs alternate the two contexts; each raster
	 * frees the context its TA used (docs/kernel/execution.md). */
	u32 dpm_id, dpm_next;
	spinlock_t fence_lock;
	u64 fence_context, seqno;
	bool zls_raised;
	u32 jobs, oom_resumes, submits;
	/* Jobs stopped with the engine (HOLD): they never complete. */
	u32 held;
	/* The parameter pages the last TA pass took, and the most any took. */
	u32 ta_pages, ta_pages_max;
	/* Hardware fences not yet released; the IRQ route bound; registers
	 * programmed once; schedulers parked and engine idle. */
	atomic_t fences;
	bool routed, programmed, quiesced;
	/* CPU time of the job paths, for the per-job figures in debugfs. */
	u64 submit_ns, run_ns, raster_ns;
	/* 2D jobs: count, SUBMIT_2D calls and CPU time, words fed, bit-27 events,
	 * completions that found the core still busy. */
	u32 twod_jobs, twod_submits, twod_words, twod_irqs, twod_busy_waits;
	u64 twod_submit_ns, twod_ns;
	u32 irqs, last_event1, last_event2, fault_stat, fault_addr;
	struct dentry *debugfs;
};

/* One SGX per system, as the source epoch. */
static DEFINE_MUTEX(engine_lock);
static struct poulsback_engine *engine;

static int engine_actions(void __iomem *regs, const struct sgx535_engine121_action *a, size_t n)
{
	u64 end;

	for (; n; n--, a++) {
		switch (a->kind) {
		case SGX535_ENGINE121_WRITE:
			writel(a->value, regs + a->offset);
			break;
		case SGX535_ENGINE121_READBACK:
			(void)readl(regs + a->offset);
			break;
		case SGX535_ENGINE121_WAIT_SET:
		case SGX535_ENGINE121_WAIT_CLEAR:
			/* The clock is HPET on the Dell: read it per wait, not per write. */
			end = ktime_get_ns() + ENGINE_WAIT_NS;
			while ((readl(regs + a->offset) & a->mask) != a->value) {
				if (ktime_get_ns() > end) {
					pr_err("poulsback: wait %#x & %#x == %#x timed out (read %#x)\n",
					       a->offset, a->mask, a->value, readl(regs + a->offset));
					return POULSBACK_ERRSITE(-ETIMEDOUT);
				}
				usleep_range(5, 20);
			}
			break;
		default:
			return POULSBACK_ERRSITE(-EINVAL);
		}
	}
	return 0;
}

/* INVALDC with readback; never FLUSH on SGX535 core 1.2.1. */
static int engine_invaldc(void __iomem *regs)
{
	u32 base = readl(regs + REG_BIF_CTRL);
	writel(base | 8U, regs + REG_BIF_CTRL);
	wmb();
	writel(base & ~0xcU, regs + REG_BIF_CTRL);
	return readl(regs + REG_BIF_CTRL) == (base & ~0xcU) ? 0 : POULSBACK_ERRSITE(-EIO);
}

static const char *engine_fence_name(struct dma_fence *fence)
{
	return "poulsback";
}

/* F frees the engine only when no hardware fence is alive: a sync_file may
 * keep one after every file of the device has closed. */
static void engine_fence_release(struct dma_fence *fence)
{
	struct poulsback_engine *e = READ_ONCE(engine);
	if (e)
		atomic_dec(&e->fences);
	dma_fence_free(fence);
}

static const struct dma_fence_ops engine_fence_ops = {
	.get_driver_name = engine_fence_name,
	.get_timeline_name = engine_fence_name,
	.release = engine_fence_release,
};

static void engine_qos_busy(struct poulsback_engine *e)
{
	cancel_delayed_work_sync(&e->qos_relax);
	mutex_lock(&e->qos_lock);
	if (!e->qos_held) {
		cpu_latency_qos_update_request(&e->cpu_qos, ENGINE_CPU_LATENCY_US);
		e->qos_held = true;
	}
	mutex_unlock(&e->qos_lock);
}

/* Any context: the engine may be idle from now on. */
static void engine_qos_idle(struct poulsback_engine *e)
{
	mod_delayed_work(system_wq, &e->qos_relax, msecs_to_jiffies(ENGINE_QOS_IDLE_MS));
}

static void engine_qos_relax(struct work_struct *work)
{
	struct poulsback_engine *e = container_of(to_delayed_work(work), struct poulsback_engine,
						  qos_relax);
	unsigned long irq = sgx535_engine_irq_lock();
	bool busy = e->ta_job || e->ready_job || e->raster_job || e->job2d || e->wait3d[0] ||
		    e->wait2d || e->start_ta || e->start_raster || e->start2d;
	sgx535_engine_irq_unlock(irq);
	if (busy)
		return; /* its completion queues this again */
	mutex_lock(&e->qos_lock);
	if (e->qos_held) {
		cpu_latency_qos_update_request(&e->cpu_qos, PM_QOS_DEFAULT_VALUE);
		e->qos_held = false;
	}
	mutex_unlock(&e->qos_lock);
}

/* A job the engine stops with (HOLD): its fences complete with -EIO, the
 * published loss, so that waiters return. The scheduler's finished fence,
 * which userspace sees, takes it first: on 6.1 it does not inherit the
 * hardware fence's error. free_job keeps every resource the job referenced.
 * Called with the engine IRQ lock held. */
static void engine_hold_job_locked(struct poulsback_engine *e, struct poulsback_job *job)
{
	job->held = true;
	e->held++;
	dma_fence_set_error(&job->base.s_fence->finished, -EIO);
	dma_fence_set_error(job->done, -EIO);
	dma_fence_signal(job->done);
}

static void engine_hold_locked(struct poulsback_engine *e, struct poulsback_job **slot)
{
	struct poulsback_job *job = *slot;
	*slot = NULL;
	if (job && !job->held)
		engine_hold_job_locked(e, job);
}

/* The loss's one line; the cause stays in e->error (debugfs). */
static void engine_lost_note(struct poulsback_engine *e, u32 held)
{
	drm_err(e->dev, "GPU lost (%d), %u jobs held: acceleration off until reboot\n", e->error,
		held);
}

/* An unknown GPU outcome: the engine stops for the boot (HOLD). The VM layer
 * freezes before any lost fence signals, so a close that waited for one finds
 * it frozen. F refuses a lost engine, so its module reference stays for good:
 * a reload would reset a GPU in an unknown state. */
static void engine_wedge_locked(struct poulsback_engine *e, int error)
{
	bool first = e->state != ENGINE_WEDGED;
	u32 held = e->held, i;
	poulsback_vm_freeze();
	e->state = ENGINE_WEDGED;
	if (!e->error)
		e->error = error;
	engine_qos_idle(e);
	engine_hold_locked(e, &e->ta_job);
	engine_hold_locked(e, &e->ready_job);
	engine_hold_locked(e, &e->raster_job);
	engine_hold_locked(e, &e->job2d);
	for (i = 0; i < POULSBACK_OVERLAP_CREDITS_3D; i++)
		engine_hold_locked(e, &e->wait3d[i]);
	engine_hold_locked(e, &e->wait2d);
	engine_hold_locked(e, &e->start_ta);
	engine_hold_locked(e, &e->start_raster);
	engine_hold_locked(e, &e->start2d);
	if (first)
		engine_lost_note(e, e->held - held);
}

static void engine_wedge(struct poulsback_engine *e, int error)
{
	unsigned long irq = sgx535_engine_irq_lock();
	engine_wedge_locked(e, error);
	sgx535_engine_irq_unlock(irq);
}

/* BANK0 for the jobs on the GPU (a job due to start has not written its
 * directory slot yet), then INVALDC. */
static void engine_route_locked(struct poulsback_engine *e)
{
	struct poulsback_overlap now = {
		.ta = e->ta_job != NULL,
		.ready = e->ready_job || e->start_raster,
		.raster = e->raster_job != NULL,
		.run2d = e->job2d != NULL,
	};
	writel(poulsback_overlap_bank0(&now), e->regs + REG_BIF_BANK0);
	if (engine_invaldc(e->regs))
		engine_wedge_locked(e, POULSBACK_ERRSITE(-EIO));
}

/* Whether the first waiting 3D job's TA may run beside the jobs that hold DPM
 * contexts: the same directory and another region-header array. */
static bool engine_joins_locked(struct poulsback_engine *e)
{
	struct poulsback_job *head = e->wait3d[0];
	struct poulsback_job *held[3] = {e->ready_job, e->start_raster, e->raster_job};
	u32 i;
	for (i = 0; head && i < ARRAY_SIZE(held); i++)
		if (held[i] && (held[i]->vm != head->vm ||
				(held[i]->scene == head->scene &&
				 held[i]->region_array == head->region_array)))
			return false;
	return true;
}

/* After an overlap event: the jobs it lets start go to start_work. False (an
 * event that does not fit the state) is an engine bug: wedge. */
static void engine_apply_locked(struct poulsback_engine *e, bool fits,
				const struct poulsback_overlap_start *st)
{
	u32 i;
	if (!fits) {
		engine_wedge_locked(e, POULSBACK_ERRSITE(-EPROTO));
		return;
	}
	if (st->raster) {
		e->start_raster = e->ready_job;
		e->ready_job = NULL;
		queue_work(e->wq, &e->raster_work);
	}
	if (st->ta) {
		e->start_ta = e->wait3d[0];
		for (i = 0; i + 1 < POULSBACK_OVERLAP_CREDITS_3D; i++)
			e->wait3d[i] = e->wait3d[i + 1];
		e->wait3d[i] = NULL;
		queue_work(e->wq, &e->start_work);
	}
	if (st->twod) {
		e->start2d = e->wait2d;
		e->wait2d = NULL;
		queue_work(e->wq2d, &e->twod_work);
	}
	if (!st->raster && !st->ta && !st->twod && !e->ta_job && !e->ready_job &&
	    !e->raster_job && !e->job2d && !e->wait3d[0] && !e->wait2d)
		engine_qos_idle(e);
}

static void engine_done3d_locked(struct poulsback_engine *e, struct poulsback_job *job)
{
	struct poulsback_overlap_start st;
	bool fits;
	e->raster_job = NULL;
	e->jobs++;
	dma_fence_signal(job->done);
	if (e->oom_deferred) {
		/* The TA waited for memory: the raster freed its scene's pages. */
		e->oom_deferred = false;
		writel(1, e->regs + 0x69c); /* DPM_OUTOFMEM RESTART */
	}
	fits = poulsback_overlap_event(&e->ov, POULSBACK_OVERLAP_DONE3D, engine_joins_locked(e), &st);
	engine_route_locked(e);
	engine_apply_locked(e, fits, &st);
}

static void engine_done2d_locked(struct poulsback_engine *e, struct poulsback_job *job)
{
	struct poulsback_overlap_start st;
	bool fits;
	e->job2d = NULL;
	e->twod_jobs++;
	dma_fence_signal(job->done);
	fits = poulsback_overlap_event(&e->ov, POULSBACK_OVERLAP_DONE2D, engine_joins_locked(e), &st);
	engine_route_locked(e);
	engine_apply_locked(e, fits, &st);
}

enum twod_state { TWOD_RUNNING, TWOD_BUSY_DONE, TWOD_FINISHED };

/* A fully fed 2D job: finished once BLIT_STATUS counted the plan's commands
 * and the core is idle; a count past the plan is an unknown outcome. */
static enum twod_state engine_twod_check_locked(struct poulsback_engine *e,
						 struct poulsback_job *job)
{
	u32 status = readl(e->regs + REG_TWOD_BLIT_STATUS);
	u32 done = (status - job->start) & TWOD_COUNT;
	if (done > job->expected) {
		engine_wedge_locked(e, POULSBACK_ERRSITE(-EIO));
		return TWOD_FINISHED;
	}
	if (done < job->expected)
		return TWOD_RUNNING;
	if (status & TWOD_BUSY)
		return TWOD_BUSY_DONE;
	engine_done2d_locked(e, job);
	return TWOD_FINISHED;
}

/* One note per BIF fault (it wedges the engine): the running job, its
 * directory, the PDE and PTE that translate the address in its space, and a
 * 2D job's first command words. */
static void engine_fault_note(struct drm_device *dev, struct poulsback_engine *e)
{
	static const char *const roles[] = {"3D job in its TA pass, slot 0",
					    "3D job waiting for the raster, slot 0",
					    "3D job in its raster pass, slot 0", "2D job, slot 1"};
	struct poulsback_job *jobs[4] = {e->ta_job, e->ready_job, e->raster_job, e->job2d};
	bool any = false;
	u32 i;
	for (i = 0; i < ARRAY_SIZE(jobs); i++) {
		struct poulsback_job *job = jobs[i];
		u32 pde = 0, pte = 0;
		if (!job)
			continue;
		any = true;
		poulsback_vm_peek(job->vm, e->fault_addr, &pde, &pte);
		drm_err(dev,
			"BIF fault %08x at %08x, %s, directory %08x, pde %08x pte %08x, "
			"words %u fed %u, bank0 %08x dpm %08x\n",
			e->fault_stat, e->fault_addr, roles[i], poulsback_vm_directory(job->vm), pde,
			pte, job->count, job->fed, readl(e->regs + REG_BIF_BANK0), e->dpm_id);
		if (job->twod)
			print_hex_dump(KERN_ERR, "poulsback: BIF fault 2D words ", DUMP_PREFIX_OFFSET,
				       32, 4, job->words, min(job->count, 44U) * sizeof(u32),
				       false);
	}
	if (!any)
		drm_err(dev, "BIF fault %08x at %08x, no job\n", e->fault_stat, e->fault_addr);
	for (i = 0; i < 3; i++)
		if (jobs[i])
			drm_err(dev, "BIF fault: %s job region array %u, DPM context %u\n",
				roles[i], jobs[i]->region_array, jobs[i]->dpm);
	drm_err(dev, "BIF fault: TE regions %08x, ISP regions %08x, DPM 0x700 %08x 0x724 %08x 0x72c %08x\n",
		readl(e->regs + 0x21c), readl(e->regs + 0x408), readl(e->regs + 0x700),
		readl(e->regs + 0x724), readl(e->regs + 0x72c));
}

/* The TA pass is over: the job holds its DPM context until its raster ends;
 * its raster, a waiting TA or a waiting 2D job may start now. Not inlined, so
 * that a function trace marks the TA's end. */
static noinline void engine_ta_finished_locked(struct poulsback_engine *e,
					       struct poulsback_job *job)
{
	struct poulsback_overlap_start st;
	bool fits;
	u32 us;
	/* The TA's tail pointer cache written back before anything frees the
	 * scene's pages or another TA runs, so no write lands in freed pages. It
	 * writes through the TA requester, so while
	 * BANK0.TA points at the job's directory (with TA at the kernel's it
	 * faults at the scene base). BANK0.TA stays there after the flush's event:
	 * a write can still arrive (HW-ADDR-007). */
	writel(TPC_FLUSH, e->regs + REG_TE_TPCCONTROL);
	for (us = 0; !(readl(e->regs + REG_EVENT_STATUS) & EV_TPC_FLUSH); us++) {
		if (us == ENGINE_TPC_FLUSH_US) {
			engine_wedge_locked(e, POULSBACK_ERRSITE(-ETIMEDOUT));
			return;
		}
		udelay(1);
	}
	writel(EV_TPC_FLUSH, e->regs + REG_HOST_CLEAR);
	e->ta_pages = readl(e->regs + REG_DPM_PAGE_STATUS) >> 16;
	e->ta_pages_max = max(e->ta_pages_max, e->ta_pages);
	e->ta_job = NULL;
	e->ready_job = job;
	fits = poulsback_overlap_event(&e->ov, POULSBACK_OVERLAP_TA_FINISHED, engine_joins_locked(e),
				      &st);
	engine_route_locked(e);
	engine_apply_locked(e, fits, &st);
}

static void engine_irq(struct drm_device *dev, u32 status1, u32 status2)
{
	struct poulsback_engine *e = READ_ONCE(engine);
	void __iomem *regs = to_drm_psb_private(dev)->sgx_reg;
	struct poulsback_job *job;
	if (!e)
		return;
	e->irqs++;
	e->last_event1 = status1;
	e->last_event2 = status2;
	/* Only the events this handler owns: a program's waits poll and ack
	 * their own (DPM CLEAR, cache and MADD completions), which a raster's
	 * or TA's IRQ during another pass's program must not take from it. */
	status1 &= ENGINE_HOST_ENABLE;
	writel(status1, regs + REG_HOST_CLEAR);
	writel(status2, regs + REG_HOST_CLEAR2);
	if (status2 & ENGINE_STATUS2_BIF_FAULT) {
		e->fault_stat = readl(regs + REG_BIF_INT_STAT);
		e->fault_addr = readl(regs + REG_BIF_FAULT);
		engine_fault_note(dev, e);
		engine_wedge_locked(e, -EFAULT);
		return;
	}
	if ((e->ta_job || e->ready_job || e->raster_job || e->job2d) && (status1 & EV_FATAL)) {
		engine_wedge_locked(e, -EIO);
		return;
	}
	job = e->job2d;
	if (job && (status1 & EV_TWOD_COMPLETE)) {
		/* A 2D command completed: room for more words, or the end. */
		e->twod_irqs++;
		if (job->fed < job->count ||
		    engine_twod_check_locked(e, job) == TWOD_BUSY_DONE)
			queue_work(e->wq2d, &e->twod_work);
	}
	if (e->state != ENGINE_READY)
		return;
	job = e->ta_job;
	if (job && (status1 & (EV_OOM_MT | EV_OOM_GBL))) {
		if (e->raster_job || e->start_raster) {
			/* A raster will free pages into the list: restart then,
			 * with no threshold change. */
			e->oom_deferred = true;
		} else {
			/* One resume into the ZLS reserve per job
			 * (docs/kernel/execution.md). */
			if (job->oom_resumed) {
				engine_wedge_locked(e, -ENOSPC);
				return;
			}
			job->oom_resumed = true;
			queue_work(e->wq, &e->oom_work);
		}
	}
	if (job && (status1 & EV_TA_FINISHED))
		engine_ta_finished_locked(e, job);
	job = e->raster_job;
	if (!job || e->state != ENGINE_READY)
		return;
	job->events |= status1 & EV_DONE;
	if ((job->events & EV_DONE) == EV_DONE)
		engine_done3d_locked(e, job);
}

/* A ZLS threshold update: pause vertex
 * allocations, write the threshold, reload the TA free list from its live
 * head, tail and counts, then resume (docs/kernel/execution.md). */
static int engine_zls(struct poulsback_engine *e, u32 zls)
{
	void __iomem *regs = e->regs;
	u32 dms = readl(regs + 0xa74), a, b, i;
	u64 end;
	writel(dms | 1U, regs + 0xa74);
	a = readl(regs + 0x700);
	for (i = 0; i < 1000; i++) {
		udelay(1);
		b = readl(regs + 0x700);
		if (b == a)
			break;
		a = b;
	}
	/* DPMPauseAllocations waits for the free list to stop moving; still
	 * moving after 1 ms is an unknown state, not one to reload from. */
	if (i == 1000)
		return POULSBACK_ERRSITE(-ETIMEDOUT);
	writel(zls, regs + 0x624);
	writel(readl(regs + 0x700), regs + 0x61c);
	writel(readl(regs + 0x710) << 16, regs + 0x648);
	writel((readl(regs + 0x724) >> 16) | (readl(regs + 0x72c) & 0xffff0000U), regs + 0x638);
	wmb();
	writel(1, regs + 0x684);
	end = ktime_get_ns() + ENGINE_WAIT_NS;
	while (!(readl(regs + REG_EVENT_STATUS2) & 1U))
		if (ktime_get_ns() > end)
			return POULSBACK_ERRSITE(-ETIMEDOUT);
	writel(1U | 0x80U, regs + REG_HOST_CLEAR2); /* free-list load, TRIG_TA */
	writel(dms & ~1U, regs + 0xa74);
	return 0;
}

static void engine_oom_work(struct work_struct *work)
{
	struct poulsback_engine *e = container_of(work, struct poulsback_engine, oom_work);
	int ret = engine_zls(e, ENGINE_DPM_PAGES - 4);
	if (ret) {
		engine_wedge(e, ret);
		return;
	}
	e->zls_raised = true;
	e->oom_resumes++;
	writel(1, e->regs + 0x69c); /* DPM_OUTOFMEM RESTART */
}

static void engine_raster_work(struct work_struct *work)
{
	struct poulsback_engine *e = container_of(work, struct poulsback_engine, raster_work);
	struct poulsback_job *job;
	unsigned long irq = sgx535_engine_irq_lock();
	int ret;
	u64 start = ktime_get_ns();
	job = e->start_raster;
	e->start_raster = NULL;
	if (job && e->state == ENGINE_READY) {
		/* The raster frees the DPM context its TA allocated in. */
		job->events = 0;
		e->raster_job = job;
		e->dpm_id = (e->dpm_id & ~DPM_CONTEXT_DALLOC(1)) | DPM_CONTEXT_DALLOC(job->dpm);
		writel(e->dpm_id, e->regs + REG_DPM_CONTEXT_ID);
		engine_route_locked(e);
	}
	if (e->state != ENGINE_READY)
		job = NULL;
	sgx535_engine_irq_unlock(irq);
	if (!job)
		return;
	ret = engine_actions(e->regs, engine_isp_reset, ARRAY_SIZE(engine_isp_reset));
	if (!ret)
		ret = engine_actions(e->regs, job->raster, job->raster_count);
	if (ret)
		engine_wedge(e, ret);
	e->raster_ns += ktime_get_ns() - start;
}

/* A 2D job: slot 1 at its directory, then its words. No TA pass runs; a
 * raster pass may. */
static int engine_start2d(struct poulsback_engine *e, struct poulsback_job *job)
{
	void __iomem *regs = e->regs;
	unsigned long irq;
	int ret = 0;
	/* CPU writes to WC surfaces reach memory before the core reads them;
	 * the core starts idle with an empty slave port. */
	mb();
	if (readl(regs + REG_TWOD_SOCIF) != TWOD_SOCIF_EMPTY ||
	    (readl(regs + REG_TWOD_BLIT_STATUS) & TWOD_BUSY))
		ret = POULSBACK_ERRSITE(-EIO);
	irq = sgx535_engine_irq_lock();
	if (e->state != ENGINE_READY) {
		sgx535_engine_irq_unlock(irq);
		return -EIO; /* the wedge held the job */
	}
	writel(poulsback_vm_directory(job->vm), regs + REG_BIF_BASE1);
	job->start = readl(regs + REG_TWOD_BLIT_STATUS) & TWOD_COUNT;
	e->job2d = job;
	if (e->raster_job)
		e->overlapped++;
	engine_route_locked(e);
	sgx535_engine_irq_unlock(irq);
	if (ret)
		engine_wedge(e, ret);
	return ret;
}

/* Starts a 2D job due to start, then feeds the running one's words to the
 * slave port as SOCIF has room; the only writer. Bit 27 requeues it while
 * words remain, and when the count is complete but the core still busy, which
 * it then waits out. A job cannot finish before its last word is fed, so it
 * stays allocated while words remain; otherwise it is touched only under the
 * IRQ lock. */
static void engine_twod_work(struct work_struct *work)
{
	struct poulsback_engine *e = container_of(work, struct poulsback_engine, twod_work);
	void __iomem *regs = e->regs;
	u64 start = ktime_get_ns(), end = start + ENGINE_WAIT_NS;
	unsigned long irq = sgx535_engine_irq_lock();
	struct poulsback_job *due = e->start2d;
	bool waited = false;
	e->start2d = NULL;
	sgx535_engine_irq_unlock(irq);
	if (due && engine_start2d(e, due))
		return;
	for (;;) {
		enum twod_state state = TWOD_RUNNING;
		struct poulsback_job *job;
		irq = sgx535_engine_irq_lock();
		job = e->job2d;
		u32 room = 0, from = 0, i;
		if (!job) {
			sgx535_engine_irq_unlock(irq);
			break;
		}
		if (job->fed < job->count) {
			from = job->fed;
			room = min3(readl(regs + REG_TWOD_SOCIF) & TWOD_SOCIF_FREE, TWOD_BURST,
				    job->count - from);
		} else {
			state = engine_twod_check_locked(e, job);
			if (state == TWOD_BUSY_DONE && !waited) {
				waited = true;
				e->twod_busy_waits++;
			}
		}
		sgx535_engine_irq_unlock(irq);
		if (room) {
			for (i = 0; i < room; i++)
				writel(job->words[from + i], regs + REG_TWOD_SLAVE_PORT + 4 * i);
			(void)readl(regs + REG_TWOD_SLAVE_PORT + 4 * (room - 1));
			irq = sgx535_engine_irq_lock();
			job->fed += room;
			e->twod_words += room;
			sgx535_engine_irq_unlock(irq);
			continue;
		}
		if (state != TWOD_BUSY_DONE)
			break;
		if (ktime_get_ns() > end) {
			engine_wedge(e, POULSBACK_ERRSITE(-ETIMEDOUT));
			break;
		}
		usleep_range(5, 20);
	}
	e->twod_ns += ktime_get_ns() - start;
}

/* Clears the scene root the TA starts from (engine121 scene clear pages). */
static void engine_clear_root(struct poulsback_job *job)
{
	u32 i;
	for (i = job->clear_start; i < job->clear_start + job->clear_count; i++) {
		void *v = page_address(job->scene->pages[i]);
		memset(v, 0, PAGE_SIZE);
		clflush_cache_range(v, PAGE_SIZE);
	}
	mb();
}

/* A 3D job's TA pass: the scene root cleared, ZLS back from an OOM resume,
 * slot 0 at its directory, the TA program. No 2D job and no other 3D job runs. */
/* The 3D's USSE idle: the PDS stops issuing vertex, pixel and event tasks (DMS_CTRL bits 2:0), then
 * both pipes' pixel queues drain and their task slots go idle. *DMS gets the
 * value to restore. */
static int engine_usse_idle(void __iomem *regs, u32 *dms)
{
	u64 end = ktime_get_ns() + ENGINE_WAIT_NS;
	*dms = readl(regs + REG_DMS_CTRL);
	writel(*dms | DMS_DISABLE_VERTEX_PIXEL_EVENT, regs + REG_DMS_CTRL);
	while (!(readl(regs + REG_USE0_SERV_PIXEL) & readl(regs + REG_USE1_SERV_PIXEL) &
		 USE_SERV_EMPTY) ||
	       readl(regs + REG_USE0_DM_SLOT) != USE_DM_SLOTS_IDLE ||
	       readl(regs + REG_USE1_DM_SLOT) != USE_DM_SLOTS_IDLE)
		if (ktime_get_ns() > end)
			return POULSBACK_ERRSITE(-ETIMEDOUT);
	return 0;
}

/* A TA's program and USE cache invalidate, then its kick (the plan's last
 * action). Beside a raster, everything before the kick runs with the 3D's
 * USSE idle: the USE cache invalidate requires it, and the plan's PDS and
 * MADD invalidates and DPM CLEARs then reach no pixel task either
 * (docs/kernel/execution.md). */
static int engine_ta_program(void __iomem *regs, const struct poulsback_job *job,
			     bool beside_raster)
{
	u32 dms = 0;
	int ret = 0;
	if (beside_raster)
		ret = engine_usse_idle(regs, &dms);
	if (!ret)
		ret = engine_actions(regs, job->ta, job->ta_count - 1);
	if (!ret) {
		writel(1, regs + REG_USE_CACHE);
		(void)readl(regs + REG_USE_CACHE);
	}
	if (beside_raster)
		writel(dms, regs + REG_DMS_CTRL);
	if (!ret)
		ret = engine_actions(regs, job->ta + job->ta_count - 1, 1);
	return ret;
}

static void engine_start_ta(struct poulsback_engine *e, struct poulsback_job *job)
{
	void __iomem *regs = e->regs;
	struct poulsback_job *holder;
	unsigned long irq;
	bool alone;
	int ret = 0;
	irq = sgx535_engine_irq_lock();
	alone = !e->ready_job && !e->start_raster && !e->raster_job;
	sgx535_engine_irq_unlock(irq);
	/* An OOM resume left ZLS in the reserve: back to N - 477 while the TA
	 * is idle and no raster frees pages into the list. */
	if (e->zls_raised && alone) {
		ret = engine_zls(e, ENGINE_DPM_PAGES - 477);
		e->zls_raised = ret != 0;
	}
	engine_clear_root(job);
	irq = sgx535_engine_irq_lock();
	if (e->state != ENGINE_READY) {
		sgx535_engine_irq_unlock(irq);
		return; /* the wedge held the job */
	}
	/* Beside a job that holds a DPM context: its directory (joins) and the
	 * other context; alone: the job's directory, contexts in turn. */
	holder = e->raster_job ? e->raster_job : e->start_raster ? e->start_raster : e->ready_job;
	if (holder) {
		job->dpm = !holder->dpm;
		e->ta_in_raster++;
	} else {
		writel(poulsback_vm_directory(job->vm), regs + REG_BIF_BASE0);
		job->dpm = e->dpm_next;
	}
	e->dpm_next = !job->dpm;
	e->dpm_id = (e->dpm_id & DPM_CONTEXT_DALLOC(1)) | DPM_CONTEXT_ALLOC_LS(job->dpm);
	writel(e->dpm_id, regs + REG_DPM_CONTEXT_ID);
	e->ta_job = job;
	engine_route_locked(e);
	sgx535_engine_irq_unlock(irq);
	if (!ret)
		ret = engine_ta_program(regs, job, holder != NULL);
	if (ret)
		engine_wedge(e, ret);
}

static void engine_start_work(struct work_struct *work)
{
	struct poulsback_engine *e = container_of(work, struct poulsback_engine, start_work);
	unsigned long irq = sgx535_engine_irq_lock();
	struct poulsback_job *job = e->start_ta;
	u64 start = ktime_get_ns();
	e->start_ta = NULL;
	sgx535_engine_irq_unlock(irq);
	if (job)
		engine_start_ta(e, job);
	e->run_ns += ktime_get_ns() - start;
}

/* A job joins the engine: it starts when the overlap rules let it (the TA in
 * start_work, a 2D job in twod_work), else waits for the event that does. */
static struct dma_fence *engine_run_job(struct drm_sched_job *sched_job)
{
	struct poulsback_job *job = container_of(sched_job, struct poulsback_job, base);
	struct poulsback_engine *e = READ_ONCE(engine);
	struct poulsback_overlap_start st;
	struct dma_fence *done;
	unsigned long irq;
	bool fits = true;
	if (e->state != ENGINE_READY)
		return ERR_PTR(-EIO);
	if (sched_job->s_fence->finished.error)
		return ERR_PTR(-ECANCELED);
	done = kzalloc(sizeof(*done), GFP_KERNEL);
	if (!done)
		return ERR_PTR(-ENOMEM);
	atomic_inc(&e->fences);
	dma_fence_init(done, &engine_fence_ops, &e->fence_lock, e->fence_context, ++e->seqno);
	job->done = done;
	if (job->twod && !job->count) {
		/* Every operation was clipped away. */
		dma_fence_signal(done);
		return dma_fence_get(done);
	}
	engine_qos_busy(e);
	irq = sgx535_engine_irq_lock();
	if (e->state != ENGINE_READY) {
		engine_hold_job_locked(e, job);
	} else if (job->twod) {
		fits = !e->wait2d;
		if (fits) {
			e->wait2d = job;
			fits = poulsback_overlap_event(&e->ov, POULSBACK_OVERLAP_ARRIVE2D,
						      engine_joins_locked(e), &st);
			if (!fits)
				e->wait2d = NULL;
		}
		engine_apply_locked(e, fits, &st);
	} else {
		u32 i = 0;
		while (i < POULSBACK_OVERLAP_CREDITS_3D && e->wait3d[i])
			i++;
		fits = i < POULSBACK_OVERLAP_CREDITS_3D;
		if (fits) {
			e->wait3d[i] = job;
			fits = poulsback_overlap_event(&e->ov, POULSBACK_OVERLAP_ARRIVE3D,
						      engine_joins_locked(e), &st);
			if (!fits)
				e->wait3d[i] = NULL;
		}
		engine_apply_locked(e, fits, &st);
	}
	if (!fits && !job->held)
		engine_hold_job_locked(e, job);
	sgx535_engine_irq_unlock(irq);
	return dma_fence_get(done);
}

/* A timeout wedges the device; nothing is retried or reset (docs/kernel/execution.md). */
static enum drm_gpu_sched_stat engine_timedout_job(struct drm_sched_job *job)
{
	struct poulsback_engine *e = READ_ONCE(engine);
	if (e)
		engine_wedge(e, -ETIMEDOUT);
	return DRM_GPU_SCHED_STAT_NOMINAL;
}

void poulsback_job_release(struct poulsback_job *job)
{
	u32 i;
	for (i = 0; i < job->bo_count; i++)
		poulsback_bo_release(job->bos[i], job->vm);
	kvfree(job->bos);
	kvfree(job->words);
	poulsback_scene_put(job->scene);
	poulsback_vm_put(job->vm);
	if (job->done)
		dma_fence_put(job->done);
	kfree(job);
}

static void engine_free_job(struct drm_sched_job *sched_job)
{
	struct poulsback_job *job = container_of(sched_job, struct poulsback_job, base);
	drm_sched_job_cleanup(sched_job);
	/* A held job's buffers, scene and address space stay referenced. */
	if (!job->held)
		poulsback_job_release(job);
}

static const struct drm_sched_backend_ops engine_sched_ops = {
	.run_job = engine_run_job,
	.timedout_job = engine_timedout_job,
	.free_job = engine_free_job,
};

static int engine_show(struct seq_file *m, void *unused)
{
	struct poulsback_engine *e = m->private;
	void __iomem *regs = e->regs;
	static const char *const states[] = {"down", "ready", "wedged"};
	u32 kept_maps, kept_spaces;
	u64 kept_bytes;
	mutex_lock(&engine_lock);
	seq_printf(m, "jobs %u oom_resumes %u zls_raised %u ta_pages %u max %u\n", e->jobs,
		   e->oom_resumes, e->zls_raised, e->ta_pages, e->ta_pages_max);
	seq_printf(m, "cpu_us_per_job submit %llu run %llu raster %llu (submits %u)\n",
		   e->submits ? div_u64(e->submit_ns, 1000 * e->submits) : 0,
		   e->jobs ? div_u64(e->run_ns, 1000 * e->jobs) : 0,
		   e->jobs ? div_u64(e->raster_ns, 1000 * e->jobs) : 0, e->submits);
	seq_printf(m, "twod jobs %u submits %u words %u irqs %u busy_waits %u cpu_us_per_job submit %llu run %llu\n",
		   e->twod_jobs, e->twod_submits, e->twod_words, e->twod_irqs, e->twod_busy_waits,
		   e->twod_submits ? div_u64(e->twod_submit_ns, 1000 * e->twod_submits) : 0,
		   e->twod_jobs ? div_u64(e->twod_ns, 1000 * e->twod_jobs) : 0);
	poulsback_vm_kept(&kept_maps, &kept_bytes, &kept_spaces);
	seq_printf(m, "state %s error %d bringup_us %llu irqs %u\n", states[e->state], e->error,
		   div_u64(e->bringup_ns, 1000), e->irqs);
	seq_printf(m, "lost held %u kept_maps %u kept_bytes %llu kept_spaces %u\n", e->held,
		   kept_maps, kept_bytes, kept_spaces);
	seq_printf(m, "overlap twod_in_raster %u ta_beside_3d %u base1 %08x\n", e->overlapped,
		   e->ta_in_raster, readl(regs + REG_BIF_BASE1));
	seq_printf(m, "last_event %08x %08x fault %08x at %08x\n", e->last_event1, e->last_event2,
		   e->fault_stat, e->fault_addr);
	seq_printf(m, "bank0 %08x base0 %08x base15 %08x directory %08x\n",
		   readl(regs + REG_BIF_BANK0), readl(regs + REG_BIF_BASE0),
		   readl(regs + REG_BIF_BASE15),
		   e->directory ? (u32)page_to_phys(e->directory) : 0);
	seq_printf(m, "host_enable %08x %08x event %08x %08x\n", readl(regs + REG_HOST_ENABLE),
		   readl(regs + REG_HOST_ENABLE2), readl(regs + REG_EVENT_STATUS),
		   readl(regs + REG_EVENT_STATUS2));
	seq_printf(m, "dpm 620 %08x 624 %08x 628 %08x 668 %08x\n", readl(regs + 0x620),
		   readl(regs + 0x624), readl(regs + 0x628), readl(regs + 0x668));
	seq_printf(m, "dpm context_id %08x (shadow %08x, next TA %u) pb_base %08x pages %08x\n",
		   readl(regs + REG_DPM_CONTEXT_ID), e->dpm_id, e->dpm_next,
		   readl(regs + 0x664), readl(regs + 0x724));
	seq_printf(m, "req twod %08x ta %08x 3d %08x pds %08x\n", readl(regs + REG_TWOD_REQ_BASE),
		   readl(regs + REG_TA_REQ_BASE), readl(regs + REG_3D_REQ_BASE),
		   readl(regs + REG_PDS_EXEC_BASE));
	seq_printf(m, "twod blit_status %08x socif %08x\n", readl(regs + REG_TWOD_BLIT_STATUS),
		   readl(regs + REG_TWOD_SOCIF));
	seq_printf(m, "use dms %08x dm_slot %08x %08x serv_pixel %08x %08x\n",
		   readl(regs + REG_DMS_CTRL), readl(regs + REG_USE0_DM_SLOT),
		   readl(regs + REG_USE1_DM_SLOT), readl(regs + REG_USE0_SERV_PIXEL),
		   readl(regs + REG_USE1_SERV_PIXEL));
	mutex_unlock(&engine_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(engine);

/* The two DPM areas (parameter pool, tables), zeroed and flushed, and the
 * kernel directory with the page tables that map them. */
static int engine_memory(struct poulsback_engine *e)
{
	static const u32 area_va[2] = {POULSBACK_VM_PARAM_VA, POULSBACK_VM_TABLE_VA};
	u32 area, i, *dir, *table, word;
	e->directory = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!e->directory)
		return -ENOMEM;
	dir = page_address(e->directory);
	for (area = 0; area < 2; area++) {
		e->dpm[area] = kvcalloc(ENGINE_DPM_PAGES, sizeof(struct page *), GFP_KERNEL);
		if (!e->dpm[area])
			return -ENOMEM;
		for (i = 0; i < ENGINE_DPM_PAGES; i++) {
			e->dpm[area][i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!e->dpm[area][i])
				return -ENOMEM;
			clflush_cache_range(page_address(e->dpm[area][i]), PAGE_SIZE);
		}
		for (i = 0; i < ENGINE_TABLES_PER_AREA; i++) {
			u32 t = area * ENGINE_TABLES_PER_AREA + i, j;
			e->tables[t] = alloc_page(GFP_KERNEL | __GFP_ZERO);
			if (!e->tables[t])
				return -ENOMEM;
			table = page_address(e->tables[t]);
			for (j = 0; j < 1024; j++)
				if (poulsback_vm_pte(page_to_phys(e->dpm[area][i * 1024 + j]), 0,
						     &table[j]))
					return POULSBACK_ERRSITE(-EFAULT);
			clflush_cache_range(table, PAGE_SIZE);
			if (poulsback_vm_pde(page_to_phys(e->tables[t]), &word))
				return POULSBACK_ERRSITE(-EFAULT);
			e->table_index[t] = poulsback_vm_pde_index(area_va[area]) + i;
			e->table_pde[t] = word;
			dir[e->table_index[t]] = word;
		}
	}
	clflush_cache_range(dir, PAGE_SIZE);
	return 0;
}

/* USE code windows: window w is bank 3 + 2w (vertex) and 4 + 2w (pixel). */
static int engine_code_banks(void __iomem *regs)
{
	u32 w, dm;
	for (w = 0; w < POULSBACK_VM_CODE_WINDOWS; w++)
		for (dm = 0; dm < 2; dm++) {
			struct sgx535_use_code_bank_input in = {
				.bank_index = 3 + 2 * w + dm,
				.data_master = dm,
				.code_va_bytes = POULSBACK_VM_CODE_VA + w * POULSBACK_VM_CODE_BANK,
				.code_size_bytes = 8,
				.entry_va_bytes = POULSBACK_VM_CODE_VA + w * POULSBACK_VM_CODE_BANK,
			};
			struct sgx535_use_code_bank_projection bank;
			if (sgx535_use_code_bank_project(&in, &bank) != SGX535_USE_CODE_BANK_OK)
				return POULSBACK_ERRSITE(-EINVAL);
			writel(bank.register_word, regs + bank.register_offset_bytes);
		}
	writel(POULSBACK_VM_PDS_EXEC_BASE, regs + REG_PDS_EXEC_BASE);
	return 0;
}

/* Reprogramming needs both DPM areas zeroed and flushed again; fresh
 * allocations are already zeroed (docs/kernel/execution.md). */
static void engine_zero_dpm(struct poulsback_engine *e)
{
	u32 area, i;
	for (area = 0; area < 2; area++)
		for (i = 0; i < ENGINE_DPM_PAGES; i++) {
			void *v = page_address(e->dpm[area][i]);
			clear_page(v);
			clflush_cache_range(v, PAGE_SIZE);
		}
}

/* Allocation is the memory half of bring-up (docs/kernel/execution.md).
 * The DPM pages and the kernel directory with the tables that map them, the
 * work queues, the CPU latency request and both schedulers; nothing touches
 * the SGX. The release helper frees all of it. */
static int engine_alloc(struct poulsback_engine *e)
{
	int ret = engine_memory(e);
	if (ret)
		return ret;
	e->wq = alloc_ordered_workqueue("poulsback-engine", WQ_HIGHPRI);
	e->wq2d = alloc_ordered_workqueue("poulsback-engine-2d", WQ_HIGHPRI);
	if (!e->wq || !e->wq2d)
		return -ENOMEM;
	INIT_WORK(&e->start_work, engine_start_work);
	INIT_WORK(&e->raster_work, engine_raster_work);
	INIT_WORK(&e->oom_work, engine_oom_work);
	INIT_WORK(&e->twod_work, engine_twod_work);
	INIT_DELAYED_WORK(&e->qos_relax, engine_qos_relax);
	mutex_init(&e->qos_lock);
	cpu_latency_qos_add_request(&e->cpu_qos, PM_QOS_DEFAULT_VALUE);
	spin_lock_init(&e->fence_lock);
	e->fence_context = dma_fence_context_alloc(1);
	/* 3D jobs: a raster, a TA or ready job and the next one waiting in the
	 * engine; 2D jobs: one (poulsback_overlap.h). */
	ret = drm_sched_init(&e->sched, &engine_sched_ops, POULSBACK_OVERLAP_CREDITS_3D, 0,
			     msecs_to_jiffies(ENGINE_JOB_TIMEOUT_MS), NULL, NULL, "poulsback",
			     e->dev->dev);
	if (ret)
		return ret;
	return drm_sched_init(&e->sched2d, &engine_sched_ops, 1, 0,
			      msecs_to_jiffies(ENGINE_JOB_TIMEOUT_MS), NULL, NULL, "poulsback-2d",
			      e->dev->dev);
}

/* Register programming needs allocated memory, no job in the engine and
 * the SGX state established by probe (docs/kernel/execution.md). */
static int engine_program(struct poulsback_engine *e)
{
	struct sgx535_engine121_action actions[SGX535_ENGINE121_DPM_ACTIONS];
	struct sgx535_engine121_dpm dpm = {
		.table_storage = 1,
		.table_gpu_va_bytes = POULSBACK_VM_TABLE_VA,
		.table_allocation_bytes = (u64)ENGINE_DPM_PAGES * PAGE_SIZE,
		.parameter_storage = 2,
		.parameter_gpu_va_bytes = POULSBACK_VM_PARAM_VA,
		.parameter_bytes = (u64)ENGINE_DPM_PAGES * PAGE_SIZE,
		.raster_geometry_base_bytes = POULSBACK_VM_PARAM_VA,
		.thresholds = SGX535_ENGINE121_THRESHOLDS_ENGINE,
	};
	void __iomem *regs = e->regs;
	unsigned long irq;
	size_t n;
	int ret = 0;
	/* As left by probe: idle, reset released, BIF at gma500's defaults. */
	if (readl(regs + REG_CORE_ID) != SGX535_ENGINE121_CORE_ID ||
	    readl(regs + REG_CORE_REVISION) != SGX535_ENGINE121_REVISION ||
	    readl(regs + REG_SOFT_RESET) || (readl(regs + REG_EVENT_STATUS) & ~(1U << 31)) ||
	    readl(regs + REG_EVENT_STATUS2) || readl(regs + REG_BIF_BANK0) ||
	    readl(regs + REG_3D_REQ_BASE) != POULSBACK_VM_PARAM_VA ||
	    readl(regs + REG_TWOD_REQ_BASE) != POULSBACK_VM_TWOD_REQ_BASE)
		return POULSBACK_ERRSITE(-ENODEV);
	memset(&e->ov, 0, sizeof(e->ov));
	e->dpm_id = e->dpm_next = 0;
	e->zls_raised = false;
	if (e->programmed)
		engine_zero_dpm(e);
	ret = engine_code_banks(regs);
	if (ret)
		return ret;
	if (sgx535_engine121_bootstrap_build(SGX535_ENGINE121_CORE_ID, SGX535_ENGINE121_REVISION,
					     actions, SGX535_ENGINE121_BOOTSTRAP_ACTIONS, &n))
		return POULSBACK_ERRSITE(-EINVAL);
	ret = engine_actions(regs, actions, n);
	if (ret)
		return ret;
	writel((u32)page_to_phys(e->directory), regs + REG_BIF_BASE15);
	writel(ENGINE_BANK0_IDLE, regs + REG_BIF_BANK0);
	writel(POULSBACK_VM_PARAM_VA, regs + REG_TA_REQ_BASE);
	ret = engine_invaldc(regs);
	if (ret)
		return ret;
	if (sgx535_engine121_dpm_build(&dpm, actions, SGX535_ENGINE121_DPM_ACTIONS, &n))
		return POULSBACK_ERRSITE(-EINVAL);
	ret = engine_actions(regs, actions, n);
	if (ret)
		return ret;
	irq = sgx535_engine_irq_lock();
	if (!e->routed) {
		ret = sgx535_engine_irq_bind_executor_locked(e->dev, engine_irq);
		e->routed = !ret;
	}
	if (!ret) {
		writel(ENGINE_HOST_ENABLE2, regs + REG_HOST_ENABLE2);
		writel(ENGINE_HOST_ENABLE, regs + REG_HOST_ENABLE);
		(void)readl(regs + REG_HOST_ENABLE);
		e->programmed = true;
	}
	sgx535_engine_irq_unlock(irq);
	return ret;
}

/* The first claim allocates memory, then programs the engine. */
static int engine_bringup(struct poulsback_engine *e)
{
	int ret = sgx535_epoch_executor_claim(e->dev);
	if (!ret)
		ret = engine_alloc(e);
	if (!ret)
		ret = engine_program(e);
	return ret;
}

/* State for quiesce and release, read under the engine IRQ lock. */
static struct poulsback_teardown_engine engine_teardown_state(struct poulsback_engine *e)
{
	unsigned long irq = sgx535_engine_irq_lock();
	struct poulsback_job *const slots[] = {
		e->ta_job,    e->ready_job, e->raster_job, e->job2d,	    e->wait3d[0], e->wait3d[1],
		e->wait3d[2], e->wait2d,    e->start_ta,   e->start_raster, e->start2d};
	struct poulsback_teardown_engine s = {
		.ready = e->state == ENGINE_READY,
		.quiesced = e->quiesced,
		.held = e->held,
		.fences = (u32)atomic_read(&e->fences),
	};
	u32 i;
	for (i = 0; i < ARRAY_SIZE(slots); i++)
		s.slots += slots[i] != NULL;
	sgx535_engine_irq_unlock(irq);
	return s;
}

static void engine_unpark(struct poulsback_engine *e)
{
	kthread_unpark(e->sched.thread);
	kthread_unpark(e->sched2d.thread);
}

int poulsback_engine_quiesce(struct drm_device *dev)
{
	struct poulsback_teardown_engine s;
	enum poulsback_teardown_refusal r;
	struct poulsback_engine *e;
	u64 end;
	int ret = 0;
	mutex_lock(&engine_lock);
	e = engine;
	if (!e || e->dev != dev || e->quiesced)
		goto unlock;
	s = engine_teardown_state(e);
	r = poulsback_teardown_drained(&s);
	if (r == POULSBACK_TEARDOWN_LOST || r == POULSBACK_TEARDOWN_HELD) {
		ret = POULSBACK_ERRSITE(-EBUSY);
		goto unlock;
	}
	kthread_park(e->sched.thread);
	kthread_park(e->sched2d.thread);
	end = ktime_get_ns() + (u64)ENGINE_QUIESCE_MS * NSEC_PER_MSEC;
	while (r == POULSBACK_TEARDOWN_BUSY && ktime_get_ns() < end) {
		msleep(1);
		s = engine_teardown_state(e);
		r = poulsback_teardown_drained(&s);
	}
	/* A job outliving two timeouts is an engine bug: it wedges (HOLD). */
	if (r == POULSBACK_TEARDOWN_BUSY)
		engine_wedge(e, POULSBACK_ERRSITE(-ETIMEDOUT));
	if (!r) {
		struct poulsback_teardown_idle idle = {
			.event1 = readl(e->regs + REG_EVENT_STATUS),
			.event2 = readl(e->regs + REG_EVENT_STATUS2),
			.blit_status = readl(e->regs + REG_TWOD_BLIT_STATUS),
			.bif_int_stat = readl(e->regs + REG_BIF_INT_STAT),
		};
		r = poulsback_teardown_idle(&idle);
	}
	if (r) {
		engine_unpark(e);
		ret = POULSBACK_ERRSITE(-EBUSY);
	} else {
		e->quiesced = true;
	}
unlock:
	mutex_unlock(&engine_lock);
	return ret;
}

int poulsback_engine_power_down(struct drm_device *dev)
{
	struct poulsback_engine *e;
	unsigned long irq;
	int ret = 0;
	u32 i;
	mutex_lock(&engine_lock);
	e = engine;
	if (!e || e->dev != dev)
		goto unlock;
	if (!e->quiesced) {
		ret = POULSBACK_ERRSITE(-EBUSY);
		goto unlock;
	}
	/* The host enables go to 0 first: no SGX event arrives unowned. */
	irq = sgx535_engine_irq_lock();
	for (i = 0; i < POULSBACK_TEARDOWN_POWER_DOWN_WRITES; i++)
		writel(poulsback_teardown_power_down[i].value,
		       e->regs + poulsback_teardown_power_down[i].offset);
	(void)readl(e->regs + REG_HOST_ENABLE);
	sgx535_engine_irq_unlock(irq);
unlock:
	mutex_unlock(&engine_lock);
	return ret;
}

/* Engine-owned memory: the DPM pages, the tables, the kernel directory. */
static u32 engine_free_memory(struct poulsback_engine *e)
{
	u32 area, i, pages = 0;
	for (area = 0; area < 2; area++) {
		for (i = 0; e->dpm[area] && i < ENGINE_DPM_PAGES; i++)
			if (e->dpm[area][i]) {
				__free_page(e->dpm[area][i]);
				pages++;
			}
		kvfree(e->dpm[area]);
	}
	for (i = 0; i < ENGINE_DEVICE_TABLES; i++)
		if (e->tables[i]) {
			__free_page(e->tables[i]);
			pages++;
		}
	if (e->directory) {
		__free_page(e->directory);
		pages++;
	}
	return pages;
}

int poulsback_engine_release(struct drm_device *dev)
{
	struct poulsback_teardown_engine s;
	struct poulsback_engine *e;
	unsigned long irq;
	int ret = 0;
	u32 pages;
	mutex_lock(&engine_lock);
	e = engine;
	if (!e || e->dev != dev)
		goto unlock;
	s = engine_teardown_state(e);
	if (poulsback_teardown_may_free(&s)) {
		ret = POULSBACK_ERRSITE(-EBUSY);
		goto unlock;
	}
	drm_sched_fini(&e->sched);
	drm_sched_fini(&e->sched2d);
	irq = sgx535_engine_irq_lock();
	sgx535_engine_irq_unbind_executor_locked(e->dev);
	sgx535_engine_irq_unlock(irq);
	cancel_work_sync(&e->start_work);
	cancel_work_sync(&e->raster_work);
	cancel_work_sync(&e->oom_work);
	cancel_work_sync(&e->twod_work);
	destroy_workqueue(e->wq);
	destroy_workqueue(e->wq2d);
	cancel_delayed_work_sync(&e->qos_relax);
	cpu_latency_qos_remove_request(&e->cpu_qos);
	debugfs_remove(e->debugfs);
	pages = engine_free_memory(e);
	sgx535_epoch_executor_release(e->dev);
	WRITE_ONCE(engine, NULL);
	drm_info(dev, "GPU engine freed: %u pages, %u holds\n", pages, s.held);
	kfree(e);
	module_put(THIS_MODULE);
unlock:
	mutex_unlock(&engine_lock);
	return ret;
}

struct poulsback_engine *poulsback_engine_get(struct drm_device *dev)
{
	struct poulsback_engine *e;
	u64 start;
	int ret;
	mutex_lock(&engine_lock);
	e = engine;
	if (e) {
		ret = e->state == ENGINE_READY ? 0 : -EIO;
		goto unlock;
	}
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		ret = -ENOMEM;
		goto unlock;
	}
	e->dev = dev;
	e->regs = to_drm_psb_private(dev)->sgx_reg;
	/* Published first: the IRQ route bound during bring-up reads it. */
	WRITE_ONCE(engine, e);
	start = ktime_get_ns();
	ret = engine_bringup(e);
	e->bringup_ns = ktime_get_ns() - start;
	if (ret == -EBUSY) {
		/* PM or removal closed the SGX source this boot; nothing was touched. */
		WRITE_ONCE(engine, NULL);
		kfree(e);
		goto unlock;
	}
	/* A retained engine pins the module, ready or not: its scheduler threads,
	 * work queues, IRQ route and fences run module text, and gma500's unload
	 * cannot refuse. Only F drops the reference, and teardown does not call F
	 * yet, so rmmod is refused once the engine exists. */
	__module_get(THIS_MODULE);
	/* A failed bring-up keeps everything it allocated or wrote (HOLD), and
	 * is a loss like any wedge: -EIO from here on. */
	e->state = ret ? ENGINE_WEDGED : ENGINE_READY;
	e->error = ret;
	if (ret)
		poulsback_vm_freeze();
	if (dev->primary && dev->primary->debugfs_root)
		e->debugfs = debugfs_create_file("poulsback_engine", 0444,
						 dev->primary->debugfs_root, e, &engine_fops);
	drm_info(dev, "GPU engine %s in %llu us (%d)\n", ret ? "wedged" : "ready",
		 div_u64(e->bringup_ns, 1000), ret);
	if (ret) {
		engine_lost_note(e, 0);
		ret = -EIO;
	}
unlock:
	mutex_unlock(&engine_lock);
	return ret ? ERR_PTR(ret) : e;
}

bool poulsback_engine_retained(struct drm_device *dev)
{
	bool retained;
	mutex_lock(&engine_lock);
	retained = engine && engine->dev == dev;
	mutex_unlock(&engine_lock);
	return retained;
}

void poulsback_engine_account_submit(struct poulsback_engine *e, bool twod, u64 ns)
{
	if (twod) {
		e->twod_submit_ns += ns;
		e->twod_submits++;
	} else {
		e->submit_ns += ns;
		e->submits++;
	}
}

struct drm_gpu_scheduler *poulsback_engine_scheduler(struct poulsback_engine *e, bool twod)
{
	return twod ? &e->sched2d : &e->sched;
}

void poulsback_engine_link_vm(struct poulsback_engine *e, struct poulsback_vm *vm)
{
	poulsback_vm_link(vm, e->table_index, e->table_pde, ENGINE_DEVICE_TABLES);
}
