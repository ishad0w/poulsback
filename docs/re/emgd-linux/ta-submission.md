# Linux EMGD TA submission

This chapter follows the Linux EMGD 1.8 Build 2032 OpenGL producer through its
userspace Services packet and kernel CCB. [Sources](../sources.md) identifies
the exact OGL, userspace and kernel components. The recovered host chain is
useful without treating the private Services packet as a public driver ABI.

## Freeze submitted ranges before publication

The OGL producer maintains several rings. Reserve and commit helpers count
**DWORDs**; submitted allocation extents count **bytes**.

| Analysis address / ELF VA | Operation |
| --- | --- |
| `0x1548d0 / 0x1448d0` | Reserve a ring range, reject another outstanding reservation, return `cpu_base + 4*write_cursor`. |
| `0x154ad0 / 0x144ad0` | Commit the emitted DWORD count and clear the reservation; exact-fit wraps to zero. |
| `0x154c30 / 0x144c30` | Copy the write cursor at `+0x18` to the submitted cursor at `+0x1c`. |
| `0x155940 / 0x145940` | Build a submission, convert selected cursors to byte extents and retain allocation, sync and status references. |
| `0x154090 / 0x144090` | Forward the object to imported `SGXKickTA`; nonzero return reports software submission failure. |
| `0x154c60 / 0x144c60` | On the admitted success branch, advance the committed cursor, increment a nonzero generation and refresh the completed cursor from a shared byte value divided by four. |

Generation, committed extent and completed-read cursor are separate fields. A
commit does not prove that the GPU consumed the ring. Reuse needs the previous
generation's complete set of uses, not just a changed cursor.

## User CCB and kernel CCB

The user CCB holds the larger private TA packet. The kernel CCB has 256 slots,
each containing a four-DWORD service envelope. These are different buffers.

`SGXCreateRenderContext`, userspace ELF `0x9df0`, constructs a `0x4c`-byte user
object, a `0x3c`-byte firmware-facing context aligned to `0x20`, and a separate
CCB wrapper. The CCB constructor at ELF `0x7fe0` requests `0x10400` device
bytes: a logical 64-KiB ring and contiguous `0x400` tail. A separate eight-byte
allocation holds producer and consumer cursors. This does not establish double
mapping.

The wrapper keeps client/control mem-info, CPU and GPU bases, CPU write/read
pointers and logical size. Reserve helper ELF `0x81c0` checks logical-ring
space. Backing containment must also account for the real tail. Its legacy
waits do not establish a bounded-wait or retirement contract.

| Matched i386 object | CPU pointer | GPU VA | Allocation size | Kernel handle |
| --- | ---: | ---: | ---: | ---: |
| `PVRSRV_CLIENT_MEM_INFO` | `+0` | `+8` | `+0x18` | `+0x24` |
| Kernel mem-info | `+0` | `+4` | `+0x0c` | Separate object/layout |

All offsets are bytes. Bridge selector `0xc01c675f` registers the context GPU
VA through `SGXRegisterHWRenderContextKM`. Registration does not confer
ownership of arbitrary memory at that address.

## Private packet and bridge envelope

`SGXKickTA`, userspace ELF `0x8420`, locks render-target/context state, copies a
private packet, advances the user ring cursor and calls the bridge shim at ELF
`0x7190`. The shim copies a `0xe4`-byte envelope after an eight-byte prefix and
uses selector `0xc01c674e`. Kernel handle lookup precedes `SGXDoKickKM`.

The full-state branch copies `0x230` bytes and reserves `0x240`, rounded to
64-byte alignment. The update branch copies `0x230 + 8*update_count` bytes,
also rounded for reservation. Copied initialized bytes and padding have
different extents.

| Private packet interval | Meaning |
| --- | --- |
| `[0, 0x4c)` | Private prefix |
| `[0x4c, 0x1e4)` | `0x198`-byte shared sync/status block |
| `[0x1e4, 0x230)` | Additional producer state; hardware interpretation is partial |

The TA envelope has `data0=0`. Its `data1` is the firmware-context **GPU VA**,
obtained through user context `+0x14` → client mem-info `+8`. Envelope `+0x10`
contains the user CCB allocation's **kernel handle**. Envelope `+0xbc` is the
old CCB byte write offset plus `0x4c`, locating the shared block. None is a
shader pointer. Matching `sgxkick.c` validates that block and patches sync
state; this does not decode the whole private packet.

Render-target state is another owner outside the packet. Its constructor at
userspace ELF `0xa700` requests `0x144` device bytes; the initial state copy is
only `0x88` bytes. Packet `+0x34` carries that allocation's GPU base. The
destructor is at `0xa4f0`, and `SGXAddRenderTarget` at `0xb770`. Request flags
must not be assumed to equal later mem-info flags.

The retained packet/state observation exposes two additional containment
boundaries. One state selection is `base + 0x14 + index*0x40` in a separately
requested `0x94`-byte object. An RT-related pointer is `RTbase + 0x160`, beyond
the separately requested `0x144` bytes. Neither relation proves a readable
backing extent: the requested object size, allocator backing and a pointer in
the packet are independent facts. Following either pointer needs its own live
owner and complete range check.

## Publication and completion limits

The [publication example](submission-packet.md) explains the context-VA field,
shared-header patching and kernel CCB slot. The POWER selector requests a power
transition; it is not cleanup. Scheduler return zero establishes local command
publication, not GPU completion.

A pre-patch TA packet is not the final packet consumed by firmware. Input
ownership, sync patching, packet publication, firmware consumption and final
resource use are separate boundaries. Update packets also need their own
complete field and owner-range validation.

## Source identity

The selected OGL wrapper at ELF/file `0x144090` is 81 bytes, SHA-256
`1297805e2746036cdc2323d62e826085ee5251d99b07038ff518f6e978961ccd`.
The userspace `SGXKickTA` function has disjoint ranges; its first ELF/file range
starts at `0x8420`, is 3218 bytes, and hashes to
`493716aae95717ddcda6f9b2692513f7bcec7e1a5c0769dcd356a1f4e7a3f547`.
The CCB constructor's first range at `0x7fe0` is 244 bytes, SHA-256
`fe9c434ce51313e79248aca7fa320481f8c9d532e05a4398b8dc0682722dab90`.
These are range identities, not concatenated whole-function hashes.

The [cleanup chapter](sync-cleanup-and-release.md) covers their separate
completion and failure consumers.
