# Shared health, kernel and UAPI, performance and release quality

<a id="release"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

M1 counts every mandatory case below (not the optional open-firmware
replacement or retired IDs), with its runner and result, in
[the coverage ledger](README.md#coverage-ledger). Case lists live in
`spec/cases/<case ID>.json`; a case passes only with a clean result whose
commit has the same product.

<a id="release.provenance"></a>

## Frozen artifacts, versions and reproducible build qualification

<a id="release.build-bindings"></a>

### release.build-bindings

Procedure: For each released profile pin sources, compiler/VM, modules, ABI/import CRCs,
userspace, firmware and inputs; build twice and compare.

Oracle: Complete reproducible bundle matches the deployed profile and claimed capabilities;
source/module pairs from a different configuration cannot substitute. Unexplained changes or
exceptions outside the pre-reviewed precise scope fail. Ordinary build/test reproduction needs
no private research corpus; separately required firmware inputs retain exact
acquisition/license/profile records.

<a id="release.kernel-health"></a>

## Kernel and system telemetry for hardware tests

<a id="release.telemetry-per-trial"></a>

### release.telemetry-per-trial

Procedure: Collect durable stream before dispatch through completion and closure, preserving
dmesg and all partial/failure outputs.

Oracle: No unmatched BUG/Oops/lockup/use-after-free or unexplained ownership growth; snapshots
and completion receipts have exact causal bindings, clock semantics and evidence seals.

<a id="release.power-correctness"></a>

## Power instability tracked separately from performance

<a id="release.power-edges"></a>

### release.power-edges

Procedure: Exercise observed AC/runtime-PM transitions only within safe admitted trials; segment
measurements by measured power state.

Oracle: Rendering/decode remains correct in observed states or refuses safely; power-limited FPS
does not turn corruption into a performance-only issue.

<a id="release.pm-modeset"></a>

## Device/display lifecycle and PM behavior

<a id="release.lifecycle"></a>

### release.lifecycle

Procedure: Test claimed modeset/VT/DPMS/runtime-PM and suspend/resume behavior with active and
idle work, and module unload and reload. Today `rmmod` is refused once the engine exists;
the target is that only module references refuse it, a wedge pins the module for the boot,
and every load writes EMGD's power-on clock gating.

Oracle: No use-after-free, stale mappings, corrupt image or unsafe release; identity/epoch and
recovery rules are enforced across transitions. Retire old scanout only after authenticated last
fetch, GTT removal/flush and independent SGX proof; retain both possible images after uncertain
plane mutation.

<a id="release.hang-recovery"></a>

## Hardware uncertainty and proven recovery lifetime

<a id="release.recovery-model"></a>

### release.recovery-model

Procedure: Run VM fault injection for timeout, lost completion, IRQ races, mapping/cache errors
and teardown.

Oracle: All publication-stage failures obey retained ownership and terminal-state contracts;
primary and cleanup failures are preserved. Every enumerated boundary has an independently
expected outcome. Before possible mutation, authenticated rollback leaves existing objects
untouched; after possible mutation, uncertainty retains all canonical owners, mappings, fences,
module/device refs and accounting. No changed callback is invoked and no lost source commits
publication/release. Explicit CPU fixture disposal is identified separately from physical
recovery.

The integration kernel case references this same fault matrix rather than maintaining a second
copy. Extend each injected boundary through the observable client reaction: error/timeout fence,
required context loss, bounded waits and complete ownership accounting. GPU release and display
release remain independent. No failure path permits an implicit same-episode reset or release of
HOLD owners.

M1's model cases are the kernel fault boundaries: an ioctl refusal before any publication, a
scheduler timeout that wedges and never resubmits, and a BO handle close that waits for the
BO's jobs. No host suite covers the scheduler timeout or the BO close yet;
`release.recovery-model` stays open until it has a case list and suites for them.

<a id="release.recovery-hardware"></a>

### release.recovery-hardware

Procedure: Qualify admitted safe recovery on target, then hardware watchdog/hard-hang coverage
only with a reviewed dedicated procedure.

Oracle: Actual recovery request/FIRE and returned boot identities are recorded. Manual power
cycle or a missing SSH response is not autonomous recovery proof; hard-hang gap remains
explicit.

Repeat the selected client-loss cases on the qualified physical configuration. Observe the
actual client, fence and retained-owner behavior, not merely the reset command or return of SSH.
Hard-hang recovery remains a separate unqualified claim until its own case succeeds.

M1's hardware cases (`spec/cases/release.recovery-hardware.json`) are a client killed mid-frame
and under load, X killed while flipping, a context destroyed with jobs queued, a timeout that
wedges without resubmission and HOLD on an unknown outcome.

<a id="release.irq-retirement"></a>

## Genuine per-job completion and IRQ correlation

<a id="release.completion-correlation"></a>

### release.completion-correlation

Procedure: Qualify IRQ completion (masks, ACK, lost-wakeup and shared-IRQ races) with actual
per-job correlation; a polling path, if selected, is qualified the same way.

Oracle: Completion identifies the exact generation/job; resource retirement requires its
independent visibility, drain, translation and display proofs; IRQ claim has matching event
bits, ACK and no lost/duplicate terminal outcome. Record each applicable boundary separately.
Exact completion alone cannot release mappings, resignal successful native retirement, or
authorize reuse; failed/unknown drain/cache/translation/display proof retains the entire owning
graph in HOLD. Display release does not certify SGX retirement and vice versa.

<a id="release.visual-evidence"></a>

## Unified image/frame evidence and restoration

<a id="release.images-frames"></a>

### release.images-frames

Procedure: Record every correctness result and real framebuffer snapshot; retain sampled
stress/performance results and every failure.

Oracle: All result IDs/hashes/images bind to authentic trial outputs; no synthetic fixture
passes as hardware; framebuffer memory screenshot does not claim optical LCD proof.

<a id="release.firmware-contract"></a>

## Firmware/GPU program origin, compatibility and licensing

<a id="release.firmware-inventory"></a>

### release.firmware-inventory

Procedure: Inventory every shipped firmware/GPU program with exact origin/hash/license, loader
ABI and active-path use.

Oracle: Shipped payload identities and legal redistribution basis are documented; compatible
hardware behavior is independently qualified before use.

<a id="release.firmware-open-replacement"></a>

## Optional fully open firmware replacement

<a id="release.open-firmware"></a>

### release.open-firmware

Procedure: Where production requires opaque firmware, implement and qualify an independently
documented open equivalent before claiming an entirely open stack.

Oracle: Only source-built, tested and distributable replacement closes an entirely-open-firmware
claim; static disassembly or an open host driver alone does not.

<a id="release.code-quality"></a>

## Modularity, safe UAPI and reviewable production code

<a id="release.interfaces-review"></a>

### release.interfaces-review

Procedure: Review shared ownership/MMU/queue, native2D, shader/compiler, video and frontend
module boundaries and typed public interfaces.

Oracle: Production code is reviewable with separate subsystem contracts and genuine
negative/lifetime tests; private one-shot prototypes are not exposed as general acceleration.

<a id="release.quality-gates"></a>

## Quality gates

<a id="release.quality-checks"></a>

### release.quality-checks

Procedure: Run the [quality checks](../../docs/tests.md#quality-gates): the fast gates in `make check`
(`tools/quality.py check`, QC-stale in `check-project`), the reach gates in `make check-reach`
(`tools/quality.py reach`), per-function coverage in `make coverage` (`tools/coverage.py check`)
with the tracked debt list; the planted cases are `tests/project/test_quality.py`.
The [case list](../../spec/cases/release.quality-checks.json) also declares an
external lint check of the maintainers' test scripts (`qc-lab`). Its runner is not bundled; that case stays open until
its declared inputs and result can be verified.

Oracle: Every check is green, fails on its planted case, and the debt list is empty (before
M1 it may hold only wiring debts whose step is still open). The last review pass has no open dead-code or duplication
finding.

<a id="release.documentation"></a>

## Current capability, operation and evidence documentation

<a id="release.docs-crosscheck"></a>

### release.docs-crosscheck

Procedure: Publish architecture, 2D/shader/video/firmware flows, build/deploy/recovery,
capability matrix and exact evidence-linked DoD reports.

Oracle: No unsupported API, codec, recovery or acceleration claim; reviewer can reproduce
bounded evidence and identify all open gates.

<a id="release.performance"></a>

## Performance reported separately from functional completion

<a id="release.performance-baselines"></a>

### release.performance-baselines

Procedure: Measure valid accelerated workloads after correctness, preserving
latency/FPS/CPU/memory and power conditions. The
[local performance case list](../../spec/cases/release.performance-baselines.json)
holds each EMGD/PSB comparison and its numeric bound: EMGD's
for glxgears, poulscube and the Mesa perf set, the better of EMGD and PSB for each x11perf test.
Use the BENCH result class: a fresh boot, `ondemand`, three repeats.

Oracle: The median of the three repeats meets the case's bound, and the result carries the
value and unit the report checks. Results state exact workload/configuration/backend and power
conditions; CPU fallback and missing GPU power counters are explicit. An x11perf test that
cannot reach its bound runs at the X stack's own ceiling (stock modesetting, same boot) with the
reason recorded, and stays open until a maintainer accepts it as an exclusion.

<a id="release.uapi"></a>

## Every ioctl refuses every invalid request

<a id="release.uapi-validation"></a>

### release.uapi-validation

Procedure: For each validation of BO_CREATE, BO_INFO, CONTEXT_CREATE, CONTEXT_DESTROY, SUBMIT and
DISPLAY_INFO (one case per condition, `spec/cases/release.uapi-validation.json`), send an
otherwise valid request with only that field wrong. SUBMIT_2D's cases are
[2d-native.bad-requests](2d-native.md#2d-native.bad-requests).

Oracle: The ioctl returns the case's errno; no job is queued, no mapping or BO changes and the
engine's counters do not move. Cases marked "not validated at driver HEAD" name checks the
kernel lacks today and stay open until it has them.

<a id="release.uapi-fuzz"></a>

### release.uapi-fuzz

Procedure: Mutate one to four fields of valid requests at random, per ioctl, for a fixed seed
and count; compare each outcome with a host model of the validation.

Oracle: Every request the model refuses is refused with no publication; no request reaches the
hardware unless the model admits it; no errsite outside the validation.

<a id="release.kms"></a>

## KMS display and scanout

<a id="release.kms-display"></a>

### release.kms-display

Procedure: Set the LVDS native mode with a SCANOUT BO, flip, wait for vblank and move the cursor
(`spec/cases/release.kms-display.json`). The VGA connector case is excluded from M1 for now
([exclusions](../../spec/exclusions.json)).

Oracle: Each frame shows the BO it names, flip events arrive in order with one flip in flight,
and display GTT bindings are released only after the last scanout.

<a id="release.gem-prime"></a>

## Buffer objects and PRIME sharing

<a id="release.bo-prime"></a>

### release.bo-prime

Procedure: Create, map, query and destroy BOs of every class and placement; export and import
them through PRIME in one process and across processes; share a written BO with a reader.

Oracle: Contents and VAs are as created; the per-file cap refuses and frees exactly; readers
wait for writers' fences; Shmem returns to its baseline after the cycles.

<a id="release.job-lifecycle"></a>

## Job lifecycle, ordering and overlap

<a id="release.jobs"></a>

### release.jobs

Procedure: Submit long fenced sequences, order them with syncobjs across contexts, run 2D during
raster and TA during raster, and render scenes larger than the parameter pool
(`spec/cases/release.jobs.json`).

Oracle: Every image is exact, every fence signals in order, completion comes by IRQ, and partial
renders leave no wedge.

<a id="release.soak"></a>

## Soak and leaks

<a id="release.soak-2h"></a>

### release.soak-2h

Procedure: glxgears beside the scripted desktop for 2 h, glxheads for 120 s, and the idle Dell
overnight (HW-IRQ-003).

Oracle: No HOLD, no errsite, no unexpected reboot; Shmem, BO and fence counts return to their
baselines.

<a id="release.host"></a>

## Host suites

<a id="release.host-suites"></a>

### release.host-suites

Procedure: `make check` builds and runs every CPU component of `tools/build-userspace.py` in the
strict and UBSan flavors. `tools/host-cases.py` turns the report into results only for
components listed in `spec/cases/release.host-suites.json` and
`spec/cases/integration.kernel-contracts.json`; a component in neither list yields no ledger
result.

Oracle: Each component's test and oracle commands exit 0 in every flavor, from inputs equal to
the commit's blobs. Kernel components count under
[integration.kernel-contracts](integration.md#integration.kernel-contracts), video components
after M1.

<a id="release.retired"></a>

## Retired

These catalogue IDs do not count. Use their active acceptance criteria below.

<a id="release.cpu-collector-baseline"></a>
<a id="release.collector-regression"></a>

- `release.cpu-collector-baseline`, `release.collector-regression`: see
  [release.host-suites](#release.host-suites).

<a id="release.cpu-queue-baseline"></a>
<a id="release.queue-cpu-regression"></a>

- `release.cpu-queue-baseline`, `release.queue-cpu-regression`: see
  [integration.kernel-contracts](integration.md#integration.kernel-contracts).

<a id="release.cpu-video-reference-baseline"></a>
<a id="release.video-reference-regression"></a>

- `release.cpu-video-reference-baseline`, `release.video-reference-regression`: video decode is
  after M1; its references stay with [mpeg2](mpeg2.md#mpeg2).
