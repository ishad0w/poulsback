# Tests

Use this guide for test commands, fixtures and capture procedures. The
[checklist](checklist.md) owns acceptance rules and the
[case metadata](../spec/dod.json) owns parameters, scopes and artifacts.
Detailed acceptance criteria and oracles live in the [case guides](../tests/acceptance/README.md).

- [Select checks](#selecting-checks) and [run them](#run-checks)
- [Quality gates](#quality-gates) and the [pre-commit hook](#pre-commit-hook)
- [Locate a failure](#locate-a-failure)
- [Kernel runtime](#kernel-runtime)
- [Native 2D](#native-2d)
- [Graphics APIs](#graphics-apis)
- [Offscreen rasterization](#offscreen-rasterization)
- [Video](#video)

For individual command names and inputs, use the [tool index](tools.md).
The [test directory guide](../tests/README.md) explains fixture locations.
The [standalone CPU route](#run-checks) works from this repository alone;
the full quality-gate check additionally needs prepared external build inputs.

## Selecting checks

Select tests from the changed contract and its callers, not from patch size.
A one-line address or lifetime change can affect every submission; a prose edit
does not require a new device run.

| Changed boundary | Minimum relevant checks |
| --- | --- |
| Documentation or navigation | Project checker, `test_project.py` and `git diff --check`; preserve the acceptance IDs and scopes. |
| An isolated codec or packet builder | Its strict/UBSAN suite, independent word/extent oracle and affected caller tests; include refused inputs and unchanged outputs on failure. |
| Mesa shader, draw or context behavior | Affected real Mesa/NIR fixtures and the downstream packet/transport tests, using the locked source/build pair. |
| UAPI, ownership, mappings, cache or completion | Affected kernel host suites, the module build, the image/PRIME/display callers that share the contract and a device run for the frame path. |
| Builders, input locks or test orchestration | Relevant Python regressions plus a representative actual build/run; compare input selection and collected test IDs, not just exit status. |

For documentation work, run from the repository root:

```sh
python3 -I -B tools/check-project.py check
python3 -I -B -m unittest discover -s tests/project -p 'test_project.py'
git diff --check
```

Edit Markdown directly, then run the checker. It checks local links and anchors,
README reachability, MAP coverage and acceptance references. It does not validate external
web pages or establish the truth of hardware claims.

Before qualifying an integrated kernel/Mesa candidate, run the full CPU check
and the applicable i386 kernel, runtime and actual Mesa integration checks on
that same candidate. This checkpoint supplements focused checks; it does not
replace the separate hardware cases or make unrelated video/2D regressions
optional when shared code changed.

Reuse a cached CPU result only when the complete relevant source/dependency
closure, compiler, options, fixture and oracle match. Record it as cached, with
the original result identity; a matching commit alone is insufficient. A selected
input list is not a complete compiler dependency manifest: a report of a checker's
selected inputs alone does not establish the complete Mesa/generated/system-header
and toolchain closure needed for cached evidence.
Parallel checks need separate output directories and a shared memory/job budget.
Serialize device experiments and retain a defined recovery procedure for the
selected machine.

## Run checks

On Linux, install the [CPU build dependencies](building.md#cpu-libraries). From
a standalone driver checkout, run:

```sh
make check-project check-project-tests check-tool-tests check-userspace
# Optional: run only the C suites' UBSAN flavor.
make check-ubsan
```

Those targets run documentation/catalog checks, project and tool regressions,
and strict/UBSAN C suites without external SDKs or a GPU. The C suites
alone also run through `python3 -I -B tools/build-userspace.py --build-target host`.

`make check` adds the [quality gates](#quality-gates), side by side (`JOBS`,
default 5). They need a prepared i686 Docker image, matched Linux subset,
Mesa SDK, Xorg SDK and build-root metadata, plus host Docker and Ninja.
[Building](building.md#cpu-libraries) lists these inputs and their availability
limits. This repository does not bootstrap that environment. If a matching
prepared root is already available:

```sh
make check WORKSPACE=/path/to/prepared-build-root
```

No vendor driver or firmware is executed. Explicit build targets and their
scopes are described in [Building](building.md#build-targets-and-compatibility).
Select target flags rather than `-march=native`.

Apart from the quality gates' builds, `make check` does not run the external Mesa
fixtures, Kbuild or device experiments. Some files under `tests/project/` are explicit command-line
fixtures, not unittest-discovery cases. The [kernel host tests](../tests/kernel/README.md)
list the kernel suites in `make check` and the separate checks on the patched
Linux bodies. A default CPU PASS does not cover those additional commands.

`make check-project-tests` runs unittest discovery over `tests/project/` and
`make check-tool-tests` over the CPU-builder, compiler-input and coverage-ledger
regressions under `tests/unit/`. Both are included in `make check`,
together with the C suites.
The CPU builder shares matching object configurations, but relinks and executes
tests on every call; see [object reuse](building.md#cpu-libraries). Its reports
separate dependency preparation, compilation, linking, tests and oracle time.
This is build reuse, not reuse of a test verdict.

Use independent expected values, boundary inputs, failure injection and resource
accounting. Retain command, actual exit status, source/build identities and raw
outputs with an explicit [result scope](architecture.md#result-scopes).
Device runs additionally retain [diagnostics](diagnostics.md),
publication/completion/retirement records and
full output comparisons; screenshots supplement those records.

Expand normative API cases before execution. Report unsupported mandatory cases,
primary failures and cleanup failures separately. The checklist's evidence
rules govern acceptance and HOLD; the procedures below define each fixture's
specific inputs and observations.

A fixture is a controlled test setup. An oracle supplies independently derived
expected results. A case adapter connects a frozen case family to an executable
test and checks its outputs and evidence.

### Locate a failure

Keep a separate outcome for each boundary. A later successful step does not
erase an earlier failure, and a step that never ran is not a GPU failure.

| Boundary | Check first |
| --- | --- |
| Input selection | Original producer report, every selected span and dependency identity. Refusal here precedes compilation or submission. |
| Build or CPU execution | Actual child return status and its complete stdout/stderr, with the selected target and options. A wrapper exit alone is insufficient. |
| Device admission | Actual boot, loaded module/build identity, source snapshot and command outcome. A staged upload is not an executed test. |
| Publication and execution | Last attempted and completed action, frozen resource bindings, raw IRQ/fault observations and matching issued/seen/ACK/clear history. Preserve partial effects. |
| Visibility and image | Complete cached-read transcript, layouts, guards and the fixed full-image oracle. Report exact differing samples; do not fit a new oracle to the capture. |
| Release and presentation | Independent GPU retirement and display-last-use records. Correct pixels or a completion event alone do not authorize reuse. |
| Recovery | Last captured action, actual child outcome and the next boot identity. A partial stream stays partial; restored SSH alone does not explain the restart. |

The [diagnostics guide](diagnostics.md) owns capture fields and streams. On
UNKNOWN/HOLD, stop submissions and retain owners according to the selected
procedure. Keep image correctness, cleanup and recovery results separate.

The display GTT fixture applies the current integration patch to the exact
locked Linux source, then compiles its complete GTT functions and actual
stolen-prefix and resume callers against bounded RAM, PCI and I/O models:

```sh
python3 -I -B tools/check-display-gtt.py --i386 \
  --linux-source /path/to/locked/linux --output /path/to/new/display-gtt-check
```

Run this in an i386 build environment. Strict and UBSAN cases check every active PTE for
128/256/512 MiB apertures, unchanged unused capacity, scratch/stolen separation,
overflow and undersized mappings, PCI/map failures, retained-root/geometry
changes, fallback enable-bit transitions and resume error propagation. The
fixture uses the patched production bodies; it does not load a module. Its
report retains source hashes, actual child results and input stability. These
checks establish CPU bounds and ordering, not hardware quiescence or retirement.

These checks do not establish PTE/cache effects, last display fetch or GPU
retirement; the component registers no physical provider.

## Quality gates

The gates keep dead code, duplication and stale text from coming back. Their hits must
match [`quality-debts.txt`](../quality-debts.txt) exactly: a hit missing there fails, a
count above the list fails, and an entry that no longer hits fails until it is deleted.
A count may only fall; lower it in the commit that lowers it. Every entry names the
closure label that ends it; `closed:` lists the labels that are done, and an
entry naming one fails. A label is an identifier, not a reference to an external
plan: the entry's reason must describe the required change, and its component
guide defines the applicable interface or integration boundary. `--hits` prints
the current hits in the list's format.

| Where | Command | Gates |
| --- | --- | --- |
| `make check` (every commit) | `tools/quality.py check`; QC-stale in `tools/check-project.py` | `sources`, `test-only`, `tools`, `static`, `warning`, `clones`, `size`, `stale` |
| `make check-reach` (integration checkpoints) | `tools/quality.py reach` | `dead`, `test-reach`, `never-runs` |
| `make coverage` (coverage checkpoints) | `tools/coverage.py check` | the reach gates and `coverage` |

| Gate | Fails on |
| --- | --- |
| `sources` | a file under `kernel/` or `src/` that no build list names and no built or tested file includes |
| `test-only` | a product file only test builds compile or include |
| `tools` | a file under `tools/` or `tests/` nothing uses: no make recipe, `AGENTS.md` command, ledger runner (`spec/`) or configured runner executes the tool, and no used tool loads it; a test whose runner (a build list, unittest discovery, a configured runner) is there but whose only subject (with what it includes) is unused tools or dead code; data that no used code names. A named directory that runs nothing (data, or headers an `-I` names) names its files; meson `foreach` names count |
| `static` | a global no other translation unit of any build references |
| `warning` | `-Wmissing-prototypes`, `-Wmissing-declarations`, `-Wunused*` or `-Wshadow` in product files, in the module, the Mesa archive, the DDX and the strict host TUs (counted per file and flag) |
| `clones` | windows of 50 tokens equal after identifiers and literals are normalized, counted per file as maximal regions |
| `size` | a product file over 1,500 lines or a function over 200 |
| `stale` | a `poulsback_`/`sgx535_`/`POULSBACK_`/`SGX535_` name in a comment, document or JSON file, or a `name()` in a C comment, that no code, string or file defines; a backticked driver path that does not exist (the RE book `docs/re/` is exempt from the path rule) |
| `dead` | a product function the real link drops: libgallium (the SDK's link line with this archive: members it never extracts count) and the DDX with `--gc-sections`, or a module function unreachable through relocations from the loader's sections (`.gnu.linkonce.this_module`, `__param`, tracepoints; metadata such as `__mcount_loc` lists code without calling it) |
| `test-reach` | such a function a test names, or one a test-named such function reaches through the relocation graph (its hit names that caller, whose ENDS it takes) |
| `never-runs` | a function those links keep but an LTO link of the component from its roots drops: the product's constant arguments make it unreachable (functions are told apart by name and file, so statics of one name in two files are two) |
| `coverage` | a live product function (kept, and not `never-runs`) that no host suite executes (gcov) and no counting device record names |

The builds run in the pinned image: the module with Kbuild, the Mesa archive with
Mesa's flags and the DDX, all with `-ffunction-sections -fdata-sections -fno-inline`
(without inlining, the section graph is the source call graph) and the gate's warnings;
`reach` adds `-O2 -flto -fno-inline -fno-ipa-icf` links of the archive (exporting only
what libgallium's other inputs reference) and of the DDX. LTO folds constants it can
see through calls, not arbitrary runtime state in structures. A function can
therefore survive LTO yet remain unexecuted; `coverage` detects that gap. Product functions are the
driver's own files and the functions `linux.patch` adds to gma500; unreached upstream
gma500 code is not a hit.

`make coverage` builds every host suite with gcov at `-O0` and reads the device suites'
records, `function-coverage-*.json` under `DELL_COVERAGE` (none ship with
the tree): schema `poulsback-function-coverage-v1`, `suite`,
`component`, `source` (`ftrace` for the kernel, `gcov` for a device coverage build of Mesa
and the DDX), `driver_commit`, `tree`, `run` and the executed `functions` (bare names
cover every live function of that name, or `path:name`). A record from a clean tree
covers a function while the function's file is unchanged since the record's commit, so a
change elsewhere keeps it; the changed file's functions wait for the next run. Line coverage is
printed (`--report` writes it per file), never ratcheted, so deleting tested dead code
never fails. Device records are not shipped with this repository; when neither
`DELL_COVERAGE` nor the default directory holds any, only host gcov counts.

`tests/project/test_quality.py` plants a dead function, an unused global, a function
only an LTO link proves dead, a clone, a stale comment, oversized code, unbuilt files,
an unused tool and an unexecuted function, and each must fail its gate.

**Merging the list.** `.gitattributes` merges `quality-debts.txt` line by line
(`merge=union`), so two series that edit it never conflict there; a line one side deleted
can come back. `make reconcile` (`tools/quality.py reconcile`) then deletes the entries
that no longer hit, lowers counts that fell and merges duplicates (coverage entries only
the last, as `make coverage` judges them). It never adds an entry or raises a count, so
the gates still judge the result, and a second run changes nothing. Review the
resulting list changes with the source changes that remove those hits.

### Pre-commit hook

[`tools/pre-commit`](../tools/pre-commit) runs `make check` on the staged tree, never the
working tree, which may contain unstaged edits: it exports the index to
`/dev/shm/poulsback-precommit-UID/tree` under a lock, runs there and removes it. It also
refuses a debt count that rises against `HEAD` or a new entry for a review ID
(`quality.py ratchet`; a commit that changes the gates themselves may add the hits
they newly find, and a review entry may move with its function: it must continue exactly
one entry the commit removes, of the same gate and ENDS, whose name it equals or contains).
The hook needs the complete prepared build-root environment described above; it
is not a standalone CPU-only hook. Install it once per checkout (it stays local):

```sh
ln -sf ../../tools/pre-commit .git/hooks/pre-commit
```

## Kernel runtime

The module is built with `tools/build-kernel.py --target physical`
(see [building](building.md#kernel-dependencies)); the build checks the stack bound.
Host suites and the separate checks on patched Linux bodies are listed in the
[kernel host tests](../tests/kernel/README.md). Those checks execute selected C
bodies against CPU models. There is no detached CPU fixture module: kernel
behavior on the device requires separate hardware runs; this repository does
not include a device-dispatch command.

## Native 2D

The [native raster contract](native-2d.md) owns packet and geometry behavior;
expand the `2d-native` and `2d-x11` cases for acceptance.

### CPU checks

The CPU runner's `native-2d`, `native-raster` and `native-raster-layouts`
components run strict and UBSAN variants. Their tests are
[packet compatibility](../tests/unit/test_sgx535_2d.c),
[general raster](../tests/unit/test_sgx535_2d_raster.c) and
[distinct layouts](../tests/unit/test_sgx535_2d_raster_layouts.c). Independent
pixel expectations, packet decoding, guard bytes and transactional rejection
check the encoder without accessing a device. See [building](building.md)
for the runner and output conventions. The distinct-layout corpus has 15,360
CPU cases covering all GX operations, formats, clipping, overlap directions,
pitches/offsets, no-ops, guards and source preservation.

```sh
python3 -I -B tools/build-userspace.py --build-target host \
  --component native-2d --component native-raster --component native-raster-layouts
```


### Hardware qualification

For each owned-buffer operation, retain the submitted packet, inputs,
destination and guards; compare the complete output and establish completion
and safe release. A fixed-operation result qualifies only its recorded build
and profile, not the complete native blitter or a general queue.

Expand each family into concrete inputs before dispatch:

| Purpose | Contract requirements or cases |
| --- | --- |
| Declared operation and format coverage | `2d-native.contract`, `2d-native.fill-rop`, `2d-native.copy-rop` |
| Geometry, clipping, storage and overlap | `2d-native.geometry`, `2d-native.dimension-eight`, `2d-native.clipping`, `2d-native.layout`, `2d-native.overlap` |
| Upload, GPU chaining and readback | `2d-native.upload-download`, `2d-native.gpu-chain` |
| Queue, clients, teardown and rejected input | `2d-native.queue-fences`, `2d-native.clients`, `2d-native.lifetime`, `2d-native.rejects` |
| Visible movement and sustained operation | `2d-native.numbered-motion`, `2d-native.long-stress` |
| Hardware path identification | `2d-native.path-attribution` |

The source contract owns the format, ROP, size, pitch and offset cells. A
refusal or split path for width/height 8 fails `2d-native.dimension-eight`.
Malformed requests and other pre-publication rejections must publish no device
work and preserve output. Faults injected after publication use the case's
failure oracle; uncertain completion keeps resources in HOLD. Retain primary
and cleanup errors separately. HOLD does not authorize retry, unload or release.

For correctness motion, save every frame's GPU bytes and frame-specific oracle
comparison, then the framebuffer bytes and PNG with ordered visible frame IDs.
Compare presentation separately and record restoration of the previous display.
Stress retains the complete result index and resource accounting, with the
contract's sampling rule for images. A screenshot can reveal an artifact, but
cannot identify the producer or replace raw pixel and retirement checks.

### Accelerated desktop

The native blitter does not implement alpha composition, glyphs or transformed
textured rendering. The `2d-x11` contract targets the poulsback DDX: EXA on the
2D core, DRI3 and Present. Render currently uses CPU fallbacks; the provisional
planned route adds EXA Composite on the 3D core through a render library
shared with Mesa. Glamor remains an alternative to measure, rather than an
implemented dependency of this DDX. See [graphics](graphics.md) for the current
route and [graphics testing](#graphics-apis) for programmable-renderer prerequisites.
`2d-x11.backend-identity` establishes attribution, followed by the composition,
glyph, transform, desktop-damage, presentation and desktop-stress families in
the contract. Exercise scrolling, resize and overlapping windows while saving
damage inputs, reference images, ordered frames and actual backend identities.

Report intentional CPU fallbacks explicitly. They may preserve useful desktop
behavior, but do not close hardware cells. Record kernel faults, temperatures,
memory and power-source events alongside timings; correctness and performance
remain separate results under [diagnostic rules](diagnostics.md).

## Graphics APIs

The [compiler](compiler.md) owns instruction/NIR semantics; [graphics](graphics.md)
owns Mesa interfaces. Current cases cover `gles2`, `gl21` and Vulkan feasibility;
Wayland and OpenGL 3.0 have research checks below, without registered case adapters.

- [API test selection](#external-api-test-selection) and [hardware stages](#hardware-and-api-stages)

Mesa/NIR fixtures require the matched Mesa source/build environment; they are
not part of the standalone CPU command. Their sources are versioned here:

| Fixture source | Checks |
| --- | --- |
| [test_frame.c](../src/gallium/mesa/poulsback/test_frame.c) | Frame writer words, synthetic uniform-colour draws and recorded-frame comparisons; the latter need the explicit `POULSBACK_FRAME_SCENES` input |
| [test_slab.c](../src/gallium/mesa/poulsback/test_slab.c) | Fake-BO placement, zeroing, reuse, retention, busy storage, renames, failure handling and a seeded byte model ending with every BO freed |
| [test_frame_gpu.c](../src/gallium/mesa/poulsback/test_frame_gpu.c) | GPU-shaded and CPU draws on fake BOs; reachable TA entries, PDS/SECONDARY, constants, vertex programs, indices, BO lists and transactional refusals |
| [test_fs_nir.cpp](../tests/unit/test_fs_nir.cpp) | Backend word evaluation against Mesa's NIR evaluation for a supplied shader corpus |

The overlay's [meson.build](../src/gallium/mesa/poulsback/meson.build) declares
Mesa targets. The repository does not yet ship a supported SDK setup and
fixture runner covering all these sources. Missing runner or corpus inputs
are an open check, not a pass. See the [graphics build boundary](building.md#mesa).
A NIR/model comparison has host scope, separate from device execution or API
conformance.

The strict/UBSAN `vertex`, `fs-backend`, `use-encode`, `isp-words`,
`texture-layout`, `texture-words`, `pixel-pds` and `mte-state` C suites are
runnable here through `tools/build-userspace.py`. For example:

```sh
python3 -I -B tools/build-userspace.py --build-target host \
  --component vertex --component fs-backend --component use-encode
```

Their host results do not establish that an operation is connected to Mesa or
has passed on the GPU. Use the [compiler](compiler.md) and
[3D status](status/3d.md) boundaries when selecting device cases.

### External API test selection

Current integration is tracked under [Mesa / Gallium](status/mesa.md);
[OpenGL 2.1](status/opengl-21.md) and [OpenGL ES 2.0](status/opengl-es-20.md)
have separate API readiness criteria.

`gles2.spec-to-case-map` and `gl21.spec-to-case-map` require a versioned
specification-to-case mapping before claiming coverage. Retain specification
revision and hashes, suite commits, dependencies and build flags, platform and
context configuration, advertised limits/extensions, full enumerated case
names, selection hashes and a reason for each exclusion. Include required
behavior absent from a suite as explicit project tests. A family name or a
passing sample cannot substitute for that enumeration.

The [Khronos GLES registry](https://registry.khronos.org/OpenGL/index_es.php)
provides GLES 2.0 and GLSL ES 1.00 specifications. The
[desktop GL registry](https://registry.khronos.org/OpenGL/index_gl.php) supplies
the OpenGL and shading-language specifications. Use the exact revisions selected
by the contract, rather than silently moving to a newer API.

[VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) contains Khronos graphics
API tests and documents its result logs, including embedded comparison images.
Select an applicable GLES package. For OpenGL 2.1, establish which GL CTS cases
actually apply and supplement them with pinned Piglit and explicit legacy
requirements; the current CTS package must not be assumed to certify that older
target. Mesa recommends applicable Piglit/dEQP regression checks in its
[submission guide](https://docs.mesa3d.org/submittingpatches.html), and lists
testing utilities in its [utilities guide](https://docs.mesa3d.org/utilities.html).
These references identify specifications and test tools. Acceptance also
requires a pinned, complete runtime case enumeration.

The catalogue's local case-list metadata pins the selected suite sources,
counts and hashes; for example, [GLES2 external conformance](../spec/cases/gles2.external-conformance.json)
and [GL2.1 external conformance](../spec/cases/gl21.external-conformance.json).
External suite binaries, complete external test-name files and device runners
are not bundled with this repository. A declared external runner remains
unavailable until its executable and exact inputs are supplied. Smoke and
regression selections do not substitute for the complete lists. Results count
only under the [coverage ledger's rules](../tests/acceptance/README.md#coverage-ledger).

### Hardware and API stages

The [integration checks](checklist.md#integration-checks) define dependencies
for bootstrap, offscreen rasterization, repeated depth-tested frames, EGL,
Xorg and gears. Each check requires its own inputs, complete output, completion
and retirement evidence. Full API acceptance uses the families below and is
separate from those integration results.

| Purpose | Contract requirements |
| --- | --- |
| Launch and numerical/control-flow semantics | `gles2.usse-pds`, `gles2.arithmetic`, `gles2.compiler` |
| Geometry, rasterization and sampling | `gles2.geometry`, `gles2.raster-interpolation`, `gles2.textures` |
| Render targets and fragment state | `gles2.framebuffer`, `gles2.fragment-state` |
| API errors, objects, contexts and synchronization | `gles2.api-errors`, `gles2.contexts-winsys` |
| Complete selection and repeated frames | `gles2.conformance`, `gles2.stress-visual` |
| Desktop GL legacy behavior and additional features | `gl21.legacy-pipeline`, `gl21.extended-graphics`, `gl21.glx`, `gl21.conformance` |

Mesa may lower API behavior to programmable hardware; qualification checks the
required observable semantics and the actual hardware path. Retain renderer,
loaded library/module and shader/resource identities, raw framebuffer readback,
comparison criteria and suite logs. Test rejected input and allocation failures,
context isolation, sharing, ordered fences and destruction under load. No
software renderer or hidden CPU raster fallback closes a hardware case.

Use deterministic scenes with numbered moving frames, texture/depth/blend
changes and sampled or failed framebuffer PNGs. Fullscreen glxgears is useful
for visible presentation and application regression, but does not verify all
shader or API behavior. Mandatory skips, crashes, known driver failures and XFAIL
results stay open; changing their label does not satisfy required behavior.
A bounded scene result must not advertise the complete target API;
formal Khronos certification is also a separate process.

### Vulkan feasibility

`vulkan-feasibility.spec-map` begins with the contract's candidate version and
maps every mandatory feature, limit and synchronization requirement to device
evidence or a justified implementation. Pin the relevant normative revision
through the [Vulkan registry](https://registry.khronos.org/vulkan/); its current
combined specification is not automatically the selected version's case list.
`vulkan-feasibility.verdict-review` needs an independently reviewed bounded
verdict. Missing compute, atomics or memory-model evidence means unresolved;
another PowerVR generation or a software ICD cannot prove this device supports
Vulkan. A feasibility decision does not qualify an ICD or Vulkan CTS execution.

### Wayland research

After the [integration prerequisites](graphics.md#wayland-desktop) are met, pin
the compositor/client versions and protocols, negotiated formats/modifiers,
synchronization model, loaded libraries and renderer identities. Test the client
and compositor independently: a GPU client with a software compositor, or the
reverse, does not qualify the complete accelerated path. A nested compositor
does not qualify direct KMS presentation.

Exercise dma-buf import, acquire/release ordering, buffer reuse, damage, opaque
and alpha composition, scrolling, resize, occlusion and client disconnect.
Include invalid layouts and allocation/import failures. Check VT/session changes
and compositor teardown for scanout lifetime and resource leaks. Retain reference
images, raw captures, protocol traces, kernel events and resource accounting.

Use the [Wayland protocol](https://wayland.freedesktop.org/docs/html/apa.html)
to distinguish frame pacing from buffer release. A frame callback is a drawing
hint, not proof of presentation or retirement; also honor negotiated release
fences when explicit synchronization is used. CPU rendering to `wl_shm` may test
protocol behavior, but supplies no GPU client-rendering evidence. Freeze a separate
Wayland case catalog and oracles before claiming coverage.

### OpenGL 3.0 research

Start with the [feasibility matrix](graphics.md#opengl-30), then apply the
[external-test selection rules](#external-api-test-selection) to
the exact OpenGL 3.0/GLSL revisions and context flags. Map each requirement to
applicable pinned API tests or independent project cases. Keep CPU lowering,
hardware semantics and API results separate. No current adapter executes this
selection; GL 2.1, GLES2 or gears results do not establish OpenGL 3.0 coverage.

## Offscreen rasterization

Frames reach the GPU through the
[GPU engine](kernel/execution.md#p3-engine) and are checked on the device; the
CPU checks below cover the clip-W1 conversion.

The internal [clip-W1 candidate](../src/raster/sgx535_clip_w1.h) converts finite clip XYZ in
[-1,1], W exactly +1 and viewport dimensions 1..4096 using integer arithmetic.
It computes the exact rational default viewport/depth transform, then rounds
each result once to IEEE binary32, nearest with ties to even. Signed zero and
subnormal inputs are covered; unsupported inputs leave output unchanged.
Its four output position words do not establish a native VBO/PDS wire format.
The provider separately owns that linkage and origin qualification.

`make check` runs it as the `clip-w1` component: strict and UBSAN builds read the
vectors that [clip_w1_reference.py](../tests/project/clip_w1_reference.py) writes,
each with its exact rational expectation chosen among the nearby binary32 values.

## Video

The [video component](video.md) owns firmware, descriptor and transport interfaces.
This procedure covers `mpeg2`, optional codecs/processing and encoding feasibility.

### CPU checks

The CPU runner's `mpeg2-va029`, `msvdx-proto`, `msvdx-owned`,
`msvdx-firmware`, `msvdx-transport` and `mpeg2-dc-fixture`
components run strict and UBSAN tests. Their respective
[descriptor](../tests/unit/test_mpeg2_va029.c) and
[message, stream and ring](../tests/unit/test_msvdx_proto.c) suites (every
vector of `tests/vectors/msvdx-video`),
[ownership](../tests/unit/test_msvdx_owned.c),
[firmware parser](../tests/unit/test_msvdx_firmware.c),
[fake transport](../tests/unit/test_msvdx_transport.c) and
[DC fixture](../tests/unit/test_mpeg2_dc_fixture.c) suites check layouts,
boundaries, transactional rejection, relocation policy, ring expectations and
an analytic plane oracle.

```sh
python3 -I -B tools/build-userspace.py --build-target host \
  --component mpeg2-va029 --component msvdx-proto \
  --component msvdx-owned --component msvdx-firmware \
  --component msvdx-transport --component mpeg2-dc-fixture
```

Tests check exact layouts/lengths, noncanonical packets, descriptor bounds,
transactional stream failure, ownership/epoch/access/range/alias errors and
10,000 independently checked ring-index states. Firmware plans have per-word
bank/coverage oracles. Fake transport injects failures at each load/publication
operation, checks late/duplicate/error replies and separate retirement gates.
The image oracle detects mutations across every declared plane span. Its two
optional output paths create the elementary stream and tight YUV oracle
exclusively; normal fixture execution writes neither file.
See [building](building.md) for execution and retained result reports.

Firmware/transport tests use synthetic CPU fixtures. Device consumption,
completion and decoded planes are checked by the hardware stages below.

### MPEG-2 checks

Freeze `mpeg2.profile-matrix` first: advertised codec/profile/level, dimensions,
chroma, interlace, surface layout and performance configurations must have
identified input streams and predetermined reference criteria. Retain each
stream's original bytes, provenance, hash and reference-decoder identity.
Resolution/frame-rate probes in the contract are experiments, not support
promises. A filename, container label or a player choosing a codec is inadequate
stream identification.

Record expected coded pictures, pictures submitted by the frontend, completed
decoded pictures and presented frames separately. Check the final picture at
end-of-stream and after decoder drain against the selected stream's reference.
A picture never submitted by the player is not evidence of a GPU decode drop.

| Purpose | Contract case |
| --- | --- |
| Firmware/KMD pairing, MTX transport and completion | `mpeg2.firmware-pair` |
| One independently known intra frame | `mpeg2.first-intra` |
| Prediction and reference ordering | `mpeg2.prediction-sequences` |
| Fields, crop, chroma and surface strides | `mpeg2.surface-layouts` |
| Malformed input, resources and concurrent contexts | `mpeg2.decode-error-matrix` |
| Truthful VA-API queries and configuration | `mpeg2.vaapi-configurations` |
| Decode correctness followed by visible playback | `mpeg2.headless-then-visible` |
| Declared realtime configurations and sustained playback | `mpeg2.playback-long` |

Decoded-frame correctness requires authenticated firmware/host compatibility,
owned command and frame resources, ring publication, actual device completion
and certified release. Retain transport/events and resource accounting alongside
every decoded plane. Compare coded and visible dimensions, crop, stride and plane offsets.
Freeze defined samples, permitted device padding writes and forbidden guards;
check every applicable Y/U/V sample and forbidden byte. Fix normative tolerance
before hardware execution; disagreement between software IDCT implementations
does not authorize an arbitrary blanket tolerance.

Exercise malformed streams and failed allocation/publication as well as valid
pictures. Keep primary decode and cleanup errors distinct; uncertain completion
retains resources in HOLD. Check reference surfaces, context isolation and reuse
only after their ownership and retirement conditions have been established.
VA-API capabilities must describe actual supported configurations. Opening a
VA driver, obtaining a capability string or software-decoding through a fallback
cannot close hardware decode cells.

### Visible playback and performance

Prove headless output planes first, then present those same decoded surfaces.
Retain PTS and frame IDs with submitted/completed/presented, duplicate and drop
counters and an explicit presentation timebase. Save every correctness surface
and its comparison; retain framebuffer PNGs and frame indexes according to the
selected case's capture rule. Color conversion and presentation need their own
comparison, so a visually plausible RGB screenshot cannot replace Y/U/V checks.

Decode corruption, late presentation and insufficient sustained throughput are
different findings. Realtime claims require the specific advertised configuration
to meet the contract under recorded CPU, memory and power-source conditions.
AC transitions and thermal/kernel observations accompany latency and drop
measurements under [diagnostic rules](diagnostics.md). Do not invent GPU
utilization or watt readings when the device exposes no qualified counter.

H.264 and VC-1 repeat the relevant profile, transport, plane, lifecycle and
presentation gates in their own contract domains. Video processing is separate
from decode. `encode-feasibility.device-evidence` requires actual target-device
evidence and an independently reviewed verdict; a decoder, software encoder or
another platform's encoder symbols cannot establish a US15W hardware encoder.
