# System sleep

[Kernel](../kernel.md) · [Execution](execution.md) · [Display](display.md)

The PSB system-sleep path preserves open DRM files, execution contexts, BOs and
their page tables. It supports the Linux suspend/resume callbacks; runtime
power-off and hibernation remain refused. The implementation requires real
device qualification before it can be described as reliable suspend/resume.

## Entry and return

The fbdev setup clears the DRM helper’s `skip_vt_switch` default. Linux then
switches to its suspend console before freezing userspace. Native KMS
clients must release their plane and restore the inherited framebuffer during
that switch. `prepare` rejects typed scanout, retained failures and unfinished
display transitions. It parks the GPU schedulers, drains accepted hardware
work and flushes both engine work queues. `complete` unparks them if another
device aborts suspend before this driver's power-off callback.

Framebuffer retirement runs on the system freezable work queue. It cannot
remove a GTT mapping while PM saves or restores it. Display polling is stopped
during the device transition. These guarantees apply to system sleep, whose
callbacks run with clients frozen; they do not admit runtime power-off.

Suspend saves all active GTT entries and the current SGX clock-gating word.
The GTT snapshot includes legacy objects and native display reservations.
The legacy resource walker cannot restore native reservations because it
casts each resource to `psb_gem_object`. Resume therefore restores the saved
table after checking its geometry and stolen-memory base. No buffer allocation
or pin is released during this transition.

After saving display registers, suspend runs the existing engine deinitialization
sequence, removes the IRQ handler and enters PCI D3hot. Resume restores PCI,
the GTT and the inherited display. It checks the retained display source and
starts any pending retirement interval from the new frame counter. Two new
frames must pass before retirement work can run again.

The GPU returns through the existing PSB probe reset sequence, using the saved
clock-gating value. This reset is permitted only after the successful idle drain
and controlled power transition. It is never a recovery attempt for a lost GPU.
The original engine object then reprograms its kernel directory, request bases,
code banks and DPM state. Its schedulers resume only after display, IRQ and GPU
restoration succeed. Per-file mappings and context handles stay valid.

Successful system sleep preserves the source epoch because the same pinned
objects and mappings were restored. A failed restore records source loss,
wedges the executor and retains GPU-reachable resources. Queued jobs complete
through the existing error path instead of remaining behind parked schedulers.

## Source basis and boundaries

Linux's [PM callback contract](https://docs.kernel.org/driver-api/pm/devices.html)
defines freezing, prepare/complete rollback and device ordering. The locked
Linux 6.1 GMA500 `power.c`, `psb_drv.c`, `gtt.c` and `psb_device.c` provide the
PCI, reset and display-register sequences. The native executor supplies its
own drain, request routing and engine program.

The matched EMGD 1.8 source explains the OFF/ON boundary:
`sgxpower.c:209-375` drains the firmware path before deinitialization and calls
`SGXInitialise` on return from OFF. `sgxinit.c:257` runs initialization around
`SGXReset`; `sgxreset.c` contains that reset sequence. Poulsback has its own
scheduler and does not run those firmware commands. These sources establish
the distinction between idle and reinitialization; they do not qualify our
native resume path.

The files are in the project's
[reference collection v2](https://github.com/ishad0w/poulsback/releases/tag/v0.0.1%2Balpha%2Bpoulsback-reference-collection-v2),
under the EMGD 1.8 source root listed in [Evidence](../re/evidence.md#select-a-source-file).
Their SHA-256 identities are:

| File | SHA-256 |
| --- | --- |
| `sgxpower.c` | `b2b745d100a111047d8149aa57086704930343038a5794361cf141a5c19096d9` |
| `sgxinit.c` | `ec84a60159e6f7a91614b7dd98b34b2629127893c680e2052b357cc8144a4cc2` |
| `sgxreset.c` | `75dd4b4f727ee5bc9d3a4f1a8fe5e8714176cf5e175041a7b913a5fe9f1d8145` |

Qualification must use the actual terminal sessions: suspend and wake with
native fbterm open, then verify input, scrolling, VT switching, panel blanking,
audio and network recovery. Check the driver logs for both restoration messages
and new GPU completions. Repeated cycles, failed admission, 3D workloads and
other hardware need their own evidence. Compilation alone establishes none of
those results.
