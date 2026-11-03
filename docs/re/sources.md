# Source profiles and interpretation

This reference identifies the legacy implementations used to derive selected
Poulsbo contracts. A filename, API name or common PVR version does not identify
bytes. Match the component hash, source family and address domain before using
an offset or combining two findings.

These identities describe source inputs. They do not imply that every function
ran, that a captured vendor result qualifies poulsback, or that a carrier hash
identifies relocated or patched device memory. The [address guide](address-domains.md)
explains how to resolve the locations quoted in the chapters.

## Linux EMGD 1.8

The primary runtime-matched reference is EMGD 1.8 Build 2032, internal PVR
`1.5.15.3226`, on Ubuntu 11.04 i386 with kernel `2.6.38-16-generic`.
The observed device is Dell Inspiron 1210, PCI `8086:8108`, subsystem
`1028:02b1`. These libraries and the module were identified in retained runtime
captures. That observation selects the stack, not all of its code paths.

The OGL, Services UM and initializer members match
`xorg-emgd_1.8.0-2032-0ubuntu1~ppa1_i386.deb`, SHA-256
`f143e9ad994ecec59395b4ffe894de5d2d3089c3df26d7e37006610822153b87`.
Matching source package `emgd-dkms_1.8.0.2032-0ubuntu1~ppa9_all.deb` has SHA-256
`1546281f697f07ea2897ea7ffd2714e27bdabf1db31159fde45827545f2404db`.
Package, PVR, kernel and runtime version strings are separate domains.

| Component | Bytes | SHA-256 | Role |
| --- | ---: | --- | --- |
| `libEMGDOGL.so` | 2791512 | `3c701b634887fa1fd719825bda8db01961f30f744fb6a9f84c5728163282bc8a` | OpenGL state, geometry/state serialization and submission producer |
| `libemgdsrv_um.so` | 136548 | `d9f741a40b293596b958b783af3581af84015dfd49c61c5f1a30240a5a459cbf` | Services contexts, allocations, private TA packets and bridge envelopes |
| `libemgdsrv_init.so` | 120740 | `3f18f0715e8d2174d9744a577912a7a5a6afba41e1e3f9ec90a859071ecf3121` | Initialization scripts, service-offset producer and embedded program carrier |
| `emgd.ko` | 548032 | `551ef2067798459e9e5b41275b2b27f97af5d2b4c4db6ef9fa32598473ed6335` | Matching Services kernel, MMU, scheduler and resource callbacks |
| `emgd_drv.so` | 210520 | `f8b261c2bab9d0038cbab3b46ef30aa85a23ee04417773e97a895922f868f051` | Xorg boundary; presence does not identify the engine used by each operation |

The matched stack supplies separate [render-state/clock/presentation](platform/runtime-render-state.md),
[vertex](emgd-linux/vertex-path.md), [X11 routing](native-2d.md#captured-x11-routing)
and [fragment/texture/FBO](emgd-linux/fragment-path.md) contracts. Captured
layouts do not automatically recover a compiler producer or its numerical model.

The selected shared-object analysis imports use a `0x10000` bias above ELF VAs.
This is specific to those imports; ELF segments still determine file offsets.
The kernel module is ET_REL: its section offset, file offset and runtime load
binding are different values. Inferred `FUN_*` names are analysis labels, not
exported vendor symbols. Assembly calling conventions take precedence over
inferred pseudocode signatures.

### Linux comparison variants

The following payloads retain similar `.so.1.5.15.3226` names but differ from the
runtime-matched 1.8 stack and from each other. No corresponding Dell run is
selected here. Check chipset dispatch, firmware patches and errata options
before reusing a field or address.

| Package variant | OGL bytes / SHA-256 | Initializer bytes / SHA-256 |
| --- | --- | --- |
| F14 `3398-1.18` | 2794172 / `e79e50dde58fa91423f7ee6b8241fe1dfb306b429c4d825f6dff7c79854edef4` | 119108 / `13f01d0d01d93f5cb32aedab0c67da09cf127774109df9ec964a68e7e755b188` |
| MeeGo 1.2 `3398-1.18` | 2794172 / `aea32db282ee55a4c3075906295c438e8e94a2bd8ed3eac42be46c0582b88e1d` | 119108 / `d1926b99af680332f0c22a5b65c8895caa4522902acc3dbeab65720f853035db` |
| MeeGo Wayland `3398-1.18` | 2794188 / `0cca2cc775b7d71e909f903a5c8e3cfb1fa0a6bccf5a37cfa63aadbd90193ae1` | 119124 / `b814dc8aacc391f97babf06eeb3d3945bc9baa92535efa65f14fe882a9d8c6f0` |
| Tizen `3398-1.16` | 2794188 / `d2d6b8ba167319e3b9446dd839da4c4d61c131e75c24b58cc1c8d47d5df45e15` | 119124 / `7bed879dd504dafb6b8adbeaa13355830c493b3aa30bf4a6ec805f6be1a77772` |

Tizen's original 1.16 label is intentional. OEM repackaging or another build of
the same PVR build does not provide an independent observation of the silicon.

## Windows EMGD 3398

The selected Windows 7 kit is `win7_video_IEMGD_130630.zip`, 11,211,960 bytes,
SHA-256 `3b892a5eba8a04f9416740f1b328b261e4038c61b77ec7a2ff810eb6d0e2f203`.
Its display version is `8.14.1180.3398`; the INF selects `8086:8108` and
`8086:4108`. `OpenGLDriverName` registers `igdogl32.dll`, which loads named SGL
entry points from `igdsgl32.dll`.

| Component | Bytes | SHA-256 | Established role |
| --- | ---: | --- | --- |
| `igdogl32.dll` | 2,040,320 | `1c42fd75f16e96c7b05314d701065a10b2cbaef3851efd061e08ad7f65b12537` | OpenGL ICD; selected context, viewport, clipping and screen-allocation producers. |
| `igdsgl32.dll` | 699,904 | `7a0d9f396a0386f102d454684a4f58aef947bc2621ca47f64d7bab1ffa782e0c` | Matched SGL entry points, state serialization and private WDDM allocation producer. |
| `igdkmd32.sys` | 1,745,152 | `9c3486957755a6cdd98b2f30fd70235e78df6910343eca9105f21c1ea506c328` | Kernel-side allocation validation, resource class, normal command parser and raster template. |
| `LPCO/igdlh32.inf` | 33,040 | `11ad54ee42919cb501ceeab2144ade31f1318a40017a09f63e3a2f2f9b277c9b` | Display version, device selection and OpenGL ICD registration. |
| `igdumd32.dll` | 1,719,808 | `7cde4e9b3e29d700b6974951256fb4ff2d6cee1528845321c6c4de038c77c6cd` | Retained user-mode D3D component. Its complete draw and DXVA paths are outside these recovered chains. |

The binary components are PE32/i386 with preferred image base `0x10000000`.
The same static VA in two files is not the same code. Original PDB types are
not recovered; private-field names in the chapters describe the analysis.
No Windows runtime on the reference Dell is established by these static paths.

Windows 7 EMGD `8.14.1100.2208` and XP EMGD `1.14.0.2443` are separate inputs.
The retained 2208 carrier is `win7_video_IEMGD_120203.zip`, 11,547,286 bytes,
SHA-256 `b7cd7bede400c4980a8d701261d2cb99ee4ffc3a6723f5bad1c501107b4ba987`.
XP 2443 supplies the source-level display SDK discussed in
[configuration controls](platform/configuration-controls.md). It does not
specify the Windows 7 control-panel implementation.

## Windows retail 2030

The Intel `win7.zip` carrier is 30,857,148 bytes, SHA-256
`25de78ae2e849eeac72c3f4d94aa6b449cd32da06accc507112a02adc8216f8b`.
Its package label `5.0.0.2030` and paired driver version `8.14.10.2030` are
both retained; neither is Windows EMGD 3398.

| Component | Bytes | SHA-256 |
| --- | ---: | --- |
| `LPCO/igdkmd32.sys` | 648,832 | `42770a2162fd0c7e227a50cdde8547581b7895d3ed771f9b7995f7409e774ced` |
| `LPCO/igdumd32.dll` | 1,418,752 | `83873d2196c5d3ed2a1735d2c60d69e6b8242942ee4c16c4b84c9d70f2408fc9` |
| `LPCO/igdlh32.inf` | 32,402 | `0c1f734d02a2f9eade00558d94a9ead4e52ac96b2f0a70e01b23f8e0bcf53d68` |

The INF selects `8086:8108` and `8086:8109`. Intel and fit-PC copies of the
selected KMD/UMD are byte-identical, so those packages are one implementation
for this comparison. The selected INF has no `OpenGLDriverName` entry; this
bounded registration fact says nothing about other packages' OpenGL paths.

The [retail bootstrap chapter](windows/retail-bootstrap-and-retirement.md)
covers code banks and completion-marker limits. The separate
[Windows decoder contracts](video/decode-pipeline.md#windows-decode-declarations-and-host-boundary)
cover queue and callback boundaries. A decoder callback or successful EndFrame does not
prove decoded output, retirement or throughput. The retained
vendor release notes describe failures as well as features; the kit is a source
of contracts rather than a universal correctness oracle.

## PSB source and runtime profiles

PSB supplies direct packet constructors and a normal-success resource-fence
consumer. The selected source is `psb-kernel-source-4.41`; the
[normal-success chapter](psb/normal-success-lifetime.md) names the concrete
functions and source ranges. Its original `psb-kernel-source_4.41.orig.tar.gz`
carrier has SHA-256
`f4ab6ba2d97a97edf6dbb5905796cdbcf36d5e6c9f8c7694e81aa143b41fcc0f`.
The reference catalog identifies its unpacked files as package
`linux-psb-kernel-source_4.41.orig`; [source lookup](evidence.md#select-a-source-file)
gives the collection-relative paths. Its source identity is separate from the Ubuntu
10.04 runtime module below. That captured stack uses kernel `2.6.32-74-generic`,
PSB kernel package 4.42.0, DDX 0.36, firmware package 0.30 and libva 0.31;
it is not a runtime qualification of the analyzed 4.41 source.
Exact deployed DRI/kernel pairing is not established
by a source-directory label or compatible API.

| Component | Bytes | SHA-256 |
| --- | ---: | --- |
| `psb_dri.so` | 2921100 | `74ca42991906741ee91be1bc088ae0b142fa71b6a85658c5dad0851ce50dd0d8` |
| `Xpsb.so` | 58888 | `da531587b1ec59fe433fa20ddd2e691b2f8b7fb35bb82525d4db99259c5e571f` |
| `psb.ko` | 202832 | `45ca112d82824d737d94e140d4307c78ff37c4899858119bd2ffb2ba21e1257f` |
| `psb_drv.so` | 136188 | `ea69a409746fde8a62cf30c9d4b0d8dd9eddb7b6136cef019d782f9acd6762d3` |

Selected `psb_dri.so` analyses also use the `0x10000` import bias, with ELF
segments controlling file ranges. Source functions have byte/line locators;
they do not acquire invented runtime addresses. The normal-success source path
is useful independently of incomplete EMGD firmware cleanup attribution, but
its ACKs and object model cannot be spliced into EMGD Services or WDDM.

## Firmware, platform and observations

- [SGX carrier](firmware/sgx.md): the initializer, extracted child programs and
  runtime patches have separate identities and address units.
- [MSVDX firmware](video/msvdx-firmware.md): EMGD's embedded MTX producer and the
  PSB file-container profile are distinct loaders and payloads.
- [Dell BIOS, display and power](platform/bios-display-power.md): update carriers,
  decompressed modules, option ROMs, VBT/EDID and ACPI have separate roles.
- [TA trace example](emgd-linux/submission-packet.md) and
  [video output example](video/surface-layout.md): selected retained
  observations explain particular fields and limits. Two captures do not share
  an object lifetime merely because a pointer or GPU VA is equal.

Chapters mark the basis of each conclusion: source text, static binary analysis,
CPU checks or retained hardware observation. Knowledge may be established within
that profile, inferred, unresolved or conflicting. These labels are independent:
a complete static record can still have unresolved indirect calls, and a correct
image can coexist with failed cleanup.

## Selected CPU instruction-decoder profile

The separate SGX535 revision121 comparison uses a retained i386 compiler ELF,
SHA-256 `9349ed4d7643d81cb638b41eb41b69507cc2f9f15bae14c8f2116e79f70e7bd9`.
Its package attributes it to PowerVR SDK3.5 Linux_x86_32; publisher authentication
was not established. The original decoder VA is `0x0829921b` and file offset
`0x25121b`, with no EMGD analysis bias. Its wrapper, runtime and licensing are
separate inputs. They are not supplied by this Git tree or newly declared
assets of the existing reference releases. The
[USSE chapter](sgx535/usse-instruction-contracts.md#sgx535-revision121-cpu-decoder)
owns the executed CPU scope, END-return interpretation and remaining limits.

## Provenance and scope

The public reference contains project-authored explanations and selected
artifact/field identities. Vendor payloads, source collections and firmware/ROM
bytes are not included in this Git tree. Selected inputs are available in the
public releases linked by the [evidence index](evidence.md), with commands for
byte verification and source inspection. Raw device captures and analysis databases are held separately. This
reference describes the mechanisms directly; normal builds and public CPU
checks require neither those files nor the vendor payloads. Where a final
consumer or matching reply is unresolved, the chapter states that boundary.

A content hash identifies bytes; it is not a check of a publisher's signature
or proof that an extracted child was executed. Keep package authentication,
selected byte ranges and runtime correspondence as separate checks.

Source attribution and applicable notices remain with their materials; see
[project notices](../../NOTICES.md). Identifying a format or documenting a
mechanism does not assign a new licence to the referenced artifacts.
Implementation and test acceptance have their existing owners in
[component documentation](../../MAP.md) and the [checklist](../checklist.md).
