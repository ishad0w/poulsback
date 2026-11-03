# OpenGL ES 2.0 status

WIP. Hardware-backed GLES 2.0 contexts render selected EGL/GBM fixtures through
[Mesa / Gallium](mesa.md). Complete GLES 2.0/GLSL ES conformance remains open;
an exposed version string does not establish it.

## Working and checked

- **Build:** standard Mesa EGL/GLES frontends are included in the overlay.
- **Dell:** a GLES 2.0 context on the poulsback renderer draws the uniform
  fixture with checked pixels. Other contexts can be created while their
  unsupported shader/state still refuses rendering.
- **Shared renderer:** GPU vertex shading, fragment programs,
  depth/stencil and offset state have the bounded device scope listed in
  [3D status](3d.md#working-and-checked).
- **CPU:** NIR/backend fixtures check instruction generation and state builders;
  a host pass is separate from GPU execution.

## In progress

- Close all mandatory operations, precision, formats and resource limits;
  remove the CPU vertex fallback and complete sampling, fragment-state,
  FBO, query and configuration matrices. Bounded sampler/blend/scissor/target
  paths are connected in [source](../graphics.md#fragment-state-and-blits);
  they still need their complete API and device selection.
- Qualify EGL on X11, surfaces, sharing, errors and object lifetime across the
  complete applicable selection. The dEQP regression subset currently contains
  expected crashes; a green regression comparison is not a conformance pass.
- Keep [X11](x11.md)'s Render route separate: EXA Composite is the working
  assumption, and glamor on GLES2 remains an alternative to measure.

Details: [GLES2 acceptance](../../tests/acceptance/gles2.md),
[API test selection](../tests.md#external-api-test-selection) and
[coverage ledger](../../tests/acceptance/README.md#coverage-ledger).

## Progress basis

Scope: GLES 2.0/GLSL ES 1.00, EGL integration and application qualification. Checked implementation stages may cover a bounded subset and do not close a conformance family. [Graphics](../graphics.md), [compiler](../compiler.md) and [GLES2 acceptance](../../tests/acceptance/gles2.md) provide the basis.

**6/20 deliverables complete = 30%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] EGL and GLES frontends build in the Mesa overlay.
- [x] Real GLES2 context and surface creation through EGL/GBM.
- [x] GLSL ES/NIR input path with host compiler fixtures.
- [x] Hardware-rendered uniform-shader fixture with checked pixels.
- [x] Shared renderer with selected depth/stencil/offset state.
- [ ] Complete GPU vertex path without CPU vertex fallback.
- [ ] Required shader arithmetic, built-ins and numerical precision.
- [ ] Required shader flow, indexing and resource-boundary behavior.
- [ ] Complete required primitives, vertex formats and raster semantics.
- [ ] Complete blend, masks, discard and scissor integration.
- [ ] Complete texture format/filter/wrap/mipmap matrix.
- [ ] Complete framebuffer formats, attachments and readback.
- [ ] Truthful precision, resource limits and query values.
- [ ] Complete API object and error behavior.
- [ ] Full EGL-on-X11 configuration and surface acceptance.
- [ ] Context sharing, image import/export and synchronization acceptance.
- [x] Unknown-reset notification on robust contexts after device loss.
- [ ] Complete dEQP-GLES2 mustpass selection.
- [ ] Complete applicable KHR-GLES2 selection.
- [ ] Sustained rendering, performance and lifetime acceptance.
