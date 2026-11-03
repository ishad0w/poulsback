# Native SGX535 command construction

`src/raster` constructs CPU-side SGX535 plans, geometry, command streams,
PDS/USE tasks, ISP state and surface descriptors. Mesa's
[frame writer](graphics.md#frame-writer) uses these builders to prepare BOs;
the [kernel engine](kernel/execution.md#p3-engine) validates submitted register
entries, owns DPM/scene storage and executes TA/raster work. Shaders and complete
command streams are generated in userspace.

Construction, upload, execution and retirement are separate operations.
Typed addresses describe GPU virtual addresses, rather than physical-page or
ownership proof. [Hardware](hardware.md) owns cross-component hardware contracts;
[compiler](compiler.md) owns shader lowering and codecs.

## Legacy submission contracts

EMGD's firmware scheduler and poulsback's host-driven engine use different
submission models. Recovered fields explain the silicon; an EMGD queue or
cleanup return is not poulsback's execution/retirement contract.

| Legacy object | Meaning and boundary |
| --- | --- |
| TA bridge packet | The userspace entry snapshot precedes kernel patching of READY, dependencies and sync/status fields, then firmware-queue publication. Slot/reservation size does not establish the complete device fetch extent. |
| Firmware service selector | EMGD's initializer converts HOSTKICK byte offsets by `>>3` (TA `0x12a0 → 0x254`, 2D `0x10f8 → 0x21f`). These are service offsets, not shader addresses or `VDM_START`. |
| ZLS address split | EMGD uses a 1-MiB-aligned base plus the subtracted offset; the selected PSB path uses a coarse `0xf0000000` base and low address bits. Each base must stay paired with its producer's offset rule. |
| Cleanup/sync words | CPU producers and bypassed or ignored cleanup handshakes prevent treating a successful API/free callback as proof of final device use. |
| Windows transfer payload | The selected retail payload, firmware-ring command and native TA stream are different objects. Similar kick offsets do not establish a compatible firmware ABI. |

[TA submission](re/emgd-linux/ta-submission.md),
[sync/release](re/emgd-linux/sync-cleanup-and-release.md),
[address domains](re/address-domains.md) and the
[Windows bootstrap contract](re/windows/retail-bootstrap-and-retirement.md)
retain the producer/consumer detail.

### Legacy geometry producers

The selected PSB DRI path uploads VBO bytes unchanged and has both pass-through
and conditional hardware-VS producers. A byte copy alone does not transform
coordinates. Its pass-through PDS contains a bounded DMA task and an index-list
record with 16-bit indices. Reservation bytes and actual data/code bytes remain
separate quantities.

The Xpsb composite path instead packs four vertices per quad and a prebuilt
raster-only parameter route. It biases XY by 1024, scales by 16, adds 0.5 and
converts with temporary x87 truncation mode; ambient rounding still affects the
earlier arithmetic. Its adjacent `1.0` word does not identify a general Z/W
format. Neither path alone defines OpenGL clipping, viewport or depth rules.

The [PSB producer contract](re/psb/pds-use-and-depth.md),
[EMGD vertex path](re/emgd-linux/vertex-path.md) and
[raster conventions](re/sgx535/raster-conventions.md) preserve these distinctions.

## Engine plans

`sgx535_engine121` constructs bootstrap, DPM, TA and raster actions for raw core
ID `0x01130000`, revision `0x00010201`. Other identities refuse. Scene dimensions
are 1–2047 per axis. The scene builder computes region/root storage; storage
identity and address arithmetic do not establish mappings.

The bootstrap contains 17 actions: twelve writes and five ordered readbacks.
READBACK is an observation, not a wait, readiness result or visibility proof.
DPM initialization has 29 actions and requires all four load events including
DHOST before acknowledgement and confirmation of clearing. The TA plan has
32 actions; the resident raster plan has 20 and keeps context 0 loaded. Counts
come from the header, rather than a separate execution profile.

The engine supplies MMU state, IRQ delivery, cache ordering, timeout checks
and authenticated owners around these plans. DPM parameters occupy the fixed
`0x30000000` window; tables, scene and per-file program mappings have separate
roles. The current VA layout and ownership belong to
[buffer objects](kernel/memory.md#buffer-objects-and-per-file-address-spaces).
`sgx535_heap_layout` validates fixed construction extents; it does not
allocate the engine's per-file virtual addresses.

### Partial-render construction

`sgx535_spm` and `sgx535_spm_state` implement the host-tested partial-render
program/state model: TE step, ABORTALL, state handoff, partial or whole-scene
render, RESTART and final render. The model tracks both DPM context owners and
holds on event sequences that do not fit. These builders remain unwired to the
product engine; the current engine's single reserve resume and subsequent HOLD
policy remain in force.

[The decoded EMGD program](re/firmware/partial-render.md) owns the recovered
sequence. A host model with several pools of geometry does not establish GPU
partial rendering, concurrent context safety or physical memory retirement.

## CPU-transformed coordinates

`sgx535_clip_w1` and `sgx535_window_vertex` provide bounded integer-only
construction/transport helpers. Clip XYZW and window XYZ/reciprocal-W are
different representations. W=+1 helpers never justify discarding arbitrary W,
applying a second viewport or substituting a captured window coordinate.

The current Mesa fallback collects VS clip-space outputs with CPU viewport,
clipping and culling bypassed; MTE performs those operations. GPU-shaded draws
supply clip-space POSITION directly. The native record carries viewport
translate/scale, WCLAMPEN, W clamp `1e-7` and cull mode with PRETRANSFORM clear.
Clipping and culling tests must distinguish coverage, interpolation and depth
errors. WCLAMPEN effects and numerical edge cases need their own checks; a
correct image alone does not identify the responsible operation. See
[hardware contracts](hardware.md#varyings-fragment-programs-and-depth).

Window and FBO orientation must remain explicit. The window uses a negative-Y
viewport; backend draws can supply the positive-Y viewport used by an FBO and
its depth range. This source path, front/back handling and general
render-to-texture are not qualified by window tests.

## Task images and launch contracts

The raster libraries describe complete CPU images: section bytes and extents,
resource-relative relocations, PDS/USE task links, MTE/ISP state, event/PBE words
and TA/raster register requirements. Builders preserve outputs on failure as
specified by their headers. Large workspaces belong off the i386 kernel stack.

The current product places programs and state in BOs and submits a BO list plus
validated register-table values. Native shader code is Mesa-generated data in
those BOs; there is no symbolic-program creation ioctl or kernel packet-version
selector. The kernel does not regenerate/compare an entire userspace scene.
A constructor's qualification fields describe its library contract. Engine
UAPI admission is a separate check.

| Construction family | Present role |
| --- | --- |
| `sgx535_task_packet`, compiled/normalized COLOR0 and background families | Bounded CPU packet/launch construction with typed resource spans and relocation checks. |
| `sgx535_window_mesh`, depth-window and ordered single-sample families | Frame/template seeds and independent word oracles for the Mesa writer. Fixed WINDOW/W=1 profiles and CLIP variants retain different input contracts. |
| `sgx535_pixel_pds`, `sgx535_mte_state`, `sgx535_isp_words` | Current backend pixel dispatch and per-draw state construction. |
| `sgx535_pixel_event`, `sgx535_pbe_state` | Event programs and surface output words used by frame construction. |
| `sgx535_depth_state`, `sgx535_depth_clear`, `sgx535_zls` | Typed depth extent, clear geometry and ZLS word construction. |
| `sgx535_texture_layout`, `sgx535_texture_words` | Texture storage and descriptors; the bounded 2D sampling path uses them ([Graphics](graphics.md#surface-and-sampler-ownership)). |

The lower-level constructors still have narrowly versioned C structures. Those
versions are library layout contracts, not supported OpenGL profiles. Their
headers define fixed target sizes, instruction budgets, aliases and relocation
bounds. The ordinary frame path is described below and in
[Graphics](graphics.md#engine-frames).

### Code and address binding

USE code extents fit a 512-KiB code bank; task code start and entry share one
32-KiB block, and the entry is 16-byte aligned. Relocation binds typed owner
extents before emitting address fields. The TA stream register consumes a full
byte address; PDS/USE entries have their own shifted/base-relative units.
Packet address widths must not be used to infer register address units.

The selected four-byte PBE surface form has a ten-bit stride field. Pitch is
128-byte aligned and at most 8192 bytes; a larger pitch overlaps the layout
field and refuses. Surface format, memory layout, packed register byte order
and colour input order remain separate contracts.

### Normalized COLOR0 task and launch

The retained normalized task/list/window families combine checked scalar IR,
geometry, background/mask and event programs. Some use W=+1 and fixed small
targets; others retain WINDOW XYZ/reciprocal-W or clip-space tuples. A successful
CPU construction establishes its words and bounds only. Submission requires
the engine's BO, register, address-space and ownership contracts.

The Mesa writer uses the ordered constructors once per template and relocates
the resulting words into its arenas. Subsequent draws write only per-draw data;
the product does not rebuild and re-prove the complete scene for every frame.

## Current draw state

### TA state records

`sgx535_mte_state` represents a header bitmask and groups in bit order: ISPA/B/C,
back-face ISPA/B/C, pixel SECONDARY/DMS/PDS, region clip, viewport, wrap, output
selection, W clamp, MTE control, terminate, texture sizes and float flags.
`sgx535_mte_record` serializes selected groups; `sgx535_mte_changed` computes
which present groups differ. A state task DMAs the record into attributes and
runs a copy-and-emit USE program. A TA STATE entry names that PDS program.
The serializer can describe all groups, while the current state task accepts
at most 16 DWORDs in one record.

The frame writer's backend draws add their own ISP and pixel-PDS groups after
the COLOR0 template's STATE. Program and uniform changes therefore do not need
new COLOR0 geometry templates. Equal program/iteration/ISP/constant keys can
reuse an earlier state task in the frame.

### Pixel PDS and ISP words

`sgx535_pixel_pds` emits DOUTU and ordered DOUTI issues, then HALT. The USE task
contains the checked code link, temporary count, iterator wait and optional
phase-2 entry. The builder marks the last USE iteration; an input list and its
PA extent must agree. `sgx535_pixel_pds_dms` constructs the task's allocation
word from primary, temporary and SECONDARY occupancy. Supporting a two-phase
encoding in the builder does not make that path executable in the current Mesa
context.

`sgx535_isp_words` derives depth comparison/write, stencil front/back operations,
face orientation, cull mode, polygon offset and pass-type/colour state. GL
compare/stencil enums are translated explicitly; Gallium's wrap/invert ordering
is not the ISP's. The backend path admits all eight depth functions,
Z24S8 stencil and two-sided stencil, and whole polygon factor/units in −16…15
without clamp. The COLOR0 path accepts LESS/write or no-depth restrictions.
Both-face culling drops the draw.

Stencil is initialized by a full-target clear object before the frame's first
stencil test: ALWAYS/REPLACE, no colour and no depth write. The background's
depth clear does not reliably initialize stencil per tile. An arbitrary stencil
clear value is supported, while a partial stencil mask refuses. A clear without
stencil can keep its previous clear value only while no draw has written it.

## Typed depth preparation and clear prefix

`sgx535_depth_state` computes D16/D24S8 requirements with pitch and height padded
to 32 elements; colour storage cannot substitute for depth. ZLS address
components reconstruct the complete aligned address under their matched
base/offset rule. The current Mesa GPU allocation uses one D24S8 physical layout
for logical Z24X8 or Z24S8. D16 metadata alone is not a working D16 render path.

`sgx535_depth_clear` builds fullscreen geometry at depth one. The depth-window
composition combines mask, clear, geometry and termination/event programs.
These are typed construction helpers, not evidence of every depth load/store,
write-containment or numerical conversion behavior. Preserved depth across
flushes/targets, general readback and depth textures need their own integration
and tests. [Depth/ZLS research](re/sgx535/depth-contract.md) describes ZLS
addressing, storage, conversion and numerical limits.

## Texture memory

`sgx535_texture_layout` and `sgx535_texture_words` implement host-tested storage
and descriptors derived from EMGD's captured textures:

- POT 2D/cube/3D data is twiddled with Y in the lowest bit; an axis drops out
  when its bits run out. DXT twiddles 4×4 blocks.
- NPOT 2D and depth data uses stride surfaces with rows padded to 32 texels.
  Mip levels follow to 1×1; cube faces follow GL order. RGBA16F/32F uses two/four
  planes. The mipmapped cube face step and non-cubic 3D twiddle are inferred,
  rather than captured.
- Upload/readback converts the declared format to/from B8G8R8A8, A4R4G4B4,
  A1R5G5B5, R5G6B5, U8/U88/U16 or F32. Sixteen-bit colour expands by bit
  replication. Border-area layout is explicit.
- DOUTT0–2 combine stored layout, view levels/plane, GPU address, wrap/filter,
  mip/LOD, anisotropy and sRGB choices. Unsupported cube rebasing, multi-level
  stride surfaces and border/wrap combinations refuse as stated in the header.
  GL_CLAMP and the LOD clamp follow GL where EMGD departs from it.

These helpers hold no BOs or Gallium resource ownership. The Gallium integration
connects a narrower single-level 2D B8G8R8A8/B8G8R8X8 stride-texture path and
GPU target-load draws. [Graphics](graphics.md#surface-and-sampler-ownership) owns
its BO retention, filtering/wrapping and framebuffer restrictions. The library's
other formats/layouts remain outside that integration; a correct descriptor or
upload round trip does not qualify GPU sampling or general render-to-texture.
[Recovered texture/fragment state](re/emgd-linux/fragment-path.md#textures)
provides the evidence boundary.

## Verification

Host suites check field encodings, complete bytes/relocations, rejected spans,
transactional output and independent instruction/state models. Mesa frame
fixtures compare COLOR0 template words after relocation and follow backend
state/constants from the TA stream. [Hardware](hardware.md#varyings-fragment-programs-and-depth)
owns hardware contracts; [Tests](tests.md#graphics-apis) owns runnable checks
and result scopes.
