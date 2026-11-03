# Public evidence and reproducibility

The following public releases provide selected research inputs outside this Git
tree. Match their payloads to the [source profiles](sources.md) and
[address domains](address-domains.md) before following a chapter's locators.
Vendor inputs and working OS images do not qualify poulsback's implementation.
Their vendor redistribution permissions and Live corresponding-source closure
also remain subject to the [licensing boundaries](../../NOTICES.md#reference-and-live-releases).
A published asset and a matching checksum are not a redistribution clearance.

## Available packages

| Release | Published assets | Independently checkable scope |
| --- | --- | --- |
| [hwsw_reference_1.0](https://github.com/ishad0w/poulsback/releases/tag/hwsw_reference_1.0) | `blobs.zip`, split copies and checksum files | Selected PSB/EMGD sources, Linux/Windows driver bytes, firmware, BIOS and documents; inner file/package provenance and read-only verification tools |
| [legacy_live_1.0](https://github.com/ishad0w/poulsback/releases/tag/legacy_live_1.0) | `poulsback-research-live-1.0-{lucid,natty}-i386.img.zst`, `SHA256SUMS`, `IMAGES-SHA256SUMS` | Prepared Ubuntu 10.04 PSB and Ubuntu 11.04 EMGD environments; image integrity and included software, not reproduction of the recorded Dell observations |

SHA-256 identities of the complete downloads:

```text
69f1099147ae2eb3988e2024963baffc05631d241485b72e5181f6ec8ab2717a  blobs.zip
e9a80105ef795e5c869d97bd9863c740a74733776151efd7c6f80cbbfe859d63  poulsback-research-live-1.0-lucid-i386.img.zst
15fc1f8475fd3fb43e75f325bba876b872cedd5236f599f35e923daec645ff7d  poulsback-research-live-1.0-natty-i386.img.zst
```

## Verify and inspect the reference collection

In an empty download directory outside the checkout, with GitHub CLI, 7-Zip,
tar and Python 3 installed:

```sh
gh release download hwsw_reference_1.0 --repo ishad0w/poulsback \
  --pattern blobs.zip --pattern blobs.zip.sha256
sha256sum --check blobs.zip.sha256
7z x -ppoulsback blobs.zip
tar -xf content.tar
cd poulsback-references
python3 -I -B tools/verify.py
```

The verifier checks bytes, sizes, manifest coverage, package references and
self-contained file links. `CATALOG.md` lists releases and gaps;
`catalog/packages.json` identifies carriers, while `catalog/files.jsonl` maps
each stored payload to its original member or byte range. Original installer
wrappers are omitted; a carrier identity alone does not mean it is included.

For example, inspect the initializer's named SGX arrays without executing it:

```sh
python3 -I -B tools/inspect_sgx_symbols.py \
  drivers/linux/lab-stacks/ubuntu-11.04-emgd-1.8/usr/lib/libemgdsrv_init.so
```

Its reported parent hash must match the Linux EMGD 1.8 initializer in
[source profiles](sources.md#linux-emgd-18). The output reports file offsets,
ELF VAs, sizes and child hashes; the [firmware chapter](firmware/sgx.md)
explains the separate runtime-patching boundary. The preserved PSB source and
matched Windows/Linux binaries similarly permit independent source inspection
or disassembly at chapter locators. They do not supply the original analysis
database, inferred names or proof of device execution.

### Select a source file

Bare source filenames and line ranges in these chapters resolve under the
following roots, relative to the unpacked `poulsback-references` directory:

| Selected source | Collection-relative root | Example consumers |
| --- | --- | --- |
| Runtime-matched EMGD 1.8 DKMS source | `drivers/linux/lab-stacks/ubuntu-11.04-emgd-1.8/usr/src/emgd-1.8.0.2032/pvr/` | `services4/srvkm/devices/sgx/sgxinit.c` and `services4/srvkm/devices/sgx/sgxkick.c` |
| PSB 4.41 source | `sources/linux/psb-kernel-source_4.41.orig/psb-kernel-source-4.41/src/` | `psb_sgx.c`, `psb_regman.c` and the [normal-success source anchors](psb/normal-success-lifetime.md#source-anchors) |

Match each selected file's complete size and SHA-256 to `catalog/files.jsonl`
before checking a quoted range. That entry also retains the original member
or source-carrier identity. The captured PSB 4.42.0 source is a separate tree
under `drivers/linux/lab-stacks/ubuntu-10.04-psb/usr/src/psb-kernel-source-4.42.0/`;
it does not replace the selected 4.41 consumer. A source locator does not
establish equivalence with a deployed binary.

### Inspect a Live image

For the Live images, download the selected compressed image and both manifests
from their release page, compare the compressed hash above, and use the release's
unpacking/checksum instructions. Remastering sources, package manifests and
validation reports are not separate assets. Image checksums and VM boot checks
do not qualify the hardware behavior of the included driver stack.

## Selected follow-up contracts

The [vertex](emgd-linux/vertex-path.md), [fragment](emgd-linux/fragment-path.md),
[X11 routing](native-2d.md#captured-x11-routing) and
[render-state](platform/runtime-render-state.md) chapters identify the matched
source profile and explain portable captured contracts. Raw captures and full
analysis reports are not public release assets. The
[SPM/TA/3D decode](firmware/partial-render.md) identifies exact ranges and hashes
within the public EMGD initializer; static flow analysis is separate from
runtime pool-pressure qualification.

The [UV2/texture route](emgd-linux/geometry-and-sampling.md#uv2-vertex-output-and-coupled-state),
[ordinary SECONDARY constants](psb/pds-use-and-depth.md#ordinary-fragment-secondary-constants)
and [USSE fields/entry state](sgx535/usse-instruction-contracts.md) identify their
exact EMGD/PSB components, address domains and remaining boundaries. Matching
legacy payloads permit source/byte inspection; CPU constructors test their own
bounded representations. Neither reproduces the original device observations.
The separate SDK revision121 decoder binary, wrapper and raw output reports
are not included in these releases. Public CPU checks exercise the project
code and models without requiring them.

## What remains unavailable

Analysis databases, GPU/PCI dumps, raw traces and frame captures are held
separately and are not supplied by these releases. Their absence does not hide
an additional build step or interface definition: the technical mechanisms and
unresolved boundaries are described in this reference. Static byte and source
claims can be inspected in the matching public payloads; independent replay of
a device observation requires its original capture inputs and hardware.

Public [CPU checks](../tests.md#run-checks) reproduce their own source/model
scope using the repository alone. They do not regenerate captured device results,
qualify a physical profile or authorize booting a device experiment. Use the
release instructions for image operation and retain the
[hardware result limits](../hardware.md#unresolved-hardware-contracts).
Each vendor/source payload keeps its own licence; see [notices](../../NOTICES.md).
