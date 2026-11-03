# PSB PDS/USE addressing and depth

The selected PSB DRI producers distinguish byte offsets from instruction
indices, emitted length from allocation capacity, and depth storage from color
storage. Their exact `psb_dri.so` identity is in [Sources](../sources.md).
Related PSB 4.41 source supplies the kernel consumers; its exact pairing with
the deployed DRI/kernel binaries is not established. These are selected-family
contracts, not rules for every SGX535 revision.

Unless labelled otherwise, function addresses below are analysis byte VAs with
`ELF VA = analysis VA - 0x10000`. Original ELF segments determine file offsets.
CPU-object offsets and program extents are bytes unless another unit is given.

## USE entry: bank and relative byte offset

The vertex producer at analysis `0x50355`, ELF `0x40355`, calls the task
relocation producer at analysis `0x4a82c`, ELF `0x3a82c`. The latter emits:

| Input | Source bits | Task-word destination | Selection mask |
| --- | --- | --- | --- |
| USE bank index | 0–3 | 0–3 | `0x0000000f` |
| Entry byte offset within bank | 15–18 | 4–7 | `0x000000f0` |
| Entry byte offset within bank | 4–14 | 8–18 | `0x0007ff00` |

The bank field uses relocation type 5; the other fields use type 4. Their
right/left shift pairs are `(0,0)`, `(15,4)` and `(4,8)`. The selected vertex
caller supplies data master zero and no additional low-field pre-add.

Each complete relocation is 40 bytes. It retains the source BO index,
destination buffer and DWORD index, mask, shifts, byte offset, background,
required code extent and data master. Related `psb_sgx.c` and `psb_regman.c`
consumers choose a 512-KiB code bank and return its number plus a relative
**byte offset**, not an instruction index.

This projection drops byte bits 0–3. A checked reconstruction must therefore
reject entries not aligned to 16 bytes. A USE instruction is eight bytes;
instruction width and this entry alignment are different constraints. Variable
destination bits cover `0x0007ffff`; other task controls need their own
producer/consumer interpretation.

## Program extent follows emission

The allocator at analysis `0x40ffd`, ELF `0x30ffd`, converts the requested
maximum instruction count to bytes by multiplying by eight. It requests
32-byte allocation alignment and zeros the code buffer. This is capacity.

The suballocation descriptor created at analysis `0x48762` holds a BO, byte
offset and initially zero emitted-length word. The finish path
`0x40fb7 → 0x484ab` computes the difference between the final emission pointer
and allocation CPU start. Descriptor word 2 stores that byte length; relocation
`arg0` subsequently carries it to the code-bank consumer.

The consumer needs the entire emitted extent, not just its first instruction
or unused capacity. The legacy finalizer stores length before asserting that
it fits. A checked implementation must perform that bound check before
publishing the descriptor. A shared code owner also needs its complete set of
TA and raster uses before reuse; its descriptive name is insufficient.

## Ordinary fragment SECONDARY constants

The selected `psb_dri.so` producer `0x38559` builds a six-DWORD USE task
record, calls `0x36434` for an eight-byte, 32-byte-aligned empty pixel task,
then calls `emit_saconsts` at analysis `0x49ea0`, ELF `0x39ea0`. The existing
linker at `0x4a82c` carries the complete emitted extent with data master 1.
An allocated empty task is distinct from an application fragment program.

The placement table distinguishes source parameter indices from hardware
destination `d`. Raw constant words go to physical `SA(d+1)`; SA0 is reserved.
With compiler-result flag `0x80`, SA0 carries a relocation to memory constants.
Its contents are not established when that flag is clear. The complete payload
is `moved_constant_count+1` plus two shader state counts. Four dense constants
therefore require at least five DWORDs; a logical uniform bitmap is not the
physical allocation budget.

| Quantity | Selected ordinary producer |
| --- | --- |
| Constant DMA | Full GPU byte address; finalized byte extent becomes a DWORD count with bit 31 in the last DMA control |
| Direct count `S<=16` | `0x80000000 \| (S-1)`; five DWORDs give `0x80000004`, without the vertex DMA stride field |
| PDS data and emitted size | Sixteen DWORDs/64 bytes of data; 76 or 80 emitted bytes inside a 96-byte, 32-byte-aligned allocation |
| MTE secondary pointer | `(GPU_byte_VA >> 4) & 0x00ffffff`; data DWORDs contribute `(count & 0xfc) << 24`, without subtracting PDS EXEC BASE |

Using the vertex direct-transfer control for a five-word secondary would add
an unrelated stride field and produce `0x80800004`. The selected ordinary
secondary contract requires `0x80000004`. The empty task's non-address controls
also remain zero; borrowing a state task's control is a different construction.

The producer stores the secondary descriptor, data count and physical SA extent
at scene byte offsets `+0x448/+0x454/+0x458`. Chain `0x39251 → 0x38fff →
0x387ff` consumes them at byte offsets `+0x34/+0x40/+0x44` of the passed record.
For physical extent `S` DWORDs, the serializer allocates `ceil(4*S/128) << 18`
and enables bit `0x8000` when nonempty. Its scheduling equation subtracts
`align_up(S,32)` DWORDs from `0x4df`; TEMP allocation separately rounds `4*T`
**bytes** to 16-byte units. A matched field equation is not a measured limit.

This ordinary PSB profile differs from the twelve-data-DWORD prepacked profile.
The corresponding ordinary Natty EMGD generator is still unresolved by this
static chain. The [captured vertex contract](../emgd-linux/vertex-path.md#partitions-and-secondary-constants)
and [pixel contract](../emgd-linux/fragment-path.md#isp-state-and-epilogues) captures
observe EMGD's actual layouts independently; they do not recover that generator.
Neither selected PSB profile establishes physical SA population, DMA visibility, task execution or
safe bank reuse. [Instruction entry state](../sgx535/usse-instruction-contracts.md)
and [normal-success lifetime](normal-success-lifetime.md) have separate owners.

## Depth storage and attachment

The attachment producer at analysis `0x573bd`, ELF `0x473bd`, installs the
depth renderbuffer region at context byte offset `0x13ae8`. It flushes the frame
tracker before replacing a changed pointer. The allocation producer at
analysis `0x59f7b`, ELF `0x49f7b`, distinguishes:

| Request | Selected storage |
| --- | --- |
| D16 | Two bytes/pixel, internal format `0x81a5`, type `0x1403` |
| Depth24/32/component | Four bytes/pixel, internal `0x88f0`, type `0x84fa`, 24 depth bits |
| Depth/stencil | Four-byte family with 24 depth and eight stencil bits |

Backing extent is `cpp × align(width,32) × align(height,32)`. Separate backing
and view objects retain pitch, size and dimensions. This source allocation
rule does not by itself prove the complete GPU write footprint or a new API's
storage layout.

The ZLS producer at analysis `0x3ab70`, ELF `0x2ab70`, encodes pitch in
16-pixel units: `((pitch_pixels >> 4) - 1) << 8`. Five relocations reference the
same depth BO, with zero pre-add and background:

| Register byte offset | Selected DRI relocation mask |
| --- | --- |
| `0xcb0` | `0xf0000000` |
| `0x484`, `0x488`, `0x48c`, `0x490` | `0x0ffffff0` |

The mask chooses patched bits. The related source combines
`(background & ~mask) | (address & mask)` after the selected shifts. EMGD
headers instead give `0xfff00000` for `0xcb0`. The family difference remains
unresolved; one literal must not silently replace the other.

The ordinary depth-state producer requires an attachment and enabled depth
testing. GL comparison operations `0x200…0x207` map to `0…0x01c00000` in
`0x00400000` steps. Write disable is `0x00100000`; the replacement mask is
`0x01d00000`. These fields do not qualify stencil or all depth API behavior.

## Clear geometry is only a prefix

At analysis `0x3a794`, ELF `0x2a794`, the clear producer builds four vertices:
`(0,0,Z,1)`, `(W,0,Z,1)`, `(0,H,Z,1)`, `(W,H,Z,1)`, with indices
`0,1,2,3,2,1`. Z is loaded from depth-object field `+0x14`; the literal
`0x3f800000` supplies W. Color and depth dimensions must match. Z is not a
hard-coded `+1` rule. The upstream clear-value setter and hardware quantization
remain separate questions.

Its expanded state mask is `0x54c5`. Serializer analysis `0x387ff`, ELF
`0x287ff`, emits 11 MTE words, including ISP primary `0x01e00100` and ISP word 3
`0x0e000000`. The retained prefix lacks final termination and the event
program. A complete scene also needs state restoration, subsequent draws,
checked termination/event, publication and matching completion. RGB BOs do not
become depth objects through pointer conversion.

## Selected range identities

These are individual file-backed ranges in the exact DRI component, not
runtime addresses. The selected import adds `0x10000` to ELF VA. File offsets
below were resolved through ELF segments.

| Producer | ELF VA / file offset | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Vertex PDS | `0x40355` | 1094 | `0aaa7e0545749ccfef23a39c798549d0ee6d9149ffa1efc51ece2a55c23eb470` |
| USE task relocation | `0x3a82c` | 332 | `71bca066e868f35915a240509140b64c6d634e62d205180b20e87c0569c83196` |
| USE allocation | `0x30ffd` | 215 | `81d763cfe83aae102161b25a0a711ee5c828416f54340f350cf9207bc1002dce` |
| Depth attachment | `0x473bd` | 791 | `6c4ebedcf3de5b0e574f76619afde1560dd77077a5278ffd250a30d0f7bff4ab` |
| Depth allocation | `0x49f7b` | 943 | `1d012c216cd0e0465b0d9d61e81d7f31eefd332f99f8386a45b2f3a57cc2b7f6` |
| Clear geometry | `0x2a794` | 975 | `1b33033c14afadada6ae992d5be923c7e1d932ae81e20a0eb267021b9704bc25` |
| State serializer | `0x287ff` | 2048 | `ba53dd47f9add7103f5e8084abf8499891e49a8d3485f4760214468fbeb4d2c2` |
| ZLS | `0x2ab70` | 1488 | `baf2bbf1a513a0e01f03ab07fb4d6704cd3e10f1c8d67f66c38aadea768648bd` |

[Hardware](../../hardware.md) owns
cross-component applicability; [normal-success lifetime](normal-success-lifetime.md)
explains the code-bank and BO release consumers.
