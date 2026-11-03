// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
// SPDX-License-Identifier: MIT

#include "../../kernel/debug/sgx535_debug.c"

static unsigned int checks;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        checks++;                                                                                  \
        assert(value);                                                                             \
    } while (0)
static void reset(void)
{
    memset(&cache, 0, sizeof(cache));
    debug = 0;
    diag_lock = diag_lifecycle = 0;
    entry = NULL;
    diag_users = 0;
    mock_privileged = true;
    mock_proc_fail = mock_open_fail = false;
    mock_lock_depth = mock_queued = mock_cancelled = mock_refs = mock_parsed = 0;
    mock_proc_mode = mock_trace_count = 0;
    mock_text_used = 0;
    mock_text[0] = 0;
    mock_proc_created = mock_proc_removed = 0;
    mock_printk_count = 0;
    mock_printk_used = 0;
    mock_printk_text[0] = 0;
    mock_cancel_hook = NULL;
}
static void final_cancel(void)
{
    CHECK(!cache.online && !diag_users && entry == &mock_entry && mock_entry.exists);
}
static void set_debug(unsigned int mask)
{
    char text[16];
    snprintf(text, sizeof(text), "0x%x", mask);
    CHECK(!binding_debug.ops->set(text, &binding_debug.param) && debug == mask);
}
/* The core category on or off; the dmesg bit is kept. */
static void controls(bool core)
{
    set_debug((core ? SGX535_DEBUG_CORE : 0) | (debug & SGX535_DEBUG_DMESG));
}
static void record(u32 kind, u64 id, s32 error, u32 reason)
{
    sgx535_diag_record(kind, id, 2, 3, 4, error, reason, 4096);
}

int main(void)
{
    /* The core category delivers every kind's records; without it each kind
     * still keeps its last cached record, without verbose delivery. */
    for (unsigned int core = 0; core < 2; core++) {
        unsigned int expected = core ? SGX535_DIAG_KINDS - 1 : 0;
        reset();
        controls(core != 0);
        CHECK(!sgx535_diag_start());
        CHECK(mock_proc_mode == 0400 && cache.online);
        for (u32 kind = 1; kind < SGX535_DIAG_KINDS; kind++) {
            record(kind, kind, 0, 0);
            CHECK(cache.last[kind].id == kind && cache.last[kind].kind == kind);
        }
        CHECK(cache.seen == SGX535_DIAG_KINDS - 1 && cache.head == expected && !cache.errors);
        CHECK(mock_queued == expected && mock_trace_count == 0);
        emit(&emit_work);
        CHECK(mock_trace_count == expected && cache.tail == cache.head);
        CHECK(!mock_printk_count);
        for (u32 at = 0; at < expected; at++)
            CHECK(mock_trace_kind[at] == at + 1 && mock_trace_id[at] == at + 1);
        sgx535_diag_stop();
        CHECK(!cache.online && !mock_entry.exists);
    }
    /* Console delivery is separately opt-in and outside producer locks.
     * Enabling its sink does not enable verbose classes or GPU operations. */
    reset();
    CHECK(!sgx535_diag_start());
    set_debug(debug | SGX535_DEBUG_DMESG);
    record(SGX535_DIAG_EPOCH, 1, 0, 0);
    CHECK(!cache.head && !mock_printk_count);
    record(SGX535_DIAG_EPOCH, 2, -EIO, 0);
    CHECK(!mock_printk_count);
    emit(&emit_work);
    CHECK(mock_printk_count == 1 && strstr(mock_printk_text, "error=-5"));
    controls(true);
    sgx535_diag_record(SGX535_DIAG_EPOCH, 3, 7, 11, 4, 0, 0, 0);
    CHECK(mock_printk_count == 1);
    emit(&emit_work);
    CHECK(mock_printk_count == 2 && strstr(mock_printk_text, "epoch=7 serial=11 state=4"));
    set_debug(debug & ~SGX535_DEBUG_DMESG);
    record(SGX535_DIAG_EPOCH, 4, -EIO, 0);
    emit(&emit_work);
    CHECK(mock_printk_count == 2 && mock_trace_count == 3);
    sgx535_diag_stop();
    /* Primary errors and reason-only records bypass all logging filters. */
    for (u32 kind = 1; kind < SGX535_DIAG_KINDS; kind++) {
        reset();
        record(kind, 11, -EIO, 0);
        record(kind, 12, 0, 7);
        CHECK(cache.errors == 2 && cache.head == 2 && mock_queued == 0);
        CHECK(cache.last_error.id == 12 && cache.last_error.reason == 7);
        CHECK(!sgx535_diag_start());
        CHECK(mock_queued == 1);
        emit(&emit_work);
        CHECK(mock_trace_count == 2);
        CHECK(mock_trace_id[0] == 11 && mock_trace_id[1] == 12);
        sgx535_diag_stop();
        unsigned int queued = mock_queued;
        record(kind, 13, -EIO, 0);
        CHECK(cache.head == 3 && mock_queued == queued);
    }
    /* The one parameter: root only, parsed before publication; a failure
     * publishes nothing. */
    reset();
    CHECK(binding_debug.mode == 0600 && !debug);
    CHECK(!binding_debug.ops->set("0x28", &binding_debug.param) && debug == 0x28);
    CHECK(binding_debug.ops->set("garbage", &binding_debug.param) == -EINVAL && debug == 0x28);
    CHECK(binding_debug.ops->set(NULL, &binding_debug.param) == -EINVAL && debug == 0x28);
    unsigned int parsed = mock_parsed;
    mock_privileged = false;
    CHECK(binding_debug.ops->set("0", &binding_debug.param) == -EPERM);
    CHECK(binding_debug.ops->set("garbage", &binding_debug.param) == -EPERM);
    CHECK(debug == 0x28 && mock_parsed == parsed);
    mock_privileged = true;
    char text[16];
    CHECK(binding_debug.ops->get(text, &binding_debug.param) == 2 && !strcmp(text, "40"));
    CHECK(!binding_debug.ops->set("0", &binding_debug.param) && !debug);
    CHECK(cache.seen == 0 && cache.head == 0);
    reset();
    controls(true);
    record(0, 1, -EIO, 1);
    record(SGX535_DIAG_KINDS, 2, -EIO, 1);
    CHECK(cache.seen == 0 && cache.head == 0 && cache.errors == 0);
    /* Ring overwrite/drop accounting and deferred trace delivery use the
     * production implementation. Logging never executes producer callbacks. */
    reset();
    controls(true);
    CHECK(!sgx535_diag_start());
    for (unsigned int i = 0; i < 80; i++)
        record(SGX535_DIAG_LEASE, i, 0, 0);
    CHECK(mock_trace_count == 0 && cache.head == 80);
    emit(&emit_work);
    CHECK(cache.dropped == 16 && mock_trace_count == 64);
    for (unsigned int i = 0; i < 64; i++)
        CHECK(mock_trace_id[i] == i + 16);
    sgx535_diag_stop();
    /* The snapshot names the parameter's categories and respects privilege and
     * refcounts; categories other than core deliver no records. */
    reset();
    set_debug(SGX535_DEBUG_SUBMIT | SGX535_DEBUG_DISPLAY);
    CHECK(sgx535_debug_on(SGX535_DEBUG_DISPLAY) && sgx535_debug_on(SGX535_DEBUG_CORE | SGX535_DEBUG_SUBMIT));
    CHECK(!sgx535_debug_on(SGX535_DEBUG_CORE) && !sgx535_debug_on(SGX535_DEBUG_DMESG));
    record(SGX535_DIAG_EPOCH, 1, 0, 0);
    CHECK(cache.seen == 1 && !cache.head);
    struct file file = {0};
    mock_privileged = false;
    CHECK(open_cache(NULL, &file) == -EPERM && mock_refs == 0);
    mock_privileged = true;
    mock_open_fail = true;
    CHECK(open_cache(NULL, &file) == -ENOMEM && mock_refs == 0);
    mock_open_fail = false;
    CHECK(!open_cache(NULL, &file) && mock_refs == 1);
    struct seq_file *seq = file.private_data;
    struct snapshot *snap = seq->private;
    CHECK(snap->debug == (SGX535_DEBUG_SUBMIT | SGX535_DEBUG_DISPLAY));
    CHECK(!show(seq, snap));
    CHECK(strstr(mock_text, "schema=sgx535-cached-lifecycle-v3") != NULL);
    CHECK(strstr(mock_text, "debug=0x18 online=") != NULL);
    CHECK(!close_cache(NULL, &file) && mock_refs == 0);
    /* Failed starts own nothing; a later isolated retry creates exactly one
     * owner. First release of nested owners cannot stop the shared worker. */
    reset();
    mock_proc_fail = true;
    CHECK(sgx535_diag_start() == -ENOMEM);
    CHECK(!cache.online && !entry && !diag_users);
    sgx535_diag_stop();
    CHECK(!mock_cancelled && !mock_proc_removed);
    mock_proc_fail = false;
    CHECK(!sgx535_diag_start());
    CHECK(diag_users == 1 && cache.online && mock_proc_created == 2);
    CHECK(!sgx535_diag_start());
    CHECK(diag_users == 2 && mock_proc_created == 2);
    sgx535_diag_stop();
    CHECK(diag_users == 1 && cache.online && entry && !mock_cancelled && !mock_proc_removed);
    record(SGX535_DIAG_EPOCH, 101, -EIO, 0);
    CHECK(mock_queued == 1 && mock_trace_count == 0);
    emit(&emit_work);
    CHECK(mock_trace_count == 1 && mock_trace_id[0] == 101);
    record(SGX535_DIAG_EPOCH, 102, -EIO, 0);
    mock_cancel_hook = final_cancel;
    sgx535_diag_stop();
    CHECK(!diag_users && !cache.online && !entry && !mock_entry.exists);
    CHECK(mock_cancelled == 1 && mock_proc_removed == 1);
    unsigned int queued = mock_queued;
    record(SGX535_DIAG_EPOCH, 103, -EIO, 0);
    CHECK(mock_queued == queued && cache.last_error.id == 103);
    sgx535_diag_stop();
    CHECK(mock_cancelled == 1 && mock_proc_removed == 1);
    printf(
        "PASS %u production cached logging/filter/control checks; CPU kernel substitutes, no device\n",
        checks);
    return 0;
}
