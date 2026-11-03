# Geometry, state PDS and sample interpretation

This chapter separates a recovered producer from a still-missing raster rule. The reference is
Ubuntu 11.04 Linux EMGD 1.8 Build 2032 `libEMGDOGL.so`, 2,791,512 bytes, SHA256
`3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a`. Every function address below
is a **Ghidra analysis byte VA** unless labelled otherwise. See [source families](../sources.md#linux-emgd-18)
and [address domains](../address-domains.md).

- [Viewport vector](#viewport-coordinate-vector-and-matrix-update-branch)
- [State upload counts](#state-object-four-counts-that-must-not-be-collapsed) and [padding](#padding-is-a-construction-policy)
- [Raw and effective samples](#raw-sample-state-and-effective-execution-state)

<a id="viewport-a-closed-prefix-an-open-downstream-join"></a>

## Viewport: coordinate vector and matrix-update branch

The checked route is:

```text
pvroglViewport 0xe74d0
 → context dispatch at +0x3678
 → __glim_Viewport 0x64d40
 → callback +0x4da0 (installed as 0x3a1e0 in the checked branch)
 → drawable-origin update 0x3b550
 → viewport-vector update 0x3b6a0
 → callback +0x4d64: matrix update, detailed in the coordinate-flow chapter
```

The dispatch-table byte offset `0x4c8` is **not** SGX MMIO register `0x4c8`. The pointer stored
at context `+0xaf40` addresses embedded CPU storage at `context+0xaf50`; it is not a hardware
register address.

With integer drawable origins Ox/Oy, buffer height H and branch parameter q, the selected
formula is:

```text
Sx = width / 2
Tx = x + width / 2 + Ox
normal Y:   Sy = height / 2;  Ty = y + height / 2 + Oy
flipped Y:  Sy = -height / 2; Ty = H - q - (y + height / 2) + Oy
vector = [Sx, Sy, Sz, 1, Tx, Ty, Tz, 0]
```

Sz/Tz are copied from the existing context depth-range fields. The factor one half is extent
scaling. It does not add a half-pixel bias. For the explicitly conditional 64×64, zero-origin,
q=0, flipped-Y example the XY values are `32, -32, 32, 32`. This does not prove that later
stages add no bias.

The actual instruction at `0xa5c5b` installs `0xa5360` in callback `+0x4d64`. The
[coordinate-flow chapter](viewport-coordinate-flow.md#callback-installation-and-relocation)
explains its relocation and call arguments. The callback receives a different matrix,
not the `+0xaf50` vector, and reaches CPU
matrix refresh and function selection. This does not close the vector-to-final-primitive upload
path.

A numeric `af40` literal in a PIC/GOT-relative expression is not a context
read, and an `af78` argument does not establish an `af50` alias. The downstream
edge equation and general W path require their actual consumers; these fields
do not justify a coordinate shift, winding change or TSP toggle.

The Linux
[DepthRange chain](../windows/depth-state-and-storage.md#linux-depthrange-reaches-the-actual-viewport-vector)
resolves DepthRange through its actual dispatch table into the Z scale and translation stores in this
vector. Read that chapter for the scaling field and floating-point boundaries rather than
substituting a Windows context offset.

## State object: four counts that must not be collapsed

The EMGD generic-coordinate producer gives a useful complete state chain:

```text
0x140310: derive output shape and linked state fields
 → 0x135550: dirty-state dispatch
 → 0x134ea0: serialize MTE state
 → 0x13cea0: construct state upload
 → 0x13afc0: DMA descriptor and 0x14f570: PDS
 → fixed state USE selected through object +0x38
```

For the checked flags/coordinate-0 dimension, `0x140310` produces eight output DWORDs,
`OUTSELECTS=0x08001000`, `TEXSIZE=7`, and explicitly writes `TEXFLOAT=0`. The serializer writes
OUTSELECTS, MTECTRL, TEXSIZE, then TEXFLOAT; its header selects both texture-format words. A
missing TEXFLOAT word is a real source construction difference, even when the omitted value
happens to be zero.

The selected fixed state USE program copies 16 then eight DWORDs and emits 24. Its producer
writes the six words:

```text
a0000000 28a1f001  a2000800 28a17001  a0200c00 fb274000
```

These are this exact producer's words, not a complete shader-language specification. The pointer
chain selecting the program is retained rather than inferred from matching instruction patterns.

The TA state serializer at `0x134db0` consumes three different values:

| Source field | Unit and destination |
| --- | --- |
| object `+0x2184` | PDS address in 16-byte units |
| object `+0x2188` | PDS data bytes in 16-byte units, state bits 26–31 |
| object `+0x218c` | Primary-attribute allocation bytes rounded up to 16-byte units, state low eight bits |

Thus the meaningful MTE record length, DMA length, primary allocation and USE output count are
four quantities. The source publishes 96 primary bytes, encoded as `ceil(96/16)=6`, for its
24-output state program. A primary field of 3 provides only 48 bytes even if the PDS data extent
is independently correct.

The important invariant is that meaningful MTE length, DMA length, primary allocation and fixed
USE output count are validated separately. [HW-PDS-005](../../hardware.md#use-banks-and-pds-addressing)
owns the shared state-allocation contract.

## Padding is a construction policy

Explicitly padding a meaningful 17-, 11- or two-DWORD record to 24 initialized words is a
separate construction policy. EMGD's meaningful DMA length is variable. Zero-padding does not
prove that hardware ignores every padding word or that the transport bytes match EMGD. A
primary-allocation discrepancy is not by itself an explanation for a coverage or smooth-color
error.

PDS-to-USE task entry units are a separate field family. The secondary [PSB linkage
chapter](../psb/pds-use-and-depth.md#use-entry-bank-and-relative-byte-offset) explains
the checked bank-relative projection, including why 8-byte instruction size does not permit
every 8-byte-aligned entry. Do not apply the initializer's `offset >> 3` service conversion to
this state or task word.

## UV2 vertex output and coupled state

The same matched library has an actual two-coordinate caller at analysis
`0x153100`: dimension selector 2 and flags `0x40` reach `0x140310`, two input
records reach `0x1404b0`, and built-in program `0x152fa0` is selected. This
narrows the generic table interpretation to a concrete **STATIC** construction.

| Quantity | UV2 | Earlier vec4 coordinate control |
| --- | --- | --- |
| Output record | Six DWORDs, 24 bytes | Eight DWORDs, 32 bytes |
| OUTSELECTS, `S+0x216c` | `0x06001000` | `0x08001000` |
| TEXSIZE, `S+0x2174` | `0x00000001` | `0x00000007` |
| TEXFLOAT, `S+0x2170` | Zero | Zero |

The selected program copies position with mask `0xf` to logical output 0 and
coordinates with mask `0x3` to output 4. The checked F32 converter associates
outputs 0..3 and 4..5. The tables at `0x289700/0x289718` and serializer
`0x134ea0` keep output width and both texture-format fields together. This
class has no synthetic third coordinate; the eight-word example above remains
a separate construction.

The six-word vertex is not the state-upload extent. `0x13cea0` retains its
96-byte PRIMARY publication and the selected generic MTE record has eleven
meaningful words. Logical GENERIC indices are also not physical texture-set
numbers: a compact one-set layout needs an explicit mapping to physical set 0.
Selected final native phase bytes, general W/depth arithmetic and actual
sampled-image delivery remain separate boundaries.

## Texture descriptors and fragment PDS

The [captured fragment contract](fragment-path.md#textures) supplements this static
producer chain with actual format/filter/mip descriptors, memory layouts and
dependent-sampling programs. The [captured vertex contract](vertex-path.md) likewise joins
ordinary vertex programs to their VDM/PDS and per-draw state. Neither moves the
older static or numerical observations to a different source profile.

The matched Linux EMGD route is:

```text
OGL validation 0xcfa40 → context+0xb014 snapshot → cache 0xcefa0
 → state constructor 0x13f8c0 → binding 0x1400c0 → state+0x2194
 → dispatcher 0x135550 → fragment setup 0x13c2a0 → generator 0x14f250
```

Surface constructor `0x1436f0` combines a format row and layout from `0x143330`.
For the selected format4 class, one descriptor is three DWORDs. The format-row
stride and per-unit storage reservation are each 48 bytes; neither is the tuple
length. State `T+0x9f0` stores the tuples. Word 2 contains the GPU **byte address**
masked by `0xfffffffc`, rather than an address divided by four. The explicit
pitch payload uses DWORD-count-minus-one and overlaps sampler fields; it cannot
be ORed into arbitrary addressing modes.

Compiled-input consumer `0x136e80` writes selector `input.word1 + input.kind*4`
and a separate iterator. `0x13c2a0` forwards both arrays. For each selector other
than `0xffffffff`, `0x14f250` reads three words at `T+0x9f0 + selector*12`,
allocates PDS data and emits DOUTT. DOUTI carries coordinates; a compatible
preceding no-texture issue can merge only under its checked conditions.
PDS data rounds to 16 bytes. Texture-last and USE-last are different controls.

### Non-gamma packed result and ordered PRIMARY inputs

Internal RGBA8 format `0x8058` leaves `texture+0x98` clear. `0xcd350` therefore
does not add create flag `0x2000`; `0x1436f0` leaves native word-0 bit 27 clear.
The selected format4 row, at ELF/file offset `0x2a8380`, selects a single packed
U8 result. Its compiler mapping `0x60a`, ordinary identity slot `0x688` and
consumer `0x23d650` join the four channels to byte offsets 2,1,0,3. This is a
surface/compiler result layout, separate from a shader or view swizzle. Gamma,
expanded two-DWORD results and unsupported addressing classes need their own
contracts; the spelling BGRA8 or LINEAR alone is insufficient.

The nondependent input chain `0x21a5f0 → 0x21a7d0 → 0x21a4a0 → 0x2406f0`
creates load records, removes/orders live records, assigns cumulative extents
and binds the complete fixed PRIMARY array at base 0. A sample is not inherently
first or four words. The selected packed sample contributes one DWORD; its
physical PA location follows all preceding retained input extents. Source SSA
identity and coordinate eligibility must be established independently of an
in-range numeric ID. DMS allocation rounding is a separate count.

### Nondependent arrival and task fields

The matched `0x136e80` pairs last-texture iterator bit `0x800` with dependency
`0x100000` at shader byte offset `+0x364`. `0x13c2a0` transfers this field into
DOUTU task word 0. `0x14f250` emits DOUTU before DOUTI/DOUTT; the task dependency
supplies the source-backed USE gating, rather than command order alone.
For SGX535 revision121's non-unlimited-phase interface, DOUTU0 bit 20 is
TEXTUREDEPENDENCY and DOUTI bit 11 is TEXLASTISSUE. A separate USE iterator has
its own dependency. A dependent USSE SMP/DRC/WDF sequence is another route and
must not be inserted as a guessed fix for nondependent delivery.

| Task quantity | Selected encoding units |
| --- | --- |
| Entry | GPU byte address relative to an owned USE heap, split into code-base index/512-KiB segment, 32-KiB COFF and 16-byte EXE units |
| TEMP count | Scalar count, alignment shift 0; low five bits in word 1 bits 31:27, upper four in word 2 bits 3:0 |
| SDSOFT | Independent word-2 bit 5 |

These are selected producer/consumer contracts. They do not establish exact
scheduler timing, filtering arithmetic, sampled pixels or real image
READ/cache/MMU ownership. Code placement, complete initialized spans, resource
fences and actual execution require their own joined checks. Source-decoder
agreement and CPU construction do not by themselves qualify a draw path
([screen admission](../../graphics.md#screen-admission)).

## Raw sample state and effective execution state

A frontend value is not necessarily the number of samples physically used by a single-sample
image. Mesa 26.2.4 can supply raw `min_samples=0` and attachment/sample-storage counts 0 while
the framebuffer has one effective sample. Its original sample record must remain unchanged; a
separate lowering record explains the effective count.

This is a **Mesa/CPU interface distinction**, not a recovered EMGD integer encoding. Keep the
copied raw request separate from a derived effective-count record; neither may silently replace
the other.

NIR printer `inputs_read: 1` and `outputs_written: 2` enumerate **set-bit indices**, not integer
masks. The corresponding masks are `1<<1` and `1<<2`. A printer interpretation cannot supply an
unobserved execution result.

The Windows coupled raster tuple in `HW-RASTER-002` is another source policy, not a Gallium
sample flag. Keeping original sample metadata, derived count, register policy and observed pixel
coverage separate prevents one layer's convenience value from changing the intended GL test.

## Depth, interpolation and next source questions

The primary EMGD viewport/state producers do not yet provide a complete decoded native
depth-storage or interpolation numerical model. The [PSB depth
chapter](../psb/pds-use-and-depth.md#depth-storage-and-attachment) provides a
precise secondary source for attachment, ZLS relocation and clear construction. Its span map, a
stored low-24-bit interpretation and a general quantizer remain different
contracts.

The `+0x4d64` installation and selected matrix-update branch are recovered in [coordinate
flow](viewport-coordinate-flow.md). The [follow-through](../sgx535/edge-w-contract.md) resolves
CPU reciprocal/window stores, but their selected caller is outside `GL_RENDER`. The native
attribute-converter and DMA chain is described in that chapter; the final selected USE position
output remains open. The [fragment iterator chain](../sgx535/interpolation-pack-contract.md)
reaches PDS/DOUTI; coefficient arithmetic and packing precision remain separate. The explicit
TEXFLOAT producer establishes that field; it does not specify internal
rounding or edge ties. A corpus of traces and a known final color do
not supply absent links by vote.
