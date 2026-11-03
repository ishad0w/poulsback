# Video command preparation

The video library prepares descriptors, messages, owned resources, ring writes
and firmware-upload plans on the CPU. It also provides a simulated MTX device
and an analytic MPEG-2 image oracle for tests. The interfaces below separate
CPU preparation, simulated effects and physical completion.

## Windows decoder contracts

The [Windows decode reference](re/video/decode-pipeline.md#windows-decode-declarations-and-host-boundary)
owns Intel's device-specific capability declarations, VLD/MC player distinctions,
retail 2030 callback/queue/error paths and publisher-reported limitations.
Embedded 3398, retail 2030 and Linux EMGD are separate implementations.
A successful callback, software-signalled event or advertised codec cannot
qualify physical decoding, surface release or sustainable playback here.

## Legacy decoder contracts

The [legacy decode pipeline](re/video/decode-pipeline.md) owns the selected
EMGD/PSB input ABIs, codec enumeration, reference/reorder behavior, output layouts,
end-of-stream accounting and unresolved nonconstant numerical comparison.
[MSVDX firmware](re/video/msvdx-firmware.md) owns the distinct containers,
message units and loader/publication rules; the
[publication and surface-layout contract](re/video/surface-layout.md) describes
message ownership, kernel patches, NV12 plane bounds and the remaining
completion/retirement boundary.

These are constraints for the independent interfaces below, not qualification
of a poulsback device provider. They demonstrate decoding rather than hardware
encoding. The CPU fixture's bitstream construction and multi-device SDK encode
symbols supply no US15W encoder contract. Encoding remains the separate
[feasibility assessment](../tests/acceptance/encode-feasibility.md).

### Reported codecs and observed output

The [legacy coverage table](re/video/decode-pipeline.md#codec-and-output-coverage)
separates codec advertisements, submitted inputs and acquired output. Its
[layout limits](re/video/decode-pipeline.md#layout-and-performance-constraints)
bound surface geometry, plane order and performance claims. These contracts do
not qualify this library's physical decode path; device completion, decoded
planes and reference-surface lifetime need their own verification.

### Firmware profiles and transport

The [firmware reference](re/video/msvdx-firmware.md) keeps EMGD's packed
descriptor, PSB's file header, Windows carrier strings and each loader's units
separate. Its [ring/reply contract](re/video/msvdx-firmware.md#ring-and-reply-semantics)
distinguishes publication, reply classification, visibility and final use.
The CPU planners below implement only their explicitly stated profiles.

## Components

- `mpeg2_va029` reads the legacy i386 libva 0.29 MPEG-2 descriptors, normalizes their fields under
  a narrow accepted profile and prepares one intra slice. It is not a parser for modern libva
  structures or arbitrary video files.
- `msvdx_proto` encodes and decodes the Poulsbo firmware envelopes (exact lengths, direction and
  field bounds; uninterpreted header bits kept), validates and stamps a whole envelope stream
  against the caller's budgets before patching (a malformed envelope leaves the stream unchanged),
  and plans one 32-byte render81 insertion in the 100-word host ring: one empty slot reserved,
  initialized wrap padding counted, a bounded plan with no ring write and no device index moved.
  Both are MIT implementations of the local
  [envelope and descriptor contract](video-protocol.md), checked against the
  [black-box reference vectors](../tests/vectors/msvdx-video/README.md).
- `msvdx_owned` validates a caller-pinned owner/epoch registry and explicit relocations. Complete
  validation precedes writes. This is a CPU ownership policy, not authentication of a fabricated
  registry or implementation of the proprietary relocation ABI.
- `msvdx_firmware` parses the version-2 four-DWORD little-endian container and plans
  code/data uploads split at RAM-bank boundaries. Counts are DWORDs; locations and source
  offsets are bytes. Bank-selector and word-address fields remain separate. Positive caller
  geometry limits are CPU policy, not a qualified device profile. This parser does not interpret
  the different EMGD seven-field allocation descriptor or AOSP firmware formats.
- `msvdx_transport` is a **closed CPU fake provider** with private RAM/register/ring fields
  and no device callbacks. It validates the whole firmware plan before effects, checks every
  modeled readback, bounds readiness/signature waits, and orders ring words, visibility,
  producer index and kick. One job is active at a time. Epoch/fence matching, completion,
  drain and read visibility are separate gates; errors follow the HOLD rules below.
- `mpeg2_dc_fixture` constructs a deterministic 16x16 progressive 4:2:0 I-picture with one slice
  and one intra macroblock. Six zero DC differences and no AC coefficients leave the initial
  precision-zero DC predictors at 128, giving a base reconstruction of 1024/8=128.
  H.262 mismatch control adds coefficient `F[7][7]=1`; its ideal contribution has magnitude
  at most one quarter per sample, so nearest-integer samples remain 128. The strict fixture
  oracle expects all 384 Y/U/V samples to equal 128; this does not impose a universal IDCT
  conformance rule. Its all-byte oracle checks separately supplied plane layouts, active samples,
  initialized padding and guards. This is one analytic fixture, not a general encoder, decoder,
  MSVDX surface format or codec conformance claim. The bitstream requires independent software
  decoding before hardware selection; separate IDCT modes of one decoder are not independent
  implementations.

The accepted little-endian i386 descriptors are picture **24 bytes**, IQ **272 bytes**
and slice **28 bytes**. The slice fields are data size, data byte offset, data flag,
macroblock bit offset, vertical position, quantiser scale code and intra-slice flag,
at byte offsets 0, 4, 8, 12, 16, 20 and 24. A 32-byte public-libva slice descriptor
has an additional horizontal-position field and is rejected unchanged. It must not
be truncated or passed directly. A separate adapter must qualify its exact source
ABI and refuse horizontal positions it cannot represent.

## Ownership and error handling

Native metadata and all declared byte spans must be valid and stable during calls. The caller pins
backing allocations and excludes concurrent mutation throughout validation and patching. GPU bases
must already be qualified by an MMU allocator. Buffer IDs, access flags, owner, epoch, ranges,
alignment, disjoint mappings and distinct destination words are checked before patching.

The ring planner receives a coherent exclusive read/write-index snapshot;
accepting those numbers does not prove that snapshot was coherent. The plan
contains host-order word values. Physical ring publication, ordering, visibility,
firmware admission and completion require a device provider; the CPU planner
supplies none.

The stateless planners preserve affected outputs on error. None of these components allocates
GPU memory, accesses MMIO, executes a vendor library or certifies physical fence completion.

The fake transport retains the active state in terminal HOLD after duplicate completion
before retirement, partial publication, timeout or uncertain modeled effects. It has no retry,
reset or HOLD-release API. Firmware bytes must remain separate from the fake-provider object.
Test-controlled faults, readiness settings and model tokens grant no physical memory or firmware
authority. Device integration must qualify ordering, cache/MMU effects, reply origin and
[retirement](architecture.md#terms) separately.

## Verification

See [video tests](tests.md#video) for CPU suites, plane oracles, codec matrices
and decode/presentation procedures, and [notices](../NOTICES.md) for source and
firmware licensing boundaries.
