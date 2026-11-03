# Native 2D command construction

The raster library validates surfaces and rectangles and builds numeric FIFO words on the CPU.
It does not map memory, submit commands or wait for the GPU. The kernel owns those responsibilities.
A successful CPU encoding does not qualify that operation on hardware.

## General raster interface

`src/raster/sgx535_2d_encoder.h` is the shared contract for the MIT
implementation. The [vector corpus](../tests/vectors/2d-encoder/README.md)
defines its compatibility checks. The implementation handles all 16 X11 GX
logical operations in ARGB8888, XRGB8888 and RGB565. Fill takes a packed native-format pixel. Copy
requires matching formats and equal rectangle dimensions. XRGB's input top byte is ignored and
its unused output top byte has no defined preservation or value guarantee. This interface has
no alpha blend, planemask, scale, rotation, format conversion or tiled surface support.

Surfaces carry an explicit backing-object identity, GPU byte offset, accessible allocation extent,
dimensions, pitch and format. Identities must come from a trusted owner registry: equal IDs mean
exactly the same allocation and layout; different IDs mean nonaliasing physical storage. This CPU
validator cannot authenticate an identity supplied by an untrusted client.

Dimensions are 1–4095, bases are aligned to eight bytes, and pitch is a
positive multiple of 128 bytes up to 16256. The core reads its 15-bit pitch
field as signed ([HW-2D-002](hardware.md)), so a 32-bit surface is at most
4064 pixels wide. The 256 MiB address-field domain is distinct from a
CPU-visible BAR aperture or available RAM. Final-row padding need not be allocated.
All byte-span calculations use checked geometry and wide arithmetic.

## Clipping and overlap

Rectangle origins are signed and extents unsigned. The builder clips against destination bounds and
an optional destination clip. Copy also clips against source bounds while preserving the displacement
between source and destination. A successful empty intersection emits no command and sets only
`written = 0`. Other errors leave both output storage and `written` unchanged.

Same-storage copy is accepted only for identical proven views. Four copy orders preserve the
pre-copy source pixels for overlapping rectangles. Different layouts of one backing object are rejected,
even when the visible pixels appear disjoint. Different backing objects cannot have overlapping GPU
allocation ranges.

Copies of every clipped width and height are encoded alike, eight included;
no width-eight workaround is required ([HW-2D-001](hardware.md)). The
[native 2D checklist](checklist.md) tracks acceptance separately from the
encoder's supported dimensions.

## Compatibility packet interface

`sgx535_2d_encoder_v1.h` retains a CPU serialization interface for linear
ARGB8888, nonempty unsigned rectangles, forward COPY and conservatively
disjoint touched-byte intervals. It is not a second kernel submission path;
`SUBMIT_2D` uses the general raster encoder above.

## Kernel submission

`SUBMIT_2D` ([execution](kernel/execution.md)) is the kernel path: its planner checks the
listed BOs, surfaces and operations in the ioctl and encodes them with this library, and the
engine feeds the words to the 2D core. X's EXA fills and copies, Present's copies and Mesa's
`pipe->blit` use it ([graphics](graphics.md)).

## Encoded word layout

The general encoder emits 32-bit slave-port words. A nonempty fill is eight
words; a copy is eleven. Addresses below are byte offsets relative to
`TWOD_REQ_BASE`, not physical addresses. Positions and sizes pack the first
coordinate in bits 23:12 and the second in bits 11:0. Size fields contain the
actual width and height, without subtracting one.

A surface control word is its command head, format in bits 18:15 and stride
in bits 14:0. Format codes are 12 for ARGB8888, 11 for XRGB8888 and 10 for
RGB565. The shared heads are FENCE `0x70000000`, destination `0xa0000000`,
source `0x90000000`, source position `0x30000000`, BLIT `0x80000000` and
FLUSH `0xf0000000`.

| Word | Fill | Copy |
| ---: | --- | --- |
| 0 | FENCE | FENCE |
| 1 | Destination control | Destination control |
| 2 | Destination offset | Destination offset |
| 3 | BLIT control | Source control |
| 4 | Expanded fill colour | Source offset |
| 5 | Destination position | Source-position head plus position |
| 6 | Width and height | BLIT control |
| 7 | FLUSH | `0xffffffff` |
| 8 | — | Destination position |
| 9 | — | Width and height |
| 10 | — | FLUSH |

BLIT control contains the ROP3 byte in both bits 7:0 and 15:8 and copy order
in bits 24:23. The general encoder uses the explicit colour word; it does
not set USE_PAT. Fill uses the pattern input to the ROP3 truth table; copy
uses the source input and ignores the supplied all-ones colour word.

The 16 GX functions use X11 protocol order, from clear (0) through set (15).
For a ROP3 entry indexed by pattern/source/destination bits `P/S/D`, evaluate
the selected GX function on `P,D` for fill or `S,D` for copy. The result is
that entry's output bit. Thus GXcopy encodes ROP3 `0xf0` for fill and `0xcc`
for copy; GXxor encodes `0x5a` and `0x66`. This defines every code directly
from the binary GX truth table.

Copy order 0 scans forward, 1 reverses both axes, 2 reverses X and 3 reverses
Y. For a reversed axis, both source and destination starts move to the last
pixel on that axis; the extent stays unchanged. Different-storage copies
use order 0. Same-storage copies reverse an axis when the source start is
less than the destination start, including after joint clipping.

### Fill colour conversion

The FIFO colour word is A8R8G8B8 for every destination format. ARGB8888 uses
the input unchanged; XRGB8888 sets alpha to `0xff`. RGB565 input must fit in
16 bits. Its R5/G6/B5 fields are widened to eight bits by shifting left and
filling the vacant low bits with the original field's least-significant bit;
alpha is `0xff`. This specific conversion is pinned by
[`rgb565-fill-colour.bin`](../tests/vectors/2d-encoder/rgb565-fill-colour.bin).

### Validation order and output guarantees

Callers provide nonoverlapping input structures, output storage and count
storage. With that precondition, refusals preserve both `out` and `written`.
An empty clipped rectangle succeeds with `written = 0`, even with insufficient
packet capacity; the required pointers and surfaces are still validated.

Validation order determines the first reported failure:

1. Check required pointers, then the GX value (0–15).
2. Check the source surface for copy, then destination: nonzero storage ID,
   known format, numeric bounds, alignment, row extent and allocation extent.
3. For fill, reject an RGB565 pixel above `0xffff`. For copy, require equal
   formats and rectangle extents, then prove storage/view alias rules.
4. Clip jointly to surface bounds and the optional destination clip.
5. Return the empty success, or require capacity for all eight/eleven words.
6. Emit the complete packet and set `written`.

Surface dimensions must be 1–4095; stride must be 1–16383 before its
128-byte alignment check. The allocation must fit wholly in the 256-MiB
offset domain and cover `(height - 1) * stride + width * bytes_per_pixel`.
The public status distinguishes null arguments, unsupported operations,
numeric range, alignment, unproven aliasing and insufficient capacity. See
the [header](../src/raster/sgx535_2d_encoder.h) for enum values and the
[vector grammar](../tests/vectors/2d-encoder/README.md) for exact refusal cases.

## Legacy command and X11 contracts

The [legacy native-2D reference](re/native-2d.md) owns the matched EMGD/PSB
producer chains and copy-order fields. Its
[PSB EXA selection](re/native-2d.md#psb-exa-route-selection) explains the
allocator, fill, upload and area-threshold branches. Vendor source policies
do not relax the current bounds, trusted-owner or alias checks.

Packet length depends on the actual command variant. The selected PSB producer
and the general poulsback raster encoder use `USE_FILL` and emit a fill word
(`0xffffffff` for source copy). The EMGD producer can instead use `USE_PAT`
(`0x10000`) for pattern-independent ROPs and omit that word, as the narrower
compatibility encoder does. The boolean result can agree while packet lengths
differ; never strip or insert a word without decoding this control bit.

Render composition through the 3D core and full native-blitter coverage remain
separate [graphics contracts](graphics.md); the current DDX uses EXA rather
than glamor. The
[X11 acceptance route](../tests/acceptance/2d-x11.md) does not require completing
the independent native-blitter operation matrix first.

## Verification

See [native 2D tests](tests.md#native-2d) for packet/pixel oracles, error
injection, hardware case expansion and presentation checks.
