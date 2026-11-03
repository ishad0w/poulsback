# Captured EMGD fragments, textures and render state

This chapter explains captured texture, fragment-state and FBO behavior of
Linux EMGD 1.8 Build 2032 on SGX535. The
[source profile](../sources.md#linux-emgd-18) identifies its OGL, Services,
initializer and kernel bytes. Captured fields interpreted on the host are
separate from inferred behavior and device qualification of poulsback.

Instruction fields are checked with the separate
[rev121 CPU decoder](../sgx535/usse-instruction-contracts.md#sgx535-revision121-cpu-decoder).
Matching disassembly is not an independent arithmetic test. Vendor-input and
raw-capture availability is described in the local
[evidence index](../evidence.md#what-remains-unavailable).

## Textures

A nondependent read issues DOUTI/DOUTT from pixel PDS. A dependent read uses
the same three texture words from pixel SECONDARY registers and a USE sample
with its DRC wait. Cube, volume, projective, biased and shadow reads take this
second route in the captured programs.

| Word | Selected fields and units |
| --- | --- |
| DOUTT0 | Channel replication, gamma, LOD bias in eighths, mip clamp, anisotropy, min/mag/mip filters and U/V/R addressing. MIPMAPCLAMP is the highest inclusive sampled level; 15 denotes no mipmapping. |
| DOUTT1 | Texture type and format. POT forms store logarithmic dimensions; stride forms store width minus one and height minus one. The captured POT form is authoritative for this build. |
| DOUTT2 | Surface byte address, masked at bits 31:2; rebasing can name a mip level rather than the allocation's first texel. |

Captured RGBA8 storage is B8G8R8A8; RGBA4 and RGB5_A1 become A4R4G4B4 and
A1R5G5B5. L8/A8/I8 use U8 with channel replication, LA88 uses U88. RGBA16F
uses two F1616 planes, RGBA32F four F32 planes, with one issue per plane and a
USE merge. DXT1 and DXT5 use formats 20 and 22 in this EMGD build. A format
name does not establish its upload conversion or renderability.

POT 2D textures are Morton-twiddled with Y in the low bit. Rectangles consist
of squares along the long axis. DXT twiddles by compressed block. Mip levels
follow consecutively without padding to 1×1; cube faces follow in GL order;
3D twiddles Y, X, Z. An NPOT clamp texture without mipmaps can use linear
rows padded to 32 texels. DOUTT names the sampled twiddled copy, not a retained
linear staging copy. Border colour is stored in border-map texels; mipmapped
and rectangular border-map cases were not captured.

The [descriptor producer](geometry-and-sampling.md#texture-descriptors-and-fragment-pds)
remains useful for the static chain and address units. Captured layout does
not establish general filtering precision, cache visibility or safe feedback.

## ISP state and epilogues

| Mechanism | Captured contract |
| --- | --- |
| Depth | ISPA selects comparison and no-depth-write. Viewport Z centre/scale implements DepthRange. |
| Stencil | ISPC carries comparison, sfail/zfail/zpass, compare mask and write mask. Two-sided state adds back-face ISP groups. Window Y orientation swaps GL front/back relative to these groups. |
| Offset and colour mask | ISPB has signed five-bit offset factor/units and UPASSCTL byte enables. A colour mask alone uses opaque pass type 0. |
| Scissor | Region clip is an inclusive tile box. Pixel-exact scissor uses viewport-pass objects to clear/set a draw mask; it is not only a tile rejection. |
| Clears | Pretransformed geometry carries clear values. A background object alone does not provide a stencil clear. |
| Facing | USE special register g16 bit 0 carries back-face state in the captured window orientation, independently of two-sided ISP enable. |

Depth testing and stencil are ISP state; blending, logic operations, alpha
test/discard and depth feedback have USE epilogues. Packed colour uses B,G,R,A
byte order. SOP2/ASOP2 select colour/alpha factors and equations; SOP2WM and
UPASSCTL provide masked writes. Constant blend colour and feedback words are
SECONDARY data, not a reason to regenerate code for each reference value.

Alpha test and discard use punch-through pass types; explicit fragment depth
uses depth feedback. A first USE phase supplies the ISP verdict and ends;
DOUTU1 names a second phase at a 16-byte boundary which writes colour, with
blending if required. An END on phase one is not the final colour store.

Pixel SECONDARY DMA starts at SA0. Captured GLSL programs reserve SA0–2 and
use SA3 onward for uniforms, then literals, dependent-texture words, blend
colour and feedback data. Fixed-function alpha testing can use SA0/1 directly.
Its alpha reference is eight bits in STATE1, with the applicable depth/stencil
state in STATE0/1. The layout is program-class dependent.

## Render targets and MSAA

Packet `+0x34` names a mapped 0x144-byte render-details block; its `+0x54`
names EOT pixel PDS, whose DOUTU names the USE program. These pointers expose
window and FBO PBE words, including twiddled targets. Each format needs its
complete descriptor/program interpretation and device validation before reuse.

SGX535's PBE uses two emits. Their sources separate pack mode, memory layout,
line stride, target byte address, origin and last pixel. Emit-2 sideband SCALE
selects the MSAA resolve. There is no general Y-flip field in these words.

The captured multisample scene uses a 4× window visual. Against 1× it sets TE_AA's
X/Y bits, 3D_AA_MODE and sample positions `0xeaa26e26`; region-header and TPC
storage grow fourfold. PBE emit-2 sideband changes from `0xc` to `0xd`, resolving
on-chip colour into a single-sample X buffer. Positions are (6,2), (14,6),
(2,10), (10,14) in sixteenths of a pixel. Depth uses the sample grid, but this
window capture enables no depth store. This is evidence of EMGD's 4× setup,
not poulsback MSAA support or a general multisample texture contract.

## EMGD departures from GL

The probes compare explicit GL expectations with EMGD output; their corrected
software-reference runs are separate from EMGD. A matching legacy encoding
does not require reproducing the following defects.

| Captured behavior | Implementation boundary |
| --- | --- |
| FBO output is vertically mirrored by sampling/readback. | Choose the API's texture orientation and adjust viewport, scissor, culling and facing together. |
| Repeat/mipmapped NPOT inputs are resampled to POT. | Exact NPOT texels need their own supported layout/addressing strategy. |
| Depth16 upload stores eight-bit values despite a U16 descriptor. | Keep the correct stored depth width. |
| Shadow GREATER and ALWAYS produce incorrect results. | Implement the GL comparison, not the observed faulty result. |
| `gl_FrontFacing` is always true in the probe; `gl_BackColor` works. | The captured compiler gates its facing read incorrectly; the hardware facing source is g16. |
| Occlusion queries count discarded fragments. | Count placement relative to feedback needs a separate device check. |
| 16-bit target readback shifts full intensity below 255. | Preserve required bit expansion rather than copying that conversion. |
| Rectangle shaders and some valid attachment combinations are refused. | Advertised extension strings do not prove an executable path. |
| Disabling multisampling leaves the captured 4× edge unchanged. | This does not implement GL's required centre-sampled behavior when disabled. |

Window depth/stencil readback does not establish stored planes: colour tests
can pass while depth reads zero and stencil reads zero or the last clear.
EMGD's tested A8-target path faults, and a separate stencil renderbuffer can
crash its userspace. Those paths cannot serve as format-correctness references.
Fixed-function ADD multiplies alpha; its RGB addition rule does not imply alpha
addition.

## Limits

Captured uniforms, descriptors and state supplement static chains; they
do not resolve every compiler producer. Transcendental/derivative precision,
divergent flow, bottom-up FBO facing, uncaptured epilogues, generic ZLS layouts,
all PBE formats and final resource use retain separate tests. General edge,
PACK and D24 numerical contracts remain unresolved. Current backend
and API admission belong to [compiler](../../compiler.md),
[graphics](../../graphics.md) and [status](../../status/README.md).
