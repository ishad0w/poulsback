# Depth: frontend state, viewport Z and storage boundaries

This comparison starts with the Linux EMGD 1.8 library matched to the preserved Ubuntu 11.04
captures. Windows EMGD 3398 supplies an applicable second host implementation; selected PSB
procedures answer narrower storage and clear questions. The result is a set of joined host
contracts, not a complete native depth model or a qualification of poulsback.

The Linux chain closes **DepthRange dispatch → arithmetic → viewport-vector writes** and
**DepthFunc/DepthMask → retained frontend state**. In PSB clear, Z comes from depth metadata;
the literal one in its vertices is W. The [depth-state consumer](../sgx535/depth-contract.md)
completes the selected Linux frontend-to-ISP chain. General quantization and an independently
qualified EMGD depth-plane oracle remain open.

- [DepthRange and the viewport vector](#linux-depthrange-reaches-the-actual-viewport-vector)
- [Comparison/write-mask state](#comparison-and-write-mask-share-frontend-state-not-an-isp-word)
- [ZLS address envelope](#from-linux-draw-state-to-the-zls-address-envelope) and [clear/storage limits](#clear-storage-and-precision-what-remains-bounded)

## Exact inputs and address domains

The primary ELF is `libEMGDOGL.so`, 2,791,512 bytes, SHA-256
`3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a`. Its Services UM peer is
136,548 bytes, SHA-256 `d9f741a40b293596b958b783af3581af84015dfd49c61c5f1a30240a5a459cbf`. See
[the Linux component pins](../sources.md#linux-emgd-18).

Linux addresses below are **Ghidra analysis VAs** unless explicitly marked ELF, file or object
offset. This import adds `0x10000` to ELF virtual addresses; file offsets are independently
resolved through `PT_LOAD`, not assumed to have the same conversion. Windows VAs use the PE
image base documented in [raster and viewport Z](raster-and-depth.md). Context offsets in one
family are not interchangeable with the other family's layout.

## Linux DepthRange reaches the actual viewport vector

The existing initializer `0x3a300` copies dispatch table `0x2b53a0` into context `+0x31b0`. The
public DepthRange wrapper calls context `+0x3634`. The selected table word is therefore at
analysis VA `0x2b5824`, ELF VA `0x2a5824`, **file offset `0x2a4824`**. It contains ELF target
`0x551d0` and has an `R_386_RELATIVE` relocation; the analysis target is `0x651d0`.

```text
pvroglDepthRange → context +0x3634
  → 0x651d0: reject execution inside Begin/End; otherwise call 0x65070
  → 0x65070: clamp finite near/far; derive and store Sz/Tz
  → context +0x76c/+0x770 and viewport vector *(context +0xaf40) +8/+0x18
```

For finite input values, the selected transform is:

```text
n = clamp(near, 0, 1)
f = clamp(far, 0, 1)
k = F32(context +0x1680)
Sz = (f - n) × (0.5 × k)
Tz = (n + f) × (0.5 × k)
```

The clamped near/far values are retained as doubles at context `+0x74c/+0x754`. The arithmetic
uses x87; Sz and Tz are stored as F32. The Sz vector update uses the rounded F32 temporary, and
Tz is copied from its saved F32 word. Consequently, these equations explain the source
transformation but are not a bit-exact arbitrary-float oracle. Runtime x87 control-word
conditions, NaNs, infinities, rounding boundaries and later hardware precision are not qualified
here.

Initializer `0x1106d0` writes F32 `1.0` at **Linux context `+0x1680`**. The retained MakeCurrent
procedure `0x3a300` calls it in the branch where `(context[+0x5c] & 6) == 0`. On that
initialized branch, a finite request `near=0, far=1` gives Sz/Tz `.5/.5`. This is an
authenticated selected initializer, not a claim that every context path forever retains that
value. The Windows factor is at **`+0x1674`**, not Linux `+0x1680`.

The route continues beyond dispatch `+0x3634` through the [viewport/matrix
chapter](../emgd-linux/geometry-and-sampling.md#viewport-coordinate-vector-and-matrix-update-branch)
for downstream geometry joins. These host
stores do not establish final USE position semantics, the depth-buffer format, or the equation
used at ZLS store.

## Comparison and write mask share frontend state, not an ISP word

Two additional dispatch-table entries close real API producers:

| API | Context dispatch | Analysis procedure | Source table ELF VA / file offset |
| --- | --- | --- | --- |
| DepthFunc | `+0x3588` | `0x2a870` | `0x2a5778 / 0x2a4778` |
| DepthMask | `+0x3500` | `0x2a960` | `0x2a56f0 / 0x2a46f0` |

DepthFunc validates GL enums `0x200…0x207`. It looks up eight DWORDs at analysis VA `0x27d2a0`,
ELF/file offset `0x26d2a0` in this particular mapped range:

| GL comparison | NEVER | LESS | EQUAL | LEQUAL | GREATER | NOTEQUAL | GEQUAL | ALWAYS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Linux frontend code | 8 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |

It replaces the comparison portion of byte `context+0xafe3`, preserving bit `0x10`. DepthMask
sets that bit when its argument is false and clears it when true. Both producers leave the state
untouched on forbidden Begin/End calls; DepthFunc also rejects an out-of-range enum. A changed
byte marks dirty state in `+0x4cf4`, `+0x4cf8` and `+0xaf2c`. Thus comparison and write-disable
are separate frontend state, with an explicit later-lowering obligation.

These frontend bytes do **not** establish the final ISP encoding. In particular, frontend
NEVER=8 is not a reason to replace PSB's emitted NEVER=0. The [depth-state
consumer](../sgx535/depth-contract.md) follows the actual Linux compiler and serialized object.

The selected PSB producer `0x3bfdf` separately requires a present depth attachment and enabled
depth testing. It emits `(GLfunc−0x200) << 22` for the validated eight-enum range and sets
write-disable `0x00100000` when appropriate. With no usable depth test it selects `0x01d00000`
instead. This is a different state layer.

## From Linux draw state to the ZLS address envelope

The already retained OGL producer `0x155940` reads a state object through `param_3[0x14]`. Its
words 6/7 become packet byte offsets `+0x3c/+0x40`; constant one becomes `+0x44`; word 5 becomes
`+0x48`. The wrapper `0x154090` passes this same packet to the matching `SGXKickTA`.

The UM consumer derives a **1-MiB-aligned byte base**, then subtracts it from the two address
inputs before applying mask `0x0ffffff0`. In its full-state branch the retained state carries:

```text
base                     = selected_address & 0xfff00000
first load/store pair    = (input[+0x3c] - base) & 0x0ffffff0
second load/store pair   = (input[+0x40] - base) & 0x0ffffff0
second pair extra bit31  = input[+0x44] != 0
```

The `input[+0x48] & 0x30000 == 0x30000` branch selects another stored allocation address as the
base. Otherwise the full-state branch selects the lower nonzero of the two address inputs before
alignment. The update branch must be retained separately: its zero/nonzero selection conditions
differ, and its caller's invariants have not been proved here. This chapter does not silently
equate both branches or give bit 31 an unverified hardware meaning.

The update-list consumer addresses render-state offsets `+4`, `+0x2c/+0x30` and `+0x34/+0x38`.
These are memory-structure offsets, **not** MMIO `0x484…0x490`. The matched firmware's
[LD/WDF/STR block](../sgx535/depth-contract.md#actual-firmware-words-including-the-retained-live-copy)
contains the actual ZLS register stores. Its incoming pointer, active MOE and entry path
remain unjoined, so the complete memory-structure-to-register mapping is still open. The
matched header names BIF ZLS request base at MMIO byte offset `0xcb0`, with mask `0xfff00000`;
a name/mask agreement alone does not resolve those inputs.

PSB's selected DRI instead emits coarse mask `0xf0000000` and four low masks `0x0ffffff0`
against the same BO. Both schemes can represent the same aligned address, but their base/offset
conventions cannot be mixed. For `0x50abcde0`, PSB's `0x50000000 + 0x00abcde0` and EMGD-style
`0x50a00000 + 0x000bcde0` agree; a mixed pair produces `0x514bcde0`. This arithmetic is not
itself an observation of on-chip address addition.

## Clear, storage and precision: what remains bounded

Windows EMGD 3398 already closes host viewport `.5/.5` and six-word MTE serialization. It does
not yet close the selected comparison/write-mask, clear/store or native quantization chain; see
[the exact Windows procedures](raster-and-depth.md#viewport-and-depth). Linux and Windows may
share SDK ancestry. Their agreement is useful corroboration, not two independent silicon
experiments.

The selected PSB allocation and ZLS branches are described in [typed
depth](../psb/pds-use-and-depth.md#depth-storage-and-attachment). A four-byte `cpp`
and a D24S8 API label do not prove native per-pixel byte order, tiling, compression, unused-bit
policy, rounding or every write extent. The source allocation extent `cpp × align(width,32) ×
align(height,32)` also does not prove actual GPU containment.

**Clear Z and W:** PSB `0x3a794` loads its vertex Z from `scene->depth +0x14` and repeats it for
all four vertices. The literal `0x3f800000` is each vertex's W. Consequently, the generic source
is `(0,0,Z,1), (width,0,Z,1), (0,height,Z,1), (width,height,Z,1)`; `Z=+1` belongs only to a
separately established clear-metadata/fixture value. The procedure builds a clear prefix,
propagates allocation failures, and marks scene state; it does not by itself submit, complete or
retire a job. A depth-only fixed profile which intentionally uses `+1` is not erroneous merely
because this source can supply other values.

## Runtime correspondence and next useful joins

The selected [Ubuntu 11.04 TA capture](../emgd-linux/submission-packet.md) authenticates the same
OGL/UM bytes and an actual legacy draw. The selected capture has 12 observer records; six
contain complete `0x230`-byte TA entry packets. It observes packets before kernel sync/status
patching, not an atomic snapshot of all pointed-to render state. The RT pointer and the
separately requested allocation also have a documented span discrepancy. These captures
therefore do not prove this chapter's depth layout, coefficient rounding or ZLS final-use
behavior. Its 64×64 RGB image comparisons are not depth-plane oracles.

| Link | Current scope | Next bounded work |
| --- | --- | --- |
| DepthRange API → saved Z transform/vector | STATIC, closed for the selected finite-input branch | Use the authenticated fields in viewport/position consumer analysis; preserve F32 rounding boundaries. |
| Initialized scale factor | STATIC, selected MakeCurrent branch writes 1.0 | Do not assume identical offsets or values in another kit/context path. |
| DepthFunc/Mask → emitted ISP word | STATIC, closed for selected Linux branches in the [depth follow-through](../sgx535/depth-contract.md) | Preserve enable/attachment predicates, the special zero-code branch, cache hits and later state modifiers. Native depth quantization and general execution remain separate. |
| Viewport/MTE → final depth interpolation | Partially closed host serialization | Connect the actual position output and primitive-depth consumer before choosing numerical tolerances. |
| Addresses → UM state fields | STATIC, closed selected assignments | Resolve the firmware/state field consumer, preserving full-state versus update branch distinctions. |
| Clear/load/store and preservation | PSB source-derived alternative, EMGD consumer incomplete | Follow clear metadata and native ZLS controls without transplanting PSB base masks. |
| D24 quantization, layout and write extent | UNKNOWN for this EMGD chain | Use a matched complete depth capture and an independently chosen model; RGB output or API names are insufficient. |
| Visibility/retirement/reuse | Independent contract | Reuse the already recovered normal-resource contract where applicable; no depth value or correct image grants release. |

These contracts do not qualify a new implementation or replace a source-bound hardware
experiment.
