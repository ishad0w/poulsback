# Interpolation and packing: separate the stored input from the arithmetic

The matched Linux EMGD library establishes how generic-varying formats are
selected and uploaded. A format request, stored PARAM representation,
interpolation arithmetic and final PACK conversion are separate contracts.
The fixed-function coefficient format and general PACK rounding rule remain
unresolved.

There is also a useful mathematical limit on further testing: **no finite binary32 literal in
[0,1] distinguishes exact scale-by-255 followed by nearest-even from exact scale-by-255 followed
by nearest-ties-up.** The proof below applies to those named mathematical models. It says
nothing about an unknown intermediate GPU precision or a separately rounded multiplication.

- [Format and upload producers](#what-the-emgd-producer-actually-controls)
- [Literal tie-model equivalence](#why-another-exact-literal-halfway-test-cannot-choose-the-tie-rule)
- [Remaining numerical boundary](#what-can-be-implemented-and-what-is-still-missing)

## Sources and address domains

The main source is Linux EMGD 1.8 / package `1.8.0-2032`, the library selected for the retained
Ubuntu 11.04 stack:

| Input | Identity and scope |
| --- | --- |
| `libEMGDOGL.so` | 2,791,512 bytes; SHA256 `3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a` |

The component hash binds these static claims to the selected binary. It does
not prove execution of every branch or identify a particular GPU allocation.

EMGD function addresses below are Ghidra analysis byte VAs. For this import, `ELF VA = analysis
VA - 0x10000`; the ELF program headers then resolve each range to file bytes. Context offsets
such as `+0x2170` are CPU-object byte offsets. PARAM offsets describe GPU-resource
bytes. None of these are MMIO addresses.

## What the EMGD producer actually controls

The full state chain is described in [geometry and
sampling](geometry-and-sampling.md#state-object-four-counts-that-must-not-be-collapsed). For
interpolation, the important distinction is between a **format request** and the **hardware's
internal representation**:

| Producer or consumer | Checked operation | What it establishes |
| --- | --- | --- |
| `0x140310` | Uses tables at `0x289700` and `0x289718`; checked values are `[0,0,1,5,3,7]` and `[0,1,2,3,3,4]` | Output word count and coordinate dimensionality |
| `0x14044d`, `0x14045c` | Stores OUTSELECTS and TEXSIZE at object `+0x216c`, `+0x2174` | For flags `0x40` and coordinate-0 dimension enum 5: eight output DWORDs, `0x08001000`, `7` |
| `0x14046b` | Unconditionally stores zero at object `+0x2170` | This branch explicitly produces TEXFLOAT zero |
| `0x1351f8` through `0x13520f`, within `0x134ea0` | Sets header bits `0xc000`; writes `+0x2174`, then `+0x2170` | Both TEXSIZE and TEXFLOAT words reach the emitted state record |
| `0x13cea0` → `0x13afc0` / `0x14f570` | Builds state upload, DMA and PDS; uses the program object at `+0x38` | An actual upload consumer, not an unconnected setter |
| `0x13cfe9` → `0x134db0` | Publishes 96 primary bytes; encodes `(bytes+15)>>4` | Primary allocation is six 16-byte units, independently of PDS data size |

The original instruction at `0x14046b` maps to ELF/file `0x13046b`. The serializer sequence at
`0x1351f8` maps to ELF/file `0x1251f8`. Original-byte disassembly confirms those stores and
their order; their nearest exported-symbol labels do not rename the recovered functions.
The complete primary-allocation consumer `0x134db0` occupies 234 original bytes at
ELF/file interval `[0x124db0,0x124e9a)`. This extent identifies that consumer independently
of the adjacent serializer and PDS generators.

The matching SGX535 definitions distinguish TEXFLOAT's coordinate-precision selection from PDS
`USECOLFLOAT`. Setting the latter is not proof that a primary-color value avoided U8 storage
earlier in the pipeline. The retained PSB producer/consumer analysis corroborates the
generic-coordinate versus primary-color linkage; it does not supply the missing EMGD coefficient
model. See [PDS/USE linkage](../psb/pds-use-and-depth.md#use-entry-bank-and-relative-byte-offset).

These selected host routines request formats, allocate attributes and upload state. They do not
provide an authenticated host-side affine coefficient calculation or a numerical instruction
sequence for the fixed-function interpolator. A request for generic F32 is therefore not proof
of F32 storage, F32 coefficient precision or a particular sequence of multiply/add operations.

## Why another exact literal halfway test cannot choose the tie rule

Let a finite binary32 literal be `x` in `[0,1]`. The two exact models can differ only when:

```text
255*x = k + 1/2
x = (2*k+1)/510,       0 <= k <= 254
```

A finite binary value is a rational number whose reduced denominator is a power of two. Since
`510 = 2*255`, the odd factor 255 must divide `2*k+1`. Within this range, the only possibility
is `2*k+1=255`: `x=1/2`, `k=127`. Both models return 128, because 127 is odd. At every other
finite binary32 input the exact product is not halfway, so the tie convention is irrelevant.
This is an exhaustive argument over all 255 rational halfway thresholds, not a sample of
floating-point literals or a hardware rounding observation.

This does **not** exclude discriminators involving interpolation or a different arithmetic
model. For example, input bits `0x3b008081` give the exact product `2147483775/4294967296`,
slightly above 1/2. Both exact models return 1. If a separate binary32 round-to-nearest-even
multiplication first reduces that product to exactly 1/2, the subsequent integer conversions
give 0 (nearest-even) or 1 (ties-up). That conditional counterexample demonstrates why operation
order must be stated; it is not evidence that SGX performs the intermediate rounding and is not
a proposed device test.

An interpolation discriminator must vary the intermediate representation or
operation order; repeating literals for mathematically equivalent models adds
no information. A dyadic gradient can check transport while still giving the
same result under several coefficient orders.

## What can be implemented, and what is still missing

The [iterator contract](../sgx535/interpolation-pack-contract.md#linux-compiled-inputs-reach-the-fragment-pds-program)
joins generic-F32 descriptors to fragment PDS/DOUTI. The
[captured fragment contract](fragment-path.md) supplies texture/iteration and
SECONDARY layouts. Neither reveals fixed-function coefficient arithmetic.

| Contract | Established | Remaining boundary |
| --- | --- | --- |
| Format construction | Matched OUTSELECTS/TEXSIZE/TEXFLOAT, serializer, upload and allocation consumer. | Keep state, iterator and PA linkage coupled. |
| PARAM interpretation | Source-selected formats and byte/word units. | Authenticate the complete primitive, vertex association and stored components for each format. |
| Interpolation | Perspective/Gouraud requests and dimensionality. | Coefficient storage, normalization, arithmetic order and precision. |
| PACK | Source-backed instruction construction and the model-equivalence proof above. | Device conversion arithmetic and interaction with interpolated input. |
| Lifetime | Separate resource, command and completion owners. | Visibility, final use and release require their own contracts. |

Follow emitted generic state into its parameter consumer. A host upload routine
cannot reveal arithmetic performed wholly in fixed-function hardware. A
numerical discriminator needs different predictions for the remaining models,
authenticated stored inputs and complete output coverage. A bit-preserving PA
observation can separate iterator output from PACK only after its own output
mapping and absence of conversion are independently established.
