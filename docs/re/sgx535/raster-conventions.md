# API rules, Gallium state and hardware rasterization

A rendering requirement, a frontend state value, an emitted native field and a measured image
are different contracts. A valid chain keeps all four visible:

```text
API requirement
 → selected Mesa/Gallium state and coordinate convention
 → matched state/vertex/program producer and consumer
 → source-bound hardware result with an independent reference
```

The [source-family guide](../sources.md) and [address conventions](../address-domains.md)
identify which implementation a recovered chain describes. The [hardware
reference](../../hardware.md) owns shared `HW-*` conclusions; this chapter explains the
distinctions needed to use the detailed legacy-driver chapters.

- [API invariance and Gallium edge state](#shared-edges-do-not-select-every-implementation-detail)
- [Clip W, varyings and window depth](#clip-coordinates-reciprocal-w-and-window-depth)
- [Stored representation](#representation-precedes-numerical-interpretation) and [result scope](#what-a-numerical-comparison-establishes)

## Shared edges do not select every implementation detail

[OpenGL 2.1 §3.5.1, printed page 109](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf)
requires exactly one polygon to cover a sample on an identical shared edge. That rule does not
choose a named top/bottom convention in that section. Appendix A separately requires
repeatability and defined invariance; it does not require pixel-identical output across
different implementations.

Mesa 26.2.4's Gallium interface is more specific. Its `docs/gallium/cso/rasterizer.rst` defines
`bottom_edge_rule` in Y-down coordinates. The State Tracker's
`src/mesa/state_tracker/st_atom_rasterizer.c` sets half-pixel centers and derives the edge flag
from framebuffer orientation and clip origin. The corresponding viewport transform changes Y
with that orientation. In the selected source comparison, ordinary window rendering uses
`half_pixel_center/bottom_edge_rule = 1/1`, while the FBO case uses `1/0`. These values describe
that source's branches, not one constant for every API or target.

A backend must preserve that complete request. A framebuffer flip, viewport-Y sign, winding
reversal and edge-policy bit are not independent substitutes. The selected [Windows allocation
policy](../windows/raster-and-depth.md#allocation-selected-raster-policy) constructs a coupled
three-register tuple from resource class and sample state. It does not identify SGX535's
triangle tie equation or directly encode the Gallium pair.

Reversed draw order, both shared-edge orientations and framebuffer orientation
need distinct checks. A plausible single image does not establish their
coverage or permit dropping requested Gallium state.

## Clip coordinates, reciprocal W and window depth

Homogeneous clip XYZW, window XYZ and reciprocal clip W have different roles. The selected
[Linux CPU chain](edge-w-contract.md#the-reciprocal-and-window-coordinate-contract) computes and
stores reciprocal W separately, then uses viewport scale and translation for window XYZ. Its
selected caller is outside `GL_RENDER`; this cannot be promoted into proof of the native
vertex-output convention.

For associated varyings, OpenGL 2.1 equation 3.8 uses perspective interpolation. With
window-space barycentric weights `lambda_i`, a representative component is:

```text
C = sum(lambda_i * C_i / W_i) / sum(lambda_i / W_i)
```

Window depth instead uses affine interpolation of the vertices' window-depth values:

```text
Zwindow = sum(lambda_i * Zwindow_i)
Zwindow_i = Sz * clipZ_i / clipW_i + Tz
```

Using unequal positive W distinguishes these roles. W=1 fixtures cannot select between affine
and perspective varying models. Applying perspective weighting to already transformed window
depth is a separate error; applying the viewport normalization twice is another. Exact rational
predictions can separate these models without choosing a PACK tie rule, depth quantizer or
arbitrary tolerance.

The [native input chain](edge-w-contract.md#native-attribute-upload-and-dma-consumer) copies a
selected float4 input bit-for-bit and reaches DMA controls. The selected USE output, final W
meaning, orientation and last coordinate adjustment remain separate consumers. A fourth input of
2.0 surviving conversion does not mean that the final rasterizer consumes 2.0 rather than a
transformed value.

## Representation precedes numerical interpretation

| Layer | Required distinction |
| --- | --- |
| Samples | Preserve the raw frontend request and attachment counts separately from the derived effective sample count. |
| Vertex/state upload | Meaningful state length, DMA length, PDS data extent, primary allocation and USE output count are separate quantities. |
| Varying format | TEXFLOAT, USECOLFLOAT, input channel layout and the actually stored PARAM representation have separate producers. |
| Interpolation | A requested F32/Gouraud issue does not reveal coefficient storage, arithmetic order or intermediate precision. |
| Output conversion | SCALE, clamp, source/destination formats, byte masks and PBE layout do not by themselves establish rounding or saturation behavior. |
| Depth | API compare/write state, viewport Z, ZLS addresses, storage controls and the stored quantizer are separate joins. |

The [iterator/PACK chain](interpolation-pack-contract.md) joins descriptor metadata to PDS/DOUTI
and native PACK construction. Its instruction encoders manipulate words; they do not implement a
host-side scalar rounding algorithm. The fixed 24-word state-upload USE program is also not an
application fragment shader or an interpolation-coefficient calculation.

The [depth chain](depth-contract.md) joins validated frontend comparison codes to a compiled
object and serialized ISP word. Its selected firmware store block has concrete register
consumers, while the incoming pointer/MOE/entry path remains unresolved. Neither the API name
D24S8 nor a matching register order specifies every layout, accepted value or write extent.

## What a numerical comparison establishes

Agreement between Linux EMGD, Windows EMGD and matching IMG definitions is useful
corroboration, but their shared lineage does not make them independent silicon experiments.
Runtime evidence adds a distinct observation only where the component, object/generation
and capture window are joined. Zero trace drops do not recover events outside that window.

A complete, source-bound image comparison can qualify its selected input, program, state,
capture interpretation and numerical hypothesis. It cannot silently qualify a new source build
or an arbitrary scene family.

A numerical mismatch can reject a named model without identifying the correct
one. A prospective error bound needs an applicable specification, input domain,
precision argument and independent validation. Depth preservation does not
select a general D24 quantizer; literal colour conversion does not identify
interpolation precision.

Correct pixels do not establish cache visibility, final device use, retirement,
release, presentation or API qualification. Those contracts belong to
[kernel lifetime](../../kernel/memory.md#memory-locking-and-lifetime),
[graphics integration](../../graphics.md#screen-admission) and
[result scopes](../../architecture.md#result-scopes).

An unresolved source consumer, an unimplemented driver connection and an unqualified
device result are different blockers. For example, recovering a PACK instruction does not
recover its arithmetic, while a known word encoder still needs an executable shader and
resource owner. Use the [compiler's operation limits](../../compiler.md#operations-and-numerical-scope)
and [screen admission contract](../../graphics.md#screen-admission) for those implementation
boundaries; the legacy chapters do not maintain a second driver status table.
