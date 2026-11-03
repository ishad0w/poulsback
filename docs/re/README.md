# Legacy GPU implementation reference

This reference explains selected Poulsbo mechanisms recovered from PSB, Linux
and Windows EMGD, Windows retail drivers, firmware, BIOS and saved device traces.
It follows data from its producer, through its representation, to its consumer.
Each chapter states the source family, address units and unresolved boundary.

Start with [source profiles](sources.md) and [address domains](address-domains.md)
before reusing a field or offset. Different releases can use similar names for
different layouts. Static analysis establishes what selected code constructs;
it does not establish every device effect or arithmetic rule.
The [public evidence index](evidence.md) links available source packages,
checksum/inspection commands and the limits of independent reproduction.

For a field lookup, search the component name together with the symbol, offset
or stable `HW-*` claim ID in `docs/re/` and `docs/hardware.md`. Claim IDs belong
to the hardware reference; the chapters below explain the corresponding chains.
Numeric offsets are meaningful only within the named component and address domain.

## Rendering and shaders

| Question | Chapter |
| --- | --- |
| Which layer defines coverage, coordinates and interpolation? | [API, Gallium and hardware conventions](sgx535/raster-conventions.md) |
| How are geometry, sampling and state carried into submission? | [Linux geometry and sampling](emgd-linux/geometry-and-sampling.md) |
| What do captured VDM, vertex PDS, viewport, partitions and constants contain? | [Captured vertex path](emgd-linux/vertex-path.md) |
| What do captured textures, ISP state, epilogues, PBE and MSAA show? | [Captured fragment path and EMGD's GL departures](emgd-linux/fragment-path.md) |
| Where do clip, window and reciprocal-W values go? | [Linux viewport flow](emgd-linux/viewport-coordinate-flow.md), [native vertex/W and edge contracts](sgx535/edge-w-contract.md) |
| How do attributes reach PDS/PARAM and color packing? | [Linux interpolation and packing](emgd-linux/interpolation-and-packing.md), [cross-source PARAM/PACK contract](sgx535/interpolation-pack-contract.md) |
| How are depth state, storage and ZLS connected? | [D24/ZLS contract](sgx535/depth-contract.md), [Windows depth state/storage](windows/depth-state-and-storage.md) |
| What does Windows serialize for raster/depth? | [Windows raster and depth](windows/raster-and-depth.md) |
| How does the PSB constructor link PDS, USE and depth? | [PSB linkage and depth](psb/pds-use-and-depth.md) |
| How do UV2 coordinates and sampled images reach a fragment? | [Vertex layout and texture delivery](emgd-linux/geometry-and-sampling.md#uv2-vertex-output-and-coupled-state) |
| Where do uniforms occupy physical secondary registers? | [Ordinary SECONDARY constants](psb/pds-use-and-depth.md#ordinary-fragment-secondary-constants) |
| What do native instruction fields and a decoder actually establish? | [USSE fields, entry state and decoder limits](sgx535/usse-instruction-contracts.md) |

## Submission, firmware and lifetime

- [Linux bootstrap and memory](emgd-linux/bootstrap-and-memory.md): initialization,
  memory ownership and address conversions.
- [Linux TA submission](emgd-linux/ta-submission.md): client packet, bridge,
  scheduler and firmware-facing command.
- [Linux synchronization, cleanup and release](emgd-linux/sync-cleanup-and-release.md):
  counters, callbacks, cleanup requests and unresolved last-use boundaries.
- [PSB normal-success lifetime](psb/normal-success-lifetime.md): resource fences,
  page-table withdrawal, cache transitions and bank release.
- [Windows retail bootstrap and retirement](windows/retail-bootstrap-and-retirement.md):
  code-bank setup, provider failure paths and completion-marker limits.
- [SGX firmware](firmware/sgx.md): carrier programs, patching and consumers.
- [Partial-render program](firmware/partial-render.md): static SPM/TA/3D OOM,
  context handoff, partial raster and restart paths.
- [TA packet publication](emgd-linux/submission-packet.md): user packet extents, kernel patching and the completion boundary.

## 2D, video and platform

- [Native 2D contracts](native-2d.md): fill/copy layouts, overlap direction,
  captured X11 engine routing.
- [Video decode pipeline](video/decode-pipeline.md): command producers, surfaces,
  transport and codec-claim limits.
- [MSVDX firmware](video/msvdx-firmware.md): distinct loader/container profiles.
- [MSVDX publication and surfaces](video/surface-layout.md): command publication, NV12 plane bounds and retirement limits.
- [BIOS, display and power](platform/bios-display-power.md): ROM extraction,
  runtime panel data, GTT, clocks and ACPI.
- [Runtime render state and presentation](platform/runtime-render-state.md): measured
  EMGD clock/power behavior, ISR registers and the copy-only X presentation path.
- [Configuration controls](platform/configuration-controls.md): SDK/control-panel
  interfaces and the limits of configuration data as hardware evidence.

## Scope and maintenance

These are project-authored technical explanations, with selected component hashes,
field layouts and byte locators. Vendor payloads and raw capture/analysis files are not part of this clone.
Selected public source archives and their inspection commands are listed
locally; their hashes identify bytes rather than redistribution permission.
The [source guide](sources.md#provenance-and-scope) and
[evidence index](evidence.md#what-remains-unavailable) explain this boundary.

Keep mechanisms here, [shared hardware claim IDs](../hardware.md) in the hardware
reference, and poulsback interfaces in their [component guides](../../MAP.md).
[Status pages](../status/README.md) describe current implementation;
the [checklist](../checklist.md) owns acceptance. A vendor result or matching
encoding does not qualify a new poulsback path.

When a contract changes, update its owning chapter and linked hardware claim.
Preserve numerical conflicts and unknown consumers. Do not replace them with
inferred completion, broader tolerances or a successful result from another
profile. Keep review history in Git and detailed experiment records outside
this reference.
