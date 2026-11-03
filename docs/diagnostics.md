# Diagnostics

Diagnostics should explain an operation's state without changing that state.
Prefer bounded retained snapshots and immutable captures over unrestricted
register access. Record request identity, source epoch, phase, primary error,
cleanup error, retained resources and the established completion outcome.

Debug output is opt-in. Correctness experiments may use detailed capture;
performance runs must state whether debug and tracing were enabled. Never infer
GPU frequency, temperature or energy from CPU/platform sensors.

Reading routes: [platform observations](#platform-observations),
[cached kernel logging](#cached-kernel-logging) and
[source-led hardware experiments](#source-led-hardware-experiments). [Tests](tests.md)
owns check procedures. This guide owns common logging controls and record semantics;
[kernel diagnostics](kernel/diagnostics.md) owns the module's diagnostic outputs.

## Platform observations

The inherited `core_freq` value comes from a chipset selector decoder and is
used in display backlight calculations. It is not a measured SGX clock or a
performance counter. Record the decoder, raw input and stepping when available;
a value from a different firmware policy does not justify programming a clock.
Even a message-bus read request writes its selector and needs the device's
access and synchronization contract.

Preserve power-supply attribute names and units. Charge/current and energy/power
are different quantities; AC online can reflect an EC/ACPI fallback or delayed
notification. Correlate these records and kernel errors with monotonic time,
without treating them as GPU power measurements or excusing image corruption.
The [kernel handoff](kernel/display.md#firmware-and-display-handoff) owns panel and
firmware input provenance.

The [BIOS and power reference](re/platform/bios-display-power.md#clock-transport-and-competing-decoders)
owns the competing clock decoders, ACPI unit paths and retained telemetry limits.
Those source findings explain how to interpret a value; they provide no
authorization for a new selector access or clock change.

## Cached kernel logging

The current [kernel diagnostics guide](kernel/diagnostics.md) owns the exact
`debug` bitmask, errsite format, cached lifecycle log and engine debugfs fields.
The module has one parameter, `debug`, default 0 and root-writable at runtime.
It controls logging, not execution or admission.

The parameter is a mask of categories (core, mmu, irq, submit, display, dmesg).
The cached lifecycle producer is the source-epoch layer: bind/unbind, claim,
quiesce, loss and PM transitions produce epoch and lease records. There is no
per-job transcript. Refused requests log only under `submit`, and the flip
timing ring fills only under `display`.

`/proc/sgx535_cached_lifecycle` is a root-only cached snapshot with no MMIO.
The bounded ring retains errors and reason records regardless of the selected
mask. Its `errors` counter counts either a nonzero error or a nonzero reason;
a reason on a successful transition is not automatically a GPU fault. The
ring's seen, delivery and drop counters identify incomplete observation.
Deferred tracepoint/dmesg delivery happens outside producer locks; console
rate limiting can omit messages, and a printk timestamp is delivery time.

The engine's debugfs file is different: it combines counters under the engine
lock with live reads of the documented registers. Consult
[engine debugfs](kernel/diagnostics.md#engine-debugfs) before interpreting it.
Error-site records identify the source line that produced an error; they do
not by themselves classify GPU completion or authorize resource release.

For userspace, `POULSBACK_TRACE=1` enables Mesa driver decision and timing
messages on stderr. Refusals also report their origin. Preserve those messages
alongside the loaded runtime identity and frame results. Tracing adds work;
record whether it was enabled when comparing performance.

Kernel logs are evaluated relative to a before-run baseline. Taint, interrupt
counts, ACPI events and runtime PM observations need their own interpretation;
they are not pixel correctness or GPU completion oracles. Prefer errsites,
normal kernel logging and opt-in function tracing when a device run needs more
observation. This repository supplies diagnostic interfaces, not device-dispatch,
netconsole setup or reboot commands. A device procedure must define those steps
for its selected machine before execution.

For image-producing work, retain raw results and compare them with an independent
oracle. A framebuffer screenshot establishes readable memory bytes. It does not
by itself establish optical LCD output, tearing, timing or hardware rendering.
On an unknown GPU outcome, stop submissions and keep resources in HOLD;
a diagnostic error grants no retry, reset, unload or release.

## Source-led hardware experiments

Freeze the question, inputs, selected source/build, expected output and
comparison policy before a device run. Retain the actual command invocation
and child status, not only a wrapper result. An oracle must not derive its expected image
from the image under test. State any dependency on earlier captures explicitly.
Alternative models remain diagnostic unless selected prospectively by a new
experiment; they cannot erase the original failure.
