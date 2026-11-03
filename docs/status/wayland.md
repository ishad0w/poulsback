# Wayland desktop status

Research. No complete poulsback client/compositor integration or qualified
Wayland runtime is available.

## Established basis

- The proposed path shares [Mesa / Gallium](mesa.md), dma-buf images and KMS
  with [X11](x11.md).
- Wayland itself requires neither OpenGL 3.0 nor Vulkan. The selected compositor
  determines the required rendering features.

## Remaining work

- Ordinary rendering, textures/blending, format/modifier negotiation and
  client/compositor/display lifetimes.
- A pinned compositor/client pair, Mesa Wayland platform integration and a case
  catalog for client GPU work, composition and direct KMS.
- Separate frame callbacks, buffer release and presentation checks. Existing CPU,
  X11 or finite offscreen results do not qualify Wayland.

Details: [integration profile](../graphics.md#wayland-desktop) and
[research checks](../tests.md#wayland-research).

## Progress basis

Scope: a Wayland feasibility/integration assessment, including prototype evidence and a reviewed plan. This bar counts research deliverables; it does not represent a working Wayland implementation. [Research profile](../graphics.md#wayland-desktop) and [research checks](../tests.md#wayland-research) define the target.

**3/20 deliverables complete = 15%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Document the proposed Gallium/EGL/GBM/dma-buf/KMS route.
- [x] Identify compositor-driven rendering prerequisites without assuming GL3 or Vulkan.
- [x] Separate frame callbacks, compositor buffer release and display retirement.
- [ ] Pin the protocol revisions and required extension inventory.
- [ ] Select and pin a compositor/client pair.
- [ ] Record the matched Mesa Wayland build and runtime dependency closure.
- [ ] Map the chosen compositor shaders and rendering requirements.
- [ ] Enumerate shared formats, modifiers and invalid-layout cases.
- [ ] Specify acquire/release ordering and implicit/explicit synchronization.
- [ ] Map KMS plane/scanout formats and direct-scanout constraints.
- [ ] Specify client/compositor/display ownership and teardown.
- [ ] Specify allocation/import/disconnect failure behavior.
- [ ] Map EGL surface/configuration and buffer-age requirements.
- [ ] Define composition-versus-direct-scanout selection and oracles.
- [ ] Freeze the complete client/compositor test catalogue.
- [ ] Collect independently attributed GPU-client prototype evidence.
- [ ] Collect independently attributed GPU-compositor prototype evidence.
- [ ] Verify pacing, visible presentation and buffer-release ordering.
- [ ] Assess resource reuse, performance, VT/session changes and teardown.
- [ ] Review the feasibility verdict, implementation gaps and next steps.
