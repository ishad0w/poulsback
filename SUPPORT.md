# Support and contact

Questions and partial reports are welcome.

Share what you know, and we can work out what information or next step would
help.

You do not need a diagnosis or a patch to start a conversation.

The public source repository is
[ishad0w/poulsback](https://github.com/ishad0w/poulsback), and the project
website is [poulsback.in](https://poulsback.in/).

Public snapshots can lag development; include the release or commit you are
using when you know it.

## Where to go

| Topic | Route |
| --- | --- |
| Bugs and regressions | [Open an issue](https://github.com/ishad0w/poulsback/issues/new/choose) using the bug report form. |
| Feature or hardware-support proposals | Use the feature request form in [Issues](https://github.com/ishad0w/poulsback/issues). |
| Usage, build and research questions | [Discussions](https://github.com/ishad0w/poulsback/discussions); an ordinary issue is also welcome when discussion is unavailable. |
| Patches and documentation | Read [Contributing](CONTRIBUTING.md), then open a [pull request](https://github.com/ishad0w/poulsback/pulls). |
| Suspected vulnerabilities | Follow the private reporting instructions in [Security](SECURITY.md). |
| Missing credit, licence or naming concerns | Contact Alex at [alex@kmn.io](mailto:alex@kmn.io). Include the affected files or distribution and available evidence. |
| Conduct concerns | Follow the private reporting instructions in the [code of conduct](CODE_OF_CONDUCT.md). |

If a public GitHub channel is unavailable, contact Alex at
[alex@kmn.io](mailto:alex@kmn.io).

Keep vulnerability details private.

## Before reporting a problem

The [current status](docs/status/README.md), [build guide](docs/building.md),
[diagnostics](docs/diagnostics.md) and existing issues may help explain a
problem.

Useful details include expected and observed behavior, steps to reproduce, the
source revision and software versions.

For hardware issues, machine model, PCI ID/revision and the loaded graphics
driver or renderer also help.

Include what you can; unknown details can be worked through in the discussion.

Provide focused logs when available and remove unrelated personal information.

State whether tracing was enabled for performance measurements.

A build failure does not require a GPU, and a useful report does not require a
complete diagnosis or a hardware experiment.

If a proposed test has no documented recovery route, discuss it before running
it.

## Useful reporting details

Pick the details relevant to your problem, using observations or logs you
already have.

Partial reports are welcome; a small description is enough to start.

| Problem | Details that help |
| --- | --- |
| Build failure | The full failing command and first error with surrounding output, compiler version, target architecture and source revision. |
| Crash or hang | What you were doing, whether the application, desktop or whole machine stopped responding, what still worked, whether a reboot was needed, and any saved errors. There is no need to repeat the failure to fill in a report. |
| Incorrect image | Application and version, a small shareable example or screenshot, expected output, window or output size, and whether this happened in a window or fullscreen. |
| Slow rendering or a performance regression | Workload command and settings, window or output size, vsync setting if known, existing before/after measurements on the same setup, and the last working version if available. |

For graphics problems, the desktop session (Xorg or Wayland) and the loaded
driver or renderer help distinguish the paths involved.

If the problem follows a particular event, such as resizing a window or waking
the machine, mention it.

For performance measurements, include the logging or tracing state alongside the
numbers.

Missing details can be clarified in the discussion.

## Support scope

This is an experimental driver maintained as time and hardware access allow.

The [hardware inventory](README.md#hardware-behind-the-project) records
reference machines; the status and build guides own tested capabilities and
target limits.

A machine's presence in the inventory does not establish every feature on it.

If you are using another system, tell us your setup and what you observed.

Even partial results can help others understand compatibility and choose the
next test.

Replies may take time in this small project.

During a discussion, we may work together on a smaller reproducer, environment
details or checks that distinguish CPU, GPU and presentation behavior.
