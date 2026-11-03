# Kernel diagnostics and checks

[Kernel overview](../kernel.md) · [Execution](execution.md) · [Memory](memory.md) ·
[Display](display.md) · [Diagnostics](diagnostics.md)

Kernel observations include error-site records in dmesg, the cached lifecycle
log behind the `debug` parameter, engine debugfs and the
[display state and timing files](display.md#static-source-and-cache-custody).
These observations do not select a different execution path.
[Diagnostics](../diagnostics.md) owns the common rules for observations;
[tests](../tests.md) own check procedures.

- [Error-site records](#error-site-records)
- [Debug parameter and lifecycle log](#debug-parameter-and-lifecycle-log)
- [Engine debugfs](#engine-debugfs)
- [Build from current sources](#build-from-current-sources)
- [CPU fixtures](#cpu-fixtures)

## Error-site records

Error origins are wrapped in `POULSBACK_ERRSITE()` from
[`poulsback_errsite.h`](../../kernel/debug/poulsback_errsite.h). In a Kbuild
object each wrapped site that produces its error logs one constant
`poulsback errsite <error> <file>:<line>` record to dmesg at `KERN_NOTICE`, so a
failed device run names the source line of its first error without a new status
field. Each site is rate-limited on its own (`printk_ratelimited`), so a site's
first records still reach dmesg while a client failing at ioctl rate cannot fill
it. The record adds no device access, allocation or module call frame.

A refused request is wrapped in `POULSBACK_REFUSE()` instead: a caller's
arguments, handles, syncobjs or registers that the render-node ioctls (`SUBMIT`,
`SUBMIT_2D`, BO, context and PRIME) refuse, and a full display domain
(`-ENOSPC`), which the caller may retry. It logs the same record, only while the
`submit` category is set, so the product's default (0) logs no refusal. Builds
without Kbuild, including host tests that compile kernel sources directly, use
the plain error value for both. A record names where an error was produced; it
does not classify completion or a hardware fault.

## Debug parameter and lifecycle log

`debug` is the module's only parameter
([`sgx535_debug.c`](../../kernel/debug/sgx535_debug.c)): a mask of the
categories in [`sgx535_debug.h`](../../kernel/debug/sgx535_debug.h), default 0,
mode `0600`, writable at runtime by `CAP_SYS_ADMIN`
(`/sys/module/poulsback/parameters/debug`, or `poulsback.debug=` at load). It
changes logging only. For device runs, `debug=0x28` (submit, dmesg) is a useful setting.

| Bit | Category | Effect |
| --- | --- | --- |
| `0x1` | core | Deliver every epoch and lease record, not only errors and reasons |
| `0x2` | mmu | Reserved; no MMU records yet |
| `0x4` | irq | Reserved; no IRQ records yet |
| `0x8` | submit | Log refused requests (`POULSBACK_REFUSE`) |
| `0x10` | display | Keep the vblank and flip timing records (`dri/N/poulsback_flips`); without it the vblank interrupt reads no pipe position for them |
| `0x20` | dmesg | Also print delivered lifecycle records to dmesg, rate-limited |

The source epoch ([`sgx535_source_epoch.c`](../../kernel/source/sgx535_source_epoch.c))
is the lifecycle log's only producer. Bind, unbind, PM entry, quiesce and loss
produce epoch records; the engine's claim (`sgx535_epoch_executor_claim`) produces
a lease record. A record carries the epoch, the claimed flag (as `state`), the
error and the loss reason. Records with an error or a reason are delivered
whatever the mask. Module load starts the log and module exit stops it.

Delivered records enter a 64-entry ring and leave it from a worker, outside the
producer's locks, to the `sgx535:sgx535_cached_lifecycle` tracepoint and, with
`dmesg`, to dmesg. While the module is loaded, `/proc/sgx535_cached_lifecycle`
(`CAP_SYS_ADMIN`) prints a snapshot of the cache without MMIO: a header line
(`schema=sgx535-cached-lifecycle-v3`, the parameter as `debug=0x…`, and the seen,
ring, delivered, dropped and error counts), the last record of each kind, the
last error record and the ring's records.

## Engine debugfs

`dri/N/poulsback_engine` in the DRM debugfs directory (mode `0444`) appears with
the boot's first `CONTEXT_CREATE`, once the [GPU engine](execution.md#p3-engine)
is ready or has wedged during bring-up. Each read prints the engine's counters,
taken under its lock, and reads the listed registers live
([`poulsback_engine.c`](../../kernel/engine/poulsback_engine.c), `engine_show`):

| Line | Fields |
| --- | --- |
| `jobs`, `oom_resumes`, `zls_raised`, `ta_pages`, `max` | TA → 3D jobs whose fence signalled after TA, raster and DPM free; TA out-of-memory resumes into the ZLS reserve; whether ZLS is raised now; the parameter pages the last TA pass took (`DPM_PAGE_STATUS` bits 31:16 at TA finished) and the most any took |
| `cpu_us_per_job submit`, `run`, `raster`, `(submits N)` | Mean kernel CPU µs in `SUBMIT` (per submit), in the start work item (3D and 2D starts) and in the raster work item (per completed 3D job); the submit count |
| `twod jobs`, `submits`, `words`, `irqs`, `busy_waits`, `cpu_us_per_job submit`, `run` | 2D jobs completed and `SUBMIT_2D` calls; words fed to the slave port; bit-27 events during 2D jobs; completions whose count was reached while the core was still busy; mean kernel CPU µs per `SUBMIT_2D` and in the feeding work item per completed 2D job |
| `state`, `error`, `bringup_us`, `irqs` | `ready` or `wedged`; the first error (the cause; later ioctls and jobs fail with `-EIO`, [the loss](execution.md#the-loss)); the bring-up time; engine interrupts handled |
| `lost held`, `kept_maps`, `kept_bytes`, `kept_spaces` | Jobs stopped with the engine (their fences read `-EIO`); mappings and their bytes that closes kept after the loss; address spaces kept whole (all 0 until a loss) |
| `overlap twod_in_raster`, `ta_beside_3d`, `base1` | 2D jobs started while a 3D job ran (in its raster pass); TA passes started beside another job's raster pass; `DIR_LIST_BASE1`, the last 2D job's directory |
| `last_event`, `fault … at …` | The last two event status words; BIF fault status and address, set by a fault (0 before) |
| `bank0`, `base0`, `base15`, `directory` | `BANK0` routing, the directory bases of slots 0 and 15, the kernel directory's physical address |
| `host_enable`, `event` | `HOST_ENABLE` and `HOST_ENABLE2`; `EVENT_STATUS` and `EVENT_STATUS2` |
| `dpm 620`, `624`, `628`, `668` | DPM threshold registers at those offsets (`0x624` is ZLS) |
| `dpm context_id`, `shadow`, `next TA`, `pb_base`, `pages` | Live DPM context selector, its software shadow, the next TA's context, the parameter-buffer base and page-status word |
| `req twod`, `ta`, `3d`, `pds` | 2D, TA and 3D request bases and `PDS_EXEC_BASE` |
| `twod blit_status`, `socif` | The 2D core's `BLIT_STATUS` (completed count, busy bit 24) and `SOCIF` (free slave-port slots) |

## Build from current sources

Follow [building](../building.md#kernel-dependencies). The Linux dependency lock
pins source, headers and ABI inputs. The builder stages current source in new
directories, applies the patch with zero fuzz and emits `poulsback.ko`. It never
installs or loads it.

## CPU fixtures

The [kernel host tests](../../tests/kernel/README.md) compile kernel code and
the UAPI outside Kbuild without loading a module; `check-gem-policy.py` runs the
staged GEM bodies with CPU substitutes. There is no fixture module.
