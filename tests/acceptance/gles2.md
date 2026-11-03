# Programmable 3D and OpenGL ES 2.0

<a id="gles2"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

[Current implementation status](../../docs/status/opengl-es-20.md).

Implementation references: [shader limits](../../docs/compiler.md#operations-and-numerical-scope),
[raster conventions](../../docs/re/sgx535/raster-conventions.md) and
[screen/image admission](../../docs/graphics.md#screen-admission).
The procedures below are acceptance targets, not a list of implemented features.

For M1 each family holds concrete cases (`spec/cases/<case ID>.json`). The
dEQP-GLES2 mustpass of VK-GL-CTS opengl-es-cts-3.2.14.0
(`aosp_mustpass/main/gles2-main.txt`, 17,165 tests, pinned by the
[local conformance metadata](../../spec/cases/gles2.external-conformance.json)) is split among the
families below by test-group prefix, without overlap; the
[conformance case](#gles2.external-conformance) holds the remainder and
[gles2.khr-gles2](#gles2.khr-gles2) the 473 KHR-GLES2 tests. Each list is
pinned by count and SHA-256, and the [coverage ledger](README.md#coverage-ledger)
counts its results.

- [Shader semantics](#gles2.arithmetic) and [NIR backend](#gles2.compiler)
- [Geometry/rasterization](#gles2.geometry), [textures](#gles2.textures) and [framebuffers](#gles2.framebuffer)
- [Fragment state](#gles2.fragment-state), [objects/errors](#gles2.api-errors) and [contexts](#gles2.contexts-winsys)
- [Gallium hooks, caps and formats](#gles2.gallium)
- [Conformance](#gles2.conformance) and [repeated rendering](#gles2.stress-visual)

<a id="gles2.spec-catalog"></a>

## Freeze GLES2/GLSL ES1.00 mandatory requirement and external test catalogs

<a id="gles2.spec-to-case-map"></a>

### gles2.spec-to-case-map

Procedure: Pin specification revision, API profile/limits/extensions, Piglit and applicable GLES
CTS revisions; enumerate every applicable external case and map mandatory requirements.

Oracle: No omitted mandatory requirement or unaccounted mandatory test; test names in this
catalog are families, never a claimed complete CTS enumeration.

The specification-to-case map is incomplete until every normative requirement and applicable
test is accounted for. Expand the per-resource minimum, limit-boundary, overflow, quota and
query-accuracy cells. A small internal framebuffer/profile limit cannot waive an API minimum.

<a id="gles2.usse-pds"></a>

## USSE/PDS launch, operands, registers and completion

<a id="gles2.hardware-microprograms"></a>

### gles2.hardware-microprograms

Procedure: Run independently encoded vertex/fragment microprograms with raw output for each
recovered operand/register/scheduling rule.

Oracle: Raw output matches predetermined semantics and launch state; instruction/static evidence
alone does not establish silicon execution.

<a id="gles2.arithmetic"></a>

## Shader numerical and control-flow semantics

<a id="gles2.alu-vectors"></a>

### gles2.alu-vectors

Procedure: Run hardware shader arithmetic, conversions, precision and built-ins required by the
frozen GLSL ES1.00 profile.

Oracle: Reference vectors satisfy specification semantics/precision; compare all shader outputs,
recording undefined/spec-permitted cases separately.

<a id="gles2.shader-flow"></a>

### gles2.shader-flow

Procedure: Compile and execute required conditionals, loops, functions, arrays, structs and
discard paths.

Oracle: Required programs execute correctly and invalid programs fail with correct API errors;
unsupported required syntax keeps DoD open.

<a id="gles2.compiler"></a>

## NIR backend, code generation and register allocation

<a id="gles2.nir-backend"></a>

### gles2.nir-backend

Procedure: Compile all frozen required shader families via genuine Mesa NIR and compare
CPU/reference and hardware outputs.

Oracle: Correct lowering at all legal resource limits; rejected excesses are safe; emitted
program/source/module hashes and full raw results are retained.

<a id="gles2.geometry"></a>

## Primitives, indexed draws and vertex fetching

<a id="gles2.draw-primitives"></a>

### gles2.draw-primitives

Procedure: Exercise every required primitive and element-index/vertex-attribute type, stride,
offset and buffer reuse.

Oracle: Independent primitive/raster reference plus applicable conformance tests; no
out-of-bounds fetch or incorrect reused attributes.

<a id="gles2.raster-interpolation"></a>

## Rasterization, viewport, scissor and interpolation

<a id="gles2.raster-states"></a>

### gles2.raster-states

Procedure: Render points/lines/triangles with clipping, culling, winding, viewport/scissor and
interpolation precision cases.

Oracle: Specification-defined coverage and interpolation within frozen tolerances; compare all
color/depth outputs where readable.

Expand the coordinate cross-cases in the metadata: all clip planes, defined W boundaries,
perspective varyings, window/FBO coordinates, both viewport Y directions, exact edge ties and
provoking vertices. Keep raw sample state distinct from effective storage and dispatch counts.
Undefined API inputs retain their specified oracle class rather than an invented pixel
expectation.

<a id="gles2.textures"></a>

## Texture formats, sampling, mipmaps and layout

<a id="gles2.texture-matrix"></a>

### gles2.texture-matrix

Procedure: Exercise all required texture targets/formats, uploads/subuploads,
sampling/filter/wrap and mip levels.

Oracle: Reference sampling and API errors match declared profile; no stale tiles/cache data;
full output pixels compared.

<a id="gles2.framebuffer"></a>

## FBO, renderbuffer and multisampling requirements

<a id="gles2.fbo-states"></a>

### gles2.fbo-states

Procedure: Create/rebind/render/read/copy all required FBO and renderbuffer combinations,
completeness failures and supported sample counts.

Oracle: Correct completeness/errors, attachment contents and ownership; no feedback-loop result
claimed where undefined.

Exercise the size and allocation-pressure cells in the metadata, not only a fixed 64×64 profile.
Failed resize/allocation must preserve live attachments and return the specified bounded error
without lost owners or waits. API minima remain normative even when an internal diagnostic only
handles smaller images.

<a id="gles2.fragment-state"></a>

## Depth, stencil, blending and color masks

<a id="gles2.fragment-state-matrix"></a>

### gles2.fragment-state-matrix

Procedure: Exercise each required depth/stencil comparison and action, blend equation/factor,
dither and color write mask.

Oracle: Per-state independent reference and conformance tolerance; every defined
color/depth/stencil value checked.

Exercise both depth orders, preserved attachments versus clear, depth range/viewport Z,
depth-format quantization and write-mask/comparison/clear interactions. Cross sample masks and
sample-query behavior with the actual attachment state. A finite D24 result with retained owners
cannot close these reusable API cases.

<a id="gles2.api-errors"></a>

## State queries, object lifetime and API error semantics

<a id="gles2.objects-errors"></a>

### gles2.objects-errors

Procedure: Exercise required object creation/binding/deletion, queries, client arrays/buffers
and invalid calls.

Oracle: Required error and lifetime semantics hold without GPU corruption or cross-context state
leakage.

<a id="gles2.contexts-winsys"></a>

## Gallium/winsys, EGL and multi-context resource synchronization

<a id="gles2.winsys-contexts"></a>

### gles2.winsys-contexts

Procedure: Create genuine EGL GLES2 contexts/surfaces and Gallium resources; exercise sharing
and presentation through owned DRM BOs.

Oracle: Every promised interface and sync dependency operates correctly; no premature free or
leaked state; Mesa reports only qualified capabilities.

Cover the concurrency and client-failure cells in the case metadata. Independent and shared
contexts across processes and threads must preserve dependency order and lock order. Check
pending submit/import/map/close, timeouts, error fences and context loss without lost wakeups,
unbounded waits or lost owners. Use only legal same-context concurrency or explicitly defined
error/safety checks. Observe the API reaction required by the chosen EGL profile and any
advertised robustness extensions; a fault test grants no reset permission.

<a id="gles2.egl-x11"></a>

### gles2.egl-x11

Procedure: Initialize EGL on X11 (DRI3) with our libraries, draw to window and pbuffer surfaces,
and check the platform's refusals and dEQP's library and config choice.

Oracle: Every case's criterion in `spec/cases/gles2.egl-x11.json`; dEQP runs on poulsback with
the config it records, and a run on any config other than `rgba8888d24s8ms0` is flagged.

<a id="gles2.gallium"></a>

## Every Gallium hook, cap and format claim

Mesa 26.2.4's `struct pipe_context` has 147 hooks (146 function pointers and
`draw_vbo`, a member of function-pointer type) and `struct pipe_screen` 78 (76
and 2); `struct pipe_caps` has 308 fields and `struct pipe_shader_caps` 32 per
stage. Each is one case, as is every format, target and bind the screen claims.
The case lists record our state (set or NULL), the frontend path that reaches
the hook under GL 2.1 or GLES 2.0 and the gating cap with its value.
A hook's verdict picks its runner: the conformance trace for a set
hook, the caps dump for a hook a cap keeps unreachable, a source review for a
hook only non-GL frontends call.

<a id="gles2.gallium-context-hooks"></a>

### gles2.gallium-context-hooks

Procedure: For a hook we set, run the named tests that reach it through st/mesa. For a NULL hook,
run the caps dump on the published screen.

Oracle: A set hook passes when its named tests pass on the GPU path. A NULL hook passes only when
the frontend cannot reach it: either the named cap reads its off value in the dump, or the
frontend checks the hook for NULL and has a correct fallback. A NULL hook that GL 2.1 or GLES 2.0
can reach is a failure, whatever the tests show.

<a id="gles2.gallium-screen-hooks"></a>

### gles2.gallium-screen-hooks

Procedure: As for the context hooks, for `struct pipe_screen`.

Oracle: As for the context hooks.

<a id="gles2.gallium-caps"></a>

### gles2.gallium-caps

Procedure: Dump every `pipe_caps` field of the published screen on the Dell and compare it with
the value the case list records, with its evidence.

Oracle: The dumped value equals the recorded one, and the recorded value has evidence: a
hardware fact, a test of the feature it enables, or the reason it is off.

<a id="gles2.gallium-shader-caps"></a>

### gles2.gallium-shader-caps

Procedure: As for the caps, for the vertex and fragment `pipe_shader_caps`.

Oracle: As for the caps; limits must also meet the GL 2.1 and GLES 2.0 minimums.

<a id="gles2.gallium-formats"></a>

### gles2.gallium-formats

Procedure: For every format, target and bind that `is_format_supported` accepts, create the
resource and use it in that role (render, sample, scan out, share, fetch vertices or indices).

Oracle: The resource works in that role on the GPU path with exact or toleranced output. A claim
without a case is a failure: the claims and the cases must be the same set.

<a id="gles2.conformance"></a>

## Execute the complete frozen applicable GLES2 test selection

<a id="gles2.external-conformance"></a>

### gles2.external-conformance

Procedure: Execute every case from the pinned mandatory GLES2 CTS/Piglit selection on the real
renderer and retain raw logs.

Oracle: Apply the shared [API conformance oracle](README.md#api-conformance-oracle)
to the complete frozen selection.

<a id="gles2.khr-gles2"></a>

### gles2.khr-gles2

Procedure: Run the 473 KHR-GLES2 tests of `khronos_mustpass/main/gles2-khr-main.txt`. They need
the `glcts` binary, which is not bundled here.

Oracle: Apply the shared [API conformance oracle](README.md#api-conformance-oracle).

<a id="gles2.stress-visual"></a>

## Repeated 3D frames, contexts and visible demo regression

<a id="gles2.repeated-scene"></a>

### gles2.repeated-scene

Procedure: Run textured/blended/depth-tested numbered scenes and a fullscreen application such
as glxgears where the API path permits.

Oracle: All deterministic scenes match references and shared health/lifetime checks; application
motion is regression evidence, not complete API conformance.
