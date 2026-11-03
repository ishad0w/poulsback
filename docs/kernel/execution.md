# Kernel execution and UAPI

[Kernel overview](../kernel.md) · [Execution](execution.md) · [Memory](memory.md) ·
[Display](display.md) · [Diagnostics](diagnostics.md)

The kernel has one GPU execution path: buffer objects in per-file SGX address
spaces ([memory](memory.md#buffer-objects-and-per-file-address-spaces)), execution
contexts and `SUBMIT`. Mesa builds each frame's TA and raster stages
([engine frames](../graphics.md#engine-frames)); the kernel checks them against its
register table and runs them. The scene, draw and IR records Mesa builds frames from
are Mesa-internal
([poulsback_render_ir.h](../../src/gallium/mesa/poulsback/poulsback_render_ir.h));
no kernel interface carries them.

- [GPU engine](#p3-engine)
- [The loss](#the-loss)
- [UAPI and clients](#uapi-and-clients)

<a id="p3-engine"></a>
## GPU engine

The execution engine (`kernel/engine/`) is the only GPU path. The first
`CONTEXT_CREATE` claims the SGX through `sgx535_epoch_executor_claim`; the
current lifecycle keeps that claim until reboot and refuses runtime PM.
A claim after PM or removal closed the source returns `-EBUSY`. KMS clients
do not claim the SGX.

**Unload and suspend are unsupported.** The teardown helpers below are not
wired into the Linux lifecycle, and an unload callback that returns early
skips cleanup; it cannot reject `rmmod`. So the engine itself holds a module
reference from its claim, ready or wedged, until release frees it: once a
client has claimed the SGX, `rmmod` is refused for the rest of the boot.
Before the first claim, open files, exported BOs and display owners hold
the module as usual. Safe unload/reload and suspend/resume remain
unvalidated.

**Bring-up (once per boot)** separates allocation from register programming.
Allocation touches no register: the two device areas, 32 MiB each, zeroed and
flushed (the parameter buffer at `0x30000000` and the DPM tables at `0x48000000`), mapped by
16 page tables of a kernel directory; the work queues, the CPU latency request
and the two `drm_sched` schedulers (2 s job timeout). Register programming
needs the GPU as probe left it (core `0x01130000` rev `0x00010201`, reset
released, no event pending, `BANK0` 0):
1. use the freshly zeroed areas, or zero and flush them again on reprogramming;
2. program the USE code windows (bank 3 + 2*w* vertex, 4 + 2*w* pixel, *w* = 0–5,
   from `0x40000000`) and `PDS_EXEC_BASE`;
3. run engine121's 17-action enable program;
4. put the kernel directory in `DIR_LIST_BASE15`, route every requester there
   (`BANK0` `0x000FFFFF`), set `TA_REQ_BASE`, pulse `INVALDC` with readback;
5. run the 29-action DPM program with its thresholds (TA and global beyond the
   pool, ZLS = pages − 477, PDS = ZLS − 473);
6. bind the engine's IRQ route (once) and set the static enables (`HOST_ENABLE`
   `0x1A04300F`, `HOST_ENABLE2` `0x10`).

A failure at any step keeps what was allocated or written and leaves the engine
wedged (HOLD), which is [the loss](#the-loss). Each context has a 3D scheduler
entity and a 2D entity; creating one also links the device mappings into the
file's directory. A job timeout wedges the engine; nothing is retried or reset.

**Teardown helpers exist but have no lifecycle caller.** Quiesce refuses a
lost engine or a held job,
parks both schedulers, waits for the job slots to empty (two job timeouts and
100 ms, after which the engine wedges) and reads `EVENT_STATUS`,
`EVENT_STATUS2`, `BLIT_STATUS` and `BIF_INT_STAT`: an unacknowledged event, a
busy 2D core or a BIF fault refuses it and unparks the schedulers.
Power-down, after quiesce, writes the EMGD deinit sequence: `0xa58`, `0xa68`,
`0xa6c`, `0xa70`, `0xa48`, `HOST_ENABLE`, `HOST_ENABLE2`, `CLKGATECTL` ← 0.
Release, after quiesce with the engine ready and no hardware fence alive, stops
the schedulers, unbinds the IRQ route, frees the work queues, the latency
request, the debugfs file and the 16,401 pages, releases the claim and logs
one line, then drops the engine's module reference. The decisions are
`kernel/engine/poulsback_teardown.c` (host suite `teardown`).

**Per-file state (`kernel/engine/poulsback_file.c`).** The driver's `.open` creates
the file's address space and its context table; `.postclose` destroys the contexts
first (their entities flush queued jobs), then drops the address space, which jobs
keep alive with their own references. A context is held by the file's table and
by each `SUBMIT` or `SUBMIT_2D` from its lookup until its job is pushed (or
refused), so `CONTEXT_DESTROY` on a shared file frees it only after a
concurrent submission's push. A job is armed and pushed under the context's
lock, so jobs reach its entities in the order of their fences
(`drm_sched_entity_push_job`'s contract); the lock is taken with the job's
reservations held, and nothing waits for a reservation under it.

**Jobs.** `SUBMIT` validates once and builds the whole job; the executor runs it
without re-checking:
- the BO list names every buffer the job reads or writes; each is resolved to its
  VA in the file's space, and the job waits for and then fences their reservations
  (read or write), plus an optional in-syncobj; the out-syncobj gets the job's
  finished fence;
- TA and raster registers come only from the register table
  (`kernel/engine/poulsback_regtable.c`, host-tested): TE/MTE state and the VDM
  stream base for the TA; ISP, ZLS, TSP, background object and pixel-event state
  for the raster. Address registers (`0x238`, `0xA5C`, the ZLS bases) take a BO plus
  offset and get its VA or its 28-bit/high part. `BGOBJTAG` (`0x4C4`) takes a
  16-byte-aligned PARAMETER BO address: bits 23:0 are its offset from
  `3D_REQ_BASE` in 16-byte units, and the supplied value may contain only
  `TSPDATASIZE` in bits 29:24. Zero without a BO disables the background.
  `TE_PSG` bit 24 stays 0. The kernel derives tile, macrotile and scene words
  from the job's size; kicks, resets, routing, DPM, code banks and enables
  are kernel-owned;
- the kernel appends engine121's TA and resident raster programs (scene,
  DPM state, caches and kicks). The raster frees the parameter pages in the
  DPM context the TA used, without a state-table store/load. Each execution
  context lazily allocates its scene buffer on its first 3D submit, sized
  for 2047 × 2047 with two region-header arrays, at `0x4C000000` + 1 MiB ×
  (handle − 1) (`kernel/engine/poulsback_scene.c`, host suite `scene`). The
  allocation is zeroed and flushed; each TA clears the root pages it uses. A
  failed allocation or mapping returns everything it took and refuses the
  submit with `-ENOMEM`.

`SUBMIT` accepts dimensions 1–2047, 1–64 BOs and, per phase, exactly the
registers of that phase's table: all 7 TA and all 26 raster registers, each
once. A job inherits no register state from an earlier job, so an incomplete
set is refused before anything is queued. A register's `bo` is a
one-based index into the BO list, not a GEM handle; `bo_offset` must be
inside that BO. A plain register has `bo = 0` and `bo_offset = 0`.

A 3D job's TA pass may run beside another job's raster pass, and a 2D job
beside a raster pass, never beside a TA pass
(`kernel/engine/poulsback_overlap.c`, host-tested). A job's TA allocates its
scene in one of the DPM's two contexts and its raster frees that context: the
engine writes `0x630` (ALLOC and LS before the TA program, DALLOC before the
raster's), and consecutive jobs take the contexts and their context's two
region-header arrays in turn ([HW-DPM-002](../hardware.md));
no DPM state is stored or loaded. So at most one TA pass, one raster pass and
one job between them (its TA done, its raster waiting) hold contexts, two at
most. A TA joins a raster only for a job in the same directory and
the other region-header array; otherwise it waits until no job holds a
context. 3D and 2D jobs have a scheduler each: 3D with three credits (a
raster, a TA or waiting job, and the next one in the engine), 2D with one.

Starts run on the engine's work items: a raster (`raster_work`: DALLOC, the ISP
reset, the raster program; their waits poll, so not in hard IRQ), then a TA
(`start_work`), on one ordered queue, so a raster always kicks before the TA
the same event lets start; a 2D job in its feeding work item. A TA points
`DIR_LIST_BASE0` at the file's directory and `BANK0.TA`/`.3D` at slot 0
(`0x000F0F0F`), pulses `INVALDC` and runs the TA program; beside a raster it
keeps the directory. Before the program the engine invalidates the USE cache,
beside a raster with the 3D's USSE idle: `DMS_CTRL` stops the PDS issuing tasks, both pipes' pixel queues
drain and their task slots go idle, the TA program runs up to its kick, the
PDS issues again, the TA kicks. When the TA finishes, the IRQ handler flushes
the tail pointer cache, which writes back through the TA requester, and leaves
`BANK0.TA` at slot 0 while a 3D job holds the directory: a TA-requester access
can follow the flush's event ([HW-ADDR-007](../hardware.md)). The job's raster
starts if none runs. `PIXELBE` end plus DPM memory free end a raster: the fence
signals, a TA that waited for memory restarts, and the next raster and TA may
start; with no 3D job left, `BANK0.TA`/`.3D` go back to slot 15. The IRQ
handler acks only the events it owns (`HOST_ENABLE`); a program's waits poll
and ack their own (DPM CLEARs, cache and MADD completions). A 2D job holds
`BANK0.TA` during a raster ([HW-ADDR-006](../hardware.md)); the 2D core uses
the TA and 2D requesters ([HW-ADDR-004](../hardware.md)), so a 2D job never
runs during a TA pass, and a TA waits for a running 2D job and goes first when
both could start. Each `BANK0` write follows the jobs on the GPU and is
followed by `INVALDC`; an event that does not fit the engine's state wedges
it. The TA beside a raster needs Mesa's vertex tasks to declare their maximum
input instances ([HW-SHADER-013](../hardware.md)).

A first TA out of memory raises ZLS into the reserve, reloads the free list and
restarts the TA, unless a raster runs: then it restarts when that raster has
freed its pages; ZLS goes back down at a TA
that starts with no raster running. Partial-render builders are host-tested
but not wired into execution, so this reserve policy remains the implemented
limit. A second OOM, `TA_TERMINATE`, ZLS out of memory, a DPM or BIF fault, a
failed wait or a timeout wedge the engine: the job's fences complete with
`-EIO` so that waiters return, and the job's buffers, scene and address space
are never released (HOLD, [the loss](#the-loss)).

While jobs run, and for 100 ms after the last one ends, the engine holds a CPU
latency limit of 10 µs (PM QoS), so the CPUs idle in C1E at most. On this Atom
the LAPIC timer stops in C2 and deeper; the latency limit avoids relying on
HPET broadcast wakeups during engine work ([HW-IRQ-003](../hardware.md)).

**2D jobs.** `SUBMIT_2D` lists up to 256 fills and copies, with up to 16
surfaces and 16 BOs, for the 2D core on a context. The planner
(`kernel/engine/poulsback_twod.c`, host-tested) checks them in the ioctl:
- every BO in the 2D class, each listed once with `flags` 0;
- every surface (BO index, offset, stride, size, format) under the encoder's
  layout rules;
- every operation's fields.

It encodes operations through the
[2D encoder](../../src/raster/sgx535_2d_encoder.h), using
`sgx535_2d_raster_fill` and `sgx535_2d_raster_copy`. A rectangle is
clipped to its surfaces and an optional clip, so a no-op encodes nothing. The
2D core writes a destination that starts off a 64-byte boundary about half as
fast ([HW-2D-006](../hardware.md)), so an operation that does and keeps at
least 192 bytes of 8 rows after clipping becomes two commands: a head up to
the boundary and an aligned body. Inside one surface a copy toward larger x
runs the body first, so neither command overwrites what the other still
reads. Smaller operations stay one command to avoid the second command's overhead. A
copy between formats gets `-EOPNOTSUPP`, a malformed request `-EINVAL`, and
so does a job that would write more than `POULSBACK_2D_MAX_PIXELS` (2²⁵) pixels
after clipping: about half a second of the core, a quarter of the job timeout.

The kernel derives each BO's fence usage from the operations: a destination
writes, everything else reads. One surface onto itself copies as if through a
temporary, and the kernel picks the copy order. Two different views of one BO
are refused. A job that encodes anything ends with a 1×1 fill of the file's
scratch page at `TWOD_REQ_BASE` (`0xD0000000`, mapped in every file's address
space), because the 2D core touches the last page a job wrote again when the
next 2D job starts, under that job's directory ([HW-2D-004](../hardware.md)).

Clients: the X server's DDX submits the Present copy of each windowed GL frame
as a 2D job on its own context ([graphics](../graphics.md)); Mesa's images are
2D-class BOs, so the copy reads the client's buffer in X's file. Mesa's
`pipe->blit` copies between its own images as 2D jobs on its engine context.

2D jobs ride the context's 2D entity on the 2D scheduler. A context's 3D and 2D
jobs are ordered by their buffers' reservations, not by the context. A 2D job
never overlaps a TA pass: the 2D core writes through `BANK0.TA` and reads
through `BANK0.2D` ([HW-ADDR-004](../hardware.md)). It starts at once when no TA
pass runs, else when that pass finishes, unless a waiting TA starts then (the
TA goes first).
The start drains CPU write-combining with `mb()`; WB BOs need no cache
maintenance ([HW-CACHE-002](../hardware.md)). It refuses to start unless
the core is idle with an empty slave port (`SOCIF` `0x80`). It points
`DIR_LIST_BASE1` at the file's directory and TA and 2D at slot 1 (`BANK0`
`0x0001FF1F` alone, `0x00010F1F` during a raster pass) and reads the
`BLIT_STATUS` count. A work item then feeds the
words to the slave port (`0x4000`), at most `0x60` per burst and no more than
`SOCIF` has room for. Each bit-27 interrupt either requeues the feeding or
completes the job: the count must have advanced by 2 per encoded operation
and the core must not be busy. A count past the plan wedges the engine. A job
whose operations were all clipped away signals at once.

The [engine's debugfs file](diagnostics.md#engine-debugfs) `dri/N/poulsback_engine`
shows the state, the error, the bring-up time, job, interrupt and OOM-resume counts,
the held jobs and what the loss kept,
the kernel CPU time per job, the 2D jobs started during a raster pass and
`DIR_LIST_BASE1`, the last events and fault, and the routing, enable, DPM
threshold and request-base registers.

## The loss

A wedge or a failed bring-up loses the GPU until reboot. Nothing
the GPU may still reach is retried, reset or released, the wedge pins the
module, and the CPU paths go on:
- **One published error.** `CONTEXT_CREATE`, `SUBMIT` and `SUBMIT_2D` return
  `-EIO`, and so does `BO_CREATE` in a file that was open at the loss; no
  other path in them returns it. The first cause
  stays in debugfs (`error`), and dmesg gets one line at the loss: `GPU lost
  (cause), N jobs held: acceleration off until reboot` (BIF faults add their
  own note before it).
- **Lost fences.** Every job the engine holds (running, waiting or due to
  start) completes with fence error `-EIO`, the scheduler's finished fence
  first, so the out-syncobj, the BOs' reservations and sync_files exported from
  them read `-EIO` on 6.1 too. A job that reaches the engine later fails in
  `run_job` with `-EIO`, never touches the GPU and releases normally.
- **Nothing the GPU may reach is freed.** The VM layer freezes before any lost
  fence signals. Every address space that existed then is held: closing a BO
  keeps its mapping, pages and a reference; a scene keeps its pages; a file's
  close keeps its directory, tables and scratch page, and such a file creates
  no BO. An address space created later is never named by a BIF slot: its
  file creates BOs (a restarted X gets its front, unaccelerated) and releases
  them normally ([memory](memory.md#memory-locking-and-lifetime)).
- **The display shows what is there.** A writer lost with the GPU does not
  refuse a show ([display](display.md)).

Debugfs's `lost` line counts the held jobs and what the freeze kept.

## UAPI and clients

Public headers are in `include/uapi/poulsback`; the driver's ioctl table
(`psb_ioctls`) is in [linux.patch](../../kernel/integration/linux.patch). Every
request carries its header's ABI number in `abi` (1 in both headers), and
the kernel refuses a mismatch; layouts, reserved fields and ioctl numbers are
versioned, and a change does not reinterpret an old wire format. All seven ioctls
are allowed on the render node; the driver slots not listed are unassigned. The
client is Mesa's winsys ([engine frames](../graphics.md#engine-frames)).

| Driver slot | Interface |
| --- | --- |
| 46–47 | [Buffer objects](memory.md#buffer-objects-and-per-file-address-spaces): `BO_CREATE` (size, placement, class → handle, VA, mmap offset) and `BO_INFO` (a handle's size, placement, class, VA in this file, mmap offset); `poulsback_bo_uapi.h` ABI 1 |
| 48–49 | [Execution contexts](#p3-engine): `CONTEXT_CREATE` (→ handle; the first in a boot brings the engine up) and `CONTEXT_DESTROY`; `poulsback_exec_uapi.h` ABI 1 |
| 50 | `SUBMIT`: one TA → 3D job (context, size, BO list, TA and raster table registers, in/out syncobjs); `poulsback_exec_uapi.h` ABI 1 |
| 51 | `DISPLAY_INFO`: the geometry a `SCANOUT` buffer needs to be page-flipped (the first enabled CRTC's width and height, a 64-byte-aligned XRGB8888 pitch), all zero with no CRTC on; software state only. Mesa places a back buffer in `SCANOUT` only when it matches ([display](display.md)); `poulsback_bo_uapi.h` ABI 1 |
| 52 | `SUBMIT_2D`: up to 256 fills and copies over 16 surfaces and 16 BOs on a context's 2D entity ([2D jobs](#p3-engine)); `poulsback_exec_uapi.h` ABI 1 |
| 52 | `SUBMIT_2D`: 2D-core fills and copies (context, BO list, surfaces, operations, in/out syncobjs), encoded by the kernel; `poulsback_exec_uapi.h` ABI 1 |
