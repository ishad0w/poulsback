# Working memory

Read [AGENTS](AGENTS.md) for rules and [MAP](MAP.md) for documentation owners.
This file holds the working boundary and decisions. Project history, dated
progress, device queues and experiment records stay out of product documentation.

## Working boundary

- The [GPU engine](docs/kernel/execution.md#p3-engine) is the only GPU path:
  GEM BOs in per-file SGX address spaces, `CONTEXT_CREATE`, `SUBMIT` for TA/3D
  and `SUBMIT_2D` for the 2D core.
- Unchanged glxgears, poulscube and glxheads run in Xorg on the Dell reference
  machine. [Status pages](docs/status/README.md) own current capabilities,
  limits and remaining API/desktop qualification.
- Accepted vertex shaders run on the USSE; Mesa's draw module handles refused
  shaders and draws. Removing this CPU fallback remains unfinished.
- The fragment backend wires per-draw depth/stencil/offset, blend, colour masks,
  logic operations, alpha/discard and depth feedback into GPU draws. The source
  also connects bounded 2D sampling, scissor objects and target preservation;
  full texture/FBO and API qualification remain open. [Graphics](docs/graphics.md)
  owns exact integration limits; [Compiler](docs/compiler.md) owns shader limits.
  Source integration does not extend the named device results on the status pages.
- Presentation uses our Xorg DDX: fullscreen SCANOUT buffers flip, while
  windowed and covered-fullscreen frames use 2D copies. BO/fence and display
  ownership are defined in [Memory](docs/kernel/memory.md) and
  [Display](docs/kernel/display.md).
- An unknown GPU outcome loses the engine for the boot. Lost fences signal
  an error while GPU-reachable BOs, scene storage and address spaces stay held.
  No retry, reset or release follows. Safe unload/reload and suspend/resume
  remain unsupported; teardown helpers are not wired into the lifecycle.
- The [hardware inventory](README.md#hardware-behind-the-project) owns machine
  models and availability.
  Results apply to their named device; Dell qualification does not establish
  GMA 600 qualification on the Fujitsu.

## Decisions

- One DRM/KMS and Gallium stack. Normal presentation flips or copies BOs;
  readback is diagnostic. Full API conformance requires its complete selection.
- The Render route under evaluation is EXA Composite through a render library
  shared with Mesa; glamor remains a measured alternative.
- Prioritize the 2D/3D/Mesa/X11 milestone. Video decode follows it. MPEG-2 has
  no Vulkan prerequisite.
- [Building](docs/building.md) owns recipes and dependencies. Full `make check`
  needs the prepared build image and SDK inputs; standalone CPU checks have their
  own documented targets. These commands grant no device action.
- [The checklist](docs/checklist.md) and [case metadata](spec/dod.json) own
  acceptance criteria and IDs. Implementation, host checks, device execution,
  presentation and API results have separate scopes.
- Resolve hardware questions through matched source contracts and the
  [RE source guide](docs/re/sources.md). [Hardware](docs/hardware.md) owns
  shared facts; component guides own interfaces. Source versions, hashes and
  address units belong with the contract; experiment chronology stays out of product documentation.
- README keeps compact 20-cell progress bars and WIP/Research labels. Each
  filled cell is 5 percentage points of the approximate milestone estimate.
  The linked status page owns its milestone basis, weights and scope. Credit
  partial implementation explicitly; acceptance counts do not measure feature
  completeness.
