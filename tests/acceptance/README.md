# Acceptance case guides

[The checklist](../../docs/checklist.md) owns acceptance and review rules. These guides contain
the detailed criteria; [machine metadata](../../spec/dod.json) holds case parameters, scopes and
dependencies. Neither is a run result.

M1 (the complete 2D, 3D, Mesa/Gallium and X11 stack):

- [Complete native raster blitter](2d-native.md)
- [Accelerated X11 desktop](2d-x11.md)
- [Programmable 3D and OpenGL ES 2.0](gles2.md), with every Gallium hook, cap and format
- [OpenGL 2.1 compatibility](gl21.md)
- [Shared health, kernel and UAPI, performance and release quality](release.md)
- [Bounded integration checks](integration.md)

After M1:

- [Vulkan feasibility decision, not a promised implementation](vulkan-feasibility.md)
- [MPEG-2 hardware decode and VA-API](mpeg2.md)
- [Optional H.264 expansion](h264.md)
- [Optional VC-1 expansion](vc1.md)
- [Optional hardware video post-processing](vpp.md)
- [US15W hardware encoder feasibility, separate from decode](encode-feasibility.md)

These shared oracles are part of every case that links to them. Case-specific
procedures and stricter criteria remain in the individual guides. They describe
acceptance requirements, not completed test results.

## Coverage ledger

The ledger is the catalogue (`spec/dod.json`), the case lists
(`spec/cases/<case ID>.json`), the exclusions (`spec/exclusions.json`) and
these guides. `coverage_policy.m1_domains` names M1's domains; the integration
milestones they require count with them. Mandatory requirements count;
optional ones do not.

- **Case.** A catalogue case without a list is one case. A case with a list
  expands to `<case ID>/<list ID>` cases. A list is either written out, or
  external: a suite's own list (dEQP, piglit, xts5) or a generator's (bo-2d-matrix)
  pinned by count and the SHA-256 of its sorted names. An external list without
  a pin is not enumerated yet; it counts as one open case, so its domain cannot
  complete.
- **Runner and criteria.** Every case declares a runner (entrypoint, arguments,
  scope, repository selector and builder) and criteria, its own or its list's.
  The project checker rejects a missing declaration. A runner counts as
  available only when its entrypoint exists. External runner paths are
  declarations, not executables supplied by this repository; `--workspace`
  resolves them against a configured build root. An external suite runner
  with suite/step arguments also needs a sibling `<entrypoint>.steps.json`
  listing that pair. Without the external executable or exact suite inputs,
  the corresponding checks remain open.
- **Results.** Runners write `cases.jsonl` into their run record, one JSON
  object per case: `schema` `poulsback-case-result-v1`, `case`, `commit` (the
  40-hex driver commit), `tree` (`clean` or `dirty`), `class` (one of the run
  classes below), `run`, `verdict` (pass, fail, crash, timeout, skip or xfail)
  and, for a case with a bound, `value` and `unit`. A result counts at a commit when its tree was clean and its commit
  has the same product (`kernel/`, `src/`, `include/`, `support/`, the build
  tools; plus `tests/` for host and source cases). A result for a listed suite
  also carries `list_sha256`, the SHA-256 of the whole list file the run used,
  and counts only when that is the family's pinned source: a smoke or
  regression run of the same suite is a record, not coverage. Results are
  separate files supplied to `coverage-report.py --results`; they are not
  versioned with this source tree. The run classes are:
  - `CHAIN`: the landing smoke run on a candidate commit.
  - `EXPERIMENT`: a development run.
  - `BENCH`: a performance run with fixed settings.
  - `SOAK`: a long stability run.
  - `NIGHTLY`: a scheduled full run.
  - `HOST`: a CPU host-suite run converted by `tools/host-cases.py`.
- **Pass.** A case passes when its runner exists, at least one counted result
  passes and none fails; a pass below a case's bound is a failure. A skip,
  XFAIL, timeout or crash is never a pass: the case stays open. Host runs under
  llvmpipe only check the harness; the expected list is the whole selection.
- **Derived cases.** A feature or extension case may be a view on a listed
  family (`derive`: the family and name patterns). It passes when every
  matching test passes; a case whose patterns match no test fails.
- **Retired.** An ID is never deleted: `retired` in the catalogue keeps it
  with its reason and replacement, and the regression fixture refuses a lost
  one. A listed case is retired in its list (`"retired": reason`). Retired
  cases are not counted.
- **Exclusions.** An exclusion names a case (or a case-ID prefix), the reason,
  its evidence (either independent evidence that the case does not apply, or a
  deferral a maintainer accepted, such as VGA while no VGA sink is at hand) and
  a maintainer's acceptance with its date. The report counts exclusions apart and never as passes.
- **Report.** `python3 -I -B tools/coverage-report.py --missing-runners` prints,
  per domain and for M1, the cases, those with a runner, those run at the
  commit and those passed. M1 is complete only when every case passed or is
  excluded and every list is enumerated.

## API conformance oracle

Every mandatory behavior and applicable case meets the pinned suite/specification
acceptance policy. No mandatory failure, crash, timeout, skip or driver-bug/XFAIL waiver is
accepted. Exclusions require independent nonapplicability evidence; neither demos nor this
internal acceptance are formal Khronos certification.

## Decoded plane oracle

All Y/U/V samples match predetermined codec-appropriate reference criteria, with
verified job completion and safe release. Apply the [plane layout oracle](#plane-layout-oracle)
to coded/alignment padding and guards.

## Plane layout oracle

Correct defined visible/coded samples, forbidden guard bytes and the predeclared
permitted padding policy in each plane; unsupported mandatory layout leaves that profile DoD
open. Apply the predeclared plane-write-domain policy to coded/alignment padding; every
forbidden byte stays unchanged and every defined sample satisfies its reference. The analytic DC
fixture retains its stricter exact guard policy.

## Video stress oracle

Zero decode corruption; no drops for specifically claimed realtime cells; CPU load,
latency, AC events, memory and frame counters retained without invented GPU watts. Every
advertised profile has nonempty functional stress coverage meeting the active minimum duration
with correct decoded output and safe reference/surface reuse, even when no realtime throughput
is claimed.
