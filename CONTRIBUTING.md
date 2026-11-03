# Contributing to poulsback

Help make these old GPUs useful again.

Whether you are learning about drivers, trying Poulsback on your machine or
working on a patch, your participation is welcome.

Questions, small fixes and unfinished ideas are useful starting points.

## Ways to help

- Tell us what worked or went wrong on your machine. A useful report can start
  with what you observed, even if you do not know the cause.
- Improve a confusing explanation, fix a typo or share where the build
  instructions left you stuck.
- Reduce a bug to a small example, improve a test or help interpret a result.
  Many CPU tests and documentation tasks need no GPU.
- Share source references, hardware observations or a focused code change.

If you are unsure where to start, open a question through [Support](SUPPORT.md)
and tell us what interests you.

Draft pull requests are welcome.

You can discuss an approach before you have a finished patch, and explain any
checks or build inputs you are missing so we can work out the next step during
review.

Small fixes can go straight to a pull request.

For a large interface, architecture or dependency change, an early discussion
helps everyone agree on the direction before investing substantial work.

Existing issues and the [current status](README.md#status) can provide useful
context.

## Finding your way around

The [architecture guide](docs/architecture.md) and [map](MAP.md) point to
component documentation.

[Building](docs/building.md) has dependencies and recipes;
[Tests](docs/tests.md) explains check selection and commands.

You can read the parts relevant to your change as you go.

## Preparing a pull request

1. Tell us which upstream revision you started from. Public source snapshots can
   lag development, so a patch may need adapting during review.
2. Explain the problem, the change and any interface or compatibility impact.
   Include a minimal reproducer or an independent expected result when useful.
3. Follow the affected files' style and the existing
   [formatting rules](docs/building.md#source-formatting). Avoid unrelated
   formatting or generated-file changes.
4. Update the relevant documentation when behavior or an interface changes.
5. Run the relevant checks you can and describe their results. If hardware,
   dependencies or a working build are unavailable, say where you got stuck.
   Review can help identify the checks still needed before integration.
6. Keep contributors' credit and licence notices with the work. Before the patch
   is merged, add the sign-off described below; ask if you need help with it.

Use the public repository's pull request form through [Support](SUPPORT.md).

Maintainers preserve your authorship and applicable notices when adapting or
combining patches.

The [author record](AUTHORS.md) and file notices keep credit with public source
snapshots, including releases without the full development history.

## Verification

For a documentation change, run from the repository root:

```sh
python3 -I -B tools/check-project.py check
python3 -I -B -m unittest discover -s tests/project -p 'test_project.py'
git diff --check
```

For code changes, follow the [test selection
guide](docs/tests.md#selecting-checks) and [standalone CPU
route](docs/tests.md#run-checks).

Full `make check` needs the prepared build inputs documented there.

Report build, CPU tests, kernel runtime, GPU execution and presentation
separately.

A CPU pass does not establish a GPU result.

Include the machine and loaded runtime identities for hardware results.

Hardware experiments need identified source contracts, an expected outcome and a
recovery procedure.

The [diagnostic guide](docs/diagnostics.md) explains these boundaries, including
stopping submissions after an unknown GPU outcome.

Discuss an unfamiliar device experiment before running it.

Normal build and CPU-check commands do not load a module or run GPU work.

## Authorship, licences and sources

Credit belongs with everyone's work.

You retain the rights you hold in your contribution, and contributing does not
require assigning copyright to the maintainer.

Changes use the applicable licence of the affected material;
[NOTICES](NOTICES.md) explains the MIT/GPL boundaries and redistribution terms.

- Keep existing copyright and permission notices. Add accurate notices for new
  copyright holders alongside existing ones, rather than replacing them.
- Project-authored source files use the SPDX header described in
  [the repository guide](AGENTS.md#licensing), with the applicable licence.
  Add an individual `SPDX-FileCopyrightText` line when appropriate.
- Identify the origin, version and licence of imported or adapted material.
  Preserve its notices and explain compatibility before adding it.
- For hardware work, use the [source guide](docs/re/sources.md) and checked
  interface contracts. Vendor code, binaries, firmware and captures retain their
  own terms; translating restricted code does not make it an independent
  implementation.
- Credit collaborators with accurate `Co-authored-by` trailers, with their
  agreement, and update [AUTHORS](AUTHORS.md) when adding a named contributor.
  Do not put another person's private contact details in a public contribution.
- AI-assisted changes need the same understanding, provenance review and tests
  as other changes. Describe material assistance in the pull request; the
  submitter remains responsible for the contribution.

## Sign-off for patches

For patches, we use the [Developer Certificate of Origin 1.1](DCO): a
certification that you have the right to contribute the work under its indicated
licence.

After reading it and confirming it applies, add your `Signed-off-by` trailer:

```sh
git commit -s
```

The command adds your current Git committer name and email, which can differ
from the original author's identity.

Use your own identity and an email you are willing to publish: commits and
sign-offs become part of the public contribution record.

A sign-off certifies your right to submit the work under its indicated licence;
it does not transfer copyright or replace source and licence review.

Do not add a sign-off on another person's behalf.

Each new submitted commit needs the appropriate sign-off before integration.

You can open a draft first and ask for help with this step.

Questions, bug reports and discussions do not need a sign-off, and existing
history does not need retrospective changes.

Maintainers preserve sign-offs and agreed coauthor credit when adapting or
combining contributions.

Please help keep participation welcoming by following the [code of
conduct](CODE_OF_CONDUCT.md).

For suspected vulnerabilities, use the private reporting route in
[Security](SECURITY.md).
