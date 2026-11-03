# Kernel host tests

Run commands from the repository root. [Test selection and result scopes](../../docs/tests.md)
apply to every check below. These tests compile kernel code or its UAPI outside
Kbuild with CPU substitutes; none loads a module or touches a device.

## In `make check`

The CPU runner builds each suite in strict and UBSAN modes. These suites also
run without the prepared quality-gate environment through `make check-userspace`
or the direct builder command below:

| Component | Test | Covers |
| --- | --- | --- |
| `vm-layout` | [test_poulsback_vm_layout.c](test_poulsback_vm_layout.c) | Fixed per-file VA layout against independent literals |
| `vm` | [test_poulsback_vm.c](test_poulsback_vm.c) | The per-file address space on CPU stand-ins ([vm-mocks](vm-mocks/vm_kernel_mocks.h)): maps and unmaps write and free PTEs and tables, a failed map undoes itself; after the loss's freeze an address space that existed keeps every PTE, table, page and its directory through unmaps, failed maps and release, and counts them, while one created after it releases normally; a release that finds a mapping left keeps it with a warning |
| `regtable` | [test_poulsback_regtable.c](test_poulsback_regtable.c) | `SUBMIT` register table against independent literals |
| `scene` | [test_poulsback_scene.c](test_poulsback_scene.c) | A context's scene buffer (`poulsback_scene.c`) on the `vm` suite's stand-ins with the real address space: the pages array's allocation failure (which once released the scene unsafely), each page's and the mapping table's, and an occupied slot, each unwound without a leak or a lost reference; a scene's PTEs at its slot and its last put; a frozen space keeps its pages |
| `teardown` | [test_poulsback_teardown.c](test_poulsback_teardown.c) | The engine's teardown library (`poulsback_teardown.c`): eight power-off writes as literals, drain and idle checks, and every combination of ready, quiesced, slots, holds and live fences against the free rule (only when ready, quiesced, with no job, hold or fence), from claim to free |
| `context` | [test_poulsback_context.c](test_poulsback_context.c) | A file's execution contexts on CPU stand-ins ([context-mocks](context-mocks/context_kernel_mocks.h) over vm-mocks): a submission's hold keeps the context, its entities and push lock through `CONTEXT_DESTROY` or the file's close until it drops it; the last holder destroys both entities; failed creates and the 64-context limit unwind |
| `bo-use` | [test_poulsback_bo_use.c](test_poulsback_bo_use.c) | Who holds a BO's mapping in one address space (`poulsback_bo_use.c`): handles and jobs as sequences; a job that resolved the BO before its last handle's close keeps the mapping until the job's release, and a kept (HOLD) mapping is never dropped |
| `twod-plan` | [test_poulsback_twod.c](test_poulsback_twod.c) | `SUBMIT_2D`'s planner: an accept/refuse row per field and encoder status, literal words, HW-2D-006's head/body splits (copy orders, clipping, phases, the size rule's edges, word capacity), and 10⁵ fuzzed jobs whose accepted plans an independent decoder keeps exactly inside their clipped operations and BOs, split by its own rule |
| `overlap` | [test_poulsback_overlap.c](test_poulsback_overlap.c) | The engine's overlap rules: orderings of TA passes, raster passes and 2D jobs with `BANK0` words as literals, refusals that change nothing, and 4,000 random event sequences against the invariants (no TA pass beside a 2D job, at most two DPM contexts held, a ready job only behind a raster, nothing waiting that could start) |
| `uapi` | [test_uapi.c](test_uapi.c) | BO, context, submit, 2D submit and display ioctl layouts and numbers |
| `page-address` | [test_page_address.c](test_page_address.c) | SGX-addressable page bounds |
| `foreign-mmu` | [test_foreign_mmu.c](test_foreign_mmu.c) | gma500 MMU wrappers inside the epoch's foreign transaction |
| `debug` | [test_debug.c](test_debug.c) | The `debug` parameter, cached log and its filters |
| `display-vblank` | [test_display_vblank.c](test_display_vblank.c) | The vblank count of a pipe and the flip completion it gives: never before the latch, at most a frame after |

Select one with `python3 -I -B tools/build-userspace.py --build-target host
--output /absolute/new/dir --component <name>`.

## Separate checks on the patched Linux bodies

These need a staged kernel build or the locked Linux source:

- [GEM policy](test_legacy_gem.c): `tools/check-gem-policy.py --kernel-build
  <build-kernel.py output> --output <new dir>` extracts the compiled
  `psb_gem_pin`, `psb_gem_unpin` and `psb_gem_free_object` bodies and runs them
  on CPU models of pages, the GTT and HOLD. It requires an i686 environment; the
  pinned build image works (`setarch i686`).
- [Display GTT](test_display_gtt.c): see
  [`check-display-gtt.py`](../../docs/tests.md#run-checks).

Device behavior is tested only by runs on the Dell; a host pass is never a GPU
result.
