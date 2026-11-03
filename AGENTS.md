# Agent guide

Read this file, then [MEMORY.md](MEMORY.md) for the working context and
[MAP.md](MAP.md) to find a component's documentation and tests.

## Before changing code

1. Inspect `git status`, the recent log and the working diff; keep other work's changes.
2. Read the affected component in MAP and its implementation. For a hardware
   question, start from the [legacy contracts](docs/re/README.md).
3. Separate what the source builds, what an experiment observed and what the
   public interface admits. Evidence qualifies only its recorded source and device.

## Rules

- **Product, not lab.** One build, no lab or trial parameters, no CPU fallbacks for
  GPU work. Do not add a compile-time selector or an endpoint per implementation
  step. Debug output goes through the single `debug` module parameter
  ([diagnostics](docs/diagnostics.md)) and `POULSBACK_TRACE` in userspace.
- **Validate once, at the trust boundary.** Check sizes, handles, ownership,
  arithmetic and packet fields where input enters (ioctl, CSO). Re-verifying the
  driver's own data (frozen copies, recompile-and-compare, "nothing changed during
  this call") compiles only with `POULSBACK_SELFCHECK` (debug builds and host tests).
- **Keep it simple.** One mechanism per concern; reuse before adding; delete dead
  code instead of keeping it behind a flag. Comments state invariants and
  ownership, not the implementation.
- **Hardware care.** An unknown GPU outcome keeps its resources in HOLD: no retry,
  reset, unload or release. A known outcome is not unknown: a job whose client exits
  still finishes and releases normally. Never run the GPU outside EMGD's clocks or
  power sequences.
- **Hardware facts come from sources.** Resolve a hardware question from the matched
  EMGD/PSB producer and consumer before probing the device; never brute-force
  registers, flags or packet values. A device run tests a source-backed change or
  separates hypotheses whose predictions are written down first.
- **Errors.** Keep primary and cleanup errors separate; mark error origins with
  `POULSBACK_ERRSITE()`.
- **UAPI.** No raw MMIO, physical addresses or unchecked packets. Keep versioned
  layouts and values when renaming.
- **Style.** Kernel conventions in kernel code, the local style elsewhere; format only
  the files you change ([source formatting](docs/building.md#source-formatting)).
  Keep strict C warnings; check Python with the existing Ruff rules.

## Verification

- In the prepared build environment, `make check` runs the documentation/link
  checker, Python regressions, strict + UBSAN host C suites and quality gates.
  The gates require its build image and Linux/Mesa/Xorg SDKs. For a standalone
  checkout, use the [CPU checks](docs/building.md#cpu-libraries).
  Run the full checks for product changes in that environment.
- Mesa-side changes also need their Gallium fixtures; kernel changes a module build
  (the stack bound is checked on every build) and, for the frame path, a device
  run. Device runs, presentation, performance and conformance are separate scopes
  from CPU results; a CPU pass is never a GPU claim.
- Documentation-only changes need `python3 -I -B tools/check-project.py check` and
  `git diff --check`, not a device run.
- **Quality gates** ([tests](docs/tests.md#quality-gates)): no new dead, test-only or
  unbuilt code, clone, warning, oversized file or stale name. A fix deletes its
  `quality-debts.txt` entry in the same commit; a count only falls. A new entry is only
  for code a locally described integration goal wires, never to pass a check.
- The pre-commit hook checks the staged tree; never stash, never `--no-verify`.

## Documentation

Each fact has one owner in MAP: component documents own interfaces,
`docs/building.md` recipes, `docs/tests.md` verification, `docs/hardware.md`
cross-component GPU facts with their claim IDs, `docs/re/` legacy mechanisms.
Update the owner when a contract changes and link to it from elsewhere. Keep
run journals, raw research, local paths and vendor payloads out of this
repository; update MEMORY only when the working boundary or a decision changes.
Status describes the checked-in implementation. Host-only library features and
experimental patches are labelled separately. Keep project history, dated progress
and run journals out of product documentation, which describes current
contracts and limits. Remove instructions for deleted interfaces.
Describe required contracts and procedures here, with repository-local links.
The same rule applies to explanatory comments in headers, tests and tools.
Do not replace an explanation or runnable command with a path to an external
work tree. State missing build inputs or tooling as explicit limitations.
The [hardware inventory](README.md#hardware-behind-the-project) owns machine
models and availability.

## Commands

| Task | Command |
| --- | --- |
| Full checks, prepared build environment | `make check` |
| Standalone CPU checks | `make check-project check-project-tests check-tool-tests check-userspace` |
| Documentation and links | `python3 -I -B tools/check-project.py check` |
| Lint / format changed files | `make lint FILES="…"`, `make format FILES="…"` |
| Builds, kernel and Mesa | [Building](docs/building.md), then the component in MAP |
| Before committing | `git diff --check`, `git diff --stat` and the affected diff |
| Slow quality gates (links, LTO) | `make check-reach`; with host coverage `make coverage` |
| Install the pre-commit hook | `ln -sf ../../tools/pre-commit .git/hooks/pre-commit` |

## Licensing

Project-authored source files carry a short SPDX header:

```text
SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
SPDX-License-Identifier: MIT
```

Keep third-party notices and licenses; never relabel third-party or uncertain code
as MIT. Kernel files keep the license identifier on the first commentable line.
[NOTICES](NOTICES.md) owns exceptions and open licensing questions; the root
[LICENSE](LICENSE) covers project-owned MIT files.

Preserve contributor credit under [Contributing](CONTRIBUTING.md) and
[Authors](AUTHORS.md). New patches require the [DCO](DCO) sign-off described
there before integration; do not add sign-offs for another person.
[NOTICES](NOTICES.md#authorship-and-redistribution)
owns redistribution guidance, and [TRADEMARK](TRADEMARK.md) owns project identity.

## Boundaries

Normal builds and tests never load modules, submit GPU work, reboot devices or run
vendor binaries. Hardware experiments need an explicit procedure with source-backed
inputs, predicted outcomes and resource-retention rules.
Derive implementations from checked contracts, not from copied third-party code.
