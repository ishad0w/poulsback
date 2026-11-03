# Graphics benchmarks

Build and run graphics checks from local sources. The tools use the GL/EGL/X11
libraries of the driver stack being tested.

| Directory | Contents |
| --- | --- |
| `2D/` | rendercheck and x11perf sources, notices and selected cases. |
| `3D/` | Gears, metrics, poulscube and small geometry/lighting examples. |
| `OpenGL/` | GL/EGL examples, glmark2, required data and shared helpers. |
| `tools/` | The build script, single-case runner and local regression tests. |

## Build

Run these commands from the repository root. On Debian 12, install the build
and development packages first:

```sh
sudo apt install build-essential python3 patch binutils pkg-config meson ninja-build \
  libx11-dev libxext-dev libxrender-dev libxmu-dev libxmuu-dev libxft-dev libgl-dev \
  libglu1-mesa-dev freeglut3-dev libegl-dev libgles-dev libpng-dev libjpeg-dev

python3 -I -B benchmark/tools/build.py build --jobs 2
```

All program sources are in this repository. Building requires no source downloads
and does not run graphics tests. Meson builds the tools from the files in these
directories. Only the X11 GL and GLES2 variants of glmark2 are included. The metrics patch is
applied to a generated copy of gears; the two acceptance sources remain unchanged.

The output is `build/benchmark/bundle/`, with executables in `bin/2D/`, `bin/3D/` and
`bin/OpenGL/`. `--output build/benchmark/native-v1` selects another new directory.
Existing bundles are never overwritten. Build objects stay in
`build/benchmark/.work/` and are reused on subsequent builds. `--jobs` accepts 1–8.

The build records source and executable hashes, compiler options, ELF dependencies
and command output. Build on Debian 12 i386 for the Dell; a host x86_64 build has
its host ABI. Compatibility with Ubuntu 11.04 needs a separate build/runtime check.

## Run

List cases without opening a display:

```sh
python3 -I -B benchmark/tools/build.py list
python3 -I -B benchmark/tools/build.py list --group 2D
```

Use an existing X11 session and run one selected case:

```sh
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run 2D/rendercheck \
  --results build/benchmark/results/rendercheck-001
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run OpenGL/glmark2-build \
  --results build/benchmark/results/glmark2-001
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run 3D/gears --seconds 120 \
  --results build/benchmark/results/gears-window-001
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run 3D/gears-fullscreen --seconds 120 \
  --results build/benchmark/results/gears-fullscreen-001
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run 3D/heads --seconds 120 \
  --results build/benchmark/results/heads-001
DISPLAY=:0 python3 -I -B benchmark/tools/build.py run 3D/poulscube --seconds 120 \
  --results build/benchmark/results/poulscube-001
```

`--bundle` selects another built bundle. Each run needs a new result directory.
Logs include the command, executable hash, relevant environment, stdout/stderr,
elapsed time and actual exit status. GNU coreutils `stdbuf` preserves live output.

Interactive programs receive SIGINT after `--seconds` (default 30). Finite
programs exceeding that interval are recorded as `TIMED_OUT`. A process that
remains blocked is recorded as `STILL_RUNNING`; stop submissions and retain
GPU-reachable resources according to the [device-loss policy](../docs/kernel/execution.md#the-loss).
The runner performs no reset or retry.

`--seconds` limits how long the runner waits. The catalog's glmark2 performance
scenes last three seconds, and metrics gears lasts ten seconds. A larger runner
limit does not extend those workloads. Changing a scene's duration creates a
different case configuration, which must be recorded with the result.

### Selecting checks

| Cases | What to examine |
| --- | --- |
| `2D/rendercheck` | Numerical results for the selected fill/blend tests, Src/Over operators and ARGB8888 format; check the summary and that tests actually ran. |
| `2D/x11-fill`, `2D/x11-copy` | Throughput of small/large fills, window copies and overlapping scroll; retain each operation's result. |
| `3D/lighting`, `clipping`, `culling`, `flat`, `arrays`, `elements` | Visual behavior of the named geometry/state operation, compared with a recorded reference. Lighting exercises fixed-function API behavior; vertex work may run on CPU. |
| `3D/gears`, `gears-fullscreen`, `heads` | Correct animation, resize/expose/input and multiple GLX connections; record actual directness and presentation separately. |
| `3D/poulscube`, `poulscube-fullscreen` | The neon logo scene described [below](#poulscube): crisp block edges, culling and depth while the pieces fly apart; the FPS lines for throughput. |
| `3D/poulscube-timed` | The summary of a fixed 24-second run: FPS, the frame time's median, 99th percentile and worst case, and CPU load. |
| `OpenGL/texture`, `texture-update`, `fbo`, `fbo-clear`, `shared-texture` | Sampling, texture updates, render-to-texture, clear and sharing; exercise the relevant interactive controls. |
| `OpenGL/blend`, `mask`, `scissor`, `stencil` | The corresponding visible state effect; preserve a screenshot or a precise observation. |
| `OpenGL/glmark2-build`, `glmark2-texture`, `glmark2-gles` | Performance of the selected scene and API, with its renderer and options. |
| `OpenGL/glmark2-validate` | The printed `Validation: Success`, `Failure` or `Unknown` result at 800×600. |

### poulscube

**What.** `3D/poulscube/poulscube.c` draws the poulsback terminal logo in retro
neon colours. The logo cube turns about its vertical axis at 70° per second,
seen from below as in the logo. It sits between the logo's terminal brackets,
which breathe (4 % larger and smaller every 3 seconds), under the `poulsback`
wordmark and its blinking magenta block cursor. The brackets and letters are
the logo's own pixel shapes (`assets/logo`: the terminal SVG's bracket
rectangles and `lettering.json`'s glyph rows), extruded into blocks. The cube
is 3×3×3 pieces in neon colours (hot pink, violet, indigo, cyan, mint,
chartreuse, yellow, orange). The colours flow along its diagonal, and the faces
glow. Every 8 seconds the cube shatters, turns one layer a quarter turn and
back, and closes again.
Opening, layer turns and closing use quintic easing, with continuous velocity
and acceleration at their boundaries. The scene, cycle length and GL draw
structure stay the same; captured poses belong to the animation version used.

**How.** The catalog runs it like gears: `poulscube` (windowed at `{size}`),
and `poulscube-fullscreen`, both with swap interval zero. Every five seconds
the program prints a line in gears' format, so existing FPS parsers read it
unchanged, followed by that interval's mean frame time and CPU load:

```text
<frames> frames in <seconds> seconds = <fps> FPS, frame <ms> ms, CPU system <percent>%, process <percent>%
```

It draws no metrics, so they cost nothing in the frame.

For deterministic frame export, `-record FILE` reads whitespace-separated
animation times in seconds from standard input. It renders each supplied time
and writes a complete binary PPM image, top row first, into one concatenated
stream. `FILE` can be a regular file or a FIFO. No swaps, wall-clock sampling,
FPS calculation or CPU statistics run in this mode. The current GL renderer
still renders every frame; record its identity with the output.

Optional `-record-fps FPS` gives the blinking cursor an independent playback
clock: exported frame index divided by FPS, plus a quarter-second phase.
FPS must be an integer from 1 to 1000 and requires `-record`. It describes
the export's intended playback rate, not measured rendering performance;
use the same rate when encoding the stream. The cube and brackets still
follow the supplied scene timestamps. Without this option, the cursor follows
those timestamps too, preserving the default export behavior.

The phase starts midway through the cursor's illuminated half-cycle. For
whole-second takes at normal video rates, reflecting the interior frames
then joins two half-pulses at the turn and wrap instead of holding a complete
on/off phase for a full second.

For example, after building the bundle and selecting an existing X display:

```sh
python3 -c 'for frame in range(60): print(format(frame / 30, ".17g"))' | \
  build/benchmark/bundle/bin/3D/poulscube -geometry 640x640 -record frames.ppm -record-fps 30
```

Each timestamp must be finite and nonnegative, at most `UINT_MAX * 8` seconds
to keep the existing effect-cycle counter representable. Empty streams,
malformed timestamps and read/write/GL errors fail with a nonzero exit status.
`-record` rejects combination with `-duration` or `-dump`. A failed export can
leave a partial output file. On success, one JSON line goes to standard error:

```text
{"poulscube_record":{"frames":60,"first_time":0,"last_time":1.9666666666666666,"cursor_fps":30}}
```

The `cursor_fps` field is zero when the option is absent. This is an animation
export summary, not a performance measurement. A video consumer supplies the
playback frame rate independently of the scene timestamps.

`poulscube-timed` is the case for comparing builds. It runs exactly 24 seconds,
three effect cycles, so the load repeats exactly. Then it exits and prints a
summary, which fits inside the runner's default 30-second limit:

```text
poulscube: <seconds> s, <frames> frames, <fps> FPS
poulscube: frame time ms: mean <ms>, median <ms>, p99 <ms>, max <ms>
poulscube: CPU: system <percent>%, process <percent>%
{"poulscube_summary": {"seconds": <seconds>, "frames": <frames>, "fps": <fps>, ...}}
```

The angle-bracket values above describe the output fields, not measured results.

The frame time runs from one swap's return to the next. The median and the
99th percentile (nearest rank) show stutter that the mean FPS hides. System CPU
is the average over all CPUs (`/proc/stat`); process CPU is this program's
user and system time over the run, where 100 % is one CPU. The last line
carries the same values as JSON for scripts, with the window size, swap
interval and effect setting. Options:

| Option | Effect |
| --- | --- |
| `-geometry WxH[+X+Y]`, `-fullscreen` | Window size, as in gears; the scene always fits the shorter side. |
| `-swapinterval N` | Swap interval through `GLX_EXT_swap_control` or `GLX_MESA_swap_control`; 0 for maximum FPS. |
| `-info` | Renderer, version, vendor and visual. |
| `-duration SECONDS` | Run that long, then print the summary above and exit with status 0. |
| `-noeffect` | Turning only, no shatter. |
| `-dump PREFIX` | The first frame after 1.0, 3.5, 5.0 and 6.5 s (solid, opening, mid-turn, closing) as `PREFIX-N.ppm`, read back with `glReadPixels`. |

Keys: Escape or `q` quits, `e` toggles the effect.

**Rendering structure.** The program batches geometry and computes its piece
placement and face colours on the CPU. The selected GL driver determines where
its vertex shader executes:
- **The cube is one draw.** Each frame the CPU places the 26 pieces in eye
  space: the shatter spacing, the turning layer, the cube's own turn and the
  logo pose. It keeps only the faces that face the eye, and drops the pieces'
  inner faces while the cube is closed and no layer turns. It writes them into
  one vertex and colour array for a single `glDrawElements` of triangles.
- **No GL lighting runs.** The one directional light (ambient 0.5, plus the
  neon faces' glow) is evaluated on the CPU and baked into flat colours: once
  for the static frame, and once per face per frame for the cube.
- **The static frame is culled once.** The brackets, wordmark and cursor are
  display lists built when the program starts. Each extruded shape keeps its
  front as greedily merged rectangles. Of its sides it keeps only the outline,
  merged into runs, and only the sides the eye can see from its fixed place.
  The static geometry uses 16 bracket and 141 wordmark quads.
- **A frame is three or four draws:** wordmark, cursor (half the time),
  brackets (scaled for the breathing) and cube.

The GL it uses is client vertex and colour arrays, display lists, flat
shading, the depth test, back-face culling, a frustum and double buffering.
There is no blending, texture or FBO. Its frames can be judged at a glance, and
the dumps compared with a reference image: wrong culling, depth order or
colours, or missing pieces, show at once against the logo.

In `texture-update`, an ordinary key updates the next texture row. In `fbo`,
`a` toggles animation and `s` advances rotation. In `shared-texture`, `t` changes
the shared texture. Some upstream demos return nonzero on Escape; keep the
actual exit status and the action taken rather than interpreting it as a GPU fault.

### Window size

Gears, metrics gears, poulscube and glmark2 performance cases default to **640×640**.
`--size 320x320` requests another size where the utility supports it. Fullscreen
uses the actual X screen size. The log records the requested size; check the
actual window size when comparing driver versions.

| Utilities | Fixed sizes or exceptions |
| --- | --- |
| `x11perf` | 600×600 work window; each operation has its own rectangle size. |
| `rendercheck` | Internal numerical test images, without a common viewport. |
| Heads, GLES triangle/gears, shared texture | 300×300 windows. |
| Small GLUT demos | Usually 250×250; blend 200×200, stencil/scissor 300×300. |
| FBO demos | 512×512. |
| glmark2 validation | Upstream forces 800×600 for this mode. |
| Information tools | No benchmark viewport. |

Some Mesa demos override GLUT's geometry option in their own initialization.
The catalog keeps their fixed sizes, and `--size` is rejected for those cases.
Use the same case and dimensions across drivers. Gears requests swap interval
zero through its native option; verify actual vsync behavior before comparing FPS.

## Offline use

Copy the whole bundle to the notebook. It includes the executables, runner,
catalog, data, sources, notices and `SHA256SUMS`. The notebook needs Python 3.11+
and the matching architecture's runtime GL/EGL/GLES/X11/GLUT libraries, plus
coreutils for the runner. Driver libraries are supplied by the installed stack.

From the copied bundle:

```sh
sha256sum -c SHA256SUMS
DISPLAY=:0 python3 -I -B tools/build.py run 3D/gears \
  --seconds 120 --results /tmp/poulsback-gears-001
```

On Debian 12 i386, check the loader and dependencies without opening a display:

```sh
for tool in bin/*/*; do
  /lib/ld-linux.so.2 --verify "$tool" && /lib/ld-linux.so.2 --list "$tool" || exit
done
```

## Reviewing and comparing results

For GLX tests, save `OpenGL/glx-info`; for GLES tests, save `OpenGL/gles-info`.
Record the actual libraries, renderer, API version and context/configuration.
Keep unsupported extensions or formats distinct from failures of an advertised
feature. Leave API/version overrides unset.

`EXITED` means the process returned zero; `STOPPED` means the runner ended an
interactive observation with SIGINT. Review the program output and image as well.
In particular, glmark2 can print `Validation: Failure` or `Unknown` and still
return zero. `Unknown` is not a validation pass.

Compare the same executable, catalog, dimensions, duration, swap settings and
display configuration across driver versions. Save a reference run and the
candidate run separately, including errors and visual observations. Performance
comparisons use one separate warm-up followed by three measured runs; report
the individual values, median and range. Keep power source, temperature and
background load comparable. Preserve failures even when a later run succeeds.

X11perf measures X operations, which may use CPU, native 2D or a 3D composition
path; native 2D execution
needs separate engine evidence. Numerical depth/interpolation and resource
release use the driver's own fixtures and oracles. Application acceptance stays
in `tests/acceptance/integration.md` in the driver repository: observation time,
active rendering time, GPU jobs and displayed frames are separate measurements.
These selected benchmark cases do not establish full API conformance.

## Licenses and local checks

[Root notices](../NOTICES.md#benchmark-tools) identify the upstream versions and license boundaries.
Project scripts use the existing root MIT license. Original third-party licenses
remain beside the selected source files. The standalone bundle carries the same
root MIT text.

```sh
python3 -I -B -m unittest discover -s benchmark/tools -p 'test_*.py' -v
make lint FILES="benchmark/tools/build.py benchmark/tools/test_build.py"
make format-check FILES="benchmark/tools/build.py benchmark/tools/test_build.py"
python3 -I -B tools/check-project.py check
```
