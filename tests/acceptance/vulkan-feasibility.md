# Vulkan feasibility decision, not a promised implementation

<a id="vulkan-feasibility"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

The [test-selection guide](../../docs/tests.md#vulkan-feasibility) distinguishes
an evidence-backed feasibility verdict from implementation or API conformance.
Use SGX535-specific source contracts and keep every mandatory requirement in
the assessment; successful GL or finite raster output does not replace them.

<a id="vulkan-feasibility.requirements"></a>

## Pin the candidate Vulkan version and all mandatory capabilities

<a id="vulkan-feasibility.spec-map"></a>

### vulkan-feasibility.spec-map

Procedure: Use Vulkan1.0 as the assessment target and map every mandatory requirement to silicon
evidence or a correct proposed lowering.

Oracle: Each requirement is supported, unresolved or incompatible with cited evidence; no
inference from Rogue, Zink or unrelated PowerVR generations.

<a id="vulkan-feasibility.verdict"></a>

## Publish independently reviewed supported or unsupported verdict

<a id="vulkan-feasibility.verdict-review"></a>

### vulkan-feasibility.verdict-review

Procedure: Resolve the assessment with an explicit implementable lowering plan or concrete
mandatory blockers.

Oracle: All mandatory capabilities have evidence and residual unknowns do not masquerade as
support. A negative verdict closes assessment only, never Vulkan functionality.
