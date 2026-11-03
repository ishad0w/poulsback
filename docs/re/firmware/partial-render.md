# EMGD partial-render program

This is a static host decode of the SGX535 SPM, TA and 3D programs in Linux
EMGD 1.8's initializer. It explains the OOM/partial-render handoff; it does not
claim those paths were captured under pool pressure or qualify poulsback's
implementation. The selected carrier is `libemgdsrv_init.so`, SHA-256
`3f18f0715e8d2174d9744a577912a7a5a6afba41e1e3f9ec90a859071ecf3121`
([source profile](../sources.md#linux-emgd-18)).

## Program map

| Symbol | ELF VA | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `pbuKernelSPMProgram` | `0xf260` | 18328 | `7c6bde17c9b6f4ed76c19da4df2c815e09bd4edf3d157fbf9bef2d755e8fa81e` |
| `pbuKernelTAProgram` | `0x9ca0` | 21928 | `457f60adad497bebe2fbbd982a81e878e261261ca664d6acc2705df6aa87f794` |
| `pbuKernel3DProgram` | `0x13a00` | 26776 | `ab3b07f88436966ff213bf038d69d2f7f382c4d0e75225cf4a21c5c806fab8fd` |

For these `.data` symbols only, file offset is ELF VA minus `0x1000`.
Offsets below are bytes within the selected program, not x86 VAs or register
indices. The [rev121 decoder](../sgx535/usse-instruction-contracts.md#sgx535-revision121-cpu-decoder)
decodes all 2291 SPM instructions, 2741 TA instructions and 3347 3D instructions.
Routine names are descriptive labels; matched EMGD control flow and stores
select the actual behavior.

| SPM range | Role |
| --- | --- |
| `0x0340–0x0910` | OOM handling and requester-dependent TE step or abort. |
| `0x0918–0x1600` | Abort completion, DPM-context handoff and partial-render kick. |
| `0x2270–0x2340` | Reserve removal and ZLS threshold helpers. |
| `0x2348–0x25b0` | Partial-render completion and TA restart. |
| `0x3218–0x32b8` | DPM clear/store/load helpers. |

## OOM and partial-render sequence

An OOM beside a running raster can defer work until that raster finishes.
After settling, the SPM program reads the TE/MTE requester at register `0x734`.
The following paths are distinct; ABORTALL is not a host-side cancellation.

1. **TE step:** raise the threshold by one, pause allocations, wait for MTE
   flush, set TE_PSG COMPLETEONTERMINATE (`0x400000`), reload the free list and
   restart. This sequence does not add ZSTOREENABLE.
2. **Abort:** flush MTE, request ABORTALL and wait for TA_TERMINATE. The reached
   store to `DPM_OUTOFMEM` (`0x69c`) is 4; a store of 2 exists in the binary
   but no decoded branch reaches it.
3. **Handoff:** set PARTIAL_RENDER (`0x658 = 1`) before rearranging allocation
   and load/store context fields. Store, clear and load the DPM state in the
   required contexts. An existing owner of the other context has a separate
   store path; it cannot simply be overwritten.
4. **Partial raster:** use the render's 3D registers, bind the proper directory
   and invalidate caches before the kick. With depth, the additional ZLS bits
   are `0x400054`, including FORCEZSTORE. The decoded EMGD path does not perform
   the independent PSB-style ISP reset before every raster.
5. **Restart:** on partial-render completion, remove the extra reserve pages,
   restore threshold/state, reload/clear as required, enable the vertex master
   and restart the TA. The eventual ordinary raster loads preserved depth/stencil
   according to whether a partial render already occurred.

If the TA finishes after TE threshold steps, EMGD takes another path:
TA `0x0cb8–0x0e20` and SPM handoff render the whole scene from the other DPM
context under STATE_FREE_ABORT. The scene's later 3D kick is completion-only,
not a second raster. A host executor must account for this route when deciding
which event completes the job.

The initializer also allocates two 256-KiB dummy stencil buffers.
Its x86 stores at `0x2bf4–0x2c22` fill TA3D_CTL words 68–70: requester base and
enabled load/store offsets. They are offsets from the named ZLS base, not
unrelated absolute addresses. The 3D/SPM code consumes those words when the
render has no depth/stencil plane.

## Limits

EMGD contains extra requester, deadlock/page-injection, owner and erratum paths.
A deliberately narrower implementation must name its refused conditions rather
than treating the subset as a complete firmware reconstruction. Static stores
do not prove runtime entry, effective MOE, completion, safe teardown or the
reason for every policy choice. The full analysis and raw programs retain
their [evidence boundary](../evidence.md#what-remains-unavailable).

[SGX carriers](sgx.md) owns service-offset and runtime-patch distinctions;
[kernel execution](../../kernel/execution.md) owns poulsback scheduling and OOM
policy. An error-signalled fence still does not authorize releasing resources
whose final use is unknown.
