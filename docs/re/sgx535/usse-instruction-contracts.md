# USSE instruction fields, entry state and decoder limits

This chapter separates selected instruction construction, compiler state models,
CPU decoding and device execution. It does not provide a complete SGX535 ISA or
qualify a general fragment backend. [Source profiles](../sources.md) identify the
matched Linux EMGD and PSB inputs; [address domains](../address-domains.md) owns
VA conversions. PDS data and USE CODE are different instruction streams.

- [Matched logical SHR construction](#matched-constant-logical-right-shift)
- [Compiler defaults and incoming task state](#initial-state-is-not-a-pds-allocation)
- [Revision121 CPU decoder](#sgx535-revision121-cpu-decoder)
- [Remaining numerical and execution boundaries](#remaining-numerical-and-execution-boundaries)

## Matched constant logical right shift

The subject is EMGD 1.8 Build 2032/PVR 1.5.15.3226 `libEMGDOGL.so`, SHA-256
`3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a`.
Assembler dispatcher analysis `0x277850` calls constructor `0x275270` at
`0x27923a`. Descriptor opcode `0x53` selects logical SHR; `0x54` selects ASR.
The source-2 type at descriptor **byte** `+0x40` is 8 for an immediate count,
whose value is at `+0x3c`. These analysis VAs have the `+0x10000` import bias;
the constructor's ELF VA is `0x265270`.

For the selected full-scalar, direct TEMP0..123, repeat-one/SKIPINV construction,
counts 1,2,4,8,16,31 have this two-DWORD representation:

```text
word0 = 0x20000000 | (destination_TEMP << 21) | (source_TEMP << 7) | count
word1 = 0x68a10000
```

One USE instruction is eight bytes, word 0 first in little-endian order.
This subset has no predicate, indexing, source modifier, rotation, inversion,
partial write, NoSched or END. Repetition/masks and predicates are
instruction-class-specific fields; another MOV form is not an interchangeable
encoding oracle. The complete constructor supports the selected field join;
the dispatcher's truncated high-pcode limits broader value-flow conclusions.
A GL frontend producer and actual shift values remain separate evidence.

## Initial state is not a PDS allocation

The same matched compiler pass `0x208960` installs callbacks for register bases
(`0x202950`), operand increments (`0x202a90 → 0x202a20`) and format controls
(`0x2029d0`). The initial model is four zero bases, increment-one operands,
EFO format control off and colour format control on. Main/secondary initialization
`0x202bc0` and CFG consumer `0x202e70` copy this state into the entry block.
Initializing these CPU structures emits no SMBO, SMLSI or SETFC instruction.

The format scanner `0x206b40`, block consumer `0x206970` and classifier
`0x2620e0` treat selected EFO, C10 and TEST forms specially. The matched ILIMM
and ISHR descriptors lack the C10-control bit and bypass this handling.
An absent SETFC before a literal-to-SHR interval therefore does not establish a
bug, and colour-on does not make these two operations consume C10 values.
This source model does not observe physical register selection or raw literal
delivery at a new task's entry. Existing stricter codec preconditions remain
unchanged. TEMP/DMS counts, code-bank entries and PRIMARY/SECONDARY allocation
also do not establish those incoming execution controls.

CPU instruction bytes do not observe physical TEMP values or task-entry MOE.
A missing local prelude alone neither diagnoses bad entry state nor supports a
guessed initializer. A raw witness must preserve the input bits and validate
its output mapping before attributing a changed value to a shift operation.

## SGX535 revision121 CPU decoder

A separately retained SDK decoder uses compiler ELF SHA-256
`9349ed4d7643d81cb638b41eb41b69507cc2f9f15bae14c8f2116e79f70e7bd9`.
Its selected profile is SGX535 revision121. This is not the SGX540 revision101
profile from a different disassembler setup. The compiler's original decoder
VA is `0x0829921b`, file offset `0x25121b`; these fixed original addresses do
not use the EMGD analysis bias.

The decoder can corroborate opcode, register/bank, predicate and END fields.
Recognized instruction text is not independent arithmetic validation, a
reproduction of the original compiler build or GPU execution. Its selected
revision must match the target; SGX540 revision101 is a different profile.

The decoder returns whether END flag `0x00020000` is set after a successful
decode. Zero is normal for valid non-END instructions, while decode errors can
also return zero. It cannot serve as an instruction-validity gate. The wrapper
initializes colour-on/EFO-off for each call; it is not a sequential MOE
interpreter and cannot observe the incoming state of the actual task.

The decoder binary, wrapper and raw reports are not included in this clone.
Public CPU checks exercise the project code and models independently; citing
the decoder does not grant rights to redistribute it.
The SDK version attribution is retained package metadata, not a verified
publisher signature. [Evidence availability](../evidence.md#what-remains-unavailable)
keeps these inputs separate from the published reference assets.

## Remaining numerical and execution boundaries

Known encodings, matching decoded fields and correct CPU assembly are useful
checks. They do not establish PA delivery, physical LIMM values, general MOE
state, floating-point precision, floor/FRC semantics, sampler visibility or
GPU completion. A comparison source's floor-by-FRC/subtraction construction is
not a matched EMGD floor producer or a proof of IEEE edge cases. Keep signed
zero, nonfinite values and arithmetic order explicit.

The [interpolation/PACK](interpolation-pack-contract.md) and
[depth](depth-contract.md) chapters identify unresolved numerical and
observation contracts. [Texture delivery](../emgd-linux/geometry-and-sampling.md#nondependent-arrival-and-task-fields)
and [SECONDARY constants](../psb/pds-use-and-depth.md#ordinary-fragment-secondary-constants)
are distinct input contracts. Current compilation and admission belong to the
[compiler guide](../../compiler.md), with GPU results in [Hardware](../../hardware.md).
