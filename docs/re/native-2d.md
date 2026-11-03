# Native 2D: requests, packet words and observed output

The retained native-blitter implementation supplies a useful packet contract
for fill, copy, ROP, clipping and overlap. It is a separate engine path from
textured SGX 3D composition (EMGD's Render transfer queue, below). A native
transfer accepted by its queue is not yet a completed operation or permission
to release its storage.

This chapter describes selected legacy EMGD and PSB implementations and retained
X11 observations. It does not report a new poulsback hardware test. See
[source families](sources.md) and [address domains](address-domains.md) before
applying an address or combining records from different branches.

Reading routes: [producers](#selected-producers), [packet construction](#request-to-transfer-envelope),
[captured X11 routing](#captured-x11-routing), [PSB EXA selection](#psb-exa-route-selection),
[width eight](#the-width-eight-workaround-is-not-a-recovered-solution) and
[execution/lifetime](#remaining-execution-and-lifetime-boundaries).

## Selected producers

Linux EMGD 1.8 uses `libEMGD2d.so`, 31,056 bytes, SHA-256
`71d6577ff8140cd9598bbcafcec00d9930bca70efc7fb4cf7accd8368e2719ac`.

| Function | Analysis address | ELF VA / file offset | Established scope |
| --- | --- | --- | --- |
| Main blit producer | `0x130b0` | `0x30b0` | A 6,153-byte noncontiguous body validates the selected request, clips coordinates, constructs native surface/blit words and calls `SGXQueueTransfer`. |
| `PVR2DBltClipped` | `0x14960` | `0x4960` | Rejects a null request and forwards arguments to the producer. |

These analysis addresses have a load bias of `0x10000`. The executable ELF
`PT_LOAD` maps the selected ELF VAs to equal file offsets; the writable segment
does not generally have that property. Full body/value-flow coverage of these
two functions does not establish every callee or make an inferred register-call
signature a public ABI.

The PSB comparison is the selected patched `psb_accel.c`, 36,017 bytes, SHA-256
`6efd9506bd6a5b9ce2f51e7cc2424c71c58cfccaa3f650abc90212255abc5f09`.
Its relevant functions are `psbAccelSuperCopyHelper`, `psbExaPrepareSuperCopy`
and `psbExaSuperCopy`. Package, source-root and DDX-reported versions are
separate identities; their differing version strings do not name one measured
binary interchangeably.

## Request to transfer envelope

The EMGD producer performs the following transformation:

1. Reject missing storage, invalid positive geometry and unsupported clip
   shape; determine whether the ROP consumes source or pattern.
2. Clip source and destination together. The legacy function changes request
   coordinates, so its mutation behavior must be kept separate from a copied,
   immutable submission interface.
3. Emit format, pitch and address words. For overlapping copies within one
   owner, select traversal order and move reverse starts to the correct corner.
4. Emit source coordinates, blit control, control-selected payload, destination
   coordinates and extent.
5. Place the DWORD count and words in a type-2 transfer envelope with source
   and destination synchronization owners, then call `SGXQueueTransfer`.

Useful analysis addresses in the main body are `0x13760` for pitch masking,
`0x13799` and `0x1433f` for address masking, `0x1480e–0x148ac` for overlap and
corner selection, and `0x13b17` for the queue call.

| Field or branch | Recovered rule | Constraint |
| --- | --- | --- |
| Coordinates and extent | Selected words use 12-bit components; pitch is masked with `0x7fff`. | Validate representability and complete memory ranges before encoding. Truncation is not validation. |
| Reverse copy | Order bits 23–24 accompany `width−1` / `height−1` corner adjustments. | Direction and start coordinates form one contract; establish aliasing from owner identity. |
| ROP and payload length | The EMGD branch can retain `USE_PAT=0x10000` for pattern-independent operations and omit the fill DWORD. The selected PSB copy sets `USE_FILL=0` and emits `0xffffffff` explicitly. | These are two legitimate producer layouts. A word cannot be removed from one because the other omits it. |
| Clipping | The legacy request is changed in place. | An independent implementation needs its own checked projection of the whole request. |
| Queue result | Nonzero errors become PVR2D errors; zero reports local queue success. | The matched submitter discards a later 2D bridge error, so zero does not establish bridge admission, completion, cache visibility or last resource use. |

EMGD's address mask permits alignment different from a more conservative
consumer. That alone cannot relax ownership, alignment or whole-span checks.
The producer also has color-key, mask, pattern, rotation and blend branches;
their presence does not supply a complete qualified contract for those features.

The recovered chain ends at the queue call. A complete type-2 envelope →
firmware → register-store → final memory-use chain remains unresolved. Kernel
entry observations during the X11 batch corroborate activity without joining
every packet argument to an operation and its completion.

## Captured X11 routing

The captured [EMGD 1.8 stack](sources.md#linux-emgd-18) exposes Xorg's
engine calls and native command words per operation. These are vendor
mechanisms; they do not qualify poulsback's implementation. Full call and
command records remain outside the public
[evidence releases](evidence.md#what-remains-unavailable).

Each captured SUBMIT2D carries one rectangle in one 256-byte slot. Word 3 is
the DWORD count; command words follow and end with the queue's appended FLUSH.
EMGD does not batch multiple rectangles in these phases.

| Shape | Command contract | Observed use |
| --- | --- | --- |
| Fill | Destination surface, PATCOPY with USE_FILL and colour, XY, extent. | Solids, polygon spans, clears and Render fills. |
| Copy | Destination/source surfaces, source offset, SRCCOPY with USE_PAT, XY and extent; COPYORDER for overlap. | Window/pixmap copies, scrolling, Render Src and GL swaps. |
| Clipped copy | Copy plus CLIP block and CLIP_ENABLE. | Selected rendercheck copies. |
| Over | Destination, alpha CTRL, source, source offset, clip and SRCCOPY with ALPHA_ENABLE. | Premultiplied unmasked Over into pixmaps. |

These X phases use GXcopy, not every supported ROP. No pattern surface, mask
surface, colour key or rotation was observed. Lines, text, Put/GetImage,
copyplane, trapezoids, transforms and gradients mostly use the CPU, sometimes
with a final GPU copy. During GL phases the server's engine work is one native
2D copy per client frame; no Xorg TA kick is observed
([vertex capture](emgd-linux/vertex-path.md#limits)).

Render also uses a separate 3D transfer queue. The selected call shape has
one source sync, one destination sync and a fixed 0x440-byte CCB step. Its
attributed operations are constant-mask Over and format conversion; this shape
does not establish chained intermediates or a general composition interface.
Real texture masks, component alpha, Add, wrapping repeats and A8 destinations
stay off this transfer path in the tested set.

Transfer command windows and programs are not available in this corpus.
The shape and routing follow bridge records and rendercheck request order;
precise blend-program modes remain inferred. A native
2D Over command does not establish arbitrary Render coverage on that core.
Performance measurements do not supply missing transfer instructions or extend
this command-contract scope.

Equal X11 output does not identify which GPU engine or CPU path produced it.
Command attribution, matching completion and final resource use remain
independent of image equality. Depth-24 XImage readback also leaves its unused
upper byte unqualified as alpha.

## PSB EXA route selection

In the selected Lucid PSB DDX, flags `0x49` select driver-managed mixed pixmaps,
but the selected `CreatePixmap2` returns `NULL`. Ordinary new pixmaps therefore
cannot acquire driver storage on that route. The `GXcopy` solid-fill branch
refuses acceleration, and upload uses CPU `memcpy`.

`PrepareCopy` tests a threshold of 256 pixels of total pixmap area, not a
256-pixel minimum width. Enlarging a 64×64 test cannot repair the allocator
route. These restrictions describe that DDX build, not SGX535 silicon.
These are matched source/binary constraints. Failure to observe one selected
entry point does not prove that every frame used the CPU or that another GPU
route was inactive.

The separate [Xpsb composite producer](../native-3d.md#legacy-geometry-producers)
uses prepacked quads and a raster-only branch instead of this native-blit packet.
Likewise, Intel's [Windows EMGD feature matrix](https://www.intel.com/content/dam/www/public/us/en/documents/drivers/emgd-v1-16-feature-matrix.pdf)
declares GDI acceleration without identifying each operation's SGX route.
A public API name, correct XImage or transfer-queue call cannot assign every
operation to the native blitter. Poulsback's DDX uses EXA: fills and copies run
on the 2D core and Render composition stays on the CPU (fb). A glamor or
3D-core Render route is not implemented
([graphics](../graphics.md#real-glamor-startup-requirements)).

## The width-eight workaround is not a recovered solution

The selected PSB source contains a disabled `PSB_FIX_BUG_W8` split path. Its
reverse-X second chunk moves by `+4` after selecting the reverse start. An
independent 8×9 copy model from `(4,4)` to `(6,4)` produces 72 wrong pixels,
including writes outside the intended destination.

That counterexample rejects copying the workaround; it does not identify the
silicon erratum or establish a replacement. No width-eight special case was
found in the selected EMGD function body, but its downstream path and other
hardware profiles were not exhaustively analyzed. The size-eight boundary
therefore needed its own qualified contract: the Dell's dimension/edge matrix
([HW-2D-001](../hardware.md#hw-2d-001--rectangle-sizes)), covering GXcopy and
GXxor fills and copies, found widths and heights of eight exact; poulsback uses
no workaround.

## Requester base is not a display offset

The selected PSB, EMGD and Linux register headers place the native-2D requester
at SGX MMIO **byte offset `0x0c88`**. EMGD selects mask `0xfff00000` and shift
20 for its base field. Poulsback's engine reads this register at start and
refuses to run unless it holds the GATT start gma500 wrote, `0xD0000000`; its 2D
addresses are offsets in the 256-MiB window above that base. `0x0c80` is a
different register.

In PSB, `psb_init_2d` writes `pg->gatt_start` and retains the same byte base as
`mmu_2d_offset`. `psb_apply_reloc` forms a packet offset from BO SGX VA plus
byte preaddition minus that base, with a 256-MiB representable domain.
`psb_blit_sequence` uses the same subtraction for its communication page.
A display GTT offset, host PFN or similarly named software field cannot stand
in for this requester binding. The selected EMGD heap is another source profile.

The current owned consumer
has separate source, snapshot, visibility, fence and retirement obligations.

## Remaining execution and lifetime boundaries

The basic word layouts can inform independent encoders and software oracles.
The captured commands attribute selected X operations to engine streams. They do
not by themselves join every operation to completion, device-write visibility,
final resource use, serial/multi-client ordering or failure/teardown. Format
and overlap coverage need guarded boundaries; the size-eight result above
belongs to the separate poulsback HW-2D-001 matrix. Current execution and shared
storage contracts belong to [native 2D](../native-2d.md) and
[kernel execution](../kernel/execution.md).

Common memory infrastructure can be shared with SGX work while retaining each
engine's own command and completion semantics. The current implementation
interface is described separately in [native 2D](../native-2d.md).
