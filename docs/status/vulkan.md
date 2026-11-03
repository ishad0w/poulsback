# Vulkan status

Research. There is no poulsback Vulkan ICD or advertised Vulkan version.

## Established basis

- SGX addressing, instruction and synchronization findings supply inputs to a
  feasibility assessment. They do not establish Vulkan's execution or memory model.

## Remaining work

- Compare mandatory requirements with actual hardware and lowering capabilities.
- Record unsupported features and a reviewed feasibility verdict. Missing features
  cannot be replaced with invented capabilities or version overrides.

Implementation is outside the complete-stack M1 milestone. Vulkan is not a
prerequisite for native rendering, X11/Wayland or MSVDX decode.
Details: [feasibility checks](../tests.md#vulkan-feasibility) and separate
[release criteria](../checklist.md).

## Progress basis

Scope: a Vulkan 1.0 feasibility assessment with a reviewed supported or unsupported verdict. This bar counts research outputs, not an ICD or Vulkan functionality. [Assessment criteria](../../tests/acceptance/vulkan-feasibility.md), [SGX contracts](../hardware.md) and [research checks](../tests.md#vulkan-feasibility) provide the basis.

**2/20 deliverables complete = 10%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Select Vulkan 1.0 as the candidate assessment target.
- [x] Collect SGX535 instruction, addressing and fence-contract inputs.
- [ ] Pin the exact normative specification revision.
- [ ] Enumerate all mandatory features and minimum limits.
- [ ] Assess SPIR-V execution and shader-interface requirements.
- [ ] Resolve mandatory compute-shader execution requirements.
- [ ] Resolve integer, floating-point and precision requirements.
- [ ] Assess storage buffers, image access and required atomics.
- [ ] Establish barrier, ordering, availability and visibility semantics.
- [ ] Assess descriptors and pipeline resource binding.
- [ ] Assess required image formats, layouts and tiling.
- [ ] Assess required sampling and texture operations.
- [ ] Map fixed-function graphics pipeline requirements.
- [ ] Assess render-pass, load/store and input-attachment behavior.
- [ ] Specify queue submission and command-buffer execution.
- [ ] Assess fence, semaphore and cross-queue synchronization.
- [ ] Specify compatible allocation and address-space ownership.
- [ ] Assess surface/swapchain and presentation requirements.
- [ ] Map every requirement to evidence and independent tests.
- [ ] Independently review a supported/unsupported verdict and blockers.
