# SGX firmware carriers and host services

The Linux EMGD 1.8 initializer contains SGX program bytes and named entry
offsets. The host-side chain from those offsets to Services commands is
recovered; instruction behavior, runtime patches and memory lifetime need
their own checks. [Sources](../sources.md) identifies this carrier and the
separate Windows retail family. SGX program carriers are distinct from the
MSVDX MTX firmware used for video.

## Carrier identity

The selected `libemgdsrv_init.so` is 120740 bytes, SHA-256
`3f18f0715e8d2174d9744a577912a7a5a6afba41e1e3f9ec90a859071ecf3121`.
Its unpatched `pbuKernelProgram` is 22552 bytes at ELF VA `0x4480`, file
interval `[0x3480,0x8c98)`, SHA-256
`73d2b865c872534789901c27fc035423eb4624512312050c3f1ef5c7a3635e1f`.
The file interval is resolved through the ELF file-backed segments. Subtracting
an analysis base from an arbitrary address is not a general ELF mapping rule.

A decompiler's high-level view is not a complete control-flow proof. Check the
selected assembly, GOT bindings and original bytes when resolving an indirect
edge. The source locators below describe those selected contracts.

## Host offset to service word

The host conversion is `selector = program_offset_bytes / 8`, without adding
a heap base. The complete
[host-selector table](../emgd-linux/bootstrap-and-memory.md#service-selectors-have-their-own-units)
owns the seven offsets, exact structure bases and unchanged bridge copy through
the kernel service table. The selected table maps TA to `0x254`, 2D to
`0x21f` and POWER to `0x207`; the [publication contract](../emgd-linux/submission-packet.md)
explains their use. POWER requests power-off rather than completion or cleanup. Dynamic ELF interposition can change default
bindings, so the carrier/GOT chain alone does not identify live resolver values.

This host conversion does not decode the reached microkernel instructions,
establish their fetch extent, or make every PDS/USE address an eight-byte-unit
field.

## Code-bank setup is a separate field

The same initializer's
[code-bank producer](../emgd-linux/bootstrap-and-memory.md#code-banks-use-a-different-address-field)
uses 128-byte base units and a phase-specific data-master field. It is a
different contract from the service offset. Script construction does not prove
execution, bank exclusion, cache handoff or safe replacement. The selected
[PSB PDS/USE entry projection](../psb/pds-use-and-depth.md) has another address
representation and entry alignment. Keep each producer with its own consumer
and phase.

## Runtime patching

The initializer arrays are templates. Runtime patch producers can change code
or data words before use, so an unpatched carrier is not the complete executing
task. Match each live word to its allocation generation, patch producer and
readable extent before comparing it with the template.

An opcode pattern alone does not establish the executed MOE state, effective
address, branch path or final write ordering. SGX firmware bytes require a USE
decoder; they are not x86 instructions.

## Selected immediate-zero store

The same initializer contains `pbuKernel3DProgram` (26776 bytes, SHA-256
`ab3b07f88436966ff213bf038d69d2f7f382c4d0e75225cf4a21c5c806fab8fd`)
and `pbuKernelSPMProgram` (18328 bytes, SHA-256
`7c6bde17c9b6f4ed76c19da4df2c815e09bd4edf3d157fbf9bef2d755e8fa81e`).
At byte offsets `0x27e0` and `0x0de0` within those symbols, respectively,
both contain instruction DWORDs `a0008004 / fe2b0000`. The complete retained
host assembler `libglslcompiler.so`, SHA-256
`0819f3e7461bd21a0e5dbd0fb614ab875436389b6b4cb573a7e583b583b3ba01`,
identifies the extended source bank as immediate and the value as zero. Together
with its global-register encoding, this gives **`STR index 0x104, #0`**,
corresponding to byte register `0x410`. Symbol offsets, encoded register indices
and byte-register offsets are distinct units; see the
[address conventions](../address-domains.md). This source-derived decode
does not prove execution, later writers or a width/height formula, and cannot
choose between tile counts and inclusive render-box extents. The adjacent
[depth consumer](../sgx535/depth-contract.md#zls-publication-and-the-selected-firmware-store-block)
has its own pointer and load-state boundary; it is not supplied by this zero
operand. No new hardware result follows from this static check.

## Completion boundary

The [SPM/TA/3D decode](partial-render.md) extends the static analysis from
individual stores to the OOM, partial-render and restart control flow, with
exact symbol/range identities. It does not establish execution under real
pool pressure or replace the completion and release boundaries below.

The [Windows retail comparison](../windows/retail-bootstrap-and-retirement.md)
separates task END, FREEP, PB_END and firmware counter candidates. An instruction
terminator is not a generic unmap fence. Likewise, an EMGD
[cleanup callback return](../emgd-linux/sync-cleanup-and-release.md) does not
authenticate a new firmware acknowledgement.

These gaps do not make the distinct
[PSB normal-success resource contract](../psb/normal-success-lifetime.md)
unknown. Its source consumers reach concrete page freeing and bank replacement
within their successful scope. [Hardware](../../hardware.md) owns the shared
applicability rules; native execution, cache visibility and display last-use
are separate from carrier identification.
