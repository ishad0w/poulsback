# OpenGL 2.1 status

WIP. Ordinary hardware-backed desktop contexts currently report OpenGL 2.0.
Selected applications work through [Mesa / Gallium](mesa.md); complete
OpenGL 2.0 and 2.1/GLSL 1.20 qualification remains open.

## Working and checked

- **Build:** standard Mesa desktop GL/GLX frontends and compatibility state are
  included in the overlay.
- **Dell:** unchanged glxgears, poulscube and glxheads render in Xorg through
  DRI3/Present. Windowed and covered frames use 2D copies; fullscreen windows
  can flip ([X11 status](x11.md)).
- **Rendering:** accepted vertex shaders run on the GPU. The fragment
  backend and selected depth/stencil/two-sided/offset fixtures have the scope
  listed in [3D status](3d.md#working-and-checked). CPU vertex fallback still exists.

## In progress

- Complete mandatory compatibility state, shader operations, textures/FBOs,
  formats, limits, queries, errors and required window-system configurations.
- Qualify sharing, concurrent clients, resource lifetime and full application
  acceptance cases. A successful application run does not qualify every OpenGL
  feature.
- Run complete applicable API selections and close their failures. The GL
  regression subset includes expected failures; a green regression comparison
  is separate from API conformance. Mesa must derive exposed versions and
  extensions from implemented capabilities.

Details: [OpenGL 2.1 acceptance](../../tests/acceptance/gl21.md),
[application acceptance](../../tests/acceptance/integration.md#integration.gears)
and [API test selection](../tests.md#external-api-test-selection).

## Progress basis

Scope: desktop OpenGL 2.1/GLSL 1.20 and the required GLX/application integration. A checked item is a completed integration stage with its stated subset, not an accepted specification family. [Graphics](../graphics.md) and [OpenGL 2.1 acceptance](../../tests/acceptance/gl21.md) define the target.

**6/20 deliverables complete = 30%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Desktop GL/GLX compatibility frontends build in the overlay.
- [x] Direct hardware-backed GL 2.0 contexts through X11/DRI3.
- [x] Unchanged glxgears, poulscube and glxheads application integration.
- [x] GPU transformation/lighting for accepted fixed-function-generated shaders.
- [x] Programmable fragment path for accepted programs.
- [x] Selected window depth/stencil/two-sided/offset integration.
- [ ] Complete required primitive and compatibility raster behavior.
- [ ] Complete required texture targets, formats, filtering and mipmaps.
- [ ] Complete fixed-function texture combination, texgen and fog.
- [ ] Complete GLSL 1.20 language, resource and precision behavior.
- [ ] Complete blend, mask, logic-op, alpha-test and discard behavior.
- [ ] Required framebuffer, query and multisample integration.
- [ ] Required GL 2.1 extra pixel, buffer, matrix and colour-space features.
- [ ] Normative limits, queries, errors and object behavior.
- [ ] Concurrent contexts, sharing and object-lifetime acceptance.
- [ ] Required GLX visual/fbconfig depth, stencil and MSAA configurations.
- [ ] Texture-from-pixmap, pixmap/pbuffer and sharing acceptance.
- [ ] Complete required advertised-extension selection.
- [ ] Complete applicable GL/GLSL conformance selection.
- [ ] Sustained application correctness, performance and lifetime acceptance.
