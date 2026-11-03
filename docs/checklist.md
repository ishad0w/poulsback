# Acceptance checklist

This is the human-maintained acceptance guide. Edit it and the linked criteria
as ordinary Markdown. [Case metadata](../spec/dod.json) holds the stable IDs,
scopes, test parameters, artifacts and dependencies used by tools.

An unchecked item is open. A checked item needs a reviewed basis bound to its
source, build and profile. Add an indented basis link below the item, labelled
with every constituent case scope for a mixed requirement. For example:

```md
  Basis (CPU_VM): [review](path/to/review.md)
```

The checker
validates this binding and its links; it does not authenticate an experiment or
turn a checkbox into a driver capability. Reports remain with their tested
configuration after later code or editorial changes.

## Acceptance rules

Keep implementation, CPU/VM, hardware functionality, resource lifetime,
presentation, performance and API conformance separate. A useful finite hardware
observation can be recorded while retirement stays unresolved. It cannot close
the ordinary renderer or permit resource reuse. CPU_PASS is a CPU result;
software rasterization cannot satisfy a hardware requirement.

Expand each case family into concrete inputs and applicable external test names
before execution. Missing, skipped, failing, unsupported or XFAIL mandatory
cases stay open. A family count is not a complete CTS inventory. The GLES2 and
GL2.1 specification-to-case maps remain incomplete until their normative
requirements, limits, suite revisions and exclusions are reviewed in full.

Freeze outcome, producer, publication stage, output domains and numerical
tolerances before execution. Accepted rendering/decode needs every required
pixel or plane sample and forbidden guard check. Negative, empty and transport
cases need their applicable records, without fabricated image output. A rejected
or eliminated request before publication needs proof of zero publication and
typed absence of output. After possible publication, an unsafe readback is
recorded as unavailable; it cannot substitute for an output oracle. Retain
actual child exit/signal, primary and cleanup errors, exact package identities
and continuous diagnostics required by the case. Three screenshots do not
replace an all-frame oracle.

Completion, CPU/device visibility, device-write drain, translation/reference
retirement and the final display release are separate conditions. Uncertain
publication retains the full owner graph in HOLD and stops new submissions;
it does not authorize retry, reset, unload or release. Qualified recovery has
its own scope and device configuration.

The layers below are navigation, not a serial release schedule. Only the
explicit case and milestone dependencies gate a bounded result. Full native2D,
Vulkan, encoding research and full API conformance do not gate every limited
3D scene. MPEG-2 has no GL or Vulkan prerequisite. Optional domains are outside
the base release only while unadvertised; an advertised feature needs its
complete applicable matrix. A negative feasibility verdict claims no API.

An open host stack may use qualified, distributable compatible firmware. A fully
open firmware claim needs the separate replacement criterion. Functional output
and sustained performance are accepted separately; record power changes and
thermal conditions without inventing GPU utilization or watt measurements.

## Maintaining this checklist

Edit criteria here or in [the case guides](../tests/acceptance/README.md). Change
machine parameters and dependencies in `spec/dod.json` when the test contract
changes. Preserve existing requirement and case IDs. The regression fixture
records their identity and scope, not a frozen total: new cases are allowed.
Run `python3 -I -B tools/check-project.py check` and the relevant tests; there is
no prose generation step. A runner registration names a reviewed entrypoint,
arguments and scope. Registration alone does not record execution or PASS.

A runner whose `repository` is `workspace` belongs to the maintainers' lab
repository, which is not published. Its entrypoint, lists and evidence paths
(`eng/…`) name files in that lab, not in this tree. Readers of a public release
can read those cases' criteria but cannot run them. Moving the acceptance inputs
into this repository is planned after the alpha release.

Use the current case metadata. A result counts only under the
[coverage ledger's identity and completeness rules](../tests/acceptance/README.md#coverage-ledger).

<a id="integration-checks"></a>

The [bounded integration criteria](../tests/acceptance/integration.md) qualify
their selected paths independently of the complete release domains below.

## M1: the complete stack

M1's domains and the integration milestones they require. The
[coverage ledger](../tests/acceptance/README.md#coverage-ledger) counts their
cases, runners and results; a checked box here is a reviewed assertion and does
not replace the ledger's results.

### Native 2D (SUBMIT_2D on the 2D core)

- [ ] `2d-native.contract` —
  [Freeze the complete supported native blitter contract](../tests/acceptance/2d-native.md#2d-native.contract).
- [ ] `2d-native.fill-rop` —
  [Accelerated fills for all 16 binary S/D operations](../tests/acceptance/2d-native.md#2d-native.fill-rop).
- [ ] `2d-native.copy-rop` —
  [Accelerated disjoint copies for all 16 binary S/D operations](../tests/acceptance/2d-native.md#2d-native.copy-rop).
- [ ] `2d-native.geometry` —
  [Dimensions and rectangle boundaries](../tests/acceptance/2d-native.md#2d-native.geometry).
- [ ] `2d-native.dimension-eight` —
  [Width/height 8 on the ordinary encoding (HW-2D-001: no erratum)](../tests/acceptance/2d-native.md#2d-native.dimension-eight).
- [ ] `2d-native.clipping` —
  [Clipping and signed coordinate handling](../tests/acceptance/2d-native.md#2d-native.clipping).
- [ ] `2d-native.layout` —
  [Legal pitches, offsets and guard preservation](../tests/acceptance/2d-native.md#2d-native.layout).
- [ ] `2d-native.overlap` —
  [Same-BO overlap in all four directions](../tests/acceptance/2d-native.md#2d-native.overlap).
- [ ] `2d-native.visibility` —
  [CPU upload, GPU chains and CPU download coherence](../tests/acceptance/2d-native.md#2d-native.visibility).
- [ ] `2d-native.queue-fences` —
  [Reusable jobs, ordering and fences](../tests/acceptance/2d-native.md#2d-native.queue-fences).
- [ ] `2d-native.clients` —
  [Multi-client isolation and sharing](../tests/acceptance/2d-native.md#2d-native.clients).
- [ ] `2d-native.lifetime` —
  [BO/client teardown and resource lifetime](../tests/acceptance/2d-native.md#2d-native.lifetime).
- [ ] `2d-native.rejects` —
  [Malformed requests and publication boundary safety](../tests/acceptance/2d-native.md#2d-native.rejects).
- [ ] `2d-native.motion` —
  [120-frame visible 2D motion with every result checked](../tests/acceptance/2d-native.md#2d-native.motion).
- [ ] `2d-native.stress` —
  [At least 10000 operations and 30-minute hardware stress](../tests/acceptance/2d-native.md#2d-native.stress).
- [ ] `2d-native.hardware-attribution` —
  [No hidden CPU raster fallback counted as hardware acceleration](../tests/acceptance/2d-native.md#2d-native.hardware-attribution).

### X11 desktop on the poulsback DDX

- [ ] `2d-x11.backend` —
  [The DDX and its acceleration paths](../tests/acceptance/2d-x11.md#2d-x11.backend).
- [ ] `2d-x11.core` —
  [X core rendering requests](../tests/acceptance/2d-x11.md#2d-x11.core).
- [ ] `2d-x11.composite` —
  [Composition operators, alpha and masks](../tests/acceptance/2d-x11.md#2d-x11.composite).
- [ ] `2d-x11.text` —
  [Text, glyph caching and image upload](../tests/acceptance/2d-x11.md#2d-x11.text).
- [ ] `2d-x11.transform` —
  [Scaling, filtering, repeats and transforms](../tests/acceptance/2d-x11.md#2d-x11.transform).
- [ ] `2d-x11.damage` —
  [Repaint, scroll, resize and overlapping windows](../tests/acceptance/2d-x11.md#2d-x11.damage).
- [ ] `2d-x11.presentation` —
  [Present and DRI3](../tests/acceptance/2d-x11.md#2d-x11.presentation).
- [ ] `2d-x11.output-control` —
  [Outputs: RandR, DPMS and VT switch](../tests/acceptance/2d-x11.md#2d-x11.output-control).
- [ ] `2d-x11.xv` —
  [Xv textured video](../tests/acceptance/2d-x11.md#2d-x11.xv).
- [ ] `2d-x11.device-lost` —
  [A device lost with X running](../tests/acceptance/2d-x11.md#2d-x11.device-lost).
- [ ] `2d-x11.server` —
  [The rest of the X server](../tests/acceptance/2d-x11.md#2d-x11.server).
- [ ] `2d-x11.desktop-stress` —
  [One hour of real desktop](../tests/acceptance/2d-x11.md#2d-x11.desktop-stress).

### OpenGL ES 2.0 and Gallium

- [ ] `gles2.spec-catalog` —
  [Freeze GLES2/GLSL ES1.00 mandatory requirement and external test catalogs](../tests/acceptance/gles2.md#gles2.spec-catalog).
- [ ] `gles2.usse-pds` —
  [USSE/PDS launch, operands, registers and completion](../tests/acceptance/gles2.md#gles2.usse-pds).
- [ ] `gles2.arithmetic` —
  [Shader numerical and control-flow semantics](../tests/acceptance/gles2.md#gles2.arithmetic).
- [ ] `gles2.compiler` —
  [NIR backend, code generation and register allocation](../tests/acceptance/gles2.md#gles2.compiler).
- [ ] `gles2.geometry` —
  [Primitives, indexed draws and vertex fetching](../tests/acceptance/gles2.md#gles2.geometry).
- [ ] `gles2.raster-interpolation` —
  [Rasterization, viewport, scissor and interpolation](../tests/acceptance/gles2.md#gles2.raster-interpolation).
- [ ] `gles2.textures` —
  [Texture formats, sampling, mipmaps and layout](../tests/acceptance/gles2.md#gles2.textures).
- [ ] `gles2.framebuffer` —
  [FBO, renderbuffer and multisampling requirements](../tests/acceptance/gles2.md#gles2.framebuffer).
- [ ] `gles2.fragment-state` —
  [Depth, stencil, blending and color masks](../tests/acceptance/gles2.md#gles2.fragment-state).
- [ ] `gles2.api-errors` —
  [State queries, object lifetime and API error semantics](../tests/acceptance/gles2.md#gles2.api-errors).
- [ ] `gles2.contexts-winsys` —
  [Gallium/winsys, EGL and multi-context resource synchronization](../tests/acceptance/gles2.md#gles2.contexts-winsys).
- [ ] `gles2.gallium` —
  [Every Gallium hook, cap and format claim](../tests/acceptance/gles2.md#gles2.gallium).
- [ ] `gles2.conformance` —
  [Execute the complete frozen applicable GLES2 test selection](../tests/acceptance/gles2.md#gles2.conformance).
- [ ] `gles2.stress-visual` —
  [Repeated 3D frames, contexts and visible demo regression](../tests/acceptance/gles2.md#gles2.stress-visual).

### OpenGL 2.1 and GLSL 1.20

- [ ] `gl21.spec-catalog` —
  [Freeze OpenGL2.1/GLSL1.20 legacy requirements and cases](../tests/acceptance/gl21.md#gl21.spec-catalog).
- [ ] `gl21.legacy-pipeline` —
  [Complete required legacy/fixed-function behavior](../tests/acceptance/gl21.md#gl21.legacy-pipeline).
- [ ] `gl21.extended-graphics` —
  [Additional GL2.1 formats, textures, shaders and pixel operations](../tests/acceptance/gl21.md#gl21.extended-graphics).
- [ ] `gl21.extensions` —
  [Every extension EMGD advertised](../tests/acceptance/gl21.md#gl21.extensions).
- [ ] `gl21.window-system` —
  [Window-system configs: EGL on X11 and GLX](../tests/acceptance/gl21.md#gl21.window-system).
- [ ] `gl21.glx` —
  [GLX/DRI3 contexts, sharing and presentation](../tests/acceptance/gl21.md#gl21.glx).
- [ ] `gl21.conformance` —
  [Complete frozen GL2.1 test selection and application regression](../tests/acceptance/gl21.md#gl21.conformance).

### Kernel, UAPI, stability, performance and release quality

- [ ] `release.provenance` —
  [Frozen artifacts, versions and reproducible build qualification](../tests/acceptance/release.md#release.provenance).
- [ ] `release.kernel-health` —
  [Kernel and system telemetry for hardware tests](../tests/acceptance/release.md#release.kernel-health).
- [ ] `release.power-correctness` —
  [Power instability tracked separately from performance](../tests/acceptance/release.md#release.power-correctness).
- [ ] `release.pm-modeset` —
  [Device/display lifecycle and PM behavior](../tests/acceptance/release.md#release.pm-modeset).
- [ ] `release.hang-recovery` —
  [Hardware uncertainty and proven recovery lifetime](../tests/acceptance/release.md#release.hang-recovery).
- [ ] `release.irq-retirement` —
  [Genuine per-job completion and IRQ correlation](../tests/acceptance/release.md#release.irq-retirement).
- [ ] `release.visual-evidence` —
  [Unified image/frame evidence and restoration](../tests/acceptance/release.md#release.visual-evidence).
- [ ] `release.firmware-contract` —
  [Firmware/GPU program origin, compatibility and licensing](../tests/acceptance/release.md#release.firmware-contract).
- [ ] `release.firmware-open-replacement` — optional
  [Optional fully open firmware replacement](../tests/acceptance/release.md#release.firmware-open-replacement).
- [ ] `release.code-quality` —
  [Modularity, safe UAPI and reviewable production code](../tests/acceptance/release.md#release.code-quality).
- [ ] `release.quality-gates` —
  [Quality gates](../tests/acceptance/release.md#release.quality-gates).
- [ ] `release.documentation` —
  [Current capability, operation and evidence documentation](../tests/acceptance/release.md#release.documentation).
- [ ] `release.performance` —
  [Performance reported separately from functional completion](../tests/acceptance/release.md#release.performance).
- [ ] `release.uapi` —
  [Every ioctl refuses every invalid request](../tests/acceptance/release.md#release.uapi).
- [ ] `release.kms` —
  [KMS display and scanout](../tests/acceptance/release.md#release.kms).
- [ ] `release.gem-prime` —
  [Buffer objects and PRIME sharing](../tests/acceptance/release.md#release.gem-prime).
- [ ] `release.job-lifecycle` —
  [Job lifecycle, ordering and overlap](../tests/acceptance/release.md#release.job-lifecycle).
- [ ] `release.soak` —
  [Soak and leaks](../tests/acceptance/release.md#release.soak).
- [ ] `release.host` —
  [Host suites](../tests/acceptance/release.md#release.host).

### Integration milestones

- [ ] `integration.kernel-foundation` —
  [Kernel host suites](../tests/acceptance/integration.md#integration.kernel-foundation).
- [ ] `integration.bootstrap` —
  [Engine bring-up](../tests/acceptance/integration.md#integration.bootstrap).
- [ ] `integration.dpm` —
  [DPM pool](../tests/acceptance/integration.md#integration.dpm).
- [ ] `integration.triangle` —
  [First triangles through SUBMIT](../tests/acceptance/integration.md#integration.triangle).
- [ ] `integration.repeatable-3d` —
  [Repeated 3D, depth and viewport](../tests/acceptance/integration.md#integration.repeatable-3d).
- [ ] `integration.presentation` —
  [KMS flips](../tests/acceptance/integration.md#integration.presentation).
- [ ] `integration.egl-images` —
  [Native EGL/GBM and shared images](../tests/acceptance/integration.md#integration.egl-images).
- [ ] `integration.xorg` —
  [Xorg on the poulsback DDX](../tests/acceptance/integration.md#integration.xorg).
- [ ] `integration.gears` —
  [Standard GLX application integration](../tests/acceptance/integration.md#integration.gears).

## After M1

Video decode, the optional video domains and the two assessments. They keep
their criteria and are not counted for M1.

- [ ] `mpeg2.profile` —
  [Freeze codec profiles, levels, formats and quality criteria](../tests/acceptance/mpeg2.md#mpeg2.profile).
- [ ] `mpeg2.firmware-transport` —
  [Firmware/KMD pairing, MTX transport and genuine completion](../tests/acceptance/mpeg2.md#mpeg2.firmware-transport).
- [ ] `mpeg2.first-frame` —
  [Decoded-frame correctness and plane comparison](../tests/acceptance/mpeg2.md#mpeg2.first-frame).
- [ ] `mpeg2.picture-types` —
  [Required pictures, prediction and reference reordering](../tests/acceptance/mpeg2.md#mpeg2.picture-types).
- [ ] `mpeg2.field-crop-layout` —
  [Interlace/field, crop, chroma and stride behavior](../tests/acceptance/mpeg2.md#mpeg2.field-crop-layout).
- [ ] `mpeg2.errors-lifetime` —
  [Malformed streams, surface lifetime and multiple contexts](../tests/acceptance/mpeg2.md#mpeg2.errors-lifetime).
- [ ] `mpeg2.vaapi` —
  [Truthful VA-API capability/configuration and clients](../tests/acceptance/mpeg2.md#mpeg2.vaapi).
- [ ] `mpeg2.decode-presentation` —
  [Headless decode and decode+presentation accounting](../tests/acceptance/mpeg2.md#mpeg2.decode-presentation).
- [ ] `mpeg2.realtime-stress` —
  [30-minute qualified playback and measured throughput](../tests/acceptance/mpeg2.md#mpeg2.realtime-stress).
- [ ] `h264.profile` —
  [Freeze codec profiles, levels, formats and quality criteria](../tests/acceptance/h264.md#h264.profile).
- [ ] `h264.firmware-transport` —
  [Firmware/KMD pairing, MTX transport and genuine completion](../tests/acceptance/h264.md#h264.firmware-transport).
- [ ] `h264.first-frame` —
  [Decoded-frame correctness and plane comparison](../tests/acceptance/h264.md#h264.first-frame).
- [ ] `h264.picture-types` —
  [Required pictures, prediction and reference reordering](../tests/acceptance/h264.md#h264.picture-types).
- [ ] `h264.field-crop-layout` —
  [Interlace/field, crop, chroma and stride behavior](../tests/acceptance/h264.md#h264.field-crop-layout).
- [ ] `h264.errors-lifetime` —
  [Malformed streams, surface lifetime and multiple contexts](../tests/acceptance/h264.md#h264.errors-lifetime).
- [ ] `h264.vaapi` —
  [Truthful VA-API capability/configuration and clients](../tests/acceptance/h264.md#h264.vaapi).
- [ ] `h264.decode-presentation` —
  [Headless decode and decode+presentation accounting](../tests/acceptance/h264.md#h264.decode-presentation).
- [ ] `h264.realtime-stress` —
  [30-minute qualified playback and measured throughput](../tests/acceptance/h264.md#h264.realtime-stress).
- [ ] `vc1.profile` —
  [Freeze codec profiles, levels, formats and quality criteria](../tests/acceptance/vc1.md#vc1.profile).
- [ ] `vc1.firmware-transport` —
  [Firmware/KMD pairing, MTX transport and genuine completion](../tests/acceptance/vc1.md#vc1.firmware-transport).
- [ ] `vc1.first-frame` —
  [Decoded-frame correctness and plane comparison](../tests/acceptance/vc1.md#vc1.first-frame).
- [ ] `vc1.picture-types` —
  [Required pictures, prediction and reference reordering](../tests/acceptance/vc1.md#vc1.picture-types).
- [ ] `vc1.field-crop-layout` —
  [Interlace/field, crop, chroma and stride behavior](../tests/acceptance/vc1.md#vc1.field-crop-layout).
- [ ] `vc1.errors-lifetime` —
  [Malformed streams, surface lifetime and multiple contexts](../tests/acceptance/vc1.md#vc1.errors-lifetime).
- [ ] `vc1.vaapi` —
  [Truthful VA-API capability/configuration and clients](../tests/acceptance/vc1.md#vc1.vaapi).
- [ ] `vc1.decode-presentation` —
  [Headless decode and decode+presentation accounting](../tests/acceptance/vc1.md#vc1.decode-presentation).
- [ ] `vc1.realtime-stress` —
  [30-minute qualified playback and measured throughput](../tests/acceptance/vc1.md#vc1.realtime-stress).
- [ ] `vpp.feasibility` —
  [Evidence-backed VPP capabilities](../tests/acceptance/vpp.md#vpp.feasibility).
- [ ] `vpp.correctness` —
  [VPP all-plane quality, temporal ordering and lifetime](../tests/acceptance/vpp.md#vpp.correctness).
- [ ] `vulkan-feasibility.requirements` —
  [Pin the candidate Vulkan version and all mandatory capabilities](../tests/acceptance/vulkan-feasibility.md#vulkan-feasibility.requirements).
- [ ] `vulkan-feasibility.verdict` —
  [Publish independently reviewed supported or unsupported verdict](../tests/acceptance/vulkan-feasibility.md#vulkan-feasibility.verdict).
- [ ] `encode-feasibility.silicon` —
  [Prove or disprove an actual US15W encoder](../tests/acceptance/encode-feasibility.md#encode-feasibility.silicon).
- [ ] `encode-feasibility.verdict` —
  [Complete encoder decision and conditional implementation criteria](../tests/acceptance/encode-feasibility.md#encode-feasibility.verdict).

## Implementation and results

The [README status table](../README.md#status) summarizes implementation areas.
Component guides describe current interfaces and limitations; they are linked
from [the architecture guide](architecture.md). Reviewed run reports retain
their own source/build/profile identity. This checklist defines acceptance,
not a second component-status table or a record of passing runs.
