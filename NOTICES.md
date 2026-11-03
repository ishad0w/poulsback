# Licensing and source notices

The [root MIT licence](LICENSE) covers project-owned material unless a file or
this notice specifies different terms. Retained upstream copyright and permission
notices apply to their respective contributions. The root licence does not
relicense external material or every combined binary.

Unless a file says otherwise, the project's documentation, data (JSON and JSONL),
test shaders and build and configuration files are also MIT under the root licence.
[LICENSE](LICENSE) is the MIT text for the SPDX identifier `MIT` in file headers.

## Authorship and redistribution

Reuse, forks and redistribution are welcome under the applicable licences.
Keeping notices with the work preserves credit for everyone who contributed.

Poulsback's project copyright notice names `ishad0w and the poulsback contributors`.
[AUTHORS](AUTHORS.md) records project credit; individual files and retained upstream
notices identify additional copyright holders and applicable terms.

When redistributing copies or substantial portions of MIT-licensed Poulsback
material, retain the applicable copyright notice and MIT permission notice.
Include the full [MIT text](LICENSE) with the redistributed material. This also
applies when that material is incorporated into a differently named or larger
product; a new name does not remove the licence's notice requirement.

You may add accurate copyright notices for your own changes alongside the original
notices. Preserve the existing notices and follow each component's licence,
including the GPL conditions described below.

For binary distributions, accompany the software with the applicable licence and
copyright information, for example in the package's licence documentation or
third-party notices. Source archives should retain file notices and the
corresponding licence texts. SPDX identifiers help identify a licence; they do
not replace a full licence text required by its terms.

We also appreciate a visible acknowledgement and a link to
[Poulsback](https://github.com/ishad0w/poulsback) when you build on the project.
That public credit is a request, not an additional MIT licence condition.
This section explains existing licence obligations and adds no new licence terms.
The [name and logo policy](TRADEMARK.md) separately addresses project identity.
For missing credit or suspected notice removal, contact Alex at
[alex@kmn.io](mailto:alex@kmn.io) with the affected files or distribution.

## Source and build boundaries

| Material | Licence and attribution |
| --- | --- |
| Project-authored source marked `MIT` | Root MIT licence and the file's copyright notice. |
| GPL-marked engine, GEM and VM kernel files, listed below | `GPL-2.0-only`; preserve each file's project notices. These are parts of the current product module. |
| External Linux/KMS and Mesa sources | Their own per-file licences and notices. |
| [`src/xorg/poulsback_display.c`](src/xorg/poulsback_display.c) | MIT; parts follow xf86-video-modesetting's `drmmode_display.c`, whose Red Hat and NVIDIA copyright and permission notice the file keeps. |

These thirteen project files are `GPL-2.0-only`; every other project-authored C
source, header and `.inc` file under `kernel/` is MIT:

- `kernel/engine/poulsback_context.c`
- `kernel/engine/poulsback_engine.c`
- `kernel/engine/poulsback_engine.h`
- `kernel/engine/poulsback_file.c`
- `kernel/engine/poulsback_file.h`
- `kernel/engine/poulsback_job.h`
- `kernel/engine/poulsback_scene.c`
- `kernel/engine/poulsback_scene.h`
- `kernel/engine/poulsback_submit.c`
- `kernel/gem/poulsback_bo.c`
- `kernel/gem/poulsback_bo.h`
- `kernel/memory/poulsback_vm.c`
- `kernel/memory/poulsback_vm.h`

The [Linux integration patch](kernel/integration/linux.patch) and combined
kernel modules retain the applicable GPL conditions. Separable MIT files keep
their grants. `MODULE_LICENSE("GPL")` is kernel metadata, not a source licence.
The [GPLv2 text](LICENSES/GPL-2.0-only.txt) and
[Linux syscall exception](LICENSES/Linux-syscall-note.txt) are included.

The raster and video static libraries are MIT only. An executable's conditions
depend on the objects actually linked into it; distributed archives also retain
their members' licences and notices, and external dependency notices still apply.

## UAPI licences

The [BO](include/uapi/poulsback/poulsback_bo_uapi.h) and
[exec](include/uapi/poulsback/poulsback_exec_uapi.h) UAPI headers are project-owned
and licensed under `MIT OR GPL-2.0-only`; recipients may choose either licence.
External included headers and kernel implementations retain their own terms.

## Project compiler and raster code

[Fragment PDS](src/compiler/pds_fragment.c),
[pixel-event construction](src/raster/sgx535_pixel_event.c), its
[packet](src/raster/sgx535_task_packet.c) and
[PBE surface](src/raster/sgx535_pbe_state.c) consumers and their project fixtures
use the project MIT grant. This covers these implementations; vendor binaries,
extracted program images and external source files retain their own terms.

## Mesa patches and generated sources

The [constant-array budget patch](src/gallium/mesa/patches/nir-const-array-budget.patch)
is a project-authored MIT contribution to Mesa's MIT-licensed NIR pass.
The original Valve copyright and permission notice are preserved.

Captured Xorg/Mesa shader sources and copied fragments retain their upstream
terms and producer notices. A capture tool's licence does not relicense its
output. A shader corpus and any distributed capture must retain
its own source notices.

## Benchmark tools

The scripts in `benchmark/tools/`, its native build configuration
(`benchmark/meson.build`), `benchmark/README.md` and the case lists
`benchmark/{2D,3D,OpenGL}/cases.json` are project material under the root MIT
licence. Third-party benchmark sources retain their original terms.

| Sources | Origin and licence |
| --- | --- |
| `benchmark/OpenGL/glmark2/` | [glmark2 2023.01](https://github.com/glmark2/glmark2/tree/2023.01), by Ben Smith, Alexandros Frantzis, Jesse Barker and contributors. GPL-3.0-or-later for the main program; helpers retain their own notices. See its `COPYING` and per-file headers. |
| `benchmark/3D/geometry/`, `benchmark/OpenGL/mesa/`, `benchmark/OpenGL/common/` | Selected [Mesa demos 9.0.0](https://archive.mesa3d.org/demos/) sources and helpers, including Brian Paul's contributions. Original per-file notices apply; GLAD and Khronos headers retain their included terms. |
| `benchmark/OpenGL/common/glad/`, `benchmark/OpenGL/glmark2/src/glad/` | Generated GLAD loaders and Khronos helper headers. The upstream licence references are retained in each directory's `LICENSE`; [Apache 2.0](benchmark/OpenGL/LICENSES/Apache-2.0.txt) supplies the full text for the Apache-marked EGL platform header and Khronos specification material. |
| `benchmark/3D/gears/` | Unchanged `glxgears.c` and `glxheads.c` from Mesa demos 8.5.0, with their original copyright and permission notices. |
| `benchmark/3D/poulscube/` | Project-authored under the root MIT licence. Its brackets, wordmark and cube colours are the project's own logo geometry (`assets/logo`). |
| `benchmark/3D/metrics/` | Metrics sources and patch from [poulsback-utils](https://github.com/ishad0w/poulsback-utils). MIT; its original licence is retained as `COPYING`. `glxgears.c` is `src/xdemos/glxgears.c` from [Mesa demos 9.0.0](https://archive.mesa3d.org/demos/), with Brian Paul's original copyright and permission notice. The patch applies to it only for the separately named `glxgears-metrics`. |
| `benchmark/2D/rendercheck/` | [rendercheck 1.6](https://gitlab.freedesktop.org/xorg/test/rendercheck), combining MIT and GPL-2.0-or-later contributions. Original attribution is in `COPYING`; `COPYING.GPL-2` supplies the GPLv2 text. |
| `benchmark/2D/x11perf/` | [x11perf 1.7.0](https://gitlab.freedesktop.org/xorg/test/x11perf). Original X11-style copyright and permission notices are in `COPYING` and the source files. |

Only the source files, headers and data needed by these tools are included.
The common Mesa helpers are shared by the GL and 3D examples; glmark2 contains
its X11 GL/GLES2 source and runtime data. Upstream rendering source files and
copyright notices are unchanged; the project supplies the native build setup.

The [GLAD 0.1.34](https://github.com/Dav1dde/glad/blob/v0.1.34/README.md#whats-the-license-of-glad-generated-code-101-253)
and [GLAD 2](https://github.com/Dav1dde/glad/tree/glad2#license) upstream
documentation grants the generated loader code under Public Domain, WTFPL or
CC0. This does not replace the separate licences of included Khronos headers.
The retained GLAD licence references identify David Herberth's generator MIT
grant and the Khronos specification/header terms; they do not relicense the
benchmark programs. The GLAD 2 reference is the
[licence revision preceding generation](https://github.com/Dav1dde/glad/blob/dcd1e7fd432e5f107dfe81a8046af48e497dcb02/LICENSE)
of these 2021 loaders.

Native builds link the installed GL, EGL, GLES, X11 and GLUT libraries, which
remain separate system packages. Driver binaries and firmware are not included.
Standalone bundles include the applicable section of this notice, the benchmark
sources and their original notices, and a copy of the root MIT text.

## Research acknowledgements

Thanks to [Gaba](https://github.com/gabrieelgama) for the tips and
encouragement, and for his work on [SGX535-reMESA](https://github.com/gabrieelgama/SGX535-reMESA).

## Reference and Live releases

Firmware, extracted programs and vendor binaries retain their original terms;
research storage and a verified hash do not establish redistribution permission.
The [reference and Live packages](docs/re/evidence.md#available-packages) are
separate distributions, not MIT-licensed parts of this source tree.

Selected PSB userspace/firmware notices allow binary redistribution only under
their stated conditions, including unmodified binaries and Intel hardware.
EMGD/Windows agreements distinguish conditional OEM/IHV/ISV rights from
end-user rights; a public research subset is not automatically covered.
Standalone extracted SGX programs and selected BIOS payloads have no established
separate redistribution grant in the reviewed records. Live OS packages also
need their applicable notices and corresponding-source obligations satisfied.
Complete package-by-package clearance remains open.

Ordinary CPU builds need no vendor binary. See the
[Linux licensing rules](https://www.kernel.org/doc/html/latest/process/license-rules.html)
and [SPDX identifier guidance](https://spdx.github.io/spdx-spec/v2.3.1/using-SPDX-short-identifiers-in-source-files/)
for annotation syntax; neither grants rights in an unknown source.
