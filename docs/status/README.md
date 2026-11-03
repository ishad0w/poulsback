# Reading and maintaining status

The [README](../../README.md#status) gives the short overview. Each page below
lists what works, how it was checked, and what remains in progress or research.

- [2D](2d.md)
- [3D](3d.md)
- [Mesa / Gallium](mesa.md)
- [X11](x11.md)
- [OpenGL 2.1](opengl-21.md)
- [OpenGL ES 2.0](opengl-es-20.md)
- [Video / VA-API](video.md)
- [Wayland](wayland.md)
- [OpenGL 3.0](opengl-3.md) and [Vulkan](vulkan.md) research
- [Legacy-driver knowledge](legacy-drivers.md)

The [combined Mesa/API entry](mesa-api.md) remains a route to the separate pages.

## Updating status

Edit these Markdown lists directly when a checked capability or blocker changes.
Keep each item short and link to the component guide for details. Update the
README when the overall status changes. Its table gives the state and checked
scope of each area. Each area's **Progress basis** states its target, weights,
current contributions and component/acceptance references. Keep that basis and
the README bar synchronized when the implementation or qualification changes.

The 2D and 3D pages use weighted implementation blocks. Completed foundations,
partial library implementations, product integration and qualification have
their stated contributions. Host-only code keeps its host-only label even
when it contributes to the implementation estimate.

Other pages use their documented milestone lists; where 20 equally weighted
items are listed, each checked item contributes five percentage points. Weights
are engineering estimates and do not imply equal implementation costs. A bar
does not measure feature coverage, remaining work hours or time. Do not sum or
compare percentages across areas with different scopes. Research bars count
assessment deliverables and prototype evidence; they never mean that the
corresponding API or desktop is implemented.

Use exactly 20 cells per bar. Each `█` represents five percentage points of the
approximate milestone estimate, rounded to the nearest 5%. For incomplete bars,
place one `▒` immediately after the solid cells and fill the remaining cells with
`░`. The shadow occupies an unfinished cell and adds no progress. A complete bar
contains 20 solid cells and no shadow. Changing the scale alone preserves the
existing relative milestone estimates; it establishes no new checked capability.

The [coverage ledger](../../tests/acceptance/README.md#coverage-ledger) separately
counts acceptance cases and source-bound results. Its pass rate does not set
the progress bars. A checked CPU construction stage does not close a hardware
stage, and a bounded hardware fixture does not close a complete API selection.

Distinguish four questions:

- **Implementation:** what the current product path accepts and builds. Host
  fixtures and unlanded experiments do not establish device support.
- **Hardware:** what ran on poulsback, on which device and source/profile.
- **Presentation:** what reached the display, including flips, copies and buffer
  ownership; an offscreen result alone does not answer this.
- **API:** what the complete applicable selection passes. Context creation,
  version strings and working applications do not establish conformance.

Keep Mesa integration and API readiness separate. OpenGL 2.0 is an intermediate
milestone within the OpenGL 2.1 page. Builder, fixture and documentation changes
alone do not advance hardware or application status.

- Name the scope: CPU/VM checks, poulsback hardware, presentation or API checks.
  A legacy-driver observation belongs to research, not poulsback hardware support.
- For hardware claims, retain the tested source/profile or link to its owning
  record. A documentation commit does not requalify an earlier result.
- State completion, visibility, retirement and reuse separately where they differ.
  A correct retained image does not establish safe reuse; capture is not presentation.
- Research can establish a useful contract or limitation without a working feature.
  List the actual finding and unresolved boundary.
- Keep per-attempt journals and raw bytes out of product documentation. Do not add
  review dates or run diaries to status pages.

Component guides own technical contracts; the [checklist](../checklist.md) owns
acceptance criteria. Run the documentation/link check after editing. A status
update alone does not need a new GPU experiment or change driver admission.

The [3D page](3d.md) summarizes current rendering capabilities and open lifecycle
boundaries. Keep project history, dated progress reports and run narratives out
of the product documentation. Mesa, API, X11 and component guides link to the
owner instead of maintaining separate frame counts.
[Hardware claims](../hardware.md#varyings-fragment-programs-and-depth) own
current device contracts and their qualification limits; commands stay in the
test recipes.
