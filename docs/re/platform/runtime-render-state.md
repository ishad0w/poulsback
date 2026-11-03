# Captured EMGD render state, clocks and presentation

This chapter explains captured Linux EMGD 1.8 render state on the Dell
Inspiron 1210. The [source profile](../sources.md#linux-emgd-18) identifies its
matched libraries and module; the module SHA-256 is
`551ef2067798459e9e5b41275b2b27f97af5d2b4c4db6ef9fa32598473ed6335`.
ISR-time samples, host power calls and map flags supplement the
[BIOS/clock decoder analysis](bios-display-power.md#clock-transport-and-competing-decoders).
They describe this stack and workloads, not every stepping or poulsback's clock.
Raw traces and full reports remain separately retained
([evidence](../evidence.md#what-remains-unavailable)).

## Clock and power

The selected SGX EVENT_TIMER reload is 200000 core clocks. Counting HostCtl
TimeWraps over an interval without power-off gives
`frequency = delta_wraps * 200000 / elapsed_time`. This is a measurement method;
a BIOS selector, nominal source constant or CPU-frequency report is not the
same quantity.

The selected render profile has CLKGATECTL `0x00111111`, with SGX blocks
manual-on, and CLKGATECTLOVR zero. Host writes enable blocks at power-on and
clear them at power-off. Inter-frame power transitions depend on the workload;
this policy does not authorize a clock change or establish GPU power/temperature.

## Render state and memory

| Field | Captured gears value |
| --- | --- |
| USE_LD_REDIRECT / USE_ST_RANGE | 0 / `0xffffffff`. |
| TSP_CONFIG | 1. |
| Background object | BGOBJTAG contains an allocation-dependent object address; BGOBJ is `0x300` and BGOBJDEPTH F32 1.0 in the selected profile. |
| RENDBOX1 and RENDBOX2 | Both zero. |
| PIXELBE / IPFMISC | `0x00ffff80` / `0x000f0000`. |
| DMS_CTRL | Mostly `0x093f8200`, four vertex and four pixel partitions. |
| Tile extents | Last indices, not tile counts: ZLS extent 39 at 640 and 18 at 300; TE_SCREEN at 640 is `0x00027027`. |

Memory flags differ by heap: the captured 3D parameter and 2D mappings lack
CACHE_CONSISTENT, while some general, PDS and shader mappings are snooped.
CPU PAT attributes alone cannot describe the GPU's access path. These vendor
flags do not qualify poulsback's WB coherency or cross-engine handoff; those
have their own [hardware claims](../../hardware.md).

These values explain setup differences; they are not a register recipe to
apply without the associated program, address-space and resource contracts.
An ISR-time register is the last sampled value, so temporary changes within
a render may be invisible. The
[fragment/EOT contract](../emgd-linux/fragment-path.md#render-targets-and-msaa)
supplies the corresponding program words.

## Presentation

The captured EMGD displayclass flip and vblank-request paths are inactive under the
selected X stack: fullscreen and windowed GLX and fullscreen EGL presented by
copy. Fullscreen gears had the same measured rate at swap intervals zero and
one. Thus these observations supply no reference for hardware flip latching
or post-flip last use. Poulsback's KMS flip and old-buffer lifetime need their
own [display contract](../../kernel/display.md).

The [X11 routing contract](../native-2d.md#captured-x11-routing) identifies the
server's swap copy as native 2D. Copy presentation, application FPS, GPU time
and physical refresh are separate measures.

## Limits

Tracing adds observer cost. A clock calculation must retain its timer reload,
elapsed-clock domain and power-state assumptions; it does not resolve the
VBIOS/Linux decoder disagreement or qualify runtime PM teardown. Comparable
performance needs fixed application, size, formats, swap/presentation settings,
source identity and observer overhead
([measurement interpretation](bios-display-power.md#correctness-and-performance-need-separate-results)).
