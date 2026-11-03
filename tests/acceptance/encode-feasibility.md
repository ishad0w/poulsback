# US15W hardware encoder feasibility, separate from decode

<a id="encode-feasibility"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

The [vendor device distinction](../../docs/re/video/decode-pipeline.md#windows-decode-declarations-and-host-boundary)
excludes US15W encode acceleration from the selected declaration. Decode output,
a CPU bitstream fixture and encode exports for other devices are different
claims. The cases below own the evidence-backed feasibility decision; they do
not promise an encoder implementation.

<a id="encode-feasibility.silicon"></a>

## Prove or disprove an actual US15W encoder

<a id="encode-feasibility.device-evidence"></a>

### encode-feasibility.device-evidence

Procedure: Compare exact device registers, firmware, KMD call paths and supported product/PCI
identities for evidence of an encoder.

Oracle: Independently reviewed evidence supports a bounded feasibility decision; unknown cannot
be labelled hardware encoding.

<a id="encode-feasibility.verdict"></a>

## Complete encoder decision and conditional implementation criteria

<a id="encode-feasibility.decision"></a>

### encode-feasibility.decision

Procedure: Publish the encoder verdict. If proven, define a separate codec/profile
implementation DoD rather than inheriting decoder PASS.

Oracle: Assessment is evidence-complete and any encoding claim has its own hardware tests; no
software path counted accelerated.
