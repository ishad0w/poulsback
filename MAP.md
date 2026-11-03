# Agent map

Read [AGENTS.md](AGENTS.md) for rules and commands; [MEMORY.md](MEMORY.md) holds
working context and cross-component decisions. This map owns navigation, not
capability status. Each row identifies the document to update when its subject
changes.

For a new task, start at the boundary involved:

- Hardware meaning or units: [checked contracts](docs/hardware.md), then the component owner below.
- A legacy field or source chain: [RE question index](docs/re/README.md),
  [source identity](docs/re/sources.md) and [address domain](docs/re/address-domains.md).
- A frame or shader: [frame path](docs/graphics.md#frame-path) and [compiler operations](docs/compiler.md#operations-and-numerical-scope).
- Memory, completion or release: [lifetime invariant](docs/kernel/memory.md#memory-locking-and-lifetime).
- A test result: [scope](docs/architecture.md#result-scopes), [test
  selection](docs/tests.md#selecting-checks), then the case in [acceptance](docs/checklist.md).
- A build failure: [recipes and dependencies](docs/building.md); use the exact locked inputs.
- Current capability and next implementation boundary: [status pages](docs/status/README.md)
  linked from the [README status overview](README.md#status).

| Source or subject | Owner |
| --- | --- |
| `kernel/`, `include/uapi/`, kernel clients | [Kernel](docs/kernel.md) |
| Kernel execution (the GPU engine) and UAPI | [Kernel execution](docs/kernel/execution.md) |
| Kernel backing, withdrawal/release, PRIME and source/MMU transactions | [Kernel memory](docs/kernel/memory.md) |
| `kernel/gem/`, `kernel/memory/poulsback_vm*`: buffer objects and per-file address spaces | [Buffer objects](docs/kernel/memory.md#buffer-objects-and-per-file-address-spaces) |
| `kernel/engine/`: GPU execution engine, contexts, bring-up and scheduler | [GPU engine](docs/kernel/execution.md#p3-engine) |
| Typed framebuffer, display GTT and firmware handoff | [Kernel display](docs/kernel/display.md) |
| Engine debugfs, integration/source-epoch diagnostics and check/build routes | [Kernel diagnostics](docs/kernel/diagnostics.md) |
| `src/compiler/` | [Compiler](docs/compiler.md) |
| `src/gallium/mesa/poulsback/`, graphics source lock and API research profiles | [Graphics](docs/graphics.md) |
| `src/xorg/`: the Xorg DDX (KMS, EXA on the 2D core, DRI3, Present) | [Graphics](docs/graphics.md) |
| `src/raster/` 2D | [Native 2D](docs/native-2d.md) |
| `src/raster/` 3D plans, packets and depth | [Native 3D](docs/native-3d.md) |
| `src/video/` | [Video](docs/video.md); [message, ring and MPEG-2 descriptor protocol](docs/video-protocol.md) |
| Kernel/userspace observations | [Diagnostics](docs/diagnostics.md) |
| Component responsibilities and actual compilation boundaries | [Architecture](docs/architecture.md#component-boundaries) |
| Current subsystem progress and research coverage | [Status pages](docs/status/README.md); compact [README overview](README.md#status) |
| Shared GPU hardware facts, units and qualification limits | [Hardware reference](docs/hardware.md) |
| EMGD and PSB reference measurements on the Dell | [EMGD 1.8](docs/bench/emgd-reference.md), [PSB 4.42](docs/bench/psb-reference.md) |
| Reference machine models, roles and availability | [Hardware behind the project](README.md#hardware-behind-the-project) |
| Legacy producer/consumer mechanisms and source locators | [Legacy implementation reference](docs/re/README.md) |
| `tools/` entry points and helper modules | [Tools](docs/tools.md) |
| Make, `support/` build targets, development VM, dependencies and source formatting | [Building](docs/building.md) |
| Logo assets and their generator | [Logo rendering](assets/logo/README.md) |
| `tests/`, CPU fixtures and raster oracle | [Tests](docs/tests.md); detailed [kernel](tests/kernel/README.md) recipes |
| `benchmark/` standalone graphics tools and case catalog | [Benchmarks](benchmark/README.md) |
| Acceptance criteria and `spec/dod.json` | [Checklist maintenance](docs/checklist.md#maintaining-this-checklist); [case guides](tests/acceptance/README.md) |
| Licence scope and attribution | [Licence](LICENSE), [notices](NOTICES.md) |
| Public reports, questions and maintainer contact | [Support](SUPPORT.md) |
| Contribution workflow, source provenance and sign-offs | [Contributing](CONTRIBUTING.md); [Developer Certificate of Origin](DCO) |
| Project author and contributor credit | [Authors](AUTHORS.md) |
| Vulnerability reporting and maintenance scope | [Security](SECURITY.md) |
| Project name, logo and independent distribution identity | [Name and logo policy](TRADEMARK.md) |
| Community behavior and moderation | [Code of conduct](CODE_OF_CONDUCT.md) |

Subsystem status: [2D](docs/status/2d.md), [3D](docs/status/3d.md),
[Mesa / Gallium](docs/status/mesa.md), [X11](docs/status/x11.md),
[OpenGL 2.1](docs/status/opengl-21.md), [OpenGL ES 2.0](docs/status/opengl-es-20.md),
[Wayland](docs/status/wayland.md), [OpenGL 3.0](docs/status/opengl-3.md),
[video](docs/status/video.md), [Vulkan](docs/status/vulkan.md) and
[legacy-driver knowledge](docs/status/legacy-drivers.md).
The [combined Mesa/API entry](docs/status/mesa-api.md) keeps earlier links usable.

Legacy reference chapters:

- [Source profiles](docs/re/sources.md), [public evidence](docs/re/evidence.md)
  and [address domains](docs/re/address-domains.md).
- Linux EMGD: [bootstrap](docs/re/emgd-linux/bootstrap-and-memory.md),
  [captured vertex path](docs/re/emgd-linux/vertex-path.md),
  [captured fragment path](docs/re/emgd-linux/fragment-path.md),
  [TA submission](docs/re/emgd-linux/ta-submission.md),
  [sync/release](docs/re/emgd-linux/sync-cleanup-and-release.md),
  [submission packet](docs/re/emgd-linux/submission-packet.md),
  [geometry/sampling](docs/re/emgd-linux/geometry-and-sampling.md),
  [viewport flow](docs/re/emgd-linux/viewport-coordinate-flow.md),
  [interpolation/packing](docs/re/emgd-linux/interpolation-and-packing.md).
- SGX535: [raster conventions](docs/re/sgx535/raster-conventions.md),
  [edge/W](docs/re/sgx535/edge-w-contract.md),
  [PARAM/PACK](docs/re/sgx535/interpolation-pack-contract.md),
  [depth/ZLS](docs/re/sgx535/depth-contract.md),
  [USSE fields/entry state](docs/re/sgx535/usse-instruction-contracts.md),
  [firmware](docs/re/firmware/sgx.md),
  [partial-render program](docs/re/firmware/partial-render.md).
- PSB: [PDS/USE/depth](docs/re/psb/pds-use-and-depth.md) and
  [normal-success lifetime](docs/re/psb/normal-success-lifetime.md).
- Windows: [raster/depth](docs/re/windows/raster-and-depth.md),
  [depth state/storage](docs/re/windows/depth-state-and-storage.md),
  [retail bootstrap/retirement](docs/re/windows/retail-bootstrap-and-retirement.md).
- [Native 2D](docs/re/native-2d.md); video [decode pipeline](docs/re/video/decode-pipeline.md),
  [firmware](docs/re/video/msvdx-firmware.md) and [surface layout](docs/re/video/surface-layout.md).
- Platform: [BIOS/display/power](docs/re/platform/bios-display-power.md) and
  [configuration controls](docs/re/platform/configuration-controls.md), with
  [captured clocks and render state](docs/re/platform/runtime-render-state.md).

Keep build commands in Building and test procedures in Tests. Component guides
describe interfaces; Hardware owns shared device facts. Link across these
boundaries instead of repeating the same contract in several guides.
