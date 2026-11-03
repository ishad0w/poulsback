# Integration criteria

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

These milestones do not close release domains. They are the joins M1's domains
need on the current GPU engine path: (`CONTEXT_CREATE`, `SUBMIT`,
`SUBMIT_2D`, BOs and per-file address spaces), Mesa on the BO winsys, KMS
flips of SCANOUT BOs and the poulsback DDX. The
[frame path](../../docs/graphics.md#frame-path) explains the component joins;
[result scopes](../../docs/architecture.md#result-scopes) distinguish construction,
execution, presentation and API qualification. Each case names its runner in
the metadata and counts in the [coverage ledger](README.md#coverage-ledger).

- [Kernel foundation](#integration.kernel-foundation), [engine bring-up](#integration.bootstrap) and [DPM](#integration.dpm)
- [First triangles](#integration.triangle) and [repeated 3D](#integration.repeatable-3d)
- [KMS flips](#integration.presentation), [EGL images](#integration.egl-images) and [Xorg](#integration.xorg)
- [GLX applications](#integration.gears)

<a id="integration.kernel-foundation"></a>

## Kernel host suites

<a id="integration.kernel-contracts"></a>

### integration.kernel-contracts

Procedure: Run the kernel components of `tools/build-userspace.py` (UAPI layout, VA layout,
per-file address space, scene buffer, register table, 2D planner, overlap bookkeeping, engine
teardown, display vblank, foreign MMU, page addresses, debug) in the strict and UBSan flavors.
The display GTT check is separate ([kernel host tests](../kernel/README.md)).

Oracle: Every component suite passes from inputs equal to the commit's blobs. No GPU or
backend capability follows from a host result.

<a id="integration.bootstrap"></a>

## Engine bring-up

<a id="integration.engine-bring-up"></a>

### integration.engine-bring-up

Procedure: After a fresh boot, issue the first `CONTEXT_CREATE`, which claims the SGX and brings
it up: power, MMU root, the six USE code windows, the enable and DPM programs with their
thresholds, `BANK0` and the static host enables.

Oracle: debugfs `poulsback_engine` reports the engine ready within 200 ms, with no BIF fault and
no errsite.

<a id="integration.dpm"></a>

## DPM pool

<a id="integration.dpm-pool"></a>

### integration.dpm-pool

Procedure: Render 1,000 frames and a scene that runs the TA out of parameter memory.

Oracle: A normally completed frame frees every DPM page it allocates. The
current engine resumes a TA out of parameter memory once; a second exhaustion
loses the device and retains GPU-reachable resources. Partial-render
qualification additionally requires an oversized scene to complete through
continuations, with correct output and page accounting.

<a id="integration.triangle"></a>

## First triangles through SUBMIT

<a id="integration.triangle64"></a>

### integration.triangle64

Procedure: Draw one triangle with a constant fragment colour into a 64×64 target through EGL on
GBM and one `SUBMIT` job.

Oracle: All 4096 pixels match the CPU coverage and colour oracle, the job's fence signals, and
there is no fault or errsite.

<a id="integration.triangle-pair64"></a>

### integration.triangle-pair64

Procedure: Draw two triangles that share an edge, in one job, into a 64×64 target.

Oracle: All 4096 pixels match the two-triangle oracle and the shared edge is covered exactly
once; the fence signals.

<a id="integration.repeatable-3d"></a>

## Repeated 3D, depth and viewport

<a id="integration.repeat-depth"></a>

### integration.repeat-depth

Procedure: Execute a different second frame, then at least 256 consecutive checked frames
at each size, 64×64 and 1280×800, as specified by this case's `numbered_frames` and
`sizes` parameters. Cover deterministic geometry, interpolation, clear, culling, depth,
texture, blending and scissor scenes.

Oracle: Every required colour and depth sample matches its independent oracle. No stale
completion, early reuse of a frame slot or BO, or unexplained retained growth.

<a id="integration.presentation"></a>

## KMS flips

<a id="integration.kms-flips"></a>

### integration.kms-flips

Procedure: Flip SCANOUT BOs at 1280×800 and keep each named scene visible for at least 30
seconds, then return to the console.

Oracle: Flip events arrive in order, the visible frame equals the flipped BO, and the previous
display state is restored. Captures do not claim optical LCD proof.

<a id="integration.egl-images"></a>

## Native EGL/GBM and shared images

<a id="integration.egl-gbm-sharing"></a>

### integration.egl-gbm-sharing

Procedure: Create native headless EGL contexts/FBOs and round-trip required shared images before
depending on Xorg.

Oracle: Native SGX rendering and all required image/synchronization handoffs work with full
independent output comparisons and safe lifetime. No X11 or complete API claim follows.

<a id="integration.xorg"></a>

## Xorg on the poulsback DDX

<a id="integration.xorg-dri3"></a>

### integration.xorg-dri3

Procedure: Start Xorg with the poulsback DDX (KMS, EXA on the 2D core, DRI3, Present) and run a
GL client through DRI3.

Oracle: The DDX loads and neither glamor nor a shadow framebuffer does; DRI3 Open hands out a
render node of this device; Present flips and copies; dma-buf release and teardown are correct,
with the loaded driver and library identities recorded.

<a id="integration.gears"></a>

## Standard GLX application integration

<a id="integration.gears-acceptance"></a>

### integration.gears-acceptance

Procedure: Run unchanged mesa-demos 8.5.0 glxgears through Mesa GLX/DRI3, Gallium, SGX,
dma-buf, Xorg Present and KMS. Follow this case's `modes` and `animation_interval`
parameters: 640×640 windowed and 1280×800 fullscreen, each for at least 120 active
rotating seconds from its first confirmed application frame. Setup and paused
control probes do not count toward that interval.

Request swap interval zero only through a supported standard GLX or Mesa
swap-control mechanism. Save the offered mechanism, actual request/result and
observed swap/presentation behavior in `swap-control.original.json`. An
unsupported or unverified request is not an uncapped measurement. Do not use
API, extension or capability overrides; performance remains separate from correctness.

Oracle: Both modes meet their visible interval with rotating correct gears and working controls.
Vertices are shaded on the GPU. No memory/display corruption or unsafe reuse on
resize/expose/close.

<a id="integration.glxheads-acceptance"></a>

### integration.glxheads-acceptance

Procedure: After gears acceptance, run unchanged glxheads from the same pinned mesa-demos
release with at least two local GLX connections for 120 seconds. The interval starts
after the first confirmed application frame on every required connection, as specified
by this case's `time_origin` parameter.

Oracle: Every required local connection remains direct and produces rotating correct images
throughout the interval. Matching client/context GPU work and independent render/display release
are observed; no cross-context corruption or unsafe reuse occurs. Application output alone
cannot prove a direct hardware context.

<a id="integration.retired"></a>

## Retired

These catalogue IDs do not count. Use their active acceptance criteria below.

<a id="integration.bootstrap-only"></a>
<a id="integration.dpm-init"></a>
<a id="integration.typed-kms"></a>

- `integration.bootstrap-only`: see [integration.engine-bring-up](#integration.engine-bring-up).
- `integration.dpm-init`: DPM initialisation is part of the engine's bring-up. See
  [integration.dpm-pool](#integration.dpm-pool).
- `integration.typed-kms`: see
  [integration.kms-flips](#integration.kms-flips).
