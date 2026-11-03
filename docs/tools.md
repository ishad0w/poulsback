# Tool reference

Use this index to choose an entry point. [Building](building.md) owns dependency
versions and complete build recipes; [Tests](tests.md) owns fixture selection
and result interpretation. Run commands from the repository root with
`python3 -I -B tools/NAME.py --help` for the current options.

- [Sources and builds](#sources-and-builds)
- [Kernel checks](#kernel-checks)
- [Repository tools](#repository-tools)
- [Shared helper modules](#shared-helper-modules)

## Sources and builds

The kernel fetcher downloads only when explicitly invoked. Builders create artifacts;
they do not install packages, load a module or start a graphics session. Kernel
builds require a fresh output outside their source/dependency trees.
The CPU builder may reuse its managed output, including the default
`build/userspace`; it reruns tests after dependency-checked object reuse. See
[CPU libraries](building.md#cpu-libraries) for that output contract.

| Tool | Inputs and purpose | Output |
| --- | --- | --- |
| [fetch-kernel.py](../tools/fetch-kernel.py) | Selected `--build-target` Linux lock and optionally `--package` for an existing Debian source package | Verified source subset under `linux/` and restoration report |
| [build-userspace.py](../tools/build-userspace.py) | Required `--build-target host` or manifest ID; optional `--component`, `--flavor` and `--cc`; shares matching GNU objects in the selected output | Built fixtures, fresh strict/UBSAN results, command logs and phase timings |
| [build-kernel.py](../tools/build-kernel.py) | `--build-target`, matching `--headers`, locked `--kernel-source` and `--target physical` | Staged sources, the module, its build report and per-function plus entry-point aggregate stack bounds; see [Building](building.md#kernel-dependencies) |

For example, after installing the build dependencies, a focused library check is:

```sh
python3 -I -B tools/build-userspace.py --build-target host \
  --component use-encode --output /path/to/new/use-encode-check
```

Use `--help` to select a supported component rather than treating a filename as
a component ID. The [kernel](building.md#kernel-dependencies) and
[Mesa](building.md#mesa) sections show the full dependency
chain.

The repository does not currently ship Mesa or DDX fetch/build entry points.
[Building](building.md#mesa) describes the required SDK inputs and the
remaining standalone build-recipe gap.

## Kernel checks

Both commands below compile and execute selected Linux function bodies with
CPU models. They neither load a module nor access a physical device. There is
no detached QEMU fixture module or product GPU runner in `tools/`.

| Tool | Inputs and behavior | Procedure |
| --- | --- | --- |
| [check-display-gtt.py](../tools/check-display-gtt.py) | Locked `--linux-source`; patched GTT/resume bodies on RAM/PCI models, optional `--i386` | [Locate a failure](tests.md#locate-a-failure) |
| [check-gem-policy.py](../tools/check-gem-policy.py) | Staged `--kernel-build`; actual GEM entrypoint bodies with CPU policy models | [Kernel runtime](tests.md#kernel-runtime) |

## Repository tools

[style.py](../tools/style.py) backs `make lint`, `make format` and `make format-check`.
It selects explicit `FILES` or staged sources, checks pinned developer-tool
versions and never installs anything. Only `format` writes selected source
files; see [formatting](building.md#source-formatting). It performs no driver
build or device operation.

| Tool | Behavior and output |
| --- | --- |
| [check-project.py](../tools/check-project.py) | `check` validates case metadata, stable-ID criteria references, review bindings, local links, tool coverage and QC-stale against the debt list. It neither rewrites Markdown nor evaluates run results. |
| [quality.py](../tools/quality.py) | `check` runs `make check`'s [quality gates](tests.md#quality-gates), `reach` the slow ones (prepared-SDK links and LTO, in the pinned i686 image) against `quality-debts.txt`; `--hits` prints the hits; `ratchet` compares the list with a base commit. |
| [coverage.py](../tools/coverage.py) | `check`: the reach gates and per-function coverage from the host suites under gcov and the device suites' function records (not shipped; without records only host gcov counts); line coverage reported. |
| [pre-commit](../tools/pre-commit) | The [pre-commit hook](tests.md#pre-commit-hook): `make check` on the exported index. |
| [coverage-report.py](../tools/coverage-report.py) | `check` validates the coverage ledger (every case in M1 has a runner and criteria; case lists, pins and exclusions); `report` counts per domain the cases, runners, runs at the commit and passes from `cases.jsonl` run records. See the [coverage ledger](../tests/acceptance/README.md#coverage-ledger). It runs no test. |
| [host-cases.py](../tools/host-cases.py) | Turns a `build-userspace.py` report into ledger results (`cases.jsonl`), `clean` only when every input equals its blob at the commit. |
| [render-logo.py](../tools/render-logo.py) | Generates SVG assets; `--with-png` needs the optional rasterizer. `--check` compares existing assets. See [Logo rendering](../assets/logo/README.md). |

```sh
python3 -I -B tools/check-project.py check
```

The project checker scans source content, including new files not yet in Git.
It prunes root build/output, local environment and cache directories before
walking them, and excludes generated files listed in `.gitignore`. Nested
source directories named `build` remain included. The same selection works in
a release archive without `.git`; links and symlinks in selected content still
receive their normal checks.

## Shared helper modules

These files are imported by entry points; they are not standalone commands.

| Module | Responsibility and caller |
| --- | --- |
| [build_targets.py](../tools/build_targets.py) | Strict data-only target selection, receipts and ABI checks for existing fetch/build commands; [Building](building.md#build-targets-and-compatibility) owns commands and support boundaries |
| [compiler_inputs.py](../tools/compiler_inputs.py) | Reads compiler-produced dependency files and records stable lexical/resolved file identities for CPU object reuse; it executes no compiler or device operation itself |
| [window_scene_inputs.py](../tools/window_scene_inputs.py) | Symbolic-capture importer used by the opt-in clip/viewport fixture's `--corpus` input; it is not part of the engine build or device runner |

Keep this index current when adding or renaming a tool. The project checker
requires a link to every Python file under `tools/`; detailed flags remain in
each command's `--help`, and methods remain in their owning guide.
