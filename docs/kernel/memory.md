# Kernel memory and release

[Kernel overview](../kernel.md) · [Execution](execution.md) · [Memory](memory.md) ·
[Display](display.md) · [Diagnostics](diagnostics.md)

- [Buffer objects and per-file address spaces](#buffer-objects-and-per-file-address-spaces)
- [Memory, locking and lifetime](#memory-locking-and-lifetime)
- [Buffer-object PRIME and CPU access](#buffer-object-prime-and-cpu-access)
- [Physical source transaction boundary](#physical-source-transaction-boundary)

## Buffer objects and per-file address spaces

Buffer objects in per-file address spaces are the only GPU
and display memory path: KMS framebuffers are made of `SCANOUT` BOs.

**Address space (`kernel/memory/poulsback_vm.c`).** Every open file owns one SGX
page directory and a `drm_mm` VA allocator. Jobs and scenes hold VM references
after file close; a frozen VM or one with retained mappings stays held for
the boot. Page tables are allocated on the first mapping in their 4 MiB and
freed with the last. PTEs and PDEs are written, then flushed with
`clflush`. The [engine](execution.md#p3-engine) installs the 3D job's directory
in BIF slot 0 and the 2D job's in slot 1, and pulses `INVALDC` after routing
changes. A BO's mapping in a file goes with the last of its handles there
and of the jobs that resolved it through one (`kernel/gem/poulsback_bo_use.c`,
host-tested): a job holds the mapping from `SUBMIT`'s resolution until the
job is released, so its VA stays mapped even when the last handle closes
before the job's fence is published. Closing the last handle waits up to 10 s
for the BO's READ and WRITE fences before removing the mapping. A busy or
frozen mapping stays held rather than being unmapped under the GPU.

**Fixed layout (`kernel/memory/poulsback_vm_layout.c`, host-tested).** One layout for
every file:

| Range | Class | Constraint |
| --- | --- | --- |
| `0x20000000`–`0x2FFFFFFF` | PDS programs | MTE PDS pointers are 28-bit offsets from `PDS_EXEC_BASE` |
| `0x30000000`–`0x33FFFFFF` | device: parameter buffer, kernel objects | TA/3D request base `0x30000000`, 256 MiB window |
| `0x34000000`–`0x3FFFFFFF` | parameter: background objects | `BGOBJTAG` holds a 24-bit offset in 16-byte units from the 3D request base |
| `0x40000000`–`0x402FFFFF` | USE code | six 512 KiB windows; window *w* is USE code bank 3 + 2*w* for the vertex data master and 4 + 2*w* for the pixel one, so any code BO serves both; a code BO never crosses a window |
| `0x48000000`–`0x49FFFFFF` | device: DPM tables | 4 MiB aligned so its page tables can be shared |
| `0x4C000000`–`0x4FFFFFFF` | device: scene buffers | scene in `0x4xxxxxxx` (heap layout policy) |
| `0x50000000`–`0x5FFFFFFF` | depth | ZLS takes the top four address bits once and 28-bit bases, so a depth buffer stays in one 256 MiB window |
| `0x60000000`–`0xCFFFFFFF` | general | TA and texture pointers are full 32-bit |
| `0xD0000000`–`0xD0000FFF` | device: 2D scratch page | the file's own page at `TWOD_REQ_BASE`, which ends every 2D job ([HW-2D-004](../hardware.md)) |
| `0xD0001000`–`0xDFFFFFFF` | 2D | the 2D core addresses `TWOD_REQ_BASE` + 256 MiB; Mesa's window-system images and the X front live here, and the 3D core renders into them ([HW-ADDR-005](../hardware.md)) |

Device ranges are never handed to a BO. Everything below `0x20000000` and from
`0xE0000000` stays unmapped.

**Buffer objects (`kernel/gem/poulsback_bo.c`).** A BO is a GEM object on shmem
pages that stay pinned for its life; all BOs of the device together hold at most
256 MiB; it is not a `drm_gem_shmem_object` (its pages, VA mappings and PRIME
ops are its own). The CPU
placement is chosen at creation: write-back pages with snooped SGX PTEs (default), or
write-combined pages with non-snooped PTEs. `POULSBACK_BO_SCANOUT` (only with
`CPU_WC`, GENERAL or 2D class, at most 16 MiB) lets a KMS framebuffer be made of
the BO ([display](display.md#typed-display-lifetime-foundation)); its pages are WC
for its life, so the display needs no cache flush, and the display's claim is a
GEM reference plus a display pin count. Closing a handle waits for the BO's READ
and WRITE fences, not the display's BOOKKEEP fence. A BO gets one VA in each
file that holds handles to it (`drm_gem_object_funcs.open`); the mapping goes
with the last handle in that file (`.close`) or, when a job still holds it,
with that job's release, subject to the HOLD rules above.
[PRIME and CPU access](#buffer-object-prime-and-cpu-access) are
described below.

The `vm-layout` host suite checks ranges, limits, code-bank alignment and
PTE/PDE words. The `vm` host suite checks maps, unmaps and releases, both
healthy and frozen by the loss; see [kernel tests](../../tests/kernel/README.md).

## Memory, locking and lifetime

These rules hold for every object the GPU or the display can reach:

1. Validate handles, sizes and ownership once, where input enters (the ioctl).
2. Release storage only after the GPU and the display are done with it: a job
   holds references on its BOs and its file's address space until it completes,
   and closing a BO handle waits for the BO's jobs; display pins last until the
   display releases them.
3. An unknown GPU outcome enters **HOLD**: the engine is wedged, the job's fence
   completes with `-EIO` so waiters return, and its buffers, scene and address
   space are never released. HOLD provides no retry, reset, unload or release.
   The VM layer freezes at the same moment
   ([the loss](execution.md#the-loss)): every address space that existed then
   keeps every PTE, page table and page it maps, and its directory, through BO
   closes, scene releases and file closes, and its file's `BO_CREATE` returns
   `-EIO`. A BO whose close found it still busy keeps its mapping the same way.

Closing a DRM file destroys its contexts first (their scheduler entities flush
queued jobs), then drops its address space, which running jobs keep alive with
their own references.

## Buffer-object PRIME and CPU access

A BO exports with its own dma-buf ops (DRM's GEM helpers, the BO's `dma_resv`).
Importing such a dma-buf on this device yields the same object, mapped into the
importing file's address space; foreign dma-bufs and gma500's own GEM objects are
refused by the driver's PRIME hooks. CPU access is `mmap` of the GEM fake offset
or of the dma-buf, with the BO's placement (WB or WC). Existing Linux VMAs are not
revoked by HOLD.

## Physical source transaction boundary

SGX context 0 (gma500's default directory) stays empty. Before installing it,
boot checks PCI `8086:8108`, the raw core and revision, and that every directory
slot and the dummy table are empty
([root admission](../../kernel/integration/poulsback_sgx_root_boot.inc)). The
patch deletes gma500's default-directory insert and remove functions, so display
objects and stolen memory are reachable only through the display GTT. The
remaining gma500 MMU entry points (flush, context switch, directory free, driver
init and takedown) run inside the epoch's foreign transaction
([wrappers](../../kernel/integration/poulsback_foreign_mmu.inc)).

The source epoch (`kernel/source/sgx535_source_epoch.c`) binds the SGX at probe
and admits gma500 GEM, GTT and MMU writers through
`sgx535_epoch_external_begin`/`_end`. These guards protect the Linux
integration boundary; client BO mappings use the per-file VM above.
PM, reset, removal or a fault can permanently close the bound source. A
pre-bind loss is ignored because nothing is published yet; the probe's
start-up reset precedes the bind.

The engine's claim blocks PM until reboot in the current lifecycle.
This admission rule does not by itself pin module text or make unload
safe; see the [execution lifecycle boundary](execution.md#p3-engine).
