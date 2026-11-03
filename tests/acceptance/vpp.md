# Optional hardware video post-processing

<a id="vpp"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

The [video interface](../../docs/video.md) separates decode, transfer and
presentation. A decoder callback or correct decoded plane does not establish
a post-processing operation; the capability and all-plane cases below retain
their own source and hardware requirements.

<a id="vpp.feasibility"></a>

## Evidence-backed VPP capabilities

<a id="vpp.capability-map"></a>

### vpp.capability-map

Procedure: Establish actual US15W post-processing hardware operations and their exposed
surface/firmware contracts.

Oracle: Each claimed operation has hardware provenance and implementable safe ownership;
unsupported operations stay explicit.

<a id="vpp.correctness"></a>

## VPP all-plane quality, temporal ordering and lifetime

<a id="vpp.operation-matrix"></a>

### vpp.operation-matrix

Procedure: Run every claimed VPP operation at boundary layouts and chained with
decode/presentation.

Oracle: Every output plane satisfies predetermined quality and temporal reference; guards,
fences and ownership remain correct.
