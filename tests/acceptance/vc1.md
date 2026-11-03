# Optional VC-1 expansion

<a id="vc1"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

Use the [video interface](../../docs/video.md) for component boundaries and
the [legacy codec coverage](../../docs/re/video/decode-pipeline.md#codec-and-output-coverage)
for the distinction between VC-1 VLD/MoComp advertisements and checked output.
The [player entrypoint comparison](../../docs/re/video/decode-pipeline.md#windows-decode-declarations-and-host-boundary)
is version-specific; neither that declaration nor MPEG-2 output qualifies VC-1.

Sections: [profiles](#vc1.profile), [firmware/transport](#vc1.firmware-transport),
[pixels and prediction](#vc1.first-frame), [layouts/errors](#vc1.field-crop-layout)
and [clients, presentation and stress](#vc1.vaapi).

<a id="vc1.profile"></a>

## Freeze codec profiles, levels, formats and quality criteria

<a id="vc1.profile-matrix"></a>

### vc1.profile-matrix

Procedure: Declare every supported codec/profile/level/resolution/chroma/interlace cell and pin
reference/conformance streams before hardware acceptance.

Oracle: Every advertised cell has known syntax, normative limits and predetermined pixel
acceptance; unsupported profile/level cells are labelled without expanding support.

<a id="vc1.firmware-transport"></a>

## Firmware/KMD pairing, MTX transport and genuine completion

<a id="vc1.firmware-pair"></a>

### vc1.firmware-pair

Procedure: Prove firmware identity, loader/MTX/ring/MMU ABI and completion/error ownership on
this US15W target.

Oracle: Actual selected firmware/KMD pair executes and supplies authentic per-job
completion/error; submitted surfaces remain owned until certified retirement.

<a id="vc1.first-frame"></a>

## Decoded-frame correctness and plane comparison

<a id="vc1.first-intra"></a>

### vc1.first-intra

Procedure: Decode a tiny independently known intra frame after full transport admission; capture
all coded/visible Y/U/V bytes.

Oracle: Apply the shared [decoded plane oracle](README.md#decoded-plane-oracle)
to all captured coded/visible Y/U/V bytes.

<a id="vc1.picture-types"></a>

## Required pictures, prediction and reference reordering

<a id="vc1.prediction-sequences"></a>

### vc1.prediction-sequences

Procedure: Decode multi-frame clips spanning every mandatory picture/reference mode in the
claimed profile.

Oracle: All planes of every frame match frozen quality criteria; decode order, reference
ownership and display order are correct.

<a id="vc1.field-crop-layout"></a>

## Interlace/field, crop, chroma and stride behavior

<a id="vc1.surface-layouts"></a>

### vc1.surface-layouts

Procedure: Exercise every required field/interlace/chroma mode and all legal crop/stride/size
boundary cases for each claimed profile.

Oracle: Apply the shared [plane layout oracle](README.md#plane-layout-oracle)
to every tested surface layout.

<a id="vc1.errors-lifetime"></a>

## Malformed streams, surface lifetime and multiple contexts

<a id="vc1.decode-error-matrix"></a>

### vc1.decode-error-matrix

Procedure: Test truncated/malformed streams, unsupported configurations, multiple
surfaces/contexts and close/exit under work.

Oracle: Safe deterministic errors; no cross-context data, kernel corruption or premature surface
release; uncertainty retains ownership/HOLD.

<a id="vc1.vaapi"></a>

## Truthful VA-API capability/configuration and clients

<a id="vc1.vaapi-configurations"></a>

### vc1.vaapi-configurations

Procedure: Integrate the chosen Mesa/Gallium video frontend, query profiles/entrypoints and
exercise actual libva clients.

Oracle: Queries describe only qualified real decode cells; actual output planes match reference;
a successful vainfo alone is not decode evidence.

<a id="vc1.decode-presentation"></a>

## Headless decode and decode+presentation accounting

<a id="vc1.headless-then-visible"></a>

### vc1.headless-then-visible

Procedure: First decode without presentation, then show the same stream fullscreen with
PTS/frame IDs and framebuffer captures.

Oracle: Headless planes are correct and presentation uses those surfaces in correct order;
drops/duplicates and lateness are separately explained.

<a id="vc1.realtime-stress"></a>

## 30-minute qualified playback and measured throughput

<a id="vc1.playback-long"></a>

### vc1.playback-long

Procedure: Run at least 30 minutes of functional decode stress for a nonempty supported workload
set covering every advertised codec profile. Separately run 30 minutes for each realtime
configuration being claimed.

Oracle: Apply the shared [video stress oracle](README.md#video-stress-oracle)
to every advertised profile and each claimed realtime cell.
