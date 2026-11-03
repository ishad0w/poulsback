# MSVDX publication and NV12 output contracts

This chapter explains the Linux EMGD 1.8 Build 2032 MPEG-2 command path and
how to interpret an NV12 output descriptor. The [source profiles](../sources.md)
and [firmware profile](msvdx-firmware.md) identify its vendor components.
These are vendor contracts; [Video](../../video.md) describes the current
poulsback implementation and remaining device work.

## Source identity

The selected source is `emgd-1.8.0.2032`. Its MSVDX kernel implementation is
`msvdx.c`; relevant entry points are `populate_fence_id`,
`process_mtx_messages`, `send_to_mtx` and `msvdx_mtx_interrupt_plb`.
Relocatable module `.text` offsets and process runtime addresses are different
address domains; [address conventions](../address-domains.md) defines them.

Identifying the firmware carrier does not establish which patched MTX bytes
execute on a device. Container, loader, upload plan and executing image need
matching identities.

## Command and owner bounds

A native render command contains an allocation owner, message offset and
message count. The offset/count must select whole envelopes inside the
allocation. Pointer equality does not establish ownership: an allocator can
reuse a numeric CPU address for a different object generation.

The command allocation, firmware-facing buffers and decoded image each have
their own lifetime. A kernel alias is a lookup key, not a readable userspace
pointer. A decoder must establish the complete readable owner extent before
following packet, bitstream, LLDMA or output-surface addresses.

A zero-message context query can return context/fence state without dispatching
any trailing allocation words. Distinguish the declared message span from
unused backing bytes; an ioctl return and a stale `errno` are also separate
values.

## Kernel patching

The render envelope has ID `0x81` and length 32 bytes. The source patches these
fields before publication:

| Field | Byte offset | Kernel action |
| --- | ---: | --- |
| PTD | 4 | `process_mtx_messages` reads the selected directory register and writes its value OR 1 |
| Fence | 16 | `populate_fence_id` increments the platform fence for each message |
| Context | 12 | Retained as the envelope's context association |
| Flags | 28 | `FW_VA_RENDER_HOST_INT` is bit `0x4000`; it requests an interrupt for the selected message |

The final assigned message fence can also be returned by the enclosing
submission. That equality identifies a submitted batch; it is not independent
completion evidence. The full [CPU envelope contract](../../video-protocol.md)
separates encoding, stream validation and stamping from physical publication.

## Host publication

The matched `send_to_mtx` source orders its actions as:

```text
check ring space → copy message DWORDs to MTX local RAM
                 → publish write index → kick MTX → read interrupt status
```

The word copy is at module `.text+0x29280..0x2929d`; write-index publication
at `+0x292c6`, kick at `+0x292dd`, and status reads at `+0x292ef..0x292f5`.
The corresponding source body is `msvdx.c:334–395`. These locators explain the
producer order, not a generic execution or retirement guarantee.

Wrap, insufficient-space and oversized-message branches require their own
bounds and error handling. The vendor outer loop can retry BUSY without a
bound; it is not a safe retry policy for the new driver. A function-entry
trace cannot establish every ring byte, MMIO write, return value or firmware
reply. Timing across different clock domains requires calibration.

## NV12 plane bounds

An NV12 descriptor carries image width/height, Y and UV pitches, plane offsets
and a reported size. The complete mapped allocation must contain every active
sample; pitch alone does not prove that the chroma plane is readable.

For an even-sized image, active samples use:

```text
Y[y,x] = storage[y_offset  + y*y_pitch  + x]
U[y,x] = storage[uv_offset + y*uv_pitch + 2*x]
V[y,x] = storage[uv_offset + y*uv_pitch + 2*x + 1]
```

Y coordinates span the full width and height; U/V coordinates span half each
dimension. Check multiplication/addition overflow, each plane's final sample
and the backing extent independently. Padding bytes are not active image
samples and need a separate guard/unchanged-storage check.

For example, a synthetic 64×64 image with both pitches 128, Y offset 0 and UV
offset 8192 needs at least 12,224 readable bytes for all active samples:
Y's last byte is at 8127, and UV's last byte at 12,223. Reserving the full
padded planes instead requires 12,288 bytes. These are layout calculations,
not decoded-output or hardware observations.

A byte comparison with an independent software decode checks image samples.
It does not identify the executing firmware or prove complete LLDMA-to-YUV
addressing, final device use, display presentation or safe buffer reuse.

## Completion and retirement

The selected `msvdx_mtx_interrupt_plb` updates `mtx_completed` for successful,
failed and panic replies. Therefore a fence value or a successful API wait
alone cannot distinguish these outcomes. Match reply ID, fence, status and
flags to the submitted message generation.

PVR free, `vaTerminate` and client exit describe API cleanup. They do not
establish the last device read, device-write drain or safe reuse. Preserve
submission identity, publication, successful completion, visible pixels and
last use as separate facts. The required lifetime contract covers command,
LLDMA, bitstream, reference-picture and output-plane owners.

MSVDX's message and fence rules do not transfer to SGX TA/raster merely because
both use shared memory infrastructure.
