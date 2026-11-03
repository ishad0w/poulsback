# Building and checking poulsback

- [CPU libraries](#cpu-libraries): the first build, without external driver sources
- [Build targets](#build-targets-and-compatibility): reference inputs and portability boundaries
- [Adding a target](#adding-a-build-target): common procedure and [candidate systems](#candidate-systems)
- [Debian i386 VM](#debian-12-i386-vm): create the target environment
- [Kernel dependencies](#kernel-dependencies) and [kernel checks](#kernel-checks)
- [Mesa](#mesa)
- [Target flags and outputs](#target-flags-and-outputs) and [source formatting](#source-formatting)

The [tool index](tools.md) explains individual entry points. [Tests](tests.md)
owns fixture selection and the meaning of their results.

## Build environment

Host tools and CPU checks run on a Linux workstation. Target compilation uses
the explicitly selected ABI, toolchain and locked inputs. Debian 12 i686 is the
current reference build; an ABI-compatible build environment is separate from
the environment used to qualify kernel runtime or physical GPU behavior. These
recipes require no particular host path, laboratory machine or physical device.

## Build targets and compatibility

`--build-target debian-12-i686` selects the explicit
[Debian 12 reference requirements](../support/debian-12/i686.json). The kernel
fetch/build commands require this option; no target is implicit.
The CPU builder also requires a choice: `--build-target host` selects ordinary
host CPU checks, while a manifest ID selects its declared ABI. This is a build selection, separate
from the kernel builder's required `--target physical`, which builds the one
module. It grants no module loading or hardware admission. The Mesa/DDX build
route is described [below](#mesa); it does not use this selector.

The common compiler, raster, UAPI and renderer are shared across targets.
`kernel/integration/` owns Linux API/KMS integration and its patch; the existing
Mesa overlay and patches own the matched Gallium integration. `support/` holds
small data-only target requirements, grouped by system, rather than copies of
the driver or distro-specific builders. There is one implemented target so far.

The kernel builder resolves the one physical module into an immutable internal
`KernelBuildPlan` (files, patch, Kbuild fragment and licence inputs) before
creating output. Source staging rechecks every source byte against that plan
before copying, then applies its fixed patch. The plan does not change the
support manifest or execution admission.

Three different records have different purposes:

| Record | Owner and meaning |
| --- | --- |
| Reference lock | [Linux](../kernel/integration/linux.lock.json) and [graphics](../src/gallium/graphics.lock.json) locks own exact external versions, bytes and hashes. |
| Compatibility requirements | The support manifest declares architecture/ABI, CPU flags, toolchain requirements, input routes and native graphics layout. The selector accepts only implemented policies. A new manifest alone does not establish compatibility. |
| Build receipt | The CPU and kernel builders record selected target, manifest/helper/input identities, actual commands and outputs. The kernel builder rechecks the selection before successful publication. Dirty source files must be identified by their actual bytes; HEAD alone is insufficient. |

The `poulsback-build-target-v1` schema has exact fields: `id`, `kind`,
`architecture`, `inputs`, `toolchain`, `graphics` and `build_metadata`, alongside `schema`.
`inputs` names repository-relative lock/patch paths; it does not duplicate their
versions or hashes. CPU/ABI requirements, native compiler tools and flags are
data. Unknown fields, commands, noncanonical paths and unsupported ABI/policy
values are refused. The [selector](../tools/build_targets.py) discovers bounded
`support/*/*.json` manifests. Each ID must match `<system>-<architecture>` from
its directory/filename; duplicate IDs, symlinks and malformed data refuse
selection. There is no Python ID registry, fallback target, inheritance or
executable manifest. Other file types are not executed. Keep package dependencies
and future install hooks in a packaging recipe when that separately tested work
is implemented.

The first portability boundary preserves the following guarantees:

| Existing assumption | Current treatment |
| --- | --- |
| Locked kernel/config/Module.symvers and integration source | Retained exactly. Different headers require a compatible source base, patch and API/ownership review, followed by their own checks. |
| Debian `.deb` restoration and source subset | Retained as a source-format adapter. Archive root comes from the lock; inventory size is derived from its exact pinned file map. The reference still contains 58 inputs, without a universal 58-file or Linux-version condition. Bounded extraction, hashes and no package-script execution remain. |
| Native i686 kernel build | Still required. `ARCH=x86`, baseline flags, final ELF32/EM_386 and the existing stack/warning gates are enforced. Cross-kernel builds are not qualified. |
| Compilation environment | Compile eligibility uses actual 32-bit native userspace and C/C++ ABI/ELF probes. Runtime fixture isolation and device qualification are separate test scopes. |
| Mesa and Xorg inputs | The graphics lock records the matched SDK's provenance. Quality checks use a prepared i686 SDK; a standalone graphics build recipe remains open. |
| Cross/sysroot Mesa builds | Not implemented by the prepared-SDK relink route. Changing `pkg-config` paths alone would not isolate headers, linker inputs and runtime libraries. |
| Isolated non-GLVND Mesa runtime | Prepared-SDK builds produce a separate runtime directory selected by loader environment variables, not a DEB or a system Mesa replacement. |

The lock owns reviewed stack values and their source basis; its `.config`
identity binds the build. The builder retains the implemented x86_32 stack-size
and half-stack safety limits. Reproducible user/host/timestamp metadata comes
from the selected manifest, rather than a distro name embedded in the builder.
Tool executable basenames and portable CPU tuning are validated target data;
the actual compiler and ELF checks still establish what was produced.

Validation fixtures run in designated isolated environments under their own
admission checks. Those checks describe where a test may execute; they are not
a requirement that end users run the reference distro or a virtual machine.
The [test guide](tests.md#kernel-runtime) owns runtime verification. A new build
manifest does not qualify another executing kernel or GPU.

Reference CPU flags remain `-march=i686 -mtune=generic` for kernel/explicit CPU
builds and `-march=i686 -mtune=atom` for Mesa. No distributed artifact uses
`-march=native`. ELF32/EM_386 and ILP32 are checked; x32 and x86_64 are not accepted
as substitutes. A 64-bit build machine may run the explicitly selected CPU-only
suites with a working multilib compiler, for example:

```sh
python3 -I -B tools/build-userspace.py --build-target debian-12-i686 \
  --cc 'gcc -m32' --component pack --output /path/to/cpu-check
```

This does not enable kernel or Mesa cross compilation. Build-machine tools,
target userspace, executing kernel and physical GPU remain separate identities.
A container uses its host kernel; it cannot qualify another kernel's runtime.
No build command loads a module, changes loader defaults, starts GPU work or
removes a stock/recovery driver.

DEB installation lifecycle, package hooks/rollback, CI, DKMS, additional kernel
lines and system GLVND/DRI/EGL/GBM/Xorg activation are deferred. The first target
does not claim Debian 13, antiX or Void support. Add support through trusted
inputs, actual builds and dependency checks, appropriate runtime/package tests,
then separate device qualification before extending the claimed scope. Report
BUILD, CPU TESTED, KERNEL RUNTIME TESTED, HARDWARE and PRESENTATION separately.

### Adding a build target

This is the procedure for implementing the next target, not a claim that it is
already accepted. Do not copy a builder or change the Debian 12 reference lock.

1. Define the first scope: userspace compilation/tests, a kernel integration,
   or a complete installed system. Record build-machine architecture, target
   ABI/CPU minimum, executing kernel and intended device separately. A successful
   i386 application on amd64 does not qualify the current x86_32 module there.
2. Prepare a disposable real userspace from trusted repository inputs. Record
   its package/tool identities and check actual C/C++ headers, linker inputs,
   ELF and runtime dependencies. Missing tools/packages are blockers. Fetch
   dependencies first, then build offline. Leave the working host, active VM
   and device outside installer tests.
3. Select compatible inputs. Reuse an existing lock and patch when their exact
   source/API scope is intended. New Linux inputs need a separately checked
   lock, matching source base, integration patch, headers, configuration and
   Module.symvers. New Mesa inputs need overlay/patch and internal-API review;
   the distro's system Mesa is not automatically compatible. Locks own versions
   and hashes; package dependency/install rules belong to later packaging work.
4. Add target data under `support/<system>/<architecture>.json`, with the unique
   matching `id` `<system>-<architecture>`. Discovery requires no Python registry edit.
   Select the system/release or snapshot explicitly, using the scope in
   [candidate systems](#candidate-systems), before creating the target.
   The strict validator currently admits only implemented native
   GNU/i686 policies; extend a policy only with its corresponding checks.
5. Close concrete integration gaps in the existing tools. A new kernel lock
   supplies the exact source inventory, archive root, ABI/configuration pins and
   reviewed stack policy. A different package format needs its own bounded
   source adapter; a different Linux/Mesa API needs compatible integration.
   These are checked extensions, not removed guards or a second builder.
   Mesa cross compilation needs real native/cross files and verified host/target
   separation; a new manifest does not implement them.
6. Run target/schema and malformed-input tests, affected strict/UBSAN suites,
   actual Mesa fixtures and representative Kbuild as applicable. Recheck the
   Debian 12 reference path. Preserve ABI/ELF, stack/warning, receipt-publication
   and source-extraction gates. Verify fresh offline inputs and dependency-safe
   object reuse; rerun tests on cache hits. Record unsupported scopes explicitly.
7. Accept runtime separately: isolated library loading first, the applicable
   [CPU checks on patched Linux bodies](tests.md#kernel-runtime), then device
   and presentation qualification. There is no detached kernel fixture module.
   Package lifecycle and CI are not implied by a build target.
8. Update this guide's claimed scope and keep full source/build/test receipts in
   the build outputs. A candidate ID or successful compile does not declare distro
   support. No target-specific driver copy, global loader override or implicit
   device activation is introduced.

### Candidate systems

Only the Debian 12 reference manifest exists. The entries below are porting
directions, not additional support declarations. All use the same procedure and
builders above; adding a distro must not create another driver or builder.

| System | First useful scope | Separate work before claiming support |
| --- | --- | --- |
| Debian 13 | Real i386 userspace build/dependency checks, distinguishing applications on amd64 from an experimental full i386 system | Available packages and SSE2 baseline; a separately selected 32-bit kernel/integration for a full system, or a separately implemented x86_64 kernel path |
| antiX | An explicitly selected release's 32-bit/glibc userspace, then its selected kernel | Exact source/headers/configuration and any older-kernel backport; device permissions and loader dependencies without assuming systemd; packaging for the selected init variant |
| Void Linux | A dated i686/glibc package snapshot and real userspace/dependency build | Matching kernel source/headers/configuration and integration; trusted source restoration without treating XBPS as Debian packages; later XBPS packaging and any required runit hooks |
| Other Linux/i686/glibc systems | The same ABI/toolchain/dependency checks against explicitly identified inputs | Their actual kernel/Mesa APIs, package format, CPU baseline and activation/rollback; no support inferred from a family name |

Use a release-specific `support/<release>/i686.json` path and a dated snapshot identity for a rolling
system such as Void. These are naming examples; no placeholder manifests or
empty port directories are provided. Reuse locks when inputs really are equal;
different directory names are not a reason to duplicate source pins or code.
Kernel and userspace identities remain separate even when supplied by one distro.

For **Debian 13**, choose the scope before creating the manifest. Debian's
[i386 release notes](https://www.debian.org/releases/trixie/release-notes/issues.html#reduced-support-for-i386)
state that there is no official i386 kernel or installer, that fewer i386
packages are available, and that the remaining userspace has an SSE2 minimum.
Check the complete dependency CPU baseline; compiling our code with `-march=i686`
does not lower the requirement of prebuilt libraries.

The ordinary amd64 system with i386 applications is a userspace compatibility
scope. The existing ELF32 kernel module cannot load into its amd64 kernel. A
full i386 system with a separately built kernel is an experimental configuration
requiring verified package availability, toolchain, boot/recovery and integration;
it must not be presented as Debian's stock i386 support. Neither scope is
implemented or qualified by the current manifest. The first next-step result can
be a real Debian 13 userspace build/dependency check without upgrading a working device.

For **antiX**, select the release and init edition rather than inferring them
from its Debian ancestry. For example, the official
[antiX 23.2 release](https://antixlinux.com/antix-23-2-released/)
offers 32-bit editions and SysVinit/runit variants. That is an example source,
not a claim about the newest release or our compatibility. Running a qualified
reference kernel on antiX qualifies only that combination; it does not qualify
antiX's stock kernel. An older stock kernel requires its own checked API/backport
work. Init-specific packaging must not enter the common driver or build logic.

For **Void Linux**, start with i686/glibc. Its
[installation handbook](https://docs.voidlinux.org/installation/)
lists an SSE2 CPU minimum and provides only glibc for i686; it does not provide
an i686 musl edition. Record a concrete rolling package/rootfs snapshot and the
actual toolchain and libraries. Its
[XBPS package manager](https://docs.voidlinux.org/xbps/index.html) and
[runit service model](https://docs.voidlinux.org/config/services/index.html)
belong to later packaging/activation work, not a new compiler or renderer.
Do not add a service merely to mirror a distro's init system. A musl or x86_64
port is a separate ABI/implementation scope, not an interchangeable manifest.

## CPU libraries

The standalone CPU checks need GNU Make, Git, Python 3, GCC with
UndefinedBehaviorSanitizer support, binutils, `pkg-config` and the libdrm development
headers. On Debian 12, install these build dependencies in the build environment:

```sh
sudo apt-get install build-essential git python3 pkg-config libdrm-dev
```

No vendor library, external SDK or hardware endpoint is needed for these
standalone targets. From the repository root, run:

```sh
make check-project check-project-tests check-tool-tests check-userspace
```

This runs documentation/catalog checks, Python regressions and the strict/UBSAN
C suites. To run only the C suites directly:

```sh
python3 -I -B tools/build-userspace.py --build-target host
```

The full `make check` additionally runs the [quality gates](tests.md#quality-gates)
and is **not a standalone check**. Those gates compile the module, Mesa archive
and DDX. Their prepared inputs are:

| Input | Required content |
| --- | --- |
| i686 build image | The selected Docker image and its pinned compiler, kernel headers and development libraries |
| Linux subset | The source files matched by the [kernel lock](../kernel/integration/linux.lock.json) |
| Mesa SDK | The matched Mesa source/build trees, generated headers, compile commands, Ninja link commands and dependency libraries |
| Xorg SDK | Headers and dependency packages matching the Xorg version in the [graphics lock](../src/gallium/graphics.lock.json) |
| Build-root metadata | The image identity and SDK locations consumed by the current gate loader |

The host also needs Docker and Ninja. `WORKSPACE` selects that prepared build
root; `POULSBACK_WORKSPACE` supplies the default, otherwise the loader searches
above the checkout. The [gate loader](../tools/quality.py) expects a fixed input
layout: this variable changes its base, not the SDK or image configuration.
There is no public provisioning manifest or standalone setup command for that
layout, and the matching image metadata and SDK bundle are not shipped here.
Installing CPU dependencies or setting `WORKSPACE` does not create them. This
is an open build-tooling limitation. If a matching prepared root is already
available, the repository command is:

```sh
make check WORKSPACE=/path/to/prepared-build-root
```

Use the standalone targets above when those inputs are unavailable. Neither
route starts GPU work.

The CPU builder reuses objects in the same `--output` directory when the GNU
compiler, options, environment and compiler-reported dependencies match.
Strict and UBSAN objects, and components with different options, stay separate.
Every invocation scans dependencies again, relinks and runs the selected tests;
an object hit is not a cached PASS. Other compilers keep the uncached build path.
The output directory is locked against competing builders, and `runs/` retains
each invocation's report and command output. Remove `objects/` for a clean build.

After the CPU builder's command timeout, cleanup has a separate bounded allowance:
up to 200 ms for TERM, 200 ms to drain output after KILL, and 200 ms to
reap the direct child if needed. The report records `reaped` and
`output_complete` independently. Partial output is retained when an escaped
descendant keeps a pipe open; that descendant is outside the owned process
group. A timed-out command fails even if its direct child returned zero.
These bounds cover subprocess waits, not filesystem latency or OS scheduling.

`build-userspace.py` runs C suites. Python project and tool regressions are
separate Make targets, included by both the standalone sequence and `make check`.

CPU test results establish only the tested contracts. They do not qualify
hardware acceleration or a graphics/video API.

`--component vertex-dma --component vertex-use` selects the separate bounded
vertex word projections and their CPU checks. They provide no GPU upload path.

## Kernel dependencies

The current target is Debian 12 i386 with kernel `6.1.0-54-686-pae`, Debian
package version `6.1.190-1`. Install the matching headers, kbuild/compiler
dependencies, `libdrm-dev`, `pkg-config`, `patch`, and build tools in the build
VM. A current kernel meta-package does not guarantee this exact ABI.

In a native i386 build environment, install the exact headers and compatible
build tools through authenticated Debian package indexes:

```sh
sudo apt-get update
sudo apt-get install \
  linux-headers-6.1.0-54-686-pae=6.1.190-1 \
  linux-headers-6.1.0-54-common=6.1.190-1 \
  linux-kbuild-6.1 linux-compiler-gcc-12-x86
```

The headers supply the exact ABI inputs. The header package requires
`linux-kbuild-6.1 >= 6.1.190-1` and `linux-compiler-gcc-12-x86`; let APT resolve
those compatible tools. They are not tied to the image's exact package revision.
Record the installed tool versions with the build, for example:

```sh
dpkg-query -W linux-kbuild-6.1 linux-compiler-gcc-12-x86 gcc-12
```

The builder checks the supplied headers and source, not the build host's running
kernel release. Executing the resulting module separately needs the matching
kernel image and its module dependencies; a container build does not establish
that runtime environment.

If an active mirror no longer carries the pinned headers, retrieve
them and their dependencies through [Debian Snapshot](https://snapshot.debian.org/package/linux/6.1.190-1/)
and verify them against signed repository metadata. Do not substitute a newer
ABI or bypass the builder's lock checks. The source fetcher below restores the
Linux source subset; it does not fetch or install these binary packages.

`kernel/integration/linux.lock.json` pins the external Linux package, source
archive, 58 source inputs, header configuration, Module.symvers and utsrelease.
External source and build outputs stay outside this checkout.

Fetch the locked source explicitly, or use an already downloaded package:

```sh
python3 tools/fetch-kernel.py --build-target debian-12-i686 --output /path/to/new/linux-inputs
# Optional: --package /path/to/linux-source-6.1_6.1.190-1_all.deb
```

The fetcher verifies identities and extracts the required source subset. It
does not install a package or execute its scripts. It writes `linux/` below
the new output directory.

```sh
python3 tools/build-kernel.py \
  --build-target debian-12-i686 \
  --headers /usr/src/linux-headers-6.1.0-54-686-pae \
  --kernel-source /path/to/new/linux-inputs/linux \
  --output /path/to/new/kernel-build --target physical --jobs 2
```

This builds the Linux/KMS integration module `poulsback.ko`; it does not
install or load it. The module depends on the matching kernel's standard
`drm_shmem_helper` and `gpu_sched` modules. An initramfs or module-loader
package must include that dependency closure; the build command does not
install it on a device.

The locked i386 build records compiler stack usage and retains every warning.
Before accepting a module, its mandatory gate checks every ordinary C object:
missing or malformed records, unbounded dynamic frames, and a per-function frame
of 4096 bytes or more refuse publication. Empty records are accepted only for
objects whose ELF symbols prove they contain no functions. The kernel's
1024-byte warning threshold stays enabled.

The module also has a cumulative gate. For every module entry point it sums the
GCC frames along the deepest chain of emitted direct calls inside the module,
plus the IRQ route's executor callback; inlined wrappers require a verified
direct call and missing records never imply zero use. The limit is 6144 bytes:
the qualified 8192-byte i386 stack less an explicit 2048-byte kernel reserve
policy for indirect calls and kernel frames, which is not a measurement.
Missing, ambiguous or unbounded evidence refuses publication. The build retains
the object identities, call paths, stack sums and failure diagnostics.

Each staging directory retains the project's licence and notices under
`licensing/`; their identities are checked and recorded alongside the per-file
terms in the build report.

The module's DRM and PCI driver name is `poulsback`. It has one build: no
compile-time selectors, CPU fixture module or experiment profiles.

## Kernel checks

`make check` builds the kernel host suites; the
[kernel host tests](../tests/kernel/README.md) list them with the separate
checks on the patched Linux bodies. The [kernel runtime procedure](tests.md#kernel-runtime)
owns module verification.

## Mesa

The [graphics source lock](../src/gallium/graphics.lock.json) records the Mesa,
mesa-demos and Xorg inputs of the matched SDK; the [graphics component](graphics.md)
owns the overlay. The overlay (`src/gallium/mesa/poulsback`) builds inside that
Mesa tree, with GLX, EGL, GBM and DRIL. Its `meson.build` holds the driver's
library list and host fixtures. A driver built for another Mesa internal ABI
is not a supported substitute.

The public source needed for this component is in the overlay, shared
`src/compiler` and `src/raster` libraries, UAPI headers and
[retained Mesa patch](../src/gallium/mesa/patches/nir-const-array-budget.patch).
The overlay's `meson.build` specifies the archive and Mesa fixtures, including
`POULSBACK_SELFCHECK=0` for the product library. Target flags are in the
[reference manifest](../support/debian-12/i686.json).

A prepared Mesa SDK can supply its generated headers, compiler flags and
libgallium link command to compile this tree's archive and relink it. A runtime
also needs compatible GLX, EGL, GLES and GBM libraries from the same build.
The DDX sources in `src/xorg` need the matched Xorg and pixman headers and libdrm.
The [quality gates](tests.md#quality-gates) compile these components with a
prepared build root, but do not create an installable graphics runtime.

This repository currently ships neither a Mesa/DDX fetch/build entrypoint nor
a validated recipe to recreate the complete SDK and isolated runtime from
scratch. The graphics lock identifies inputs; it does not supply generated
files, parent Mesa registration changes, build metadata or a package recipe.
A complete standalone graphics build and the Mesa fixture runner remain gaps.
Do not substitute a differently built system Mesa or treat successful archive
compilation as runtime qualification. [Tests](tests.md#graphics-apis) describes
what the fixtures must verify and which CPU checks are runnable here.

## Debian 12 i386 VM

Use an x86 Linux host with QEMU/KVM and a Debian 12 **i386** guest. An amd64
guest with a 32-bit compiler is useful for some userspace checks, but does not
replace a native i386 environment. The laboratory uses local QEMU/KVM
for builds and a Dell Inspiron 1210 for physical checks; no private VM image is
required for the public CPU tests.

### Create and install the guest

On a Debian-based host, install `qemu-system-x86` and `qemu-utils`, and verify
that your user can access `/dev/kvm`. Download a Debian 12 i386 installer from
the [Debian image archive](https://cdimage.debian.org/cdimage/archive/).
Verify the ISO using Debian's signed checksum files and
[verification instructions](https://www.debian.org/CD/verify).
The [i386 installation guide](https://www.debian.org/releases/bookworm/i386/)
describes the installer choices.

In a new directory outside the checkout, create a sparse virtual disk:

```sh
test ! -e build-i386.qcow2 || exit 1
qemu-img create -f qcow2 build-i386.qcow2 48G
```

Set `DEBIAN_I386_ISO` to the verified ISO's absolute path, then run on the host:

```sh
qemu-system-i386 \
  -machine pc,accel=kvm -cpu host -smp 8 -m 16384 \
  -drive file=build-i386.qcow2,if=none,id=os,format=qcow2 \
  -device virtio-blk-pci,drive=os,disable-modern=on \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2230-:22 \
  -device virtio-net-pci,netdev=net0 \
  -cdrom "$DEBIAN_I386_ISO" -boot once=d
```

Port 2230 is an example; select an unused loopback port. Install a minimal
system with SSH and standard utilities, without a desktop. Give the build
account sudo access inside this disposable guest. After installation, start
with the same command minus `-cdrom` and `-boot`; connect using
`ssh -p 2230 YOUR_GUEST_USER@127.0.0.1`. No PCI or USB passthrough is required.

The 48-GiB disk is a capacity allowance, not an initial 48-GiB allocation.
Sixteen GiB RAM and eight vCPUs leave room for builds; each 32-bit process and
the kernel's low-memory zone still have their own limits. QEMU's
[invocation reference](https://www.qemu.org/docs/master/system/invocation.html)
describes these device and memory options.

### Prepare and identify the environment

Run inside the guest:

```sh
sudo apt-get update
sudo apt-get install \
  build-essential git python3 python3-venv python3-mako python3-yaml \
  python3-packaging pkg-config libdrm-dev patch binutils kmod ccache \
  ninja-build meson bison flex libexpat1-dev zlib1g-dev libzstd-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxrandr-dev libxrender-dev \
  libxshmfence-dev libxxf86vm-dev libxcb1-dev libxcb-shm0-dev libxcb-glx0-dev \
  libxcb-dri2-0-dev libxcb-dri3-dev libxcb-present-dev libxcb-sync-dev \
  libxcb-xfixes0-dev libxcb-randr0-dev libpixman-1-dev libelf-dev libudev-dev
```

Install the [exact kernel headers](#kernel-dependencies), then identify the
build userspace:

```sh
dpkg --print-architecture
python3 -c 'import struct; print(struct.calcsize("P") * 8)'
```

The expected values are `i386` and `32`. The kernel builder verifies the supplied
headers' `.config`, `Module.symvers` and `utsrelease.h` against the lock; a
matching running-kernel name would not replace those checks.

These packages prepare a general i386 development environment. They do not
recreate the prepared [Mesa/Xorg SDKs](#mesa). A future from-scratch SDK build
must also select a Meson version compatible with the locked Mesa source and
record its complete configuration and dependency closure. The current relink
route uses the SDK's existing build metadata.

Clone the repository in the guest and select the commit being checked. Create
a separate output parent, then follow the [CPU](#cpu-libraries),
[kernel](#kernel-dependencies) and [Mesa](#mesa)
recipes. The [separate checks on patched Linux bodies](tests.md#kernel-runtime)
need their selected inputs; no kernel fixture module is loaded. The guest has
no SGX535, so those results have CPU scope.

### Resources and reproducibility

Limit all concurrent builds in one guest to eight total jobs. Check `MemAvailable`, `LowFree` and
the Normal zone in `/proc/zoneinfo`; adding guest RAM does not remove i386
low-memory constraints. Keep at least 2 GiB free in the output filesystem before a run
and allow additional space for its expected outputs.

For an optional compiler cache, run `ccache --max-size=2G` in the guest. Check
`df -h` in the guest and `du -h build-i386.qcow2` on the host; deleted guest
files do not necessarily shrink the host image. Remove only disposable build
outputs whose results and unique inputs have been saved. Stop the VM before
making a whole-disk archival copy; a copy of a running disk is not a consistent
snapshot.

Keep the selected commit and any diff, ISO identity, QEMU command/version,
`dpkg-query -W` package manifest, compiler/Meson versions, source locks, build
reports and test outputs together. Record a Python environment's package list
as well if one was used. These records let another developer recreate the
tested inputs; a fresh install from moving package mirrors alone cannot promise
byte-identical binaries.

## Target flags and outputs

Use explicit i686/target flags rather than `-march=native` for laptop artifacts.
The build VM exposes a modern host CPU. Keep qualification runs in new output
directories and retain command, source, toolchain and result identities.

Retain compiler flags, source identities, the unstripped binary and its build
report when resolving device addresses. Debug-prefix mappings must be recorded
with the build rather than assumed from another tool's output layout. The
CPU builder's [object reuse](#cpu-libraries) never reuses a test verdict.

Installation, boot images and hardware trials are separately qualified steps.
A successful compilation alone is not permission to dispatch GPU work.

## Source formatting

Formatting is a review aid, separate from compiler warnings and correctness
tests. Ruff also checks Python imports, names and common errors. Use four spaces
in Python and userspace C/C++, and kernel-style tabs
with width eight under `kernel/`, including C fragments in `.inc` files.
`.editorconfig`, the root and kernel `.clang-format` files, and `ruff.toml`
define those defaults. Keep changed files consistent with them.

Use [Ruff](https://docs.astral.sh/ruff/formatter/) 0.11.13 for Python and
clang-format 23.1.1 for C/C++. They can run on the development host; they do not
need to run in the i386 guest. Install them in an isolated development environment;
neither is required by the driver build. Check only the files being reviewed:

```sh
ruff format --no-cache --check tools/check-project.py
ruff check --no-fix --no-cache tools/check-project.py
clang-format --dry-run --Werror src/compiler/usse.c
# Treat an included C fragment as C when checking it directly.
clang-format --assume-filename=kernel/fragment.c --dry-run --Werror \
  kernel/integration/poulsback_gem_hold.inc
```

Use `ruff format --no-cache FILE` or `clang-format -i FILE` to apply formatting
to an agreed file set, then inspect the diff. Preserve numeric tables, shader
fixtures, macro boundaries, ABI declarations and exact-source test extracts.
Include ordering and comment reflow are disabled in the C configuration;
Ruff preserves quote style and does not rewrite code examples in docstrings.
Do not combine a repository-wide format sweep with active implementation work.
Source-sensitive fixtures still need their affected checks after formatting.

The developer commands `make lint`, `make format` and `make format-check` share
one file selector. Pass `FILES="path/to/file.py kernel/component.c"` for your
owned changes; without `FILES` they select staged sources only. `lint` runs the
existing Ruff rules without fixes. `format-check` checks Python/C/C++ formatting
without writes; `format` changes only the selected working files and never
stashes, stages or commits them. C correctness diagnostics remain in strict
component builds. Set `RUFF` or `CLANG_FORMAT` to an executable path when the
pinned development tools are outside PATH. These tools are not build or Dell
dependencies.

The selector recognizes C++ sources and headers as well as C and included C
fragments. An explicitly requested recognized source must reach its formatter
or produce a refusal; it must not disappear silently from the selection.
C++ support does not broaden explicit/staged selection into an automatic scan
of third-party trees.

For prose, use short paragraphs and headings that describe the interface or
operation. Long guides have a small set of local links; short README files do
not need a full table of contents. Keep existing heading anchors when adding
subsections. Use Mermaid for connected flows and text blocks for commands or
literal layouts. The [documentation checks](tests.md#selecting-checks) validate
local paths, anchors and case metadata; they do not assess prose quality or
verify external websites.

Markdown uses `.markdownlint.json`: prose lines target 100 columns; command
blocks and tables keep their natural width. Repeated headings are allowed under
different parents. The permitted HTML is limited to logo images/centering and
collapsible checklist details. Check the documentation with:

```sh
npx --yes markdownlint-cli2@0.23.3 '**/*.md'
```

Edit README, checklist, image descriptions and status text directly in Markdown.
For acceptance changes, also run the project checks above; Markdown lint alone
does not check requirement IDs, dependencies or result scopes.
