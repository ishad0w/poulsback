#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Check the integration-patched display GTT and resume bodies on RAM edges."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/kernel/test_display_gtt.c"
LOCK = ROOT / "kernel/integration/linux.lock.json"
PATCH = ROOT / "kernel/integration/linux.patch"


def identity(path):
    raw = path.read_bytes()
    return {"path": str(path), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def write_json(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def function(text, name):
    """Select one complete function in the pinned C source profile."""
    match = re.search(r"(?m)^(?:static )?(?:void|int|uint32_t) " + name + r"\([^;]*?\)\n\{", text)
    if not match:
        raise ValueError("Missing source function: " + name)
    start = text.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start() : end] + "\n"


def command(argv, directory, name):
    start = time.monotonic_ns()
    timeout = False
    with (
        (directory / (name + ".stdout")).open("xb") as stdout,
        (directory / (name + ".stderr")).open("xb") as stderr,
    ):
        child = subprocess.Popen(
            argv, cwd=directory, stdout=stdout, stderr=stderr, start_new_session=True
        )
        try:
            code = child.wait(timeout=120)
        except subprocess.TimeoutExpired:
            timeout = True
            os.killpg(child.pid, signal.SIGTERM)
            try:
                code = child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                code = child.wait(timeout=5)
    row = {
        "argv": argv,
        "pid": child.pid,
        "returncode": code,
        "timeout": timeout,
        "begin_ns": start,
        "end_ns": time.monotonic_ns(),
    }
    write_json(directory / (name + ".json"), row)
    if code or timeout:
        raise RuntimeError("Failed source check: " + name)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--i386", action="store_true")
    args = parser.parse_args()
    source = args.linux_source.resolve(strict=True)
    output = args.output.absolute()
    if output.parent.resolve(strict=True) != output.parent:
        raise ValueError("Use a canonical output parent")
    lock = json.loads(LOCK.read_bytes())
    selected = {}
    for relative, expected in lock["linux_source_files"].items():
        path = source / relative
        if path.resolve(strict=True) != path:
            raise ValueError("Symlinked Linux input: " + relative)
        actual = identity(path)
        if {key: actual[key] for key in ("bytes", "sha256")} != expected:
            raise ValueError("Linux input differs from lock: " + relative)
        selected[relative] = actual
    project = [identity(path) for path in (LOCK, PATCH, FIXTURE, Path(__file__).resolve())]
    output.mkdir()  # Existing and partial outputs are never reused.
    stage = output / "linux"
    stage.mkdir()
    for relative in selected:
        path = Path(relative)
        if str(path.parent) == "drivers/gpu/drm/gma500":
            shutil.copyfile(source / relative, stage / path.name)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    children = [
        command(
            [
                "/usr/bin/patch",
                "--batch",
                "--fuzz=0",
                "--no-backup-if-mismatch",
                "-p1",
                "-i",
                str(PATCH),
            ],
            stage,
            "patch",
        )
    ]
    gtt = (stage / "gtt.c").read_text()
    gem = (stage / "gem.c").read_text()
    power = (stage / "power.c").read_text()
    header = (stage / "gtt.h").read_text()
    structure = re.search(r"(?ms)^struct psb_gtt \{.*?^\};", header)
    if not structure:
        raise ValueError("Missing pinned GTT structure")
    # Keep the complete production bodies; only their kernel include directives
    # are replaced by the fixture's explicit RAM/PCI/I/O boundary.
    (output / "gtt.current.inc").write_text(
        "".join(line for line in gtt.splitlines(keepends=True) if not line.startswith("#include "))
    )
    (output / "gtt-struct.current.inc").write_text(structure.group(0) + "\n")
    (output / "gem-stolen.current.inc").write_text(function(gem, "psb_gem_mm_populate_stolen"))
    (output / "power-resume.current.inc").write_text(
        function(power, "gma_resume_display") + "\n" + function(power, "gma_power_resume")
    )
    (output / "test.c").write_bytes(FIXTURE.read_bytes())
    binaries = []
    for flavor in ("strict", "ubsan"):
        flags = [
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wshadow",
            "-Wstrict-prototypes",
        ]
        if args.i386:
            flags += ["-m32"]
        if flavor == "ubsan":
            flags += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
        binary = output / flavor
        children.append(
            command(
                ["/usr/bin/cc", *flags, str(output / "test.c"), "-o", str(binary)],
                output,
                flavor + "-compile",
            )
        )
        elf = binary.read_bytes()[:20]
        if elf[:4] != b"\x7fELF" or (
            args.i386 and (elf[4] != 1 or int.from_bytes(elf[18:20], "little") != 3)
        ):
            raise ValueError("Unexpected compiled target")
        binaries.append(identity(binary))
        children.append(command([str(binary)], output, flavor + "-run"))
    if project != [identity(path) for path in (LOCK, PATCH, FIXTURE, Path(__file__).resolve())]:
        raise ValueError("Project inputs changed")
    if selected != {relative: identity(source / relative) for relative in selected}:
        raise ValueError("Linux inputs changed")
    report = {
        "status": "PASS_SOURCE_BOUND_DISPLAY_GTT_CPU",
        "scope": "I386_RAM_CPU" if args.i386 else "HOST_RAM_CPU",
        "project_inputs": project,
        "linux_inputs": selected,
        "patched_sources": [
            identity(stage / name) for name in ("gtt.c", "gtt.h", "gem.c", "power.c")
        ],
        "binaries": binaries,
        "children": children,
        "inputs_unchanged": True,
        "hardware": False,
        "GPU_retirement": False,
        "prewrite_firmware_root_authenticated": False,
    }
    write_json(output / "report.json", report)
    print(
        json.dumps(
            {
                "status": report["status"],
                "scope": report["scope"],
                "report": str(output / "report.json"),
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
