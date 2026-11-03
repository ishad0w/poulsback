// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

/* CPU stand-ins for the kernel interfaces kernel/debug/sgx535_debug.c uses;
 * linux/ and trace/ here map its includes to this file. */
#ifndef DEBUG_KERNEL_MOCKS_H
#define DEBUG_KERNEL_MOCKS_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef unsigned long long u64;
struct kernel_param {
    void *arg;
};
struct kernel_param_ops {
    int (*set)(const char *, const struct kernel_param *);
    int (*get)(char *, const struct kernel_param *);
};
struct mock_binding {
    const char *name;
    const struct kernel_param_ops *ops;
    struct kernel_param param;
    unsigned int mode;
};
#define module_param_cb(name, operations, argument, permissions)                                   \
    static const struct mock_binding binding_##name = {#name, operations, {argument}, permissions}
#define MODULE_PARM_DESC(name, description) _Static_assert(sizeof(description) > 1, #name)
#define CAP_SYS_ADMIN 1
#define THIS_MODULE ((void *)1)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, replacement) ((value) = (replacement))
#define DEFINE_SPINLOCK(name) int name
#define DEFINE_MUTEX(name) int name
static bool mock_privileged = true, mock_proc_fail, mock_open_fail;
static unsigned int mock_lock_depth, mock_queued, mock_cancelled, mock_refs, mock_parsed;
static unsigned int mock_proc_mode, mock_trace_count;
static unsigned int mock_printk_count;
static char mock_printk_text[8192];
static size_t mock_printk_used;
static unsigned int mock_proc_created, mock_proc_removed;
static void (*mock_cancel_hook)(void);
static u32 mock_trace_kind[256];
static u64 mock_trace_id[256];
/* Delivery/lock checks only: this CPU substitute does not model the kernel's
 * rate limiter or console timing. */
static void pr_info_ratelimited(const char *format, ...)
{
    va_list args;
    assert(!mock_lock_depth);
    va_start(args, format);
    int n = vsnprintf(mock_printk_text + mock_printk_used,
                      sizeof(mock_printk_text) - mock_printk_used, format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(mock_printk_text) - mock_printk_used);
    mock_printk_used += (size_t)n;
    mock_printk_count++;
}
static int capable(int capability)
{
    assert(capability == CAP_SYS_ADMIN);
    return mock_privileged;
}
/* A documented CPU substitute for kstrtouint(base 0), not a kernel parser proof. */
static int kstrtouint(const char *value, unsigned int base, unsigned int *out)
{
    char *end;
    unsigned long parsed;
    mock_parsed++;
    if (!value || base || !*value || *value == '-')
        return -EINVAL;
    errno = 0;
    parsed = strtoul(value, &end, 0);
    if (errno || *end || parsed > UINT_MAX)
        return -EINVAL;
    *out = (unsigned int)parsed;
    return 0;
}
static int param_get_uint(char *buffer, const struct kernel_param *param)
{
    return sprintf(buffer, "%u", *(unsigned int *)param->arg);
}
#define spin_lock_irqsave(lock, flags)                                                             \
    do {                                                                                           \
        (flags) = 0;                                                                               \
        ++*(lock);                                                                                 \
        ++mock_lock_depth;                                                                         \
    } while (0)
#define spin_unlock_irqrestore(lock, flags)                                                        \
    do {                                                                                           \
        (void)(flags);                                                                             \
        assert(*(lock) > 0 && mock_lock_depth);                                                    \
        --*(lock);                                                                                 \
        --mock_lock_depth;                                                                         \
    } while (0)
static void mutex_lock(int *lock)
{
    assert(!*lock);
    ++*lock;
}
static void mutex_unlock(int *lock)
{
    assert(*lock);
    --*lock;
}
struct work_struct {
    void (*function)(struct work_struct *);
};
#define DECLARE_WORK(name, callback) struct work_struct name = {callback}
#define system_unbound_wq ((void *)1)
static void queue_work(void *queue, struct work_struct *work)
{
    assert(queue && work);
    mock_queued++;
}
static void cancel_work_sync(struct work_struct *work)
{
    assert(work);
    if (mock_cancel_hook)
        mock_cancel_hook();
    mock_cancelled++;
}
static void trace_sgx535_cached_lifecycle(u64 seq, u32 kind, u64 id, u64 epoch, u64 serial,
                                          u32 state, s32 error, u32 reason, u64 bytes)
{
    (void)epoch;
    (void)serial;
    (void)state;
    (void)error;
    (void)reason;
    (void)bytes;
    assert(seq && mock_lock_depth == 0 && mock_trace_count < ARRAY_SIZE(mock_trace_kind));
    mock_trace_kind[mock_trace_count] = kind;
    mock_trace_id[mock_trace_count++] = id;
}
struct seq_file {
    void *private;
};
struct inode {
    int ignored;
};
struct file {
    void *private_data;
};
struct seq_operations {
    void *(*start)(struct seq_file *, loff_t *);
    void *(*next)(struct seq_file *, void *, loff_t *);
    void (*stop)(struct seq_file *, void *);
    int (*show)(struct seq_file *, void *);
};
static char mock_text[32768];
static size_t mock_text_used;
static void seq_printf(struct seq_file *seq, const char *format, ...)
{
    va_list args;
    int count;
    assert(seq);
    va_start(args, format);
    count = vsnprintf(mock_text + mock_text_used, sizeof(mock_text) - mock_text_used, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(mock_text) - mock_text_used);
    mock_text_used += (size_t)count;
}
static bool try_module_get(void *module)
{
    assert(module);
    mock_refs++;
    return true;
}
static void module_put(void *module)
{
    assert(module && mock_refs);
    mock_refs--;
}
static int seq_open_private(struct file *file, const struct seq_operations *ops, size_t bytes)
{
    struct seq_file *seq;
    assert(ops);
    if (mock_open_fail)
        return -ENOMEM;
    seq = calloc(1, sizeof(*seq));
    assert(seq);
    seq->private = calloc(1, bytes);
    assert(seq->private);
    file->private_data = seq;
    return 0;
}
static int seq_release_private(struct inode *inode, struct file *file)
{
    struct seq_file *seq = file->private_data;
    (void)inode;
    free(seq->private);
    free(seq);
    file->private_data = NULL;
    return 0;
}
static ssize_t seq_read(struct file *file, char *buffer, size_t bytes, loff_t *pos)
{
    (void)file;
    (void)buffer;
    (void)bytes;
    (void)pos;
    return 0;
}
static loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
    (void)file;
    (void)whence;
    return offset;
}
struct proc_ops {
    int (*proc_open)(struct inode *, struct file *);
    ssize_t (*proc_read)(struct file *, char *, size_t, loff_t *);
    loff_t (*proc_lseek)(struct file *, loff_t, int);
    int (*proc_release)(struct inode *, struct file *);
};
struct proc_dir_entry {
    bool exists;
};
static struct proc_dir_entry mock_entry;
static struct proc_dir_entry *proc_create(const char *name, unsigned int mode, void *parent,
                                          const struct proc_ops *ops)
{
    assert(!strcmp(name, "sgx535_cached_lifecycle") && !parent && ops);
    assert(mock_lock_depth == 0);
    mock_proc_created++;
    mock_proc_mode = mode;
    if (mock_proc_fail)
        return NULL;
    mock_entry.exists = true;
    return &mock_entry;
}
static void proc_remove(struct proc_dir_entry *entry)
{
    assert(entry);
    mock_proc_removed++;
    entry->exists = false;
}
#endif
