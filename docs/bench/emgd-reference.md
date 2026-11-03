# EMGD 1.8 reference (Ubuntu 11.04, Dell Inspiron 1210)

Baseline measurements on the Dell Inspiron 1210, recorded on 2026-11-01,
with X on the panel and one application workload at a time. These are retained
measurement summaries, not qualification of the current poulsback source.
Raw measurement logs and the original automation are not bundled with this
checkout. The [source profiles](../re/sources.md#linux-emgd-18) identify the
legacy stack; the [public evidence index](../re/evidence.md) identifies the
separate reference and Live releases.

## Measurement method

For GL rates, disable the swap interval, run one warm-up, then take three
60-second measurements at each size. Keep X at 1280×800 and the governor and
clocksource recorded below. Preserve the program's full command, executable
identity, renderer/API strings, stdout/stderr, elapsed time and actual exit
status. Record `/usr/bin/time -v`, `/proc/stat` and `/proc/interrupts` before/after,
plus the X log, kernel log and system identity. CPU utilization and interrupt
rates need the same measurement interval as FPS.

The [benchmark sources and build instructions](../../benchmark/README.md)
include unchanged gears 8.5.0, separately instrumented gears 9.0.0, poulscube,
glmark2, x11perf and rendercheck. The Mesa perf programs behind the
[OpenGL rates](#opengl-rates-mesa-perf-programs) table are not bundled; those
rows are reference summaries without an in-tree reproduction. For example, after
building a bundle, one instrumented measurement is:

```sh
DISPLAY=:0 build/benchmark/bundle/bin/3D/glxgears-metrics \
  -geometry 640x640 -swapinterval 0 --duration 60
```

Repeat with fresh result records and each documented window size. The bundle's
standard `3D/metrics` case lasts ten seconds; raising the runner timeout does
not change that workload duration. Its normal build uses the current host ABI;
a legacy Ubuntu 11.04 comparison additionally requires a compatible i386 build
and runtime. The original baseline used executables built for glibc 2.13, with
GLEW 1.5 and GLUT 3 where required. A new build is a new comparison input and
must record its identity. Use identical executables and settings on both stacks.

## System

- **CPU:** Atom Z520 at 1.33 GHz (one core with HT), `ondemand` governor at
  800/1067/1333 MHz, HPET clocksource, 1 GiB.
- **Software:** kernel 2.6.38-16, EMGD 1.8.0.2032 (the legacy_live_1.0 live image,
  `performance-emgd-v1`), X server 1.10, panel 1280×800.
- **GL:** renderer "EMGD on PowerVR SGX535", OpenGL 2.1, direct rendering;
  ES2 reports "EMGD on PowerVR SGX 535". The GPU interrupt line is
  `PowerVR`.

## glxgears

Instrumented gears 9.0.0 (`glxgears-metrics`). FPS is application swap
cadence, not presented frames. CPU is the whole system's busy share during
the run; IRQ is the GPU interrupt rate.

| Window | FPS (3 × 60 s) | Frame mean / p99 | CPU busy | GPU IRQ |
| --- | --- | --- | --- | --- |
| **300×300** | **315.3** | 3.17 / 3.9–4.0 ms | 33 % | 1573/s (≈5 per frame) |
| **640×640** | **87.3** (87.2–87.5) | 11.46 / 13.4 ms | 12 % | 427/s (≈5 per frame) |
| 600×600 | 97.8 (97.7–98.0) | 10.22 / 12.2 ms | — | — |
| fullscreen 1280×800 | 47.0 | 21.3 / 23.5 ms | 10 % | 235/s |

**Unchanged glxgears 8.5.0** (our acceptance input):
- 316.9 FPS at 300×300, 87.0 at 640×640, 98.0 at 600×600, 46.9 at
  fullscreen.
- With the default swap interval: 316.4 FPS. EMGD does not wait for vblank.

**EMGD is GPU-bound.** At 640×640 the CPU is 88 % idle. Vertex work runs on
the GPU.

## poulscube

poulscube 300×300 under EMGD 1.8 on the same Dell, measured on 2026-11-02:
**528.2 FPS** (527.4, 528.2, 530.5), frame mean 1.88 ms, p99 2.48 ms.

## OpenGL rates (Mesa perf programs)

| Program | Result |
| --- | --- |
| vertexrate | 5.6 M vertices/s with VBO DrawElements, 5.3 M with DrawArrays, 2.2 M in immediate mode |
| trispd | 1.27 M triangles/s (63 M pixels/s) |
| drawoverhead | 182 k draws/s (5.5 µs per draw); 161 k with a no-op state change; 113 k with a state change |
| fill | 1.3 G pixels/s for simple, blended and textured fill; 1.1 G with shader 1; 305 M with shader 2 |
| teximage | upload: RGBA 256² 154 MB/s, 1024² 155 MB/s, BGRA 256² 260 MB/s, RGB565 168 MB/s; readback (GetTexImage) about 34 MB/s |
| swapbuffers | swap only: 3.4 k/s at 320×240, 500/s at 640×480, 126/s at 1024×768; with a clear about 30 M pixels/s |
| fbobind | 520 binds/s |
| clearspd | 2.65 M clears/s. Its "4.2·10¹¹ pixels/s" is impossible for this GPU: EMGD coalesces redundant clears. This is not a fill figure. |

Not measured:
- `readpixels` blocked for 240 s at 1 % CPU (EMGD readback stall).
- `copytex` stopped on an incomplete FBO.
- `genmipmap` needs GL_ARB_framebuffer_object, which EMGD lacks.
- es2gears and glxheads ran but print no rate.

## X11 2D

- **rendercheck** (fill, blend; Src/Over; a8r8g8b8): 18 of 18 passed.
- **x11perf** (`-repeat 1 -time 3`), rates per second:

| Operation | EMGD | Operation | EMGD |
| --- | --- | --- | --- |
| 500×500 rectangle | 194 | 500×500 copy, window → window | 144 |
| 500×500 stippled rectangle | 87.2 | 500×500 copy, pixmap → window | 169 |
| 500×500 tiled rectangle | 30.0 | 500×500 copy, window → pixmap | 229 |
| 500-pixel line / segment | 18.7 k / 18.6 k | 500×500 copy, pixmap → pixmap | 293 |
| 500-pixel solid circle | 36.0 | scroll 500×500 | 143 |
| core text, 6×13 / 8×13 | 861 k / 821 k chars | PutImage / ShmPutImage 500×500 | 211 / 865 |
| GetImage / ShmGetImage 500×500 | 56.0 / 93.2 | Composite 500×500, window / pixmap → window | 144 / 170 |

A 500×500 fill at 194/s is about 48 M pixels/s, far below the 3D fill rate,
so EMGD's X 2D is a low bar. The x11perf used for this table had no Xft tests
(the bundled 1.7.0 build has them); Xft rates are in the
[X11 Render](#x11-render-2026-11-03) section. Ubuntu-era
reports put PSB 4.42 (Ubuntu 10.04) ahead of EMGD in 2D. The
[PSB measurements](psb-reference.md#x11perf-per-second-target--the-better-of-emgd-and-psb)
now give the per-operation comparison: the 2D target is the better rate of the
two, rather than a blanket claim that either legacy stack is faster.

## X11 Render (2026-11-03)

EMGD's own X on Ubuntu 11.04, no capture library, 1280×800.
These measurements use the following workloads and settings:

- **x11perf:** the live image's x11perf (x11-apps 7.6, with Xft; DejaVu fonts),
  `-repeat 3 -time 3`, the median of three repeats.
- **rendercheck:** 1.6 on `a8r8g8b8,x8r8g8b8,a8`; blend, composite and
  cacomposite one Render op a process.
- **Separate Render probe:** a desktop-operation probe, 3 s per operation. Its
  source and original command records are not supplied here, so the custom
  rates below are reference summaries without a public reproduction recipe.
  The included [benchmark cases](../../benchmark/README.md#selecting-checks)
  have their own documented scope. [PSB rates](psb-reference.md#x11-render-2026-11-03)
  use the same probe.

| x11perf test | EMGD | x11perf test | EMGD |
| --- | --- | --- | --- |
| aa10text, aa24text | 88,200, 43,300 | a10text, a24text | 88,600, 43,300 |
| rgb10text, rgb24text | 27,200, 8,500 | aatrap10, 100 | 44,800, 7,110 |
| aa1trap10, 100 | 105,000, 20,100 | aatrap2x10, 100 | 51,500, 7,520 |
| aatrapezoid10, 100 | 7,450, 2,410 | compwinwin10, 100 | 17,300, 4,010 |
| comppixwin10, 100 | 16,700, 4,290 | | |

| Render probe op | EMGD/s | Render probe op | EMGD/s |
| --- | --- | --- | --- |
| over-argb32-win | 8,905 | over-argb500-win | 263.9 |
| over-argb500-pix | 452.6 | opacity500-win | 241.5 |
| shadow500-win | 53.2 | mask10-win | 37,446 |
| ca100-win | 655.7 | linear500-win | 37.3 |
| radial500-win | 14.1 | scaled500-win | 6.4 |
| add-a8-10 | 79,565 | src-pix500-win | 474.1 |

rendercheck passed every group but two:
- composite, cacomposite and blend: all 38 ops, 152 of 152 checks each;
  fill 48/48, repeat 304/304, the coordinate groups, bug7366, gtk_argb_xbgr,
  libreoffice_xrgb, shmblend;
- triangles 240 of 456 fails, the same count as the Ubuntu 11.04 Xvfb
  comparison. This points to the X 1.10 server's rasterizer, but does not by
  itself isolate every failing triangle;
- gradients reached its 120 s limit with no result line.

The 13 groups took 219 s and the op groups 802 s.

## Caveats

- **Persistent logging was off.** The Live system's data partition was unclean
  and unmounted. RAM logs were copied to the host every minute; this collection
  limit remains part of the baseline.
- **The old record is not comparable.** The historical 84.8 FPS at
  600×600 (2026-10-15) used older gears and settings. The 97.8 here
  replaces it as a reference. 310 FPS at 300×300 was under apitrace.
