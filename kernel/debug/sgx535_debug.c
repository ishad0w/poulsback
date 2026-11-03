// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include <linux/capability.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "sgx535_debug.h"
#define CREATE_TRACE_POINTS
#include "sgx535_trace.h"
#define DIAG_RING 64U
struct record {
	u64 seq, id, epoch, serial, bytes;
	u32 kind, state, reason;
	s32 error;
};
struct snapshot {
	u64 seen, head, tail, dropped, errors;
	u32 debug; /* the parameter when the snapshot was taken */
	bool online;
	struct record last[SGX535_DIAG_KINDS], last_error, ring[DIAG_RING];
};
static DEFINE_SPINLOCK(diag_lock);
static DEFINE_MUTEX(diag_lifecycle);
static struct snapshot cache;
static struct proc_dir_entry *entry;
/* References under diag_lifecycle (psb_init holds one). */
static unsigned int diag_users;
/* The module's single parameter: the SGX535_DEBUG_* categories. */
static unsigned int debug;
static void emit(struct work_struct *);
static DECLARE_WORK(emit_work, emit);
static int debug_set(const char *value, const struct kernel_param *param)
{
	unsigned int mask;
	int ret;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = kstrtouint(value, 0, &mask);
	if (ret)
		return ret;
	WRITE_ONCE(*(unsigned int *)param->arg, mask);
	return 0;
}
static const struct kernel_param_ops debug_ops = {.set = debug_set, .get = param_get_uint};
module_param_cb(debug, &debug_ops, &debug, 0600);
MODULE_PARM_DESC(debug, "Debug categories, root-writable at runtime: 0x1 core (epoch and lease "
			"records), 0x2 mmu, 0x4 irq, 0x8 submit (refused requests), 0x10 display "
			"(vblank and flip timing), 0x20 also dmesg (rate-limited)");

bool sgx535_debug_on(u32 categories)
{
	return READ_ONCE(debug) & categories;
}

void sgx535_diag_record(u32 kind, u64 id, u64 epoch, u64 serial, u32 state, s32 error, u32 reason,
			u64 bytes)
{
	struct record r = {.id = id,
			   .epoch = epoch,
			   .serial = serial,
			   .bytes = bytes,
			   .kind = kind,
			   .state = state,
			   .error = error,
			   .reason = reason};
	unsigned long flags;
	if (!kind || kind >= SGX535_DIAG_KINDS)
		return;
	spin_lock_irqsave(&diag_lock, flags);
	r.seq = ++cache.seen;
	cache.last[kind] = r;
	if (error || reason) {
		cache.errors++;
		cache.last_error = r;
	}
	if ((READ_ONCE(debug) & SGX535_DEBUG_CORE) || error || reason) {
		cache.ring[cache.head % DIAG_RING] = r;
		cache.head++;
		if (cache.online)
			queue_work(system_unbound_wq, &emit_work);
	}
	spin_unlock_irqrestore(&diag_lock, flags);
	/* Trace/log delivery runs on a worker, never within caller critical locks. */
}
static void emit(struct work_struct *work)
{
	struct record batch[8];
	unsigned long flags;
	unsigned int round, i, n;
	bool again;
	(void)work;
	for (round = 0; round < 8; round++) {
		spin_lock_irqsave(&diag_lock, flags);
		if (cache.head - cache.tail > DIAG_RING) {
			cache.dropped += cache.head - cache.tail - DIAG_RING;
			cache.tail = cache.head - DIAG_RING;
		}
		n = 0;
		while (n < ARRAY_SIZE(batch) && cache.tail < cache.head) {
			batch[n++] = cache.ring[cache.tail % DIAG_RING];
			cache.tail++;
		}
		spin_unlock_irqrestore(&diag_lock, flags);
		for (i = 0; i < n; i++) {
			struct record *r = &batch[i];
			trace_sgx535_cached_lifecycle(r->seq, r->kind, r->id, r->epoch, r->serial,
						      r->state, r->error, r->reason, r->bytes);
			if (READ_ONCE(debug) & SGX535_DEBUG_DMESG)
				pr_info_ratelimited(
					"poulsback: diag seq=%llu kind=%u id=%llu epoch=%llu "
					"serial=%llu state=%u error=%d reason=%u bytes=%llu\n",
					r->seq, r->kind, r->id, r->epoch, r->serial, r->state,
					r->error, r->reason, r->bytes);
		}
		if (!n)
			break;
	}
	spin_lock_irqsave(&diag_lock, flags);
	again = cache.online && cache.tail < cache.head;
	if (again)
		queue_work(system_unbound_wq, &emit_work);
	spin_unlock_irqrestore(&diag_lock, flags);
}
static void print_record(struct seq_file *s, const char *label, const struct record *r)
{
	seq_printf(s,
		   "%s seq=%llu kind=%u id=%llu epoch=%llu serial=%llu state=%u error=%d reason=%u "
		   "bytes=%llu\n",
		   label, r->seq, r->kind, r->id, r->epoch, r->serial, r->state, r->error,
		   r->reason, r->bytes);
}
static void *first(struct seq_file *s, loff_t *pos)
{
	return *pos ? NULL : s->private;
}
static void *next(struct seq_file *s, void *v, loff_t *pos)
{
	(void)s;
	(void)v;
	(*pos)++;
	return NULL;
}
static void stop(struct seq_file *s, void *v)
{
	(void)s;
	(void)v;
}
static int show(struct seq_file *s, void *v)
{
	struct snapshot *p = v;
	u64 start = p->head > DIAG_RING ? p->head - DIAG_RING : 0, at;
	u32 i;
	seq_printf(s,
		   "schema=sgx535-cached-lifecycle-v3 scope=CACHED_NO_MMIO debug=0x%x online=%u "
		   "seen=%llu ring_head=%llu trace_tail=%llu dropped=%llu errors=%llu\n",
		   p->debug, p->online, p->seen, p->head, p->tail, p->dropped, p->errors);
	for (i = 1; i < SGX535_DIAG_KINDS; i++)
		print_record(s, "last", &p->last[i]);
	print_record(s, "last_error", &p->last_error);
	for (at = start; at < p->head; at++)
		print_record(s, "event", &p->ring[at % DIAG_RING]);
	return 0;
}
static const struct seq_operations seq_ops = {
	.start = first, .next = next, .stop = stop, .show = show};
static int open_cache(struct inode *inode, struct file *file)
{
	struct seq_file *s;
	unsigned long flags;
	int ret;
	(void)inode;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	ret = seq_open_private(file, &seq_ops, sizeof(struct snapshot));
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}
	s = file->private_data;
	spin_lock_irqsave(&diag_lock, flags);
	memcpy(s->private, &cache, sizeof(cache));
	((struct snapshot *)s->private)->debug = READ_ONCE(debug);
	spin_unlock_irqrestore(&diag_lock, flags);
	return 0;
}
static int close_cache(struct inode *inode, struct file *file)
{
	int ret = seq_release_private(inode, file);
	module_put(THIS_MODULE);
	return ret;
}
static const struct proc_ops proc_ops = {.proc_open = open_cache,
					 .proc_read = seq_read,
					 .proc_lseek = seq_lseek,
					 .proc_release = close_cache};
int sgx535_diag_start(void)
{
	unsigned long flags;
	int ret = 0;
	mutex_lock(&diag_lifecycle);
	if (!diag_users) {
		entry = proc_create("sgx535_cached_lifecycle", 0400, NULL, &proc_ops);
		if (!entry) {
			ret = -ENOMEM;
			goto out;
		}
	}
	diag_users++;
	spin_lock_irqsave(&diag_lock, flags);
	cache.online = true;
	if (cache.tail < cache.head)
		queue_work(system_unbound_wq, &emit_work);
	spin_unlock_irqrestore(&diag_lock, flags);
out:
	mutex_unlock(&diag_lifecycle);
	return ret;
}
void sgx535_diag_stop(void)
{
	unsigned long flags;
	mutex_lock(&diag_lifecycle);
	if (!diag_users || --diag_users)
		goto out;
	spin_lock_irqsave(&diag_lock, flags);
	cache.online = false;
	spin_unlock_irqrestore(&diag_lock, flags);
	/* No producer may queue work after offline transition; drain before teardown. */
	cancel_work_sync(&emit_work);
	if (entry) {
		proc_remove(entry);
		entry = NULL;
	}
out:
	mutex_unlock(&diag_lifecycle);
}
