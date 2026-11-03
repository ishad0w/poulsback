<!-- markdownlint-disable-file MD041 -- The logo is the page heading. -->
<p align="center">
  <a href="assets/logo/README.md">
    <img src="assets/logo/poulsback-terminal-dark-crisp.svg" alt="poulsback" width="656">
  </a>
</p>

`poulsback` is an experimental open Linux driver for Intel GMA 500 / 600 (Poulsbo).

It contains a DRM/KMS kernel module, a Mesa Gallium driver for SGX535,
an Xorg DDX and libraries that prepare MSVDX video decode commands.

## Status

| Area | Progress | Current status |
| --- | --- | --- |
| [2D](docs/status/2d.md) | `██████████████████▒░` | WIP |
| [3D](docs/status/3d.md) | `███████████████▒░░░░` | WIP |
| [Mesa / Gallium](docs/status/mesa.md) | `██████████▒░░░░░░░░░` | WIP |
| [X11](docs/status/x11.md) | `████████▒░░░░░░░░░░░` | WIP |
| [OpenGL 2.1](docs/status/opengl-21.md) | `██████▒░░░░░░░░░░░░░` | WIP |
| [OpenGL ES 2.0](docs/status/opengl-es-20.md) | `██████▒░░░░░░░░░░░░░` | WIP |
| [Video / VA-API](docs/status/video.md) | `████████▒░░░░░░░░░░░` | WIP |
| [Wayland](docs/status/wayland.md) | `███▒░░░░░░░░░░░░░░░░` | Research |
| [OpenGL 3.0](docs/status/opengl-3.md) | `██▒░░░░░░░░░░░░░░░░░` | Research |
| [Vulkan](docs/status/vulkan.md) | `██▒░░░░░░░░░░░░░░░░░` | Research |

Unchanged glxgears, poulscube and glxheads run in Xorg on the Dell reference
machine. Full API conformance and everyday-desktop acceptance remain open.
The approximate estimates and their scope are explained on the linked
[status pages](docs/status/README.md).

Legacy-driver knowledge: [Research](docs/status/legacy-drivers.md).
The [checklist](docs/checklist.md) owns acceptance criteria.

## Build and test

For a standalone source checkout on Linux, install the
[CPU build dependencies](docs/building.md#cpu-libraries), then run:

```sh
make check-project check-project-tests check-tool-tests check-userspace
```

This runs the documentation, Python and strict/UBSAN host C checks without a
GPU or vendor firmware. For a focused check, use the
[component test commands](docs/tests.md#run-checks).
The full `make check` also runs quality gates that require the prepared
build image and SDK inputs. The [build guide](docs/building.md)
describes those dependencies and the kernel, Mesa and DDX build routes.

For the Debian i386 development environment, start with the
[Debian i386 VM setup](docs/building.md#debian-12-i386-vm).

## Documentation

- **Understand the design:** [architecture](docs/architecture.md) explains the
  component boundaries; the [GPU reference](docs/hardware.md) records checked
  registers, address units, shader contracts, BIOS/display and legacy-driver findings.
- **Understand the sources:** the [legacy implementation reference](docs/re/README.md)
  explains driver/firmware mechanisms, source identities and unresolved contracts.
- **Follow an interface:** [kernel memory and jobs](docs/kernel.md),
  [Mesa and desktop integration](docs/graphics.md), [compiler](docs/compiler.md),
  [2D](docs/native-2d.md), [3D packets and depth](docs/native-3d.md),
  [video decode and firmware](docs/video.md). Each guide states its supported
  inputs and unresolved limits.
- **Build and verify:** [build recipes](docs/building.md), [test procedures](docs/tests.md),
  [tool reference](docs/tools.md), [diagnostics](docs/diagnostics.md)
  and [acceptance criteria](docs/checklist.md).
- **Explore API feasibility:** [Wayland and OpenGL 3.0](docs/graphics.md#research-profiles),
  [Vulkan assessment](docs/tests.md#vulkan-feasibility).
- **Work on the repository:** [agent guide and commands](AGENTS.md),
  [documentation map](MAP.md), [working context](MEMORY.md) and
  [logo rendering](assets/logo/README.md).

## Contributing and support

Help make these old GPUs useful again. Questions, bug reports, documentation,
tests, hardware observations and patches are welcome at any experience level.
You can start with what you know, ask for help or open a draft pull request.

[Support](SUPPORT.md) points to the community channels and maintainer's contact.
[Contributing](CONTRIBUTING.md) suggests ways to help and explains checks,
contributor credit and the [Developer Certificate of Origin](DCO) for patches.

Report suspected vulnerabilities privately as described in [Security](SECURITY.md).
Project participation follows the [code of conduct](CODE_OF_CONDUCT.md).
[Authors](AUTHORS.md) records project credit, [Notices](NOTICES.md) explains
licence and attribution obligations, and the [name and logo policy](TRADEMARK.md)
describes references, distribution builds and independent forks.

## Hardware behind the project

| Machine | Platform | Role and availability |
| --- | --- | --- |
| **Sony VAIO P Series VPCP11AKJ** | Intel Atom Z560 · GMA 500 | The machine that started poulsback. RIP. |
| **Dell Inspiron 1210** | Intel Atom Z520 · GMA 500 | Current reference machine. |
| **Fujitsu STYLISTIC Q550/C** | Intel Atom Z670 · GMA 600 | Current reference machine. |

## AI disclosure

Parts of this driver were developed with assistance from both
OpenAI Codex and Anthropic Claude, used in a mixed development workflow.

AI-assisted changes follow the same review and scoped verification requirements
as other changes. CPU, kernel-runtime and hardware results are recorded separately.

[License](LICENSE) · [Third-party notices](NOTICES.md)
