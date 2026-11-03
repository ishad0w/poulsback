# Address domains and reusable locators

An address is useful only with the component SHA-256, its address domain and the
mapping that connects it to bytes. The [source profiles](sources.md) identify the
selected components. Chapter locators cover selected ranges, not every symbol
in every release. A function name inferred by an analyst is labelled as such;
it must not silently become an exported vendor symbol.

## Host code

| Value | Meaning | Check before using it |
| --- | --- | --- |
| File offset | Byte position in the hashed file | Range is within that file; a carrier offset is not a child-module offset |
| ELF VA | Link-time virtual address in an ELF image | Select its section/segment; check file-backed extent, not only memory extent |
| PE RVA | Virtual address relative to that PE's ImageBase | Select the matching section and its raw-data range |
| Static VA | ImageBase plus RVA for a PE | Confirm the analysis tool did not rebase the image |
| Analysis address | Address assigned by the selected analysis import | Use the retained import map; it may differ from ELF VA or PE static VA |
| Runtime VA | Process/module virtual address at the capture | Bind module hash, load mapping/base and capture record; historical pointers select no live process |

For an ordinary ELF load segment, conversion is
`file_offset = p_offset + (elf_va - p_vaddr)` **only when** the full selected
range is in that segment's file-backed `p_filesz`. A range in `p_memsz` alone
can refer to zero-initialized memory with no source bytes. ET_REL kernel module
addresses additionally need the section identity and relocations; they are not
one already-linked executable address space.

For an ordinary PE section, conversion is
`file_offset = PointerToRawData + (rva - VirtualAddress)` only within the
section's actual raw-data extent. Section alignment padding and virtual-only
tails are not executable bytes obtained by this formula. Headers have their
own mapping. Function extent and byte-window extent are separate: an eight-byte
validated window does not validate the inferred end of the whole function.

A concrete EMGD example is `SGXInit` in the matched Linux initializer. Its
retained Ghidra import adds `0x10000` to ELF VAs; this is a property of that
analysis, not a universal Ghidra rule. The [bootstrap chapter](emgd-linux/bootstrap-and-memory.md)
binds the original file, import and selected shift/store instructions. Applying
its analysis address directly as a file offset would select the wrong bytes.

## GPU and command data

| Domain | Typical producer/consumer | Common mistake |
| --- | --- | --- |
| GPU virtual byte address | Allocation/relocation → SGX MMU requester | Treating it as CPU physical memory or a host pointer |
| Physical page/PFN | Pinned backing → MMU/GTT entry | Combining display GTT and SGX page-table rules |
| MMIO offset | Register definition → mapped BAR window | Omitting the SGX subwindow when quoting a device BAR address |
| Service selector | Matched initializer → services bridge → CCB scheduler | Applying one firmware entry conversion to all PDS/USE fields |
| USE instruction/task entry | Selected code bank and task builder → PDS/USE consumer | Losing bank/DM context or confusing bytes with instructions |
| PDS code/data field | PDS allocation/layout → packet or register field | Treating code, data, base and size shifts as interchangeable |
| Packet-relative field | Versioned serializer → matched parser/firmware | Reusing a structure offset from a different driver family |
| CCB cursor | Matched ring producer/consumer | Assuming every ring uses byte rather than DWORD cursors |

The matched EMGD service conversion and code-bank base encoding deliberately
appear as different contracts in [SGX firmware](firmware/sgx.md).
The [PSB linkage chapter](psb/pds-use-and-depth.md) covers another
selected PDS→USE constructor. Agreement in names does not merge their units.

## BIOS, extracted children and firmware

A Dell update carrier, its decompressed module and a PCI option ROM are three
different byte spaces. A 16:16 real-mode address also needs the segment mapping
used by that analysis. Keep extraction method, parent hash, selected range and
child hash together. A decompressor's reported length is not proof that the
last ROM byte was retained. [Dell VBIOS](platform/bios-display-power.md)
explains the corrected extraction and GTT-range findings.

Firmware extracted from a host ELF symbol has both a parent range and a child
identity. Its instruction architecture is independent of the carrier's x86
architecture. Runtime-patched bytes are a further identity. Do not decode SGX
USE/PDS or MSVDX MTX streams as x86 to obtain plausible pseudocode.

## Minimal locator

A portable locator contains the component/package, complete hash and size,
original or inferred symbol, address kind, selected start/size, mapping rule,
producer/consumer and remaining coverage limits. Retained observation records
are identified by their content hash and record selector when available; a
workstation path is not an artifact identity. Use unknown for an unproved
mapping. The source bytes and complete analysis records are separate from this
reference; a short locator does not establish complete semantic coverage.
