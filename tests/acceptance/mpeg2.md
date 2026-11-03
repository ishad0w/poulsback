# MPEG-2 hardware decode and VA-API

<a id="mpeg2"></a>

[Main checklist](../../docs/checklist.md) · [Case metadata](../../spec/dod.json)

The [video interface](../../docs/video.md) separates CPU preparation from a
physical provider. The [legacy decode reference](../../docs/re/video/decode-pipeline.md)
explains selected input ABIs, reference lifetime and numerical limits; its
saved MPEG-2 results do not qualify a new source/build or widen this matrix.

Sections: [profiles](#mpeg2.profile), [firmware/transport](#mpeg2.firmware-transport),
[pixels and prediction](#mpeg2.first-frame), [layouts/errors](#mpeg2.field-crop-layout)
and [clients, presentation and stress](#mpeg2.vaapi).

<a id="mpeg2.profile"></a>

## Freeze codec profiles, levels, formats and quality criteria

<a id="mpeg2.profile-matrix"></a>

### mpeg2.profile-matrix

Procedure: Declare every supported codec/profile/level/resolution/chroma/interlace cell and pin
reference/conformance streams before hardware acceptance.

Oracle: Every advertised cell has known syntax, normative limits and predetermined pixel
acceptance; unsupported profile/level cells are labelled without expanding support.

<a id="mpeg2.firmware-transport"></a>

## Firmware/KMD pairing, MTX transport and genuine completion

<a id="mpeg2.firmware-pair"></a>

### mpeg2.firmware-pair

Procedure: Prove firmware identity, loader/MTX/ring/MMU ABI and completion/error ownership on
this US15W target.

Oracle: Actual selected firmware/KMD pair executes and supplies authentic per-job
completion/error; submitted surfaces remain owned until certified retirement. Reject
malformed/incompatible firmware before device publication. Hardware acceptance requires matching
selected firmware bytes, authentic request-associated reply and separate
visibility/drain/retirement proof. CPU fake replies and numeric signatures alone cannot accept
this case; uncertain mutation retains firmware, command, bitstream and surface owners. Mandatory
produced output must be complete and authentic; declared absence is allowed only for the
pre-frozen stage where no such output may exist. It never converts missing required image/plane
evidence into PASS for accepted rendering/decode.

<a id="mpeg2.first-frame"></a>

## Decoded-frame correctness and plane comparison

<a id="mpeg2.first-intra"></a>

### mpeg2.first-intra

Procedure: Decode a tiny independently known intra frame after full transport admission; capture
all coded/visible Y/U/V bytes.

Oracle: Apply the shared [decoded plane oracle](README.md#decoded-plane-oracle)
to all captured coded/visible Y/U/V bytes.

<a id="mpeg2.picture-types"></a>

## Required pictures, prediction and reference reordering

<a id="mpeg2.prediction-sequences"></a>

### mpeg2.prediction-sequences

Procedure: Decode multi-frame clips spanning every mandatory picture/reference mode in the
claimed profile.

Oracle: All planes of every frame match frozen quality criteria; decode order, reference
ownership and display order are correct.

<a id="mpeg2.field-crop-layout"></a>

## Interlace/field, crop, chroma and stride behavior

<a id="mpeg2.surface-layouts"></a>

### mpeg2.surface-layouts

Procedure: Exercise every required field/interlace/chroma mode and all legal crop/stride/size
boundary cases for each claimed profile.

Oracle: Every defined visible/coded sample matches the reference; every forbidden guard or
outside-domain byte stays unchanged. Each permitted coded/alignment padding interval follows
its predeclared qualified plane-write-domain policy. Unsupported mandatory layout leaves that
profile DoD open. The analytic DC fixture retains its stricter exact guard policy.

<a id="mpeg2.errors-lifetime"></a>

## Malformed streams, surface lifetime and multiple contexts

<a id="mpeg2.decode-error-matrix"></a>

### mpeg2.decode-error-matrix

Procedure: Test truncated/malformed streams, unsupported configurations, multiple
surfaces/contexts and close/exit under work.

Oracle: Safe deterministic errors; no cross-context data, kernel corruption or premature surface
release; uncertainty retains ownership/HOLD. Every required failure cell records independent
expected prepublication refusal or retained HOLD; reference/output resources cannot be reused on
completion alone. Mandatory produced output must be complete and authentic; declared absence is
allowed only for the pre-frozen stage where no such output may exist. It never converts missing
required image/plane evidence into PASS for accepted rendering/decode.

<a id="mpeg2.vaapi"></a>

## Truthful VA-API capability/configuration and clients

<a id="mpeg2.vaapi-configurations"></a>

### mpeg2.vaapi-configurations

Procedure: Integrate the chosen Mesa/Gallium video frontend, query profiles/entrypoints and
exercise actual libva clients.

Oracle: Queries describe only qualified real decode cells; actual output planes match reference;
a successful vainfo alone is not decode evidence.

<a id="mpeg2.decode-presentation"></a>

## Headless decode and decode+presentation accounting

<a id="mpeg2.headless-then-visible"></a>

### mpeg2.headless-then-visible

Procedure: First decode without presentation, then show the same stream fullscreen with
PTS/frame IDs and framebuffer captures.

Oracle: Headless planes are correct and presentation uses those surfaces in correct order;
drops/duplicates and lateness are separately explained.

<a id="mpeg2.realtime-stress"></a>

## 30-minute qualified playback and measured throughput

<a id="mpeg2.playback-long"></a>

### mpeg2.playback-long

Procedure: Run at least 30 minutes of functional decode stress for a nonempty supported workload
set covering every advertised codec profile. Separately run 30 minutes for each realtime
configuration being claimed.

Oracle: Apply the shared [video stress oracle](README.md#video-stress-oracle)
to every advertised profile and each claimed realtime cell.
