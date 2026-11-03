# Legacy-driver knowledge and documentation

Research. The reference describes selected legacy implementation contracts.
This is knowledge about legacy implementations, not poulsback driver qualification.

## Documented and checked

- **Linux EMGD 1.8:** submission/state producers and PDS/USE linkage, plus
  captured vertex streams, constants and programs; fragment interpolation,
  texture layouts, blend/depth/stencil state and MSAA setup. The reference
  separates captured words, inferred behavior and observed departures from GL.
- **X11 references:** the 2D command stream and 3D transfer route, per-operation
  routing, and EMGD/PSB performance references. A legacy GPU path does not
  establish the corresponding poulsback acceleration.
- **PSB:** packet constructors, normal-success resource-fence lifetime and X11
  interfaces for the Ubuntu 10.04 stack.
- **Linux video:** PSB and EMGD firmware transport and decoder contracts
  ([video status](video.md)).
- **Windows EMGD 3398 and retail 2030:** separate raster/state, queue, reset and
  decoder chains. The branches are not interchangeable.
- **Firmware:** selected instruction encodings, register-store consumers and
  the EMGD partial-render program's TA/DPM/3D ordering. Static decoding is
  separate from executing that path on poulsback.
- **Dell BIOS/VBIOS/ACPI:** panel tables/runtime changes, display/GTT setup and
  clock/power-reporting distinctions.

The [hardware reference](../hardware.md) owns checked cross-component facts,
units and claim IDs. [Source selection](../hardware.md#source-selection)
identifies each family's useful scope. The [legacy implementation reference](../re/README.md)
contains the public producer/consumer explanations and selected source locators.
Original blobs and raw traces are separate reference inputs; they are
not redistributed in this source repository.

## Remaining work

- Close the remaining numerical, format and state matrices needed by the
  [3D renderer](3d.md).
  Already traced producer/consumer chains do not need rediscovery.
- Verify firmware input-pointer provenance, cache visibility and final memory use.
- Adapt source-known lifetime contracts to poulsback owners/cache handling and
  qualify the implementation on hardware.
- Validate unresolved BIOS/display/power interpretations. BIOS tables do not
  specify SGX execution.

New findings must identify producer, representation, consumer, units and the
remaining boundary.

Source agreement and runtime agreement answer different questions. OEM copies
of one PVR build are not independent hardware observations. Host instruction encoding
does not by itself establish internal arithmetic precision. The
[2D](2d.md) and [video](video.md) status pages describe poulsback implementation
and qualification separately from these reference contracts.
