# PSB 4.42 reference (Ubuntu 10.04, Dell Inspiron 1210)

The 2D target is the better rate of [EMGD](emgd-reference.md) and PSB per
operation. Measured on 2026-11-02 on the Dell Inspiron 1210, using the Ubuntu
10.04 Live environment: kernel 2.6.32-74, `psb.ko` 4.41.1, Xpsb 5.0.1 and
X server 1.7.6. The package/release name 4.42 and the module's own version
string are separate identities; [source profiles](../re/sources.md#psb-source-and-runtime-profiles)
identify the retained source and deployed-stack boundaries.

The x11perf and gears comparison used the EMGD reference's executable set; the
[X11 Render](#x11-render-2026-11-03) tables used each live image's own x11perf
(x11-apps 7.6 on EMGD, 7.5 on PSB). Follow the EMGD reference's
[self-contained measurement method](emgd-reference.md#measurement-method) and
record the exact tool bytes and settings for a new comparison. Raw measurement
logs and original automation are not bundled here; this page preserves the
summary and its limits, not an independently reproducible result archive.

- **2D engine use:** the retained command-buffer observation found no submission
  by PSB's X in 45 phases, which points to CPU paths for these x11perf rates on
  the same Atom; it does not establish the behavior of every PSB operation.
- **rendercheck:** PSB's X crashed in the blend test (a8r8g8b8; a segfault in
  `libexa` under `CompositePicture`, no kernel error), so PSB has no passing
  rendercheck set; the corresponding EMGD selection passed 18 of 18.
- **Collection limits:** the x11perf wide step ran alone in a restarted X after
  that crash. A non-ASCII kernel-log byte stopped the capture daemon before it
  was patched for the boot; the early steps retained the kernel stream and
  netconsole but lacked the health log.

## x11perf (per second; target = the better of EMGD and PSB)

| Operation | EMGD | PSB | Target | Better |
| --- | --- | --- | --- | --- |
| Scroll 500x500 pixels | 143 | 49 | 143 | EMGD |
| Copy 10x10 from window to window | 15500 | 20700 | 20700 | PSB |
| Copy 500x500 from window to window | 144 | 32.5 | 144 | EMGD |
| 10x10 rectangle | 17400 | 241000 | 241000 | PSB |
| 500x500 rectangle | 194 | 186 | 194 | EMGD |
| 500x500 stippled rectangle (8x8 stipple) | 87.2 | 111 | 111 | PSB |
| 500x500 tiled rectangle (4x4 tile) | 30 | 66 | 66 | PSB |
| 500-pixel line segment | 18600 | 26500 | 26500 | PSB |
| 500-pixel line | 18700 | 27000 | 27000 | PSB |
| 500-pixel solid circle | 36 | 228 | 228 | PSB |
| Char in 80-char line (6x13) | 861000 | 719000 | 861000 | EMGD |
| Char in 70-char line (8x13) | 821000 | 629000 | 821000 | EMGD |
| Copy 500x500 from pixmap to window | 169 | 176 | 176 | PSB |
| Copy 500x500 from window to pixmap | 229 | 53.1 | 229 | EMGD |
| Copy 500x500 from pixmap to pixmap | 293 | 414 | 414 | PSB |
| PutImage 500x500 square | 211 | 63.6 | 211 | EMGD |
| ShmPutImage 500x500 square | 865 | 175 | 865 | EMGD |
| ShmGetImage 500x500 square | 93.2 | 34.5 | 93.2 | EMGD |
| GetImage 500x500 square | 56 | 28.9 | 56 | EMGD |
| Composite 500x500 from window to window | 144 | 43 | 144 | EMGD |
| Composite 500x500 from pixmap to window | 170 | 175 | 175 | PSB |

## rendercheck

- EMGD: 18 tests passed of 18 total
- PSB: Beginning blend test on a8r8g8b8

## X11 Render (2026-11-03)

PSB's X on Ubuntu 10.04, using the
[EMGD Render measurement settings](emgd-reference.md#x11-render-2026-11-03):
x11perf from x11-apps 7.5 with Xft, rendercheck 1.6 and the same separate
Render probe. That probe's source is not bundled; its rates retain the
reproduction limitation described with the EMGD results.

| x11perf test | PSB | EMGD | Target (the better) |
| --- | --- | --- | --- |
| aa10text, aa24text | 75,500, 37,700 | 88,200, 43,300 | EMGD |
| a10text, a24text | 74,700, 37,800 | 88,600, 43,300 | EMGD |
| rgb10text, rgb24text | 74,800, 34,300 | 27,200, 8,500 | PSB |
| aatrap10, 100 | 41,000, 6,150 | 44,800, 7,110 | EMGD |
| aa1trap10, 100 | 54,200, 39,800 | 105,000, 20,100 | EMGD, PSB |
| aatrap2x10, 100 | 45,500, 6,730 | 51,500, 7,520 | EMGD |
| aatrapezoid10, 100 | 13,700, 1,240 | 7,450, 2,410 | PSB, EMGD |
| compwinwin10, 100 | 29,400, 1,030 | 17,300, 4,010 | PSB, EMGD |
| comppixwin10, 100 | 42,200, 3,860 | 16,700, 4,290 | PSB, EMGD |

rendercheck:
- fill, dcoords, scoords, mcoords, tscoords, tmcoords and repeat passed;
- then PSB's X died in gradients (exit 1);
- the composite, cacomposite and blend ops and the other groups never ran
  (the earlier PSB measurement's X died in blend).

The separate Render probe ran in a restarted X until X died again after
`opacity500-win`:
- `over-argb32-win` 9,189/s;
- `over-argb500-win` 54.9/s;
- `over-argb500-pix` 250.5/s;
- `opacity500-win` 53.8/s.

PSB's X composites 500×500 ARGB at about a fifth of EMGD's 3D-transfer rate.
No PSB X submission was seen in the x11perf phases, which points to CPU paths;
engine use for this Render run is not recorded, and Xpsb also contains a 3D
composite producer ([native 3D](../native-3d.md#legacy-geometry-producers)).

## gears (median FPS of the timed runs)

| Program, size | EMGD | PSB |
| --- | --- | --- |
| gears85 300x300 | 317 | 255 |
| gears85 300x300-default-swap | 316 | 254 |
| gears85 600x600 | 98 | — |
| gears85 640x640 | 87 | 162 |
| gears85 fullscreen | 46.9 | 66 |
| metrics 300x300 | 315 | 264 |
| metrics 600x600 | 97.8 | — |
| metrics 640x640 | 87.3 | 163 |
| metrics fullscreen | 47 | 64.9 |
