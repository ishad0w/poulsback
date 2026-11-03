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
and any buffer-size field's hardware unit need their own producer/consumer join.

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
