# MSVDX firmware containers and host transport

MSVDX uses MTX firmware, separate from SGX's program carriers and USE/PDS
instruction streams. This chapter identifies the matched legacy EMGD and PSB
containers, their units and the host publication path. It contains no firmware
payload or vendor source. Static carrier identity does not identify executing
MTX RAM; see [decode lifetime](decode-pipeline.md) and the
[publication and surface contracts](surface-layout.md).

Reading routes: [EMGD descriptor](#emgds-packed-descriptor),
[PSB container](#psbs-four-word-file-container),
[native requests](#a-render-request-is-not-a-firmware-request) and
[ring/reply semantics](#ring-and-reply-semantics).

## EMGD's packed descriptor

The Linux EMGD 1.8 Build 2032 carrier is `emgd_drv_video.so`, 612,644 bytes,
SHA-256 `9176c80fbb5e33dee86567e291fd9e9dd310027ace76e19a0122e4088e7f0ad2`.
The matched source is `emgd-1.8.0.2032`; its selected kernel module is
548,032 bytes, SHA-256
`551ef2067798459e9e5b41275b2b27f97af5d2b4c4db6ef9fa32598473ed6335`.
These are not the newer EMGD 1.18/3398 components.

The x86 producer occupies ELF VA `[0x2243,0x266c)`. It allocates a packed
firmware object, writes seven u32 fields, copies text/data, appends the version
and converts three temporary pointers to offsets within the object. At ELF VA
`0x2622` it submits command `0x29` with 28 argument bytes, `cmd=2`, `engine=1`.
Its allocation expression yields `0x3e00` bytes. The version reserves 44 bytes,
of which 41 are copied including NUL. Uninitialized padding is not a recovered
firmware dump.

| Payload | ELF VA | File range, end exclusive | SHA-256 |
| --- | --- | --- | --- |
| MTX text: 2,858 u32 / 11,432 bytes | `0x87720` | `[0x86720,0x893c8)` | `c14ba1096291a9969bf97ed9c9fe697091144f56f51d13e982c2dc87d46afe1b` |
| MTX data: 1,010 u32 / 4,040 bytes | `0x8a3e0` | `[0x893e0,0x8a3a8)` | `2f855d33993530b9c4f856fb3b13f44e8fcf9ee81251018e1013219b0277d4cd` |
| Version: 41 bytes | `0x2c618` | `[0x2c618,0x2c641)` | Identified by the exact carrier and bounded range above. |

The version string is `BUILD_DXVA_FW1-00-10-0944_SliceSwitching`. Matching
source selects MTX text destination `0x80900000` and data destination
`0x82882cc0`. These are MTX addresses, not host pointers or SGX USE banks.
The producer is x86 code; its contained MTX payload is not.

The table uses ELF VAs. The carrier's local `R_386_RELATIVE` relocations affect
GOT/version-pointer slots rather than the selected text/data payload ranges. That establishes original static bytes, not a runtime upload or a
decoded MTX instruction set. [Address domains](../address-domains.md) explains
the distinction.

The matching loader interprets sizes in words and descriptor pointers as
offsets. A later initialization may pass `fw_size=0`, `fw_priv=0` and reuse
`priv_fw`. A successful later ioctl or the carrier's disk hash therefore
cannot establish which bytes currently execute in MTX RAM.

## PSB's four-word file container

The separate firmware-package 0.30 file `msvdx_fw.bin` has 15,564 bytes,
SHA-256 `61e799209466adbfbad9202611c3d496e6e253472118870b07f16807d4345c55`.
Its header consists of four little-endian u32 values:

| Header field | Value / unit |
| --- | --- |
| Version | 2 |
| Text length | 2,841 u32 words |
| Data length | 1,046 u32 words |
| MTX data destination | `0x82882c80` |

Text starts at byte 16 and data at byte 11,380. The exact file extent is
`16 + 4*2841 + 4*1046 = 15564` bytes. Text SHA-256 is
`6b1f95670fce23214d74e86cb6728cddfead91f5889890a8a5654b8324a59791`;
data SHA-256 is
`5cb5cebbd7c7d40302c207b06facf61b6491911d0e5f3dca825c59b3acfa7d82`.
Both payloads differ from EMGD's. The matched PSB source identity is
`98b5307e5158a9ac401b29128ddd1184ae06b4d7`.

Both selected loaders use code base `0x80900000` and data-memory base
`0x82880000`. The header's `0x82882c80` is a destination within that MTX data
domain; EMGD's `0x82882cc0` selects a different offset. Common address domains
do not make the payloads interchangeable.

A disk read of this file is not a dump of executing MTX RAM. Its four-field
container, data destination and loader profile cannot be silently combined
with EMGD's seven-field descriptor or per-message fence convention.

The selected Windows carriers have different markers again: retail 2030 has
`BUILD_DXVA_FW1-00-10-0858_SliceSwitching`, while embedded 3398 has
`BUILD_DXVA_FW1-00-10-0858-02_0006_SliceSwitching`. These are container strings,
not proof of compatible MTX code, messages or executing firmware. Extraction,
patching, upload destinations and runtime identity need their own joins.

## A render request is not a firmware request

Native render command `0x27` has a 20-byte envelope containing a nested return,
engine, offset, alias and returned fence. It is distinct from the producer's
`0x29` firmware request and its 28-byte argument.

The render allocation's header declares a message offset and count. A zero
count reaches the context-query return before `populate_fence_id`,
`process_mtx_messages` and `send_to_mtx`; trailing backing words are not
messages and the branch is not firmware bootstrap.

For a nonempty render request, validate the complete declared span against its
allocation owner. Render messages have size 32 bytes and ID `0x81`. The kernel
assigns a platform fence at byte 16 and the selected directory word OR 1 at
byte 4 before sending the message. [Publication and surfaces](surface-layout.md)
explains the separate command, output and completion boundaries.

Useful locations in source `emgd-1.8.0.2032` and its exact module are:

| Contract | Selected source lines | Relocatable module location |
| --- | --- | --- |
| Zero-count context query | `msvdx_init.c:581–682`, `process_video_decode_plb` | Branch at `.text+0x28eaa..0x28ec7` |
| Fence patch | `msvdx.c:145–168`, `populate_fence_id` | `.text+0x29110` |
| Directory patch and transport call | `msvdx.c:176–223`, `process_mtx_messages` | `.text+0x29360` |

The `.text` locations are section offsets, not live virtual addresses.
Envelope words, GPU VAs, kernel aliases and MTX addresses are different
namespaces. Numeric subtraction between them does not prove an LLDMA mapping,
DMA extent or external-reference ownership. The per-slice address linkage
needs its own producer/consumer join. The PSB render-size producer below does
not establish EMGD's corresponding unit.

## PSB render sizes and DMA addresses

The xpsb-glx 0.18 `psb_drv_video.so` is 188,948 bytes, SHA-256
`f0103b67f8c7bc522fe67bdb0d0fc47c0e15cd1a08963aebe07e1a1176c6495b`.
The reference collection contains identical copies in the original source
archive and Ubuntu 10.04 PSB stack. Its x86 code gives the producer-side
contracts below. All locations are ELF virtual addresses, not live pointers.
This identifies host command construction, not executing MTX firmware or
successful decoding.

### Render buffer size counts bytes

The render producer at `0x6890` reads command-start and command-end pointers
from object offsets `0x114` and `0x118`. With no continuation segment, the branch
at `0x6a48` subtracts these pointers without a shift. With continuation segments,
it uses the saved first-segment length at offset `0x148`. The segment builder
at `0x5bb0` saves that length as a byte-pointer difference, including the eight
bytes it reserves for the continuation command.

The producer at `0x698a..0x699d` writes that byte count into the low 12 bits of
the render halfword at byte 2. It does not divide by four. The kernel consumer
`psb_msvdx_map_command` in the same stack's `psb-kernel-source-4.42.0/psb_msvdx.c`
(SHA-256 `6870b7822b7c6017c2150868eff4052275c6232c26c4b97aee3ed391bf7b2f0d`)
patches the fence and PTD but leaves this halfword unchanged.

This differs from the comparative AOSP `dfc4b4b` implementation:
`psb_cmdbuf.c` writes `cmdbuffer_size / 4` to `FW_DEVA_DECODE_BUFFER_SIZE`.
That is a different message profile. Its word count must not replace the byte
count in this PSB `0x81` render message. The separate message header at byte 0
still counts envelope bytes; host-ring indices count DWORDs.

### Render commands point through an LLDMA record

The render producer passes the command allocation, command-start byte offset
and segment byte count to the helper at `0x5a00`. The helper returns the
new LLDMA record's byte offset within that allocation. The relocation call
at `0x6953..0x6985` places the full device address of that record in render
byte 8. It is not the address of the first register command.

The record builder at `0x5750` reserves 32 bytes per record. It clears each
record, writes the device destination at byte 8, and relocates the source
allocation address into byte 24. It divides the supplied byte count by the
selected peripheral width before writing the transfer count at byte 4.
The width lookup at `0x20660` is `{4, 2, 1}` bytes. Transfers larger than
65,535 peripheral units create another record; the next-record address at
byte 28 is encoded as `device_address >> 4`. The last record clears that
address and sets bit 31 of the word at byte 4.

The producer's eight-word DMA profiles begin at `0x20500`. Relevant entries are:

| Profile index | Device destination | Peripheral width | MMU group |
| ---: | ---: | ---: | ---: |
| 0, VLC table | `0x3000` | 2 bytes | 0 |
| 1, bitstream | `0x0820` | 1 byte | 0 |
| 3, MC command buffer | `0x0350` | 4 bytes | 1 |
| 4, VLD command buffer | `0x0350` | 4 bytes | 0 |

The render caller selects profile 4 when its context field at offset `0x104`
equals 2; otherwise it selects profile 3. The bitstream address is carried by
its own LLDMA source relocation, separate from the three shift-register setup
words. Full source spans, alignment, reference-picture ownership and output
planes still need validation by the native submission boundary. These static
locators do not authorize reuse of the vendor's unchecked relocation ioctl.

### PSB render operation flags

The producer adds flags at final publication, after constructing the render
message. At `0x6138..0x617c`, it ORs `0x4000` into byte 28 of the last message
in the batch (`0x2000` for earlier messages). It also ORs `0x0800` when context
field `0x104` equals 2. The MPEG-2 path sets that field to 2 at `0x7735`.
The matched kernel consumer preserves these flags.

The comparative AOSP `dxva_fw_flags.h` names the old render flag
`FW_VA_RENDER_IS_VLD_NOT_MC`. Its newer DEVA profile assigns another meaning
to the same bit; that meaning does not apply to this PSB render envelope.
Poulsback submits each slice as a one-message batch, so byte 28 must contain
`0x4800`: VLD operation and a host completion interrupt. The VLD DMA profile
alone does not select the firmware operation.

### PSB compressed-input allocation extent

The matched PSB host producer rounds buffer allocation sizes to 32 KiB at
`0x2ed7..0x2ee0` before the BO-create call at `0x2f00`. The comparative AOSP
`psb__allocate_BO_buffer` uses the same rounding for slice-data buffers.
Poulsback keeps this allocation extent for compressed input and clears the
padding. Slice validation and DMA lengths still use the logical payload size;
allocated padding does not extend the accepted bitstream.

The matching PSB 4.42 kernel defaults `drm_psb_trap_pagefaults` to zero in
`psb_drv.c:40` and passes it to `psb_mmu_driver_init` at lines 609–612.
`psb_mmu_alloc_pd` in `psb_mmu.c:224–267` then fills unused PDEs and PTEs
with mappings to a cleared dummy page. BO bindings use this default directory
through `drm_psb_tbe_bind` in `psb_buffer.c`. Reads beyond a BO can therefore
reach zeros in that driver instead of faulting on an unmapped page.

Poulsback keeps one cleared tail page after the 32-KiB-rounded compressed
input allocation. This bounds the compatibility mapping to the page immediately
after the input; other unused addresses remain unmapped. The tail belongs to
the job and follows its normal completion or HOLD lifetime. Logical payload,
slice bounds and DMA lengths remain unchanged. This differs from PSB's mapping
of every unused address and still needs device qualification; it does not
establish a general MSVDX read-ahead limit or malformed-input recovery.

### PSB context tags

The same PSB producer builds a context tag at `0x36df..0x36ec` from the low
16 bits of its VA context ID and a process-derived upper halfword
(`getpid() << 16` at `0x4858..0x4863`). It writes
that tag at render byte 12 (`0x6909..0x6912`). The matched kernel consumer
preserves it while patching the fence and directory. Normal userspace decoder
contexts therefore carry a nonzero tag; zero is not the native producer's
first-context convention.

The native kernel decoder owns one serialized engine and private address
space, so it uses one nonzero opaque tag, `0x00010001`. This preserves the
producer's tag form without exporting process IDs or accepting userspace tags.
Whether a zero tag caused a particular device failure still requires a matched
physical run; static construction alone does not establish firmware behavior.

## Ring and reply semantics

The selected source orders transport as checked ring space → copy message
DWORDs → publish write index → kick MTX → read interrupt status. The ring uses
word indices, reserves a slot, and has padding/wrap and insufficient-space
branches. The PSB producer converts message size from bytes to DWORDs as
`(size + 3) / 4`; that conversion does not change the message header's byte unit.
A function-entry trace establishes entry-path activity, not every ring write,
index or reply.

Reply classification must retain success, failed command and hardware panic
separately: all can advance `mtx_completed` in the selected implementation.
Fence assignment, observed completion, CPU/display visibility and last
reference use are separate facts. Neither a successful firmware-init call nor
correct decoded planes closes those missing joins. The broader
[decode pipeline](decode-pipeline.md) describes the output and lifetime limits.

## MSVDX memory-fault diagnostics

The EMGD 1.8 source `emgd/include/msvdx.h` identifies the MSVDX MMU status
register at block offset `0x068c`. Its `msvdx.c` fault handler reads that register
when interrupt status contains `0x00000f00`. The comparative AOSP core header
places the fault page address in bits 31:12 and access status in bit 0.
Poulsback records this status, the current page-table entries and the job's
mapped ranges when a memory fault occurs. These reads do not clear the fault,
reset the engine or authorize releasing its mappings.
