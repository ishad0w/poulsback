# Complete native raster blitter

<a id="2d-native"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

The [native raster interface](../../docs/native-2d.md) defines the validation
and encoding that `SUBMIT_2D`'s planner uses; the [legacy producer reference](../../docs/re/native-2d.md)
explains source layouts and their unresolved execution boundaries. Neither CPU
encoding nor saved vendor images satisfy the hardware cases below. X11 Render
composition (CPU today, a 3D-core route planned) has a separate
[acceptance guide](2d-x11.md).

The hardware path is `SUBMIT_2D` (`include/uapi/poulsback/poulsback_exec_uapi.h`): the kernel
planner checks surfaces and operations and encodes them for the 2D core. For M1 the families
hold concrete cases in the [case-list metadata](../../spec/dod.json): fixture jobs,
the size and width-8 matrix pinned by count and SHA-256 of its sorted names,
and every `SUBMIT_2D` validation. External matrix/device runners are not bundled
here; their absence leaves those acceptance checks open.
Each job compares every byte of its BOs with an independent CPU model; the
[coverage ledger](README.md#coverage-ledger) counts the results.

Sections: [contract](#2d-native.contract), [pixels and geometry](#2d-native.fill-rop),
[visibility and ordering](#2d-native.visibility), [lifetime and refusal](#2d-native.lifetime)
and [visible movement/stress](#2d-native.motion).

<a id="2d-native.contract"></a>

## Freeze the complete supported native blitter contract

<a id="2d-native.contract-manifest"></a>

### 2d-native.contract-manifest

Procedure: Declare dimensions, alignment, pitch, offsets, clipping, overlap, formats, ROPs and
the `SUBMIT_2D` bounds (BOs, surfaces, operations and pixels per job; stride alignment and
maximum; maximum width and height) before expanding tests.

Oracle: Manifest is independently source-reviewed and every declared limit has boundary cases;
an unsupported mandatory family leaves this DoD open.

<a id="2d-native.fill-rop"></a>

## Accelerated fills for all 16 binary S/D operations

<a id="2d-native.fill-all-rops"></a>

### 2d-native.fill-all-rops

Procedure: For each format/ROP, use constant S and patterned initial D; execute full-surface and
subrectangle fills.

Oracle: Independent per-bit S/D truth table using snapshots taken before the operation; compare
every defined format bit of every destination pixel, unchanged source, padding and all guards.
Do not reuse the driver packet builder as oracle.

<a id="2d-native.copy-rop"></a>

## Accelerated disjoint copies for all 16 binary S/D operations

<a id="2d-native.copy-all-rops"></a>

### 2d-native.copy-all-rops

Procedure: Copy distinct owned BOs with independently patterned S and D for every format/ROP.

Oracle: Independent per-bit S/D truth table using snapshots taken before the operation; compare
every defined format bit of every destination pixel, unchanged source, padding and all guards.
Do not reuse the driver packet builder as oracle.

<a id="2d-native.geometry"></a>

## Dimensions and rectangle boundaries

<a id="2d-native.size-boundaries"></a>

### 2d-native.size-boundaries

Procedure: Exercise widths/heights independently and in asymmetric pairs, including actual
declared maximum dimensions.

Oracle: Independent per-bit S/D truth table using snapshots taken before the operation; compare
every defined format bit of every destination pixel, unchanged source, padding and all guards.
Do not reuse the driver packet builder as oracle.

<a id="2d-native.zero-and-edge"></a>

### 2d-native.zero-and-edge

Procedure: Exercise empty and one-pixel clipped results at each edge/corner.

Oracle: Empty legal work writes no pixels; nonempty intersections match the reference; no
outside-surface write. Mandatory produced output must be complete and authentic; declared
absence is allowed only for the pre-frozen stage where no such output may exist. It never
converts missing required image/plane evidence into PASS for accepted rendering/decode.

<a id="2d-native.dimension-eight"></a>

## Width/height 8 on the ordinary encoding (HW-2D-001: no erratum)

<a id="2d-native.eight-workaround"></a>

### 2d-native.eight-workaround

Procedure: Exercise 8x8,8x1,1x8,8x9,9x8 and clipped results whose effective width or height is
8.

Oracle: Independent per-bit S/D truth table using snapshots taken before the operation; compare
every defined format bit of every destination pixel, unchanged source, padding and all guards.
Do not reuse the driver packet builder as oracle. HW-2D-001 found no width/height-8 erratum;
these sizes use the ordinary encoding, and a refusal or split path for them fails the case.

<a id="2d-native.clipping"></a>

## Clipping and signed coordinate handling

<a id="2d-native.clip-every-side"></a>

### 2d-native.clip-every-side

Procedure: Clip source/destination rectangles separately and together at every edge and corner.

Oracle: Reference intersection preserves the source-to-destination translation; exact clipped
pixels and unchanged remainder; invalid arithmetic rejects before publication.

<a id="2d-native.layout"></a>

## Legal pitches, offsets and guard preservation

<a id="2d-native.pitch-offset"></a>

### 2d-native.pitch-offset

Procedure: Use distinct legal source/destination pitches and offsets, crossing cache lines/pages
and ending at the final legal byte.

Oracle: Independent per-bit S/D truth table using snapshots taken before the operation; compare
every defined format bit of every destination pixel, unchanged source, padding and all guards.
Do not reuse the driver packet builder as oracle. Invalid layouts leave destination and
command-publication count unchanged.

<a id="2d-native.overlap"></a>

## Same-BO overlap in all four directions

<a id="2d-native.overlap-directions"></a>

### 2d-native.overlap-directions

Procedure: Exercise in-place copies left/right/up/down, diagonal overlap, identical rectangles
and nonoverlap within one BO.

Oracle: Use pre-operation S and D snapshots with memmove-style source semantics; exact defined
pixel bits and canaries.

<a id="2d-native.visibility"></a>

## CPU upload, GPU chains and CPU download coherence

<a id="2d-native.upload-download"></a>

### 2d-native.upload-download

Procedure: Map/write CPU data, submit fill/copy, await certified completion and read CPU data;
repeat with new data at the same address.

Oracle: Every read matches the newest submission, never an earlier generation; guards, source
and resource accounting are exact.

<a id="2d-native.gpu-chain"></a>

### 2d-native.gpu-chain

Procedure: Run fill→copy→xor→copy chains without intermediate CPU raster writes.

Oracle: Final and selected intermediate readbacks match independent chain oracle; fences
correlate with actual jobs and data.

<a id="2d-native.queue-fences"></a>

## Reusable jobs, ordering and fences

<a id="2d-native.queue-order"></a>

### 2d-native.queue-order

Procedure: Submit serial and queued operations with dependent and independent BOs, then
wait/read status.

Oracle: Each accepted job has one terminal state; signalled success requires actual retirement
and correct pixels, with no reordered dependency or lost wakeup.

<a id="2d-native.clients"></a>

## Multi-client isolation and sharing

<a id="2d-native.client-isolation"></a>

### 2d-native.client-isolation

Procedure: Run 2 and 4 clients with separate BOs and explicit supported shared-BO imports.

Oracle: No cross-client memory corruption or authority leak; all supported shared dependencies
honor fences and exact pixel results.

<a id="2d-native.lifetime"></a>

## BO/client teardown and resource lifetime

<a id="2d-native.close-under-load"></a>

### 2d-native.close-under-load

Procedure: Close handles/fds, kill userspace processes, unmap and induce allocation pressure at
prepublication, published and retired stages.

Oracle: GPU-used memory is never freed early; confirmed retired work releases resources,
uncertain work enters retained HOLD; successful pixel readbacks remain correct.

<a id="2d-native.rejects"></a>

## Malformed requests and publication boundary safety

<a id="2d-native.bad-requests"></a>

### 2d-native.bad-requests

Procedure: Submit invalid UAPI fields, handles, enums, sizes, coordinates and combinations.

Oracle: Deterministic refusal before publication; no user-supplied MMIO/physical address or
unchecked FIFO words; destination, ownership and existing jobs unchanged. Mandatory produced
output must be complete and authentic; declared absence is allowed only for the pre-frozen stage
where no such output may exist. It never converts missing required image/plane evidence into
PASS for accepted rendering/decode.

<a id="2d-native.motion"></a>

## 120-frame visible 2D motion with every result checked

<a id="2d-native.numbered-motion"></a>

### 2d-native.numbered-motion

Procedure: GPU-render numbered patterned frames with moving/clipped rectangles and scrolling;
show each completed frame on Dell.

Oracle: Every GPU readback matches the frame-specific oracle; IDs are ordered and unique;
presentation copies are separately compared, with no unexplained artifacts.

<a id="2d-native.stress"></a>

## At least 10000 operations and 30-minute hardware stress

<a id="2d-native.long-stress"></a>

### 2d-native.long-stress

Procedure: Run deterministic mixed valid operations after all correctness gates, with repeated
BO reuse and multiple clients.

Oracle: Both minima met; every result correct; no unmatched kernel fault, lost job, memory
corruption or unexplained growth in retained/pinned resources.

<a id="2d-native.hardware-attribution"></a>

## No hidden CPU raster fallback counted as hardware acceleration

<a id="2d-native.path-attribution"></a>

### 2d-native.path-attribution

Procedure: Bind each accepted operation family to reviewed submission code, runtime
job/completion identity and exact resulting pixels.

Oracle: Documented accelerated path actually performs GPU work; eliminated no-op has no CPU
destination write; CPU fallback is reported separately and does not satisfy acceleration
coverage.
