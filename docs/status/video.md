# Video and VA-API status

WIP. Command preparation and models exist; poulsback has no live hardware decoder.

## Working and checked

- **CPU:** MPEG-2 descriptors, typed owners, messages, ring publication and checked
  firmware-upload plans. Tests cover simulated MTX transport, malformed inputs,
  publication failures and an independent DC-only MPEG-2 plane oracle.

## In progress

- Physical MSVDX firmware/loader integration, command transport and matching completion.
- Typed YUV/reference lifetime, complete plane/guard checks and safe release/reuse.
- VA-API integration and presentation; separate decode/composition/throughput checks.

## Research

- H.264, VC-1 and other legacy-advertised profiles remain research inputs;
  poulsback hardware decoding is not yet qualified.
- No US15W hardware encoder is established. CPU fixture generation is not
  accelerated encoding.

New video implementation follows the complete-stack M1 milestone. Details:
[video interfaces](../video.md),
[Windows device distinctions](../video.md#windows-decoder-contracts),
[tests](../tests.md#video) and [acceptance](../checklist.md).

## Progress basis

Scope: the first MPEG-2 hardware-decoding path through VA-API and presentation. H.264/VC-1 expansion and encoder feasibility remain separate research. The [video component](../video.md) and [MPEG-2 acceptance](../../tests/acceptance/mpeg2.md) supply the basis. CPU preparation is counted only as CPU preparation.

**8/20 deliverables complete = 40%.** Each item below has equal weight (5 percentage points). This measures the listed deliverables, not time, API coverage or the acceptance-ledger pass rate.

- [x] Typed legacy MPEG-2 descriptor profile and input validation.
- [x] CPU preparation of the supported MPEG-2 intra-slice descriptors.
- [x] Firmware-envelope encode/decode and whole-stream validation.
- [x] Host-ring insertion planner with wrap and capacity checks.
- [x] Owner/epoch registry and relocation-span validation.
- [x] Firmware-container parser and bank-split upload planner.
- [x] Simulated MTX transport, completion and HOLD state machine.
- [x] Independent DC-only plane oracle with padding/guard checks.
- [ ] Qualified distributable firmware input for the physical device.
- [ ] Physical firmware upload, readiness and startup verification.
- [ ] Physical command publication, kick and matching completion.
- [ ] Device MMU/cache visibility and memory-use qualification.
- [ ] Verified physical output-surface layouts and complete readback.
- [ ] Correct I/P/B reference and reorder behavior.
- [ ] Required codec/profile/resolution numerical selection.
- [ ] Surface/fence lifetime, safe reuse and error retirement.
- [ ] Current libva descriptor adapter with exact ABI checks.
- [ ] VA-API frontend and unmodified decoder-application integration.
- [ ] Decoded-image presentation and required composition/conversion.
- [ ] Sustained throughput, stress, power and lifecycle acceptance.
