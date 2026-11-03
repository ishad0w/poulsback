# Mesa / Gallium status

WIP. The published screen runs ordinary GL clients through the
[submission engine](../kernel/execution.md#p3-engine), the only GPU submission path.
Creating a context and rendering selected applications do not establish complete
[OpenGL 2.1](opengl-21.md) or [OpenGL ES 2.0](opengl-es-20.md) support.

## Working and checked

- **Build:** standard GLX/EGL/GBM/DRIL components are included in the Mesa overlay.
- **Dell:** glxgears at 300×300, 640×640 and fullscreen, poulscube, and glxheads
  with two contexts render through DRI3/Present. Fullscreen windows flip and
  other frames use 2D-core copies; `pipe->blit` preserves back buffers when a
  fullscreen client becomes covered ([X11 status](x11.md)).
- **Shaders:** accepted vertex shaders run on the USSE; refused draws retain
  the CPU vertex path. The fragment backend is connected to GPU-shaded
  draws, with per-draw constants and ISP state. Selected depth, stencil,
  two-sided and offset fixtures pass on the Dell
  ([3D status](3d.md#working-and-checked)).
- **CPU:** host fixtures cover the NIR compiler, packet builders and frame
  writer. The writer builds state once and emits each draw into frame-slot BOs
  for one `SUBMIT` ([engine frames](../graphics.md#engine-frames)).
- **Device loss:** engine submission errors reach Mesa, and robust contexts
  report an unknown reset. Robust buffer access remains refused
  ([loss policy](../kernel/execution.md#the-loss)).

## In progress

- Remove Mesa's draw-module vertex fallback.
- Complete and qualify the fragment/raster-state, sampling and render-target/FBO
  matrices, queries and MSAA, with truthful formats, caps and resource limits.
  The source already connects bounded blend/sampler/target paths described in
  [Graphics](../graphics.md#fragment-state-and-blits); host tests do not qualify
  their device behavior.
- Qualify all resource/surface/sampler callbacks, window-system configurations,
  context sharing, lifetime and complete API selections. The
  [coverage ledger](../../tests/acceptance/README.md#coverage-ledger) distinguishes
  a declared runner, execution and a passing result at the applicable source.

Details: [frame path](../graphics.md#frame-path),
[screen admission](../graphics.md#screen-admission) and [tests](../tests.md).

## Progress basis

Scope: the Gallium overlay and winsys integration, from a usable screen to complete hooks, formats, caps and integration qualification. [Graphics](../graphics.md) and the [Gallium acceptance catalogue](../../tests/acceptance/gles2.md#gles2.gallium) define the boundaries; API conformance is counted on the API pages.

**10/20 deliverables complete = 50%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Mesa overlay builds with GLX, EGL, GLES, GBM and DRIL frontends.
- [x] Published DRM screen and context creation on the native device.
- [x] BO-backed engine contexts and frame submission bridge.
- [x] Buffer resources, transfers and slab-backed vertex/index storage.
- [x] dma-buf image import/export for the current window-system path.
- [x] Native fence import/export and resource reservation ordering.
- [x] NIR shader/state-object adapters for the current renderer subset.
- [x] Frame writer with per-draw state and frame-slot BOs.
- [x] 2D-core pipe blit and DRI3/Present buffer preservation.
- [x] Device-loss reporting and reset status for robust contexts.
- [ ] GPU vertex path with no draw-module fallback.
- [ ] Complete fragment and raster-state callback behavior.
- [ ] Complete texture/sampler resources and callback behavior.
- [ ] General surface/FBO/render-target callback behavior.
- [ ] Required query callbacks and accurate results.
- [ ] Complete truthful cap and shader-limit audit.
- [ ] Complete format-by-bind acceptance matrix.
- [ ] Full GLX/EGL/GBM configuration and sharing selection.
- [ ] Concurrent context/thread, destruction and object-lifetime acceptance.
- [ ] Complete Gallium hook selection and sustained integration acceptance.
