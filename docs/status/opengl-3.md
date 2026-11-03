# OpenGL 3.0 status

Research. There is no implemented or qualified poulsback OpenGL 3.0 context.

## Established basis

- Mesa lowering and checked SGX535 instruction contracts supply inputs to a
  requirement matrix.
- Existing CPU tests, vendor version strings and finite hardware scenes do not
  establish GL 3.0. [OpenGL 2.1](opengl-21.md) and
  [OpenGL ES 2.0](opengl-es-20.md) also need their own qualification.

## Remaining work

- Map OpenGL 3.0/GLSL 1.30 requirements to operations, resource/precision limits,
  valid lowering and independent tests.
- Cover integer/array textures, transform feedback, framebuffer/multisample
  behavior, queries and normative limits.
- Add a real API test executor; no current adapter runs this selection. Versions
  and extensions must not bypass missing behavior.

Details: [feasibility profile](../graphics.md#opengl-30) and
[research checks](../tests.md#opengl-30-research).

## Progress basis

Scope: an OpenGL 3.0/GLSL 1.30 feasibility assessment. This bar counts research outputs, not implemented GL3 features or conformance. [Research profile](../graphics.md#opengl-30), [SGX contracts](../hardware.md) and [research checks](../tests.md#opengl-30-research) provide the basis.

**2/20 deliverables complete = 10%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Identify the candidate OpenGL 3.0 and GLSL 1.30 profile.
- [x] Collect SGX535 instruction, addressing and launch-contract inputs.
- [ ] Pin exact normative specification revisions.
- [ ] Enumerate every mandatory feature and minimum limit.
- [ ] Map GLSL 1.30 operations to native instructions or valid lowering.
- [ ] Resolve integer arithmetic, conversion and precision requirements.
- [ ] Assess integer texture formats and sampling semantics.
- [ ] Assess array/3D texture and required format coverage.
- [ ] Assess transform-feedback generation, storage and synchronization.
- [ ] Assess framebuffer, multisample and resolve requirements.
- [ ] Assess queries and conditional-rendering requirements.
- [ ] Assess required texture lookup, LOD and interpolation behavior.
- [ ] Validate the required Mesa frontend/NIR lowering paths.
- [ ] Map uniform, varying, temporary and resource limits.
- [ ] Map synchronization and cross-context visibility requirements.
- [ ] Map context, object and API error semantics.
- [ ] Freeze the requirement-to-test map and independent oracles.
- [ ] Run discriminating hardware microprogram and resource probes.
- [ ] Build an attributed prototype context and applicable test executor.
- [ ] Review the feasibility verdict and remaining implementation gaps.
