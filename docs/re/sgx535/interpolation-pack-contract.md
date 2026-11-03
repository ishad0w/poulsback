# Interpolation and PACK: the iterator contract and the numerical boundary

The Linux EMGD compiler path can be followed from its input descriptors through the
generated fragment PDS program. The corresponding Windows EMGD builder confirms the same
generic-coordinate versus primary-color distinction. This closes a host producer/consumer link.
It does **not** disclose the fixed-function interpolator's coefficient representation or
rounding.

The [format and numerical guide](../emgd-linux/interpolation-and-packing.md)
separates requested input precision, stored representation, interpolation and
PACK conversion. The coefficient format and general conversion rule remain
unresolved.

- [Iterator metadata to PDS/DOUTI](#linux-compiled-inputs-reach-the-fragment-pds-program)
- [Numerical responsibilities](#five-different-numerical-responsibilities)
  and [a prospective discriminator](#one-small-discriminator-with-genuinely-different-predictions)
- [Native PACK fields](#native-pack-producer-the-instruction-fields-are-now-joined)
- [Available PARAM extent](#what-param-data-is-actually-available-for-this-question)
  and [the PA/PACK boundary](#next-observation-separate-pa-from-pack)

## Source and scope

| Source | Exact input | Scope |
| --- | --- | --- |
| Linux EMGD 1.8, package `1.8.0-2032` | `libEMGDOGL.so`, 2,791,512 bytes, SHA256 `3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a` | Primary static implementation; this library also appears in the retained Ubuntu 11.04 stack's mappings |
| Windows 7 EMGD `8.14.1180.3398` | `igdsgl32.dll`, 699,904 bytes, SHA256 `7a0d9f396a0386f102d454684a4f58aef947bc2621ca47f64d7bab1ffa782e0c` | Matching-family static corroboration; no Windows runtime execution observed here |
| Selected SGX535 revision-121 definitions | SGX535 revision-121 macro definitions, SHA256 `114f5993acacd570de95d13015208f3268204b285ef214713f8a1f97af05a45d` | Field names and positions; not a numerical specification |

Linux addresses below are Ghidra analysis byte VAs. Subtract `0x10000` for ELF VAs, then resolve
through `PT_LOAD`. Windows addresses are PE VAs with image base `0x10000000`; subtract it for
RVA, then use the section mapping. Object offsets and PDS-resource offsets are bytes. They are
not MMIO offsets or GPU instruction addresses.

## Linux: compiled inputs reach the fragment PDS program

`0x136e80` builds a fragment shader object from compiled input metadata. The selected fragment
path is `*program_descriptor == 2`.

Each input descriptor is eight DWORDs. Its kind, binding, component count and format fields
control a separate iterator word. The semantic remap selects TC0–TC9 or primary/secondary color.
Numeric descriptor values below are the observed branch values, not a claim that every compiler
enum is recovered.

| Producer or consumer | Exact operation |
| --- | --- |
| `0x135550` dirty-state dispatcher | Calls `0x136e80` with the selected fragment descriptor at `*(ctx+0xaa64)+0x217c`; later calls `0x13c2a0` when the fragment PDS dirty bits require rebuilding |
| `0x137878`–`0x1378a2` | Reads kind at descriptor `+0`, binding at `+8`, and its semantic remap; kind `0x11` takes the non-texture iteration branch |
| `0x137948`–`0x137984` | Loads selector table `0x289660`, ORs `0x0100000f`, and, for generic coordinates, inserts `(components-1)<<22` |
| `0x137993`–`0x1379b8`, `0x137b0d` | Format selectors 1 and 2 add `0x20000000` and `0x10000000`; selector 0 adds neither |
| `0x1379f1`–`0x137a3b` | Separate semantic-10/11 color branch: selected nonpacked format adds `USECOLFLOAT`, bit 20; this is not the generic-TC0 branch |
| `0x13790b`–`0x137922`, `0x137848`–`0x13785d` | Optional centroid bits and interpolation selection; generic coordinates receive Gouraud bits `0x0c000000` |
| `0x138166`, `0x13819a`, `0x1381e1`–`0x1381ed` | Publishes issue count at shader `+0x474`, last USE issue bit 25, texture/PA counts and task dependency flags |
| `0x13703e` | Publishes the constructed shader object into program descriptor `+0x8c`; `0x13c2a0` reads this field from the same selected fragment descriptor |
| `0x13c2a0`, specifically `0x13c531`–`0x13c546` | Copies shader `+0x370` iterator words and `+0x3f0` texture sentinels into a temporary descriptor at `+0x18` and `+0x218`; count is at `+0x14` |
| `0x13c2a0` → `0x14f250` | Allocates PDS storage and passes that descriptor to the real generator |
| `0x14f250` | Stores the iterator word in PDS data and emits its DOUTI instruction; also emits USE task controls and terminal HALT |

`0x136920` aligns and copies generated **USE code**. It is a different consumer from the
iterator-data path above; a code-copy routine alone does not close the iterator-data connection.

### A reproducible single-issue construction

For one non-texture TC0 input, four components, format selector 0, no centroid, and no appended
fog/Z issue, the Linux branch produces:

```text
TC0 selector             0x00000000
perspective / no texture 0x0100000f
four components         0x00c00000
Gouraud                 0x0c000000
last USE issue          0x02000000
                        ----------
iterator word           0x0fc0000f
```

Table `0x289660` contains `[0<<12, 1<<12, …, 13<<12]`; the flat selector at `0x289640` is
`[0x0c000000, 0, 0x04000000, 0x08000000]`. Both were read from original file bytes, not inferred
from macro names.

The small helpers at analysis `0x14ec60`, `0x14ecd0` and `0x14ed10` implement bank allocation,
allocated extent and MOVS encoding. With this one-issue descriptor, allocation indices are `0,
1, 8, 9`: three USE-control words and the iterator word at PDS **byte 36**. Data occupies 48
aligned bytes. The following three words are:

```text
0x07000365  MOVS to DOUTU, starting at the task-control tuple
0x070418a2  MOVS to DOUTI, selecting the iterator data
0xaf000000 HALT
```

The complete selected construction is 60 bytes. This is a reconstruction of the stated branch
and its stores, not a captured live EMGD packet or a new GPU qualification. The source generator
can combine compatible texture issues; that branch is outside this one-non-texture example.

## Windows corroboration and the issue-bit distinction

Windows EMGD `0x10008031` builds the corresponding issue array at object `+0x330`, texture
sentinel array at `+0x3b0` and count at `+0x434`. Its selector tables are `0x100a5318` and
`0x100a5308`. The stated single-TC0 branch gives the same `0x0fc0000f`; semantic 10/11 has a
separate `USECOLFLOAT` branch. Windows differs in some packed-color component-count handling, so
the functions are not interchangeable implementations for all formats.

This is useful agreement between applicable EMGD artifacts. Their shared compiler/SDK lineage is
not independent evidence for internal hardware arithmetic. The Windows builder-to-PDS-upload
consumer is not established here.

The EMGD generic TC0 construction is `0x0fc0000f`. A separate PSB-derived recipe uses
`0x0fd0000f`; the difference is bit 20, `USECOLFLOAT`. The EMGD compiler sets that bit for the
selected primary-color branch and leaves it clear for this generic-TC0 case.

A canonical codec may accept one word and reject the other. That choice does not establish that
hardware rejects the alternate word: `USECOLFLOAT` may have no effect on generic coordinates,
and the selected host source does not prove its applicability there. Preserve the two recipes
and their source families; the difference alone does not justify toggling an admitted packet's
bit.

## Five different numerical responsibilities

| Layer | Established here | Still unresolved |
| --- | --- | --- |
| Vertex input representation | Generic OUTSELECTS/TEXSIZE/TEXFLOAT construction is source-known. | Actual PARAM representation and complete source-vertex association for each format. |
| Iterator issue and PA allocation | Compiler metadata → source-selected issue → PDS data/DOUTI; primary color and generic coordinates differ | Whether a specific bit is ignored for another issue class; actual per-branch PA precision |
| Fixed-function interpolation | Selected issue requests perspective/Gouraud and dimensionality | Coefficient storage, normalization, precision, arithmetic order, intermediate rounding |
| Generated fragment USE | Source-backed PA, clamp and PACK instruction fields. | Actual intermediate PA values and complete program semantics. |
| Output conversion | Source-backed PCKUNPCK format and SCALE controls. | General arithmetic, rounding and interaction with interpolation. |

The fixed 24-word **state-upload USE** program is not the fragment shader and does not calculate
the interpolator's coefficients. A compiler's generic F32 input description is also not evidence
that every downstream stage uses IEEE binary32 arithmetic.

The output-mask helper `0x14df30` builds one instruction pair according to an output mask and
bank tables. Its whole-mask branch emits a different pair from its partial-mask branch. This is
an instruction-word producer, **not a recovered scalar scale-by-255/rounding algorithm**. No new
PACK rounding claim follows from it. The full compiled fragment lowering path remains a separate
investigation.

## One small discriminator with genuinely different predictions

If further static evidence does not expose the interpolator's arithmetic, a useful
**prospective** test is one 3×4 right triangle, rather than another literal PACK test. This
example describes mathematical predictions, not a device result.

Use a 64×64 target and vertices `(8,8)`, `(11,8)`, `(8,12)`, all with W=1. Let red be `[0,1,0]`,
green/blue zero and alpha one. Authenticate stored endpoint codes `[0,255,0]`
in the complete PARAM primitive; these introduce no U8 endpoint quantization
discrepancy. A generic-F32 layout is a separate representation question.

At pixel centers `(8.5,8.5)` and `(10.5,8.5)`, the red weights are exactly `1/6` and `5/6`. Both
samples are strictly interior; the sloping edge has no pixel-center ties in this geometry. The
following models give three distinct output pairs:

| Named hypothesis | Red at pixel `(8,8)` | Red at pixel `(10,8)` |
| --- | ---: | ---: |
| Exact affine input; exact ×255; nearest-even integer output | 42 | 212 |
| Exact affine input; exact ×255; nearest-ties-up integer output | 43 | 213 |
| Affine result first rounded to binary32; exact ×255; nearest output | 43 | 212 |
| `dx * binary32(1/3)`, product rounded to binary32; exact ×255 | 43 | 213 |

The third row has PA candidates `0x3e2aaaab` and `0x3f555555`; the fourth has `0x3e2aaaab` and
`0x3f555556`. The last two rows illustrate why operation order matters. Rows two and four agree:
this small test does not identify all models uniquely. An unexpected pair stays unexplained, not
tolerated.

These predictions use exact fractions and explicit binary32 roundings, independently of a new
captured image. An executing implementation must bind actual geometry/program/state, the full
PARAM primitive and immutable spans, and check the entire target and guards. Two discriminator
samples do not replace whole-image correctness.

This test separates interpolation-plus-conversion models. It still does not prove a coefficient
format or independently identify PACK if the PA value is unknown. A bit-preserving PA readout
would address that boundary more directly, but no qualified raw-F32 output/capture mechanism was
established here; adding one is a separate contract, not an assumed capability.

## Native PACK producer: the instruction fields are now joined

The following Linux and Windows chains establish instruction construction, not the scalar
arithmetic performed by the device.

### Linux assembler and the actual fragment helper

Linux analysis `0x274740` is the native PCK/UNPCK instruction encoder, reached from the larger
assembler at `0x277850`. The selected function consumes an instruction descriptor and writes two
USE words. The descriptor's opcode is **not** the hardware opcode:

| Input or store | Verified transformation |
| --- | --- |
| Descriptor `+0`, internal opcode `0x20` | Selects `0x0c00`: source format 6, destination format 0 |
| Descriptor `+8`, bit `0x100`; stores `0x274831`–`0x274845` | Sets USE **word 0** bit 18, `PCK_SCALE` |
| Store `0x274814` | Adds hardware opcode `8 << 27`, `PCKUNPCK`, to **word 1** |
| Descriptor `+0x1c`, mask bits `0x780` | Extracts four-bit destination mask and places it at word 1 bits 2–5 |
| Remaining register/control helpers | Encode banks, source/destination registers, component selectors and scheduling controls; they do not evaluate the fragment's input values |

The revision-121 definitions independently name format **6 as F32**, format **0 as U8**, opcode
**8 as PCKUNPCK**, and SCALE at word 0 bit 18. These are three separate fields. In particular,
`0x0c00` is a format pair, not an opcode, and SCALE is not the END bit at word 1 bit 18.

The smaller fragment helper at analysis `0x14e000` provides an actual caller-side construction,
rather than just available ISA encodings. Let `r` be its descriptor `+0x40`, and let `t` be zero
when `descriptor[+0x98] == 1`, otherwise one. Its first instruction is:

```text
word 0 = 0xa0040000 | r | (r << 7) | (t << 21)
word 1 = 0x40a00c3c
```

The source banks are primary attributes, the destination is temporary `t`, the formats are F32 →
U8, SCALE is set, the destination byte mask is `0xf`, and END is clear. This statement decodes
the instruction controls; it does not claim a bit-exact formula for how four components are
rounded. The helper then emits another instruction using additional descriptor fields. That
second instruction is outside the recovered numerical contract; do not replace it with an
assumed color store.

The already retained `0x136e80` fragment-object builder selects this helper when its
postprocessing descriptor `+0x48` is nonzero. The call is at `0x138336`. The resulting local
instruction pairs enter the generated code at `0x1389f7`, through analysis `0x13e1a0`, a
byte-copy wrapper around imported `memcpy`. At `0x138a02`, the destination advances by the
returned count **times eight**. Thus the Linux helper returns **two instructions**, not two
DWORDs. This is a conditional postprocessing branch, not proof that every EMGD fragment program
uses this epilogue.

### Windows agreement and count units

Windows EMGD3398 VA `0x10017f0f` is the matching helper; its caller is `0x1000845f`. Its literal
word 1 occurs at PE VA `0x10017f70`, file offset `0x17370`. The complete function is PE VA
`0x10017f0f..0x10017fc2`, file `0x1730f..0x173c2`.

Windows spells word 0 as:

```text
((((t << 14) | r | 0xff400800) << 7) | r) mod 2^32
```

For all 128 source-register values and both `t` values, this equals the Linux word above. The
first instruction's source banks, formats, SCALE and mask also match. Windows writes four DWORDs
and returns **4**; this is consistent with a DWORD count, but this selected analysis has not
followed its downstream size consumer. Linux returns **2**, and its caller explicitly consumes
that count in eight-byte instruction units. Do not substitute one return convention for the
other without checking the caller. Their related implementation lineage is corroboration, not an
independent measurement of SGX arithmetic.

The source-selected first pair has temporary destination `t`, both source
operands primary `r`, mask 15, SCALE 1 and END 0. Matching these fields in a
different program does not establish whole-program or numerical equivalence.

### The numerical boundary remains inside device execution

The inspected assembler and helper manipulate instruction words. They neither calculate
per-fragment color values nor contain a scalar scale-by-255/rounding implementation. The
selected controls contain no recovered rounding-mode selector. This is a bounded statement about
these bodies and definitions, not a claim that all compiler paths, firmware or hardware modes
have been exhaustively examined.

The critical intermediate is the primary-attribute value delivered by the
iterator before clamp/PACK. Generated PDS words and the final packed image do
not independently observe those bits. No matched firmware routine has been
joined to a scalar implementation of the fixed-function interpolation or PACK
conversion.

## What PARAM data is actually available for this question

A TA envelope names allocations; it does not contain their dereferenced PARAM
payloads. A generic-coordinate interpretation requires the actual vertex stream
and output layout, MTE/TEXSIZE/TEXFLOAT state, iterator and fragment code, SCENE
pointer and complete PARAM primitive, including every referenced vertex span.
Keep resource bases, valid capture intervals and allocation generations in the
join. A host F32 request is not a substitute for stored bytes, and packed colour
fields do not by themselves expose interpolation slopes or coefficients.

## Next observation: separate PA from PACK

The PCK opcode, F32/U8 formats and SCALE bit have a source-backed construction above.
The unresolved arithmetic needs an intermediate observation or an end-to-end discriminator:

1. A bit-preserving **PA → retained memory** diagnostic would separate iterator
   output from PACK. A candidate is a PA MOV into an output word with the known
   packed-word PBE route, without clamp or PACK. This is only a candidate:
   prove its register/output mapping and absence of PBE conversion, blending
   or byte rearrangement using known bit patterns before treating its readback
   as PA. Its transport and output semantics require independent validation.
2. Once that route is proven, use the two strict-interior samples in the
   [3×4-triangle discriminator](#one-small-discriminator-with-genuinely-different-predictions).
   Record the PA bits **and** ordinary packed output for identical immutable
   input/state. This separates a changed iterator value from a changed PACK
   result; examining packed pixels alone cannot do so.
3. If an intermediate route remains unavailable, the already specified
   end-to-end output pairs still distinguish some models. Preserve the
   deliberately stated equivalence between the remaining models. Do not call
   that test a general rounding proof or introduce a tolerance after seeing it.

The coefficient and conversion models remain unresolved. A numerical model
does not grant visibility or retirement.
