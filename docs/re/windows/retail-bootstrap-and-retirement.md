# Windows retail bootstrap and completion

The selected Windows GMA500 retail 8.14.10.2030 KMD provides static comparison
evidence for program setup, code banks, queues and completion candidates. It is
a different family from Windows EMGD 8.14.1180.3398. No Windows runtime on the
Dell is established by this analysis. Component and package identities are in
[Sources](../sources.md).

Code addresses below are PE analysis VAs with preferred base `0x10000000`.
Subtracting the base gives an RVA, not a file offset. The ranges below are
resolved through the actual file-backed PE sections.

## Generated program to initialization registers

Producer `0x100379f0` constructs a program/control/mailbox object whose parent
fields are consumed by initializer `0x10038620`. At
`0x1003809c…0x100380cb`, it masks an interface-derived value with
`0xfffffff0` into parent `P+0x80`, records a size expression shifted by four
at `P+0x84`, and stores control words at `P+0x88/+0x8c`. The initializer uses
these fields at MMIO byte offsets `0xa68/0xa6c/0xa70/0xa58`.

That closes the selected producer/consumer link. The interface value's complete
GPU mapping and visibility contract and the control words' exact profile
interpretation remain separate. `0xa58` must not be interpreted by extending
the USE-bank register formula. Parent `P+0x9c/+0xa0` configuration fields also
differ from identically numbered offsets within the allocated mailbox.

The initializer queries a descriptor interface for types 4, 5 and 2:

| Successful query | Register byte offsets | Constructed words |
| --- | --- | --- |
| Type 4 | `0xa0c…0xa30`, step 4 | `(B >> 7) \| 0x02000000`, advancing B by `0x80000` |
| Type 5 | `0xa34…0xa40`, step 4 | `B >> 7`, same advance |
| Type 2 | `0xa48` | `(B >> 7) \| 0x06000000` |

The separately selected SGX535 definitions interpret these as 128-byte base
units, a 512-KiB arithmetic stride, and indices 0–9, 10–13 and 15. This body
does not write index 14/`0xa44`; its absence is not a universal reservation
rule. Pairing those headers with the compiled Windows revision and errata
profile remains conditional.

## Dynamic provider ownership and failure branches

Factory `0x100375b0` allocates O, passes incoming interface I unchanged to
constructor `0x100372b0`, and releases O if initialization fails. The constructor
stores `O+4=I`, obtains H from `I.virtual+8`, and stores `O+0x44=H`.
Descriptor queries use `H.virtual+4`; program allocation uses `I.virtual+0xc`.
Equality of these dynamic providers is unproved. The outer object's vtable
does not identify H's allocation implementation.

A failed descriptor query skips that bank-write block, but the initializer
continues. After its final callback, it forces AL=1. Success therefore does
not certify that every bank was newly registered or that memory is visible,
ready or safe to reuse. Readback and noncomparative register reads do not
automatically establish a posted-write drain.

## END, FREEP and host counters

Raw selector `0x71` chooses a 31104-byte embedded program. Other raw selectors
choose a 30176-byte carrier at static VA `0x1008e660`, SHA-256
`7db5f4a5dee232a134c9468dca126c49f6b9314afe792028ff952d6fef5fcb90`.
A returned raw selector `0x79` is not independently authenticated Dell silicon
revision or stepping. Firmware PCs count **eight-byte USE records**; PDS words
are four bytes, and this family's CCB slots are 32 bytes with a modulo-256
cursor.

The PDS producer forwards mailbox/control tokens and two interrupt-register
values into primary attributes. Conditional field decoding of the selected
carrier shows PB-end and memory-free paths joining after their pending flags
clear. The joint tail at PC `0x62d` and another path at PC `0xac7` both call
helper `0x5b9`. That helper has an increment/store candidate and ST-path
IDF/WDF operations. Its complete effective address, active MOE, generation and
relationship to fixed-function write ordering remain unresolved. The second
caller also prevents treating it as a raster-only retirement marker.

PBE producer `0x10048f60…0x10049163` emits state LIMM and PBE EMIT records,
then ORs the last instruction's high word with `0x00040000`, the task END
bit. FREEP is a separate low-word mask `0x00200000`. In the selected raw79
feature projection, END is placed on the second PBE EMIT; other feature
branches differ. Task END is not EVENT1 PB_END despite a possible numeric-mask
coincidence. Neither END nor FREEP alone is a general unmap fence.

Host getter `0x10030f10` and setter `0x10030f30` access `O+0x5c+4*index`.
Enqueue `0x1003af40` copies that cached value into CCB word 3. It is a CPU-owned
value before enqueue, not the firmware helper's encoded load operand 3, a
physical address, or a recovered host equality test proving completion. Normal
submit can update pending state before enqueue; wider error cleanup is outside
that selected body. Pending increments are not execution credit.

## Identity and interpretation limits

The exact KMD is 648832 bytes, SHA-256
`42770a2162fd0c7e227a50cdde8547581b7895d3ed771f9b7995f7409e774ced`.
Its selected ranges are:

| Procedure | PE VA, inclusive | File start / bytes | SHA-256 of range |
| --- | --- | --- | --- |
| Initializer | `0x10038620…0x100389e1` | `230944 / 962` | `ad8254a663cf6d1e828427072b0b0f53d61c748b374695b1b9b20216f913583d` |
| Factory | `0x100375b0…0x100375e9` | `226736 / 58` | `ccc16afa3a0ae356568cff4d8ea203955caffb8ff83f34ff83548471f7b86c1a` |
| PBE producer | `0x10048f60…0x10049163` | `298848 / 516` | `a17765222adb949c4e099ddf9000112819e3ab4a8d9d4bca50c7ef1ce712e14a` |

Inferred procedure names are analysis labels, not recovered PDB types.
Indirect providers and some preliminary helpers remain unresolved; the
carrier projections do not emulate an entire firmware run. Source-byte
availability is documented in the local [evidence index](../evidence.md).

The unknown Windows marker join must not be generalized to every legacy
retirement path. The distinct
[PSB normal-success contract](../psb/normal-success-lifetime.md) is source-known
in its own scope. Native execution, WB/cache handoff and display last-use still
need separate source/build/profile-bound validation, as described by
[Hardware](../../hardware.md). Neither this static comparison nor a retained
finite image supplies that qualification.
