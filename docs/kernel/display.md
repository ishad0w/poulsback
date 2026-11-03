# Kernel display lifetime

[Kernel overview](../kernel.md) · [Execution](execution.md) · [Memory](memory.md) ·
[Display](display.md) · [Diagnostics](diagnostics.md)

- [Typed display lifetime foundation](#typed-display-lifetime-foundation)
- [Static source and cache custody](#static-source-and-cache-custody)
- [Firmware and display handoff](#firmware-and-display-handoff)
- [Display identity](#display-identity)

The current typed display path switches buffers in the inherited panel mode.
On the Dell this is pipe B at 1280×800 with a 5120-byte pitch. It does not
yet implement typed full modesets, front-buffer resizing or multi-output
operation; VT handoff with our DDX is not qualified. DPMS switches the panel through its encoder while
the pipe keeps running. VGA testing is deferred because the reference setup
has no external monitor; only `PIPECONF` may be read on a disabled pipe.
The [X11 status](../status/x11.md) owns the remaining desktop limits.

## Typed display lifetime foundation

KMS framebuffers of this driver are made of `SCANOUT` buffer objects
([memory](memory.md#buffer-objects-and-per-file-address-spaces)): write-combined
for the BO's life, in the GENERAL or 2D class, at most 16 MiB. `fb_create` sends a
`SCANOUT` BO to `poulsback_framebuffer` and refuses every other BO; the cursor
and the legacy guards refuse BOs, so a BO never reaches gma500's
`psb_gem_object` casts. The ADDFB2 command is the layout: one LINEAR XRGB8888 or
ARGB8888 plane at offset 0, a pitch that is a multiple of 64 and holds a row,
`pitch × height` within the BO, zero flags and unused planes (host-checked in the
`vm-layout` suite).

Display preparation uses a trusted provider and separate GTT reservations. It
checks the complete reservation tree and revalidates after sleeping callbacks.
SGX VAs are never reused as display GTT addresses. The display's claim on a BO is
a GEM reference plus a display pin count (`poulsback_bo_display_pin`); a shown BO
may still be mapped, exported or rendered to. Preparation adds a BOOKKEEP
`dma_resv` fence, which stays unsignalled while the mapping may be fetched:
GPU writes into a shown buffer do not wait for it, and closing a BO handle waits
for READ and WRITE fences only. Before every show the caller waits (interruptible,
at most 2 s) for the BO's WRITE fences; a failed writer refuses the show, except
one lost with the GPU (`-EIO`, [the loss](execution.md#the-loss)): its pages are
pinned and readable, so after the loss the display shows what they hold.

The PSB provider is a separate C component. Its Linux integration leaf identifies
legacy framebuffers without exposing the private framebuffer function table.
Display-domain retention transitions belong to the display owner under its lock;
the provider retains its own switch and error reports.

The `poulsback_display_gtt` binding checks the BO's pages (lowmem, below 4 GiB,
WC kernel mapping), the retained legacy scratch page's UC-minus CPU alias (the PSB
constructor's `set_pages_uc()` contract), source identity and the reserved
aperture range. Its cached entries distinguish a resource byte address, an
aperture-relative offset and a PTE slot; image words are page address | 9 (valid,
cached) and empty words point to the scratch page. The binding retains the BO and
scratch references across possible publication or HOLD.

A binding is checked whole (every page) when it is prepared and at the flush
after its last stored or withdrawn PTE. A buffer switch checks the retained
binding's identity and its first and last GTT words, under locks that exclude
page changes. A single PTE store checks its own page and the word it replaces; the
provider identity check that runs after every page checks each binding's
ownership only (`poulsback_display_gtt_publication_owned`).

The GMA500 reservation adapter reserves one 32 MiB non-stolen window, 256 KiB
aligned, in the legacy `gtt_mem` resource tree and confines the display
suballocator to it. Each buffer's GTT offset is 256 KiB aligned (EMGD's
async-flip rule), so eight 1280×800 buffers fit: the front and a fullscreen
client's three back buffers, with room for replaced ones still retiring. Source/map/scratch identity and
capacity are rechecked after sleep; partial failure retains the reachable owner.

`poulsback_framebuffer` owns the DRM metadata and embeds the typed display owner.
The final DRM put removes the base from DRM registries; a mapped framebuffer then
retires in a work item that takes every modeset lock (as `set_config` does) and
polls a not-yet-certified retirement for up to 200 ms. Uncertain or failed
retirement retains the complete owner and its GEM/device/module references. Legacy
set-config, flip, base, DPMS, disable, PSB mode-setting and CRTC restore callbacks
reject typed framebuffers before power, register, pin or event changes.

## Static source and cache custody

The selected PSB source owner authenticates the current legacy framebuffer,
CRTC, pipe, device, GTT geometry and register snapshot. Its two bounded pipe
slots retain the original DRM, framebuffer and module references before sleeping
reservation or active-only power operations. A separate resident GEM pin owns
an extra count for either system pages or boot-stolen storage. A registered
source is never cancelled: it keeps that custody for the boot. REMOVE closes source registration
and drains readers before manual GTT/MMIO/power teardown; a retained source keeps
its graph.

The first typed `set_config` on a pipe builds a per-pipe static owner (domain,
32 MiB window, source and provider) and is BASE-only: the legacy 1280×800 mode,
a 5120-byte pitch and unchanged connector routing; only `DSPBASE` changes, so
`DSPCNTR` keeps the legacy 32 bpp no-alpha format and ARGB8888 shows as opaque RGB.
A framebuffer not yet mapped is prepared when it is first selected, into a free
slot of the window, also while another buffer is shown. The switch keeps a DRM
and GEM reference on the shown and the staged framebuffer only; a successful
switch drops the replaced one's. `wmb()` precedes the `DSPBASE` store; the
provider checks each buffer's binding (its tuple) and its first and last GTT
words before it, under the GTT mutex, the domain lock and the epoch admission
that keep every other writer out, and reads the stored word back. Whole
bindings are checked where pages change: bind, unbind and retirement. The domain
runs one identity pass before the store and one after. A framebuffer whose first
show was refused with no display state left (a full domain, `ENOSPC`) may be
shown later.
Selecting the original legacy framebuffer again restores its base and returns the
pipe to legacy modesets; after one, the source no longer matches and typed
selection is refused. A replaced buffer stays mapped (PENDING) and may be shown
again.

Retirement: the provider records the pipe's frame counter (24 bits, as gma500's
vblank counter) right after the `DSPBASE` store that replaced a buffer. The old
buffer is certified once the counter, with the pipe enabled, is at least two past
that value; before that retirement returns `EAGAIN`. A buffer never shown is
certified at once. Withdrawal then replaces the buffer's PTEs with scratch words in
descending order while every other bound buffer keeps all its PTEs (their PTEs are
read back before the first withdrawn page and around the flush), disposes the
binding, frees the slot (any slot, not only the last) and signals the fence. A
failed or ambiguous switch, withdrawal or source check freezes the owners in HOLD.
The raw frame counter increments at the start of active
([HW-DISP-004](../hardware.md#display-pipes-and-scanout)), so the old buffer's
last fetch ends one or two counts after the store: two is the bound.

Page flips. A flip between two typed framebuffers is the same switch, run by
gma500's `page_flip` under every modeset lock with the plane's mode and connector
route; a flip that involves the legacy framebuffer is refused (`EOPNOTSUPP`; it
comes and goes through `SetCrtc`), and so is one to the buffer already shown
(`EINVAL`). gma500's vblank counter counts vblank starts: the raw frame counter
plus one from the start of line `vblank_start - 1` of one `FRAMEPIXEL` read
(`poulsback_display_vblank.h`), so the vblank interrupt and every other reader
see one count. The flip event is DRM's, armed after the store
(`drm_crtc_arm_vblank_event`): it comes at the first vblank whose count passes
the count read after the store, which is never before the store has latched and
at most one frame after (`tests/kernel/test_display_vblank.c`). DRM also sends it
when the CRTC turns off. The flip holds a vblank reference and the replaced
framebuffer's references until that count has come; the vblank interrupt then
queues a work item that drops them. Until then another flip or `set_config`
returns `EBUSY`. gma500's own flip of its legacy (dumb) framebuffers takes the
same path after its `set_base` store: the pending rule, the refusal of an off pipe,
DRM's event armed after the store, and the replaced buffer's pin and reference
kept until that vblank.

`dri/N/poulsback_display` (debugfs, read-only) prints per pipe `PIPECONF`,
`DSPCNTR`, `DSPSTRIDE`, `PIPESRC`, `DSPBASE`, the cursor's `CURCNTR`, `CURBASE` and
`CURPOS`, an FNV-1a hash of the 256 palette words with how many are the identity
ramp, the frame counter and, for the buffer
the provider last stored, its GTT offset, size and how many of its GTT words equal
page address | 9 (every word is read). For a disabled pipe only `PIPECONF` is read
and the rest print as zero: reading a disabled pipe's frame counter or plane
registers hangs the machine ([HW-DISP-001](../hardware.md#display-pipes-and-scanout)),
and `psb_pipe_position` refuses such a pipe for every caller. A last line counts
framebuffers created, destroyed, queued for retirement, freed and held for the
boot, so a test can see one that was never freed.

Two more read-only files time the pipes. `dri/N/poulsback_position` prints, per
enabled pipe, one position with its ktime: the frame and pixel counters of one
`PIPExFRAMEPIXEL` read, the scanline register (EMGD's `PIPEx_SCANLINE_COUNT`,
`PIPExCONF` − 8) read right after it, and the raw timing registers.
`dri/N/poulsback_flips` prints the last 64 timing records, oldest first, with
sequence numbers. A `store` record dates a successful typed base switch or
legacy page-flip store; a `vblank` record dates the vblank interrupt before
`drm_handle_vblank`, under `dev->event_lock`. Both contain the pipe, ktime,
scanline, raw frame count and pixel count. There is no separate completion
record: the DRM event and the [flip rule above](#static-source-and-cache-custody)
determine completion.

## Firmware and display handoff

Physical display setup retains the locked gma500 OpRegion/VBT and KMS mode
selection. A firmware update file is not the active panel description. Firmware
can modify panel selection, detailed timings and checksums during POST. Keep
the raw update data, runtime VBT and cached connector EDID as separate inputs;
do not replace the selected mode with a timing from an offline ROM template.

Firmware's display GTT and stolen-memory setup do not establish an SGX address
space. The [typed display](#typed-display-lifetime-foundation) and
[SGX owners](memory.md#physical-source-transaction-boundary) still require their own mappings,
source epoch, cache transitions and independent release conditions. A matching
numeric offset in PCI configuration, display registers or SGX registers does
not identify the same resource.

Display GTT initialization installs a scratch PTE for every active aperture
page. The aperture entry count must fit the mapped table capacity and the
stored count types before mapping or PTE writes. Extra mapped capacity is left
untouched. Stores and the final posted-write read share the GTT mutex; GEM then
populates its stolen-memory prefix and later restores pinned bindings.

Resume requires the selected root, table mapping and aperture geometry to match
the retained mapping. The fallback table address is a decoded physical base,
without PGETBL enable flags. A mismatch preserves the old geometry and refuses
the clear; GTT, GEM and display-restore errors propagate before later restore or
IRQ stages. The existing caller selects the saved PGETBL root before this
comparison. These CPU checks do not authenticate the earlier firmware root or
prove display-fetch quiescence, SGX retirement or safe active-display reuse.

## Display identity

The module and DRM/PCI driver name are `poulsback`. Successful registration prints
the detected chip with a project label such as `Poulsbo (poulsback)`. This is an
identity label, not an acceleration claim. PCI names from tools such as `lspci`
come from their device database. Qualifying a build requires its actual module
and source identities, not a displayed name alone.
