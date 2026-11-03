# Captured EMGD vertex path

This chapter explains the captured vertex path of Linux EMGD 1.8 Build 2032
on SGX535. The matching OGL, Services and kernel hashes are in
[source profiles](../sources.md#linux-emgd-18). The
[static input/W investigation](../sgx535/edge-w-contract.md) supplies a separate
producer chain.

## Captured inputs

The corpus includes glxgears and vertex scenes covering triangles, VBOs,
clipping, points, lines, flat shading, varyings, uniforms and fixed-function
lighting. Memory observations contain VDM, vertex/state PDS, SECONDARY blocks,
USE programs, vertex data and indices. Raw snapshots and complete decoder
reports remain outside the public releases
([evidence](../evidence.md#what-remains-unavailable)). Captured vendor behavior
does not qualify poulsback's implementation.

## Control stream and PDS

| Object | Captured contract |
| --- | --- |
| STATE | Type 1; word 0 has LASTTASK, CMPLX and a PDS address in 16-byte units. Word 1 separates data size, MTE_EMIT, SD, USE pipe, partition count, SECONDARY and USE attribute allocation. |
| INDEXLIST | Type 2; primitive and count are followed by an index byte address and the selected optional words. Word 2 carries provoking vertex, index width and offset. Words 4/5 name vertex PDS, partition size, output size, advance, attribute allocation and data size. |
| Last STATE and TERMINATE | The last state task emits the terminate marker and tile extents; TERMINATE ends the stream. They are distinct objects. |

Vertex PDS data uses rows of eight DWORDs per bank. The captured program
multiplies the vertex index by each stream's stride, DMAs its attributes and
issues the USE task. DOUTD1 separates DWORD count minus one, line count minus
one and the first PRIMARY register. A constant attribute can select element
zero independently of the vertex index.

USE task fields are not interchangeable counts:

| Word | Meaning in this SGX535 profile |
| --- | --- |
| DOUTU0 | Code-base index, COFF and 16-byte EXE entry; separate iterator, texture and PDS-DMA dependencies. |
| DOUTU1 | Optional second/third phase entries, punch-through mode, per-instance mode and low TEMP-count bits. |
| DOUTU2 | High TEMP-count bits and SDSOFT. |

The captured gears and lighting programs use a short first phase followed by
a second phase named in DOUTU1; nine temporaries are declared. This joins the
phase entry to actual captured code. It does not make an arbitrary instruction
decoder a numerical or execution oracle.

## State and output convention

Each state block starts with a presence mask; groups follow in bit order:
front/back ISP, pixel PDS, region clip, viewport, wrap, output selects, W clamp,
MTE control, terminate, texture sizes and formats. EMGD sends changed groups,
rather than rebuilding every state for every draw. Its state USE copies 24
DWORDs, requiring 96 bytes of PRIMARY allocation even for a shorter payload.

Ordinary user vertex programs emit clip-space position. Their MTE state clears
PRETRANSFORM and supplies six viewport floats. Window rendering uses negative
Y scale; at 256×256 the X/Y centre is 128, scales are +128/−128, and Z centre
and scale are 0.5. Clear geometry is pretransformed and supplies no viewport.
Captured MTE state enables W clamp and clipped edges; its per-draw clamp is
1e−7, distinct from the 1e−6 global register value.

| State | Captured mapping |
| --- | --- |
| Culling | In the window orientation, GL back-face culling with CCW front maps to MTE CW (1); front culling maps to 2. Reversing front-face sense swaps them. |
| Flat triangle colour | INDEXLIST provoking vertex 2, MTE SHADE 3 and the colour DOUTI's FLATSHADE 2 act together. Texture-coordinate iterations remain Gouraud. |
| Points and lines | Points use sprite geometry and a point-size output; lines use their own ISP object type. These are not triangle mode bits. |
| Gears edge triangles | Three indices plus an edge-flag word describe each triangle; bits 8–10 mark visible edges. Count includes those words. |

These captures establish the selected emitted convention. Every private SGL
utility's output remapping, exact shared-edge rule, unequal-W domain and
interpolation/D24 numerical model remain separate contracts.

## Partitions and SECONDARY constants

The captured draw shapes fit this output-partition rule: round output DWORDs up to four, take `min(16, 64 / size)` vertices per
partition, encode that count minus one as PARTSIZE, and use
`min(PARTSIZE, 3)` for advance. The 64-DWORD output partition is separate from
DMS_CTRL's count of simultaneously allocated vertex partitions.

SECONDARY is a state task per USE pipe. Its PDS DMA fills SA registers and
launches an empty USE task; DMA is divided into lines of at most 16 DWORDs.
In the captured GLSL triangle, SA0 is the memory-constant base, SA1–2 are
reserved, matrix uniforms occupy SA3–18, a vector SA19–22 and a literal SA23.
The dynamically indexed array scene uses SA0 to name memory constants and a
USE load/wait sequence. Fixed-function lighting has a different 96-DWORD
layout. Neither layout can be substituted for the other's allocation budget.

The [fragment capture](fragment-path.md#isp-state-and-epilogues) supplies the
corresponding pixel SECONDARY layout. The older
[PSB producer](../psb/pds-use-and-depth.md#ordinary-fragment-secondary-constants)
is an independent static construction with different reserved-register rules.

## Limits

During these GL phases, EMGD's X server presents each frame with one native
2D copy, after rendering, and submits no TA work of its own
([X11 routing](../native-2d.md#captured-x11-routing)). These captures do not
qualify cross-client scheduling, final resource release or a new presentation
path. Uncaptured strip/fan, flat-line, resource-orientation and larger-program
cases keep their own boundaries. Current poulsback contracts belong to
[native 3D](../../native-3d.md), [graphics](../../graphics.md) and
[hardware](../../hardware.md).
