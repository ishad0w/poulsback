# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Data-only selection of reviewed build requirements, never GPU admission."""

import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
MAX_MANIFEST = 16384
MAX_TARGETS = 128


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("Duplicate build-target key: " + key)
        result[key] = value
    return result


def _fields(value, names):
    if not isinstance(value, dict) or set(value) != set(names):
        raise ValueError("Unexpected or missing build-target fields")


def _regular(path):
    if path.is_symlink() or not path.is_file() or path.absolute() != path.resolve():
        raise ValueError("Build-target input must be a canonical ordinary file: " + str(path))
    return path


def _identity(path):
    digest, size = hashlib.sha256(), 0
    with _regular(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    return {"bytes": size, "sha256": digest.hexdigest()}


def _relative(value):
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or any(char in value for char in "\0\r\n")
        or PurePosixPath(value).is_absolute()
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise ValueError("Build-target path must be repository relative")
    return value


def _cpu_flags(flags):
    if (
        not isinstance(flags, list)
        or len(flags) != 2
        or flags[0] != "-march=i686"
        or not isinstance(flags[1], str)
        or not re.fullmatch(r"-mtune=[a-z0-9][a-z0-9-]*", flags[1])
        or flags[1] == "-mtune=native"
    ):
        raise ValueError("Explicit i686 CPU baseline and portable tuning required")


def _executable(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"[A-Za-z0-9_+][A-Za-z0-9_+.-]{0,127}", value
    ):
        raise ValueError("One tool executable basename required, not a command")


def validate(target):
    """Only the currently implemented ABI/policies are selectable in v1."""
    _fields(
        target,
        (
            "schema",
            "id",
            "kind",
            "architecture",
            "inputs",
            "toolchain",
            "graphics",
            "build_metadata",
        ),
    )
    if (
        target["schema"] != "poulsback-build-target-v1"
        or not isinstance(target["id"], str)
        or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", target["id"])
        or target["kind"] not in ("reference", "candidate")
    ):
        raise ValueError("Unsupported build-target schema or identity")
    arch = target["architecture"]
    _fields(
        arch,
        (
            "cpu_family",
            "cpu",
            "abi",
            "endian",
            "pointer_bits",
            "elf_class",
            "elf_machine",
            "c_flags",
        ),
    )
    expected = dict(cpu_family="x86", cpu="i686", abi="linux-gnu-ilp32", endian="little")
    if any(arch[k] != v for k, v in expected.items()) or any(
        type(arch[k]) is not int or arch[k] != v
        for k, v in (("pointer_bits", 32), ("elf_class", 32), ("elf_machine", 3))
    ):
        raise ValueError("Unsupported build-target ABI")
    _cpu_flags(arch["c_flags"])
    _fields(target["inputs"], ("kernel_lock", "kernel_patch", "graphics_lock"))
    for name, value in target["inputs"].items():
        _relative(value)
        if not value.endswith(".patch" if name == "kernel_patch" else ".json"):
            raise ValueError("Wrong build-target input type")
    toolchain = target["toolchain"]
    _fields(toolchain, ("family", "native_machine", "c", "cpp", "pkg_config", "kernel_arch"))
    if (
        toolchain["family"] != "gcc"
        or toolchain["native_machine"] not in ("i386", "i486", "i586", "i686")
        or toolchain["kernel_arch"] != "x86"
    ):
        raise ValueError("Unsupported build-target toolchain policy")
    for name in ("c", "cpp", "pkg_config"):
        _executable(toolchain[name])
    graphics = target["graphics"]
    _fields(graphics, ("c_flags", "libdir", "pkg_config_dirs"))
    _cpu_flags(graphics["c_flags"])
    _relative(graphics["libdir"])
    paths = graphics["pkg_config_dirs"]
    if not isinstance(paths, list) or not 1 <= len(paths) <= 8:
        raise ValueError("Invalid target pkg-config search directories")
    for path in paths:
        _relative(path)
    if len(paths) != len(set(paths)):
        raise ValueError("Duplicate target pkg-config search directory")
    metadata = target["build_metadata"]
    _fields(metadata, ("host", "user", "timestamp"))
    for name in ("host", "user"):
        if not isinstance(metadata[name], str) or not re.fullmatch(
            r"[A-Za-z0-9_.-]{1,128}", metadata[name]
        ):
            raise ValueError("Invalid reproducible build metadata")
    if not isinstance(metadata["timestamp"], str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC", metadata["timestamp"]
    ):
        raise ValueError("Explicit UTC build timestamp required")
    return target


def _read(path):
    _regular(path)
    if not 0 < path.stat().st_size <= MAX_MANIFEST:
        raise ValueError("Build-target manifest size bound")
    raw = path.read_bytes()
    if len(raw) > MAX_MANIFEST:
        raise ValueError("Build-target manifest changed size")
    target = validate(
        json.loads(
            raw,
            object_pairs_hook=_pairs,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError("Nonfinite build-target JSON")
            ),
        )
    )
    return target, {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def discover():
    """Find bounded data manifests; scripts and other file types are never run."""
    base = ROOT / "support"
    if base.is_symlink() or not base.is_dir() or base.resolve() != base.absolute():
        raise ValueError("Canonical support directory required")
    result = {}
    count = 0
    for system in sorted(base.iterdir()):
        if system.is_symlink():
            raise ValueError("Symlink support entries are not allowed")
        if not system.is_dir():
            continue
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", system.name):
            raise ValueError("Invalid support system directory")
        for path in sorted(system.glob("*.json")):
            count += 1
            if count > MAX_TARGETS:
                raise ValueError("Build-target inventory limit")
            target, pin = _read(path)
            if target["id"] in result:
                raise ValueError("Duplicate build-target ID: " + target["id"])
            if (
                target["id"] != system.name + "-" + path.stem
                or path.stem != target["architecture"]["cpu"]
            ):
                raise ValueError("Build-target directory/filename/ID mismatch")
            result[target["id"]] = (path, target, pin)
    return result


def load(target_id):
    if not isinstance(target_id, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", target_id):
        raise ValueError("Explicit build-target ID required, not a path")
    rows = discover()
    if target_id not in rows:
        raise ValueError("Unknown build target: " + target_id)
    target = rows[target_id][1]
    for name in target["inputs"]:
        input_path(target, name)
    return target


def input_path(target, name):
    validate(target)
    return _regular(ROOT / _relative(target["inputs"][name]))


def receipt(target):
    if load(target["id"]) != target:
        raise ValueError("Build target changed during selection")
    path, current, pin = discover()[target["id"]]
    if current != target:
        raise ValueError("Build target changed during receipt")
    return {
        "id": target["id"],
        "manifest": {"path": str(path.relative_to(ROOT)), **pin},
        "selector": _identity(Path(__file__).resolve()),
        "requirements": target,
        "inputs": {name: _identity(input_path(target, name)) for name in target["inputs"]},
        "scope": "BUILD_REQUIREMENTS_ONLY_NOT_RUNTIME_OR_HARDWARE_ADMISSION",
    }


def compiler_source(cxx=False):
    assertion = "static_assert" if cxx else "_Static_assert"
    return (
        "#include <features.h>\n#include <stdint.h>\n"
        "#if !defined(__linux__) || !defined(__GLIBC__) || !defined(__i386__) || !defined(__i686__)\n"
        '#error "Expected Linux glibc i686 target"\n#endif\n'
        f'{assertion}(sizeof(void *) == 4, "pointer ABI");\n'
        f'{assertion}(sizeof(int) == 4 && sizeof(long) == 4, "ILP32 ABI");\n'
        f'{assertion}(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "byte order");\n'
        "int main(void) { return 0; }\n"
    )


def check_elf(path):
    with path.open("rb") as source:
        data = source.read(64)
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Build output must be little-endian ELF32")
    kind, machine, version = struct.unpack_from("<HHI", data, 16)
    if (
        kind not in (1, 2, 3)
        or machine != 3
        or version != 1
        or struct.unpack_from("<H", data, 40)[0] != 52
    ):
        raise ValueError("Build output must use the EM_386 ABI; x32 is unsupported")
    return {"elf_class": 32, "endian": "little", "machine": machine, "type": kind}
