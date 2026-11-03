# Depth: from an API comparison to an emitted ISP word

The matched Linux EMGD 1.8 binary provides a continuous host-side chain from
`DepthFunc`/`DepthMask` to the serialized ISP state word. This closes a specific gap in the
[frontend and storage chapter](../windows/depth-state-and-storage.md). It does not close the
GPU's general depth quantization, all ZLS state consumers, or ordinary submission retirement.

The useful separation is:

| Part | What the retained evidence establishes | Boundary |
| --- | --- | --- |
| Vertex Z | The API depth range produces viewport scale/translation; source-family and target flags matter. | The default `.5/.5` pair is not a universal viewport transform. |
| Compare/write control | Linux frontend byte → state compiler → bound object → serialized ISP word, with the branches below. | Other raster state can modify the compiled word; no claim that every combination has run on Dell. |
| Storage addresses | Matched EMGD host producers transform the ZLS envelope; the selected firmware contains a ZLS register-store block unchanged in a retained live CPU view. | The incoming firmware pointer and active MOE state still prevent a complete UM-memory-to-register proof. |

- [Frontend state and compare/write lowering](#frontend-state-reaches-the-compiler)
- [Binding and serialization](#the-compiled-word-is-bound-and-serialized)
- [Firmware ZLS stores](#zls-publication-and-the-selected-firmware-store-block) and [unresolved pointer](#the-remaining-pointer-boundary)
- [Selecting a pre-D24 observation](#selecting-a-pre-d24-observation)

## Exact Linux input and address convention

The primary input is the Ubuntu 11.04 EMGD 1.8 `libEMGDOGL.so`, 2,791,512 bytes, SHA-256
`3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a`. It matches the library
identified in the retained legacy TA captures. That match authenticates the binary family; it
does not prove that a particular captured kick exercised every branch described here.

Unless marked otherwise, function addresses below are **Ghidra analysis VAs**. For this ELF
import, analysis VA = ELF VA + `0x10000`. Resolve file offsets through `PT_LOAD`;
subtracting the analysis bias alone does not resolve data file offsets. Object offsets are
CPU structure offsets. Nothing in this chapter treats a Ghidra VA as a device address.

## Frontend state reaches the compiler

The [DepthFunc/DepthMask
analysis](../windows/depth-state-and-storage.md#comparison-and-write-mask-share-frontend-state-not-an-isp-word)
identified comparison codes `[8,1,2,3,4,5,6,7]` for GL enums `0x200…0x207`. They occupy byte
`context+0xafe3`. Bit `0x10` is write-disable: DepthMask false sets it, and true clears it.
Invalid enums and forbidden Begin/End calls are rejected before that update.

The connection is:

```text
frontend context +0xafdc: 0x38-byte state descriptor
                    +7: comparison code and write-disable

0xcf6f0
  → direct compiler callback, or 0xcf5a0 descriptor cache
  → 0x13e9b0 state compiler
  → handle retained at frontend context +0xafc8
  → 0x13f780 binds the compiled object into backend state
```

At analysis `0xcf72a` the caller supplies `context+0xafdc`; at `0xcf733` it calls the installed
compiler slot. The cached branch supplies the same pointer at `0xcf791`. Since
`0xafdc+7=0xafe3`, this is the actual frontend byte, not an independently guessed descriptor
layout.

The callback is also resolved rather than inferred from similar code. With ELF PIC base
`0x2a3294`, installer instructions at **ELF** `0x2f6fb/0x2f701` load GOT slot `0x2a31a8` and
store it to slot `0x2adb84` (analysis `0x2bdb84`). The GOT word is at **file offset
`0x2a21a8`**, contains ELF target `0x12e9b0`, and has an `R_386_RELATIVE` relocation. Its
analysis target is `0x13e9b0`. This is an ELF relocation/state installation contract, not a
runtime ASLR address observation.

`0xcf5a0` hashes and compares the 14-word descriptor. A cache hit returns an existing compiled
handle; a miss invokes the compiler and saves a `0x38`-byte copy for the cache. The assembly of
`0xcf6f0` skips rebinding when its returned changed flag is zero. The decompiler simplifies this
register-argument routine and removes a block; use its assembly when checking that branch. A
changed API call does not imply that the compiler executes on every draw.

## Compare and write-disable lowering

The compiler allocates a `0x70`-byte object, registers it through handle helper `0x134340`, and
then fills its fields. Its selected depth prefix is:

```text
if descriptor.flags & 0x2:
    b = descriptor.byte[7]
    compare = ((b & 7) << 22) if (b & 0xef) != 0 else 0x00c00000
    write_disable = 0x00100000 if (b & 0x10) != 0 else 0
else:
    depth_bits = 0x01d00000
```

The byte load is at `0x13eab6`; the mask and shift at `0x13eac8/0x13eacb`; the write-disable
OR/conditional selection at `0x13ead9…0x13eae2`. The special zero-code branch is at `0x13f178`.
This chapter does not infer the complete attachment/test-enable producer solely from descriptor
flag `0x2`.

| GL comparison | Frontend code | Compiled comparison bits |
| --- | ---: | ---: |
| NEVER | `8` | `0x00000000` |
| LESS | `1` | `0x00400000` |
| EQUAL | `2` | `0x00800000` |
| LEQUAL | `3` | `0x00c00000` |
| GREATER | `4` | `0x01000000` |
| NOTEQUAL | `5` | `0x01400000` |
| GEQUAL | `6` | `0x01800000` |
| ALWAYS | `7` | `0x01c00000` |

Frontend NEVER=`8` survives the `0xef` test and becomes hardware code zero through `&7`. A raw
descriptor byte `0` or `0x10` instead selects the special LEQUAL branch. Therefore “every
descriptor byte is simply shifted” would be wrong, while `(GLfunc−0x200)<<22` is correct for
these eight validated API codes. These comparison bits apply with either write-mask value;
write-disable adds `0x00100000` without changing them.

This is a prefix, not the whole state compiler. Alpha, stencil, blending, primitive and other
flags still influence the eventual object. For example, selected alpha conditions OR
`0x00300000`, including write-disable. The final store at `0x13eea9` writes the resulting value
to **compiled object DWORD 0**. The selected depth lowering does not cover all interfering state
combinations; a general renderer must account for them.

The vendor compiler also has a conditional write to descriptor byte `+0xb` when `local_54 == 0`
and `(flags & 0x40004) == 0x40004`; the full record preserves those predicates. This is mutable
host state. Reconstruct its effect in a driver-owned compilation result rather than mutating an
immutable submitted descriptor.

## The compiled word is bound and serialized

The ordinary caller passes the handle at frontend `+0xafc8` and the backend context at frontend
`+0xc164` to callback slot analysis `0x2bdbec`. Its installer uses GOT ELF `0x2a28dc`, file
`0x2a18dc`, containing ELF target `0x12f780` with `R_386_RELATIVE`. Analysis procedure
`0x13f780` rejects a null handle or failed lookup through `0x1344a0` before changing bound
state.

The handle insertion/lookup instructions retain and recover the object pointer from handle-entry
`+8` (ELF `0x1243f9`, `0x12447a`, `0x1244db`). The binder then compares the new object with the
previous one, sets backend dirty bit `0x2` when its first three words differ, and writes the
bound pointer at:

```text
*(backend_context +0xaa64) +0x2190
```

The store is at analysis `0x13f835`. Binding host state is not a GPU kick, fence, or proof that
the object has completed its final use.

An existing complete serializer record closes the next link. Procedure `0x134ea0`, in its normal
`param_4==0` branch, reads exactly this bound pointer at `0x134fc9`. If backend dirty bit `0x2`
is set, it loads object DWORD 0 at `0x134ffc`, optionally applies the primitive override, and
writes the serialized word at **record byte offset `+4`** at `0x135017`. Record-header mask
`0x1` marks that first state word as present. Conditional extra words follow according to the
flags; a fixed-length record must not be assumed.

The primitive override is `(word & 0xfff87fff) | primitive_bits`. The mask preserves
compare/write mask `0x01d00000`. The checked table entries 0 through 9 at analysis `0x2b9280` /
ELF `0x2a9280` / file `0x2a8280` are `[0,−1,−1,−1,−1,0x8000,0x18000,0x20000,0x10000,−1]`; `−1`
skips the override, and the remaining entries do not add depth bits. This bounded check is not
validation of an arbitrary table index.

Dirty-state dispatcher `0x135550` calls this serializer. Its successful output continues through
`0x13cea0` and the existing [state PDS/USE upload
chain](../emgd-linux/geometry-and-sampling.md#state-object-four-counts-that-must-not-be-collapsed).
That chapter owns data size, DMA and primary-allocation contracts. The connection runs from the
API depth byte to the actual serialized word; it does not requalify the upload class or recover
every on-chip operation.

## ZLS publication and the selected firmware store block

The next consumer is located in the **matching EMGD 1.8 firmware**, rather than inferred
only from the host structure. This is a bounded static and retained-byte result. It does not yet
prove the entire effective-address walk from that structure to the registers.

`SGXKickTA` targets matching `libemgdsrv_um.so`, SHA-256
`d9f741a40b293596b958b783af3581af84015dfd49c61c5f1a30240a5a459cbf`. The procedure's analysis VA
is `0x18420`; its complete body has 4,795 bytes across four ranges. The [address-envelope
chapter](../windows/depth-state-and-storage.md#from-linux-draw-state-to-the-zls-address-envelope)
owns the upstream OGL producer and base-selection branches.

In the new-state branch (`input.flags & 4`), UM obtains a render-state owner and copies **`0x88`
bytes** into its allocation. Distinguish the wrapper from the copied payload: wrapper word 4
becomes payload byte 0. Consequently:

| Copied render-state byte offset | Host-produced value |
| ---: | --- |
| `+0x04` | Selected ZLS base, aligned with `0xfff00000` |
| `+0x28` | ZLS control from kick input `+0x48` |
| `+0x2c`, `+0x30` | First address minus selected base, masked by `0x0ffffff0` |
| `+0x34`, `+0x38` | Second relative address with its selected bit-31 modifier |

The allocation's device address enters TA command byte `+0x34`. On the existing-state branch UM
instead emits changed `(device address, value)` pairs; its patch destinations include exactly
those six offsets. The count and patch list are at command `+0x48` and `+0x230`. These are
memory/command addresses, not writes to MMIO `0x480`. Host publication alone does not show that
firmware has read this particular allocation.

### Actual firmware words, including the retained live copy

The initializer carrier remains `libemgdsrv_init.so`, SHA-256
`3f18f0715e8d2174d9744a577912a7a5a6afba41e1e3f9ec90a859071ecf3121`. The selected
`pbuKernel3DProgram` is 26,776 bytes, SHA-256
`ab3b07f88436966ff213bf038d69d2f7f382c4d0e75225cf4a21c5c806fab8fd`, at carrier ELF VA `0x13a00`,
file offset `0x12a00`. The SPM companion is 18,328 bytes, SHA-256
`7c6bde17c9b6f4ed76c19da4df2c815e09bd4edf3d157fbf9bef2d755e8fa81e`, ELF VA `0xf260`, file offset
`0xe260`.

The following fields are decoded as SGX USE instructions using the selected revision-121
definitions. The carrier's x86 architecture does not determine the embedded program's
instruction architecture.

| 3D blob offset | SPM blob offset | Low / high instruction words | Selected interpretation |
| ---: | ---: | --- | --- |
| `0x2810` | `0x0e10` | `2001822c / fe290000` | STR global index `0x32c`, value TEMP4; corresponds to byte register `0xcb0` |
| `0x2878` | `0x0e78` | `a0608000 / e8037200` | LD: absolute, 32-bit, TEMP3 destination, base register 2, repeat 8, POST, DRC0, MOEEXPAND clear |
| `0x2880` | `0x0e80` | `00000000 / f9200000` | WDF0 |
| `0x2888` | `0x0e88` | `200081a0 / fe290000` | STR global index `0x120`, value TEMP3 |
| `0x2890` | `0x0e90` | `20008221 / fe290000` | STR global index `0x121`, value TEMP4 |
| `0x2898` | `0x0e98` | `200082a2 / fe290000` | STR global index `0x122`, value TEMP5 |
| `0x28a0` | `0x0ea0` | `20008323 / fe290000` | STR global index `0x123`, value TEMP6 |
| `0x28a8` | `0x0ea8` | `200083a4 / fe290000` | STR global index `0x124`, value TEMP7 |

The encoded global-register indices `0x120…0x124` correspond, in four-byte register units, to
`ISP_ZLSCTL`, `ZLOAD_BASE`, `ZSTORE_BASE`, `STENCIL_LOAD_BASE`, `STENCIL_STORE_BASE` at
`0x480…0x490`. Keep the encoded index, byte MMIO offset, firmware blob offset and render-state
offset separate. The word codec does not turn a firmware offset into a GPU address.

The relevant encoding is independently reconstructible. LDRSTR has opcode 31, OTHER category 2
and sub-opcode 6; STORE adds bit `0x80000`. The immediate global-register operand is:

```text
index = (low & 0x7f) | (((low >> 14) & 0x7f) << 7)
```

Its source value is TEMP `((low >> 7) & 0x7f)` for these words: source-bank bits and the
source-extension bit select TEMP. The matching assembler routines are `EncodeLDRSTRInstruction`,
`EncodeLDRSTRGlobalRegisterNumber` and the LD encoder. The encoder target has SHA-256
`0819f3e7461bd21a0e5dbd0fb614ab875436389b6b4cb573a7e583b583b3ba01`. It corroborates the fields,
not a separately matched runtime firmware lineage or every silicon memory effect.

The carrier programs identify the static store block. Runtime firmware may be
patched, so matching carrier words alone do not identify executed instructions,
active depth state or final device writes. Those runtime bindings remain a
separate qualification boundary.

### The remaining pointer boundary

The register **consumer and its immediate register feeder** are concrete. The complete
UM-to-consumer join is not. The preceding 3D block `[0x27c0,0x2888)` contains the base setup, an
IMAE, three earlier LD groups and the final LD/WDF. The effective incoming value of `r2`, the
active MOE increments and the successor path into this block have not been reconstructed.

Do not assign successive render-state offsets by adding the displayed repeat counts. In
particular, MOEEXPAND is clear on the final load, and an earlier load at `0x2838` uses a
different field combination. The matching order of host `+0x28…+0x38` and the five register
stores is useful corroboration, not proof of their effective addresses. The next bounded
analysis is this exact base/MOE path and its entry caller, joined to the already located command
`+0x34` device pointer. Repeating the API setter or scanning another binary for `0x480` does not
resolve it. WDF0 waits for its selected load; it is not evidence of fixed-function PBE/ZLS
retirement.

## Storage and quantization

The selected ZLS format value 1 names I24ZI8S for load and store. A format
name does not specify conversion precision, rounding, tiling or stencil
initialization. ZLOADEN/SLOADEN, ZSTOREEN/SSTOREEN, FORCEZLOAD/FORCEZSTORE and
load/store masks have separate roles; an implementation must bind their values
to the intended attachment lifetime rather than infer them from a format name.

The general D24 quantizer remains unresolved. Exact affine interpolation of
window Z, interpolation with rounded coefficients, intermediate F32 conversion
and integer scaling are distinct models. Dyadic samples alone cannot choose
between scaling by 2^24 and 2^24−1. Constant-depth plateaus, independent X/Y
ramps and boundary values can distinguish model families without conflating
vertex-coordinate conversion and stored-depth conversion.

A valid numerical comparison needs authenticated vertex/state inputs, the
complete stored plane, explicit depth/stencil interpretation and a model
selected before observing the output. A matching colour image is not an
independent depth observation. Numerical agreement also supplies no resource
release or display-last-use contract.

## Selecting a pre-D24 observation

Passing a copy of window POSITION through an ordinary varying observes that
varying's iterator and primary-attribute path. It does not independently observe
ISP depth before stored-D24 conversion. A scalar depth diagnostic must identify
the actual depth producer and consumer rather than assume that a copied position
component provides the same observation.

Native POSITION selector 13 is a candidate for a scalar observation. Existing
STATIC joins locate its F32 input and register-mapping algorithms in the same
matched EMGD 1.8 library. They do not yet identify the final native operand for a
selected shader or establish its value as evaluated IEEE-F32 window Z.

| Recovered producer/consumer | Scope |
| --- | --- |
| F32 source converter `0x22b130` → input helpers `0x21a3d0` / `0x21a220` | Source class 9, number 0 selects coordinate 13, format 0 and four consecutive virtual component registers. The operand is `base + component`; this branch does not reverse components 0 and 2 as the separate color branch does. |
| Fixed-input producer `0x2406f0` → map builder `0x21a4a0` → SSA consumer `0x200a20` | The current input list supplies the fixed virtual-register array and per-input prefix offsets. These offsets count DWORD register slots. `0x21a7d0` can trim unused trailing components and rebuild the map; four requested components do not guarantee four final attributes. |
| Instruction walker `0x1f6410` → argument rewrite `0x1f58c0` → partition map `0x1f5800` | The primary-register partition maps to PRIMATTR type 2 and its unchanged DWORD register number. This rewrites bank/number without numerical conversion. The register-passed callback precedes the final fixed-array rewrite at `0x1fd137..0x1fd156`. |

These are analysis byte VAs under the address convention above. Coordinate 13
does not mean PA13. The remaining source joins are the GLSL builtin and
conditional `gl_PMXPosAdjust` to this exact native source, the selected backend
semantic remap for coordinate 13, and the selected POSITION shader's final USE
operand. A recovered mapping algorithm does not observe that operand or its PA
contents. Any Y or half-pixel adjustment must be accounted for before calling
the values GL window coordinates.

The [compiler's raw vec4 output](../../compiler.md#bounded-fragment-transport)
constructs four separate output DWORD moves. That CPU construction is not yet a
qualified PA → OUT0 → ARGB8888 bit-preserving sink. Known-pattern controls must
establish the actual output mapping, byte order and absence of conversion or
blending before interpreting a retained word as scalar Z. Even an F32 POSITION
observation would leave higher internal depth precision unmeasured.

Special selector 14 remains a different, three-PA-DWORD coefficient route. Its
name and register count do not establish evaluated IEEE-F32 Z; the coefficient
representation and evaluation consumer remain open. Neither route justifies a
general scalar-depth interpretation or a tolerance.

The matched host-only ZABS route at analysis `0x138104` forms special issue14
with `0x0d80e00f`; `0x138135` reserves three PA DWORDs and `0x13819a` supplies
last-USE, giving `0x0f80e00f` for this issue. Helper `0x14dd40` emits the linked
PCOEFF visibility-feedback form. These addresses are analysis byte VAs in the
same EMGD1.8 library; subtract `0x10000` for ELF VAs. Shader-object offsets are
bytes and PA extents are DWORDs. These are STATIC host allocation/instruction
facts, not an evaluated scalar-Z or coefficient-format observation. They do not
close the selected POSITION13 Z consumer. The issue-to-PA-to-raw-output path
still needs independent qualification.

## Applying the recovered contract

The validated API codes corroborate `(compare<<22)` and write-disable `0x00100000`, with the
documented descriptor predicates and later modifiers. The PSB coarse base `0xf0000000` and EMGD
one-MiB base `0xfff00000` remain separate address families; see the [address
envelope](../windows/depth-state-and-storage.md#from-linux-draw-state-to-the-zls-address-envelope).

A generic clear may source Z from depth metadata while retaining W=`1`. A fixed `+1` clear is a
separate selected profile. Preserve the original clip/window depth convention and avoid applying
normalization twice.

Ordinary execution needs the actual typed depth owner, complete scene state, intended load/store
policy, and one clear per intended scene. Accepted non-dyadic and boundary stores,
write-mask/compare variants, general near/far and perspective Z, other sizes and actual ZLS
state require their own checks. Stencil, D16 and MSAA add separate format,
sample-grid and storage requirements. Completion, observable writes, final use,
withdrawal and backing release remain independent contracts.
