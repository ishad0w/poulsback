# Kernel integration

poulsback extends the pinned Linux GMA500 display driver with buffer objects in
per-file SGX address spaces, KMS framebuffers made of `SCANOUT` BOs and one GPU execution engine
(`CONTEXT_CREATE`, `SUBMIT` for TA → 3D jobs and `SUBMIT_2D` for the 2D core). Linux/KMS is an external dependency; this
repository supplies the integration patch and kernel components. Resource terms
are defined in [architecture](architecture.md#terms).

## Guide chapters

| Responsibility | Owner |
| --- | --- |
| GPU execution, jobs and UAPI | [Execution and UAPI](kernel/execution.md) |
| Buffer objects, address spaces, lifetime, PRIME and the source/MMU boundary | [Memory and release](kernel/memory.md) |
| Typed framebuffer, display GTT, firmware handoff and display identity | [Display lifetime](kernel/display.md) |
| Error-site records, the `debug` log, engine debugfs and check/build routes | [Diagnostics and checks](kernel/diagnostics.md) |

[Hardware claims](hardware.md) own exact qualified results; [3D status](status/3d.md)
owns current capabilities and open gates. [Building](building.md) and [Tests](tests.md) own commands
and verification procedures. The chapters below describe component contracts.

## Components

| Directory | Responsibility |
| --- | --- |
| `kernel/engine` | Per-file state, contexts, `SUBMIT` and its register table, `SUBMIT_2D` and its planner, schedulers and jobs |
| `kernel/gem` | Buffer objects, PRIME and the display pin of `SCANOUT` BOs |
| `kernel/memory` | Per-file SGX address spaces, the fixed VA layout and SGX page tables |
| `kernel/display` | Typed framebuffer, display GTT and plane interfaces |
| `kernel/source` | Source epoch: binding, legacy-writer admission, loss and the engine's claim |
| `kernel/integration` | Linux patch, gma500 hooks (GEM HOLD, MMU wrappers, root admission), engine IRQ routing |
| `kernel/debug` | The `debug` parameter, cached log, errsites and tracepoints |

## Existing section links

Existing kernel-guide fragments remain entry points to the owning chapter.

### Memory and release

- <a id="memory-locking-and-lifetime"></a>[Memory, locking and lifetime](kernel/memory.md#memory-locking-and-lifetime)
- <a id="buffer-object-prime-and-cpu-access"></a>[Buffer-object PRIME and CPU access](kernel/memory.md#buffer-object-prime-and-cpu-access)
- <a id="physical-source-transaction-boundary"></a>[Physical source transaction boundary](kernel/memory.md#physical-source-transaction-boundary)

### Execution and UAPI

- <a id="uapi-and-clients"></a>[UAPI and clients](kernel/execution.md#uapi-and-clients)

### Display lifetime

- <a id="typed-display-lifetime-foundation"></a>[Typed display lifetime foundation](kernel/display.md#typed-display-lifetime-foundation)
- <a id="firmware-and-display-handoff"></a>[Firmware and display handoff](kernel/display.md#firmware-and-display-handoff)
- <a id="display-identity"></a>[Display identity](kernel/display.md#display-identity)

### Diagnostics and checks

- <a id="build-from-current-sources"></a>[Build from current sources](kernel/diagnostics.md#build-from-current-sources)
- <a id="cpu-fixtures"></a>[CPU fixtures](kernel/diagnostics.md#cpu-fixtures)
