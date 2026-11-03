# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Copy a selected real-Mesa WINDOW source into a symbolic kernel input header.

The report selects CPU producer evidence; it is not an authentication token or
permission to submit work. Native packets, addresses and task bytes are never
imported. The kernel's typed constructor validates and compiles these inputs.
"""

import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct

SCHEMA = "poulsback-actual-st-gl-window-source-v1"
STATUS = "PASS_CPU_ACTUAL_ST_GL_WINDOW_SOURCE_CONSTRUCTION_ONLY"
PREFIX = "gl-window-w64-h64-v14."
WINDING_PREFIX = "gl-window-winding-w64-h64."
SPANS = {"clip": 96, "vertices": 192, "indices": 24, "program": 224, "window-conventions": 48}


def identity(raw):
    return dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def ordinary(path, maximum):
    """Read one bounded ordinary file without following a final symlink."""
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= maximum:
            raise ValueError("Bounded ordinary WINDOW source required")
        raw = bytearray()
        while len(raw) <= maximum:
            chunk = os.read(fd, min(65536, maximum + 1 - len(raw)))
            if not chunk:
                break
            raw.extend(chunk)
        after = os.fstat(fd)
        fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        if (
            any(getattr(before, k) != getattr(after, k) for k in fields)
            or len(raw) != before.st_size
        ):
            raise ValueError("WINDOW source changed during read")
        return bytes(raw)
    finally:
        os.close(fd)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate WINDOW report key")
        result[key] = value
    return result


def load(directory, report_sha256):
    """Return owned symbolic bytes and their complete two-flavor source pins."""
    return _load_window(directory, report_sha256, winding=False)


def load_winding(directory, report_sha256):
    """Select the distinct pre-draw winding experiment, never relabel old evidence."""
    return _load_window(directory, report_sha256, winding=True)


def _load_window(directory, report_sha256, *, winding):
    directory = Path(directory).absolute()
    if directory.resolve(strict=True) != directory or not directory.is_dir():
        raise ValueError("Canonical WINDOW source directory required")
    if type(report_sha256) is not str or not re.fullmatch("[0-9a-f]{64}", report_sha256):
        raise ValueError("Explicit original WINDOW report SHA256 required")
    report_path = directory / "report.json"
    raw_report = ordinary(report_path, 4 * 1024**2)
    if identity(raw_report)["sha256"] != report_sha256:
        raise ValueError("Selected WINDOW producer report differs")
    report = json.loads(raw_report, object_pairs_hook=unique_object)
    if (
        type(report) is not dict
        or report.get("schema")
        != ("poulsback-actual-st-gl-window-winding-source-v1" if winding else SCHEMA)
        or report.get("status")
        != (
            "PASS_CPU_ACTUAL_ST_GL_WINDOW_WINDING_SOURCE_AND_CLIP_PACKET_ONLY"
            if winding
            else STATUS
        )
        or report.get("profile") != ("gl-window-winding" if winding else "gl-window")
        or report.get("inputs_libraries_and_toolchain_unchanged") is not True
    ):
        raise ValueError("Completed real-Mesa WINDOW producer report required")
    commands = report.get("commands")
    if (
        type(commands) is not list
        or not commands
        or any(
            type(row) is not dict
            or type(row.get("exit_code")) is not int
            or row["exit_code"] != 0
            or row.get("status") != "PASS"
            or row.get("output_closed") is not True
            or row.get("process_group_stopped") is not True
            for row in commands
        )
    ):
        raise ValueError("WINDOW producer command closure is incomplete")
    if winding and (
        report.get("geometry_variant") != "second-triangle-reversed-input-order-v1"
        or report.get("input_order") != [0, 1, 2, 3, 5, 4]
        or report.get("actual_mesa_producer") is not True
        or report.get("ordinary_context_or_DRI_path") is not False
        or report.get("source_flags_rewritten") is not False
        or report.get("source_output_rewritten") is not False
        or report.get("raw_min_samples_precondition") != 1
        or report.get("selected_GL_model") != "half1-bottom-left"
    ):
        raise ValueError("Unmodified actual pre-draw winding source required")
    prefix = WINDING_PREFIX if winding else PREFIX
    names = {prefix + key + ".original.bin" for key in SPANS}
    selected, pins = {}, []
    for flavor in ("strict-cases", "ubsan-cases"):
        folder = directory / flavor
        if (
            folder.is_symlink()
            or not folder.is_dir()
            or {p.name for p in folder.iterdir()} != names
        ):
            raise ValueError("Exactly five original WINDOW records per flavor required")
        for key, size in SPANS.items():
            path = folder / (prefix + key + ".original.bin")
            raw = ordinary(path, size)
            if len(raw) != size or (key in selected and raw != selected[key]):
                raise ValueError("WINDOW extent or strict/UBSAN disagreement: " + key)
            selected[key] = raw
            pins.append(dict(path=str(path), **identity(raw)))
            source = report.get(flavor.removesuffix("-cases") + "_source")
            source_pin = source.get(key) if type(source) is dict else None
            if type(source_pin) is not dict or identity(raw) != {
                field: source_pin.get(field) for field in ("bytes", "sha256")
            }:
                raise ValueError("WINDOW span differs from selected producer: " + key)
    if struct.unpack("<6I", selected["indices"]) != tuple(range(6)):
        raise ValueError("Selected WINDOW source must retain deindexed ordinal indices")
    convention = struct.unpack("<12I", selected["window-conventions"])
    viewport = struct.unpack("<6f", selected["window-conventions"][24:])
    if (
        convention[:6] != (1, 1, 1, 0xFFFFFFFF, 1, 0)
        or not all(math.isfinite(v) for v in viewport)
        or viewport[1] >= 0
    ):
        raise ValueError("Selected WINDOW source must preserve actual GL window conventions")
    raw_manifest, manifest_sha256, raw_header = encode_source(
        selected, report_sha256, raw_report, winding=winding
    )
    # Recheck the report after the source reads. The build driver repeats this
    # complete selection after compilation before accepting the staged result.
    if ordinary(report_path, 4 * 1024**2) != raw_report:
        raise ValueError("WINDOW producer report changed during selection")
    return dict(
        header=raw_header,
        source_manifest=raw_manifest,
        pins=pins,
        receipt=dict(
            scope="MESA_CPU_SYMBOLIC_WINDOW_INPUTS_ONLY",
            report=str(report_path),
            report_sha256=report_sha256,
            header_sha256=identity(raw_header)["sha256"],
            source_manifest_sha256=manifest_sha256,
            scene="gl-window-winding" if winding else "gl-window",
            width=64,
            height=64,
            vertex_count=6,
            index_count=6,
            fragment_instructions=8,
            packet_version=15 if winding else 14,
            packet_profile=14 if winding else 13,
            outer_version=7 if winding else 6,
            half_pixel_center=1,
            bottom_edge_rule=1,
            min_samples=1,
        ),
    )


def encode_source(selected, report_sha256, raw_report, *, literal_count=None, winding=False):
    manifest = dict(
        schema=(
            "poulsback-derived-symbolic-window-winding-source-v1"
            if winding
            else "poulsback-derived-symbolic-window-source-v1"
            if literal_count is None
            else "poulsback-derived-symbolic-pack-literal-source-v1"
        ),
        producer_report=identity(raw_report),
        spans=[dict(name=key, **identity(raw)) for key, raw in selected.items()],
        scope="Derived source index, not an original collector record or hardware authority",
    )
    raw_manifest = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    manifest_sha256 = identity(raw_manifest)["sha256"]
    header = [
        "/* Copied Mesa CPU source only; no native packets or GPU admission. */",
        "#define POULSBACK_WINDOW_MESH_INPUT_VERTEX_COUNT 6U",
        '#define POULSBACK_WINDOW_MESH_INPUT_REPORT_SHA256 "' + report_sha256 + '"',
        '#define POULSBACK_WINDOW_MESH_INPUT_MANIFEST_SHA256 "' + manifest_sha256 + '"',
    ]
    for key, raw in selected.items():
        header.append(
            '#define POULSBACK_WINDOW_MESH_INPUT_'
            + key.replace("-", "_").upper()
            + '_SHA256 "'
            + identity(raw)["sha256"]
            + '"'
        )
    if literal_count is not None:
        header.append("#define POULSBACK_WINDOW_MESH_INPUT_FRAGMENT_COUNT %uU" % literal_count)
    for key, raw in selected.items():
        if key == "source-tap":
            continue
        words = struct.unpack("<" + "I" * (len(raw) // 4), raw)
        header.append("static const u32 window_mesh_input_" + key.replace("-", "_") + "[] = {")
        for offset in range(0, len(words), 8):
            header.append("\t" + ",".join("0x%08xU" % v for v in words[offset : offset + 8]) + ",")
        header.append("};")
    raw_header = ("\n".join(header) + "\n").encode()
    return raw_manifest, manifest_sha256, raw_header


def load_literal(directory, report_sha256):
    """Select the diagnostic generic-NIR literal producer, never the old DRI source.

    Only symbolic inputs enter the kernel header. The source tap is checked
    against those same arrays and indexed, but no packet/USE bytes are imported.
    The kernel still regenerates and validates its complete task and bindings.
    """
    directory = Path(directory).absolute()
    if directory.resolve(strict=True) != directory or not directory.is_dir():
        raise ValueError("Canonical literal source directory required")
    if type(report_sha256) is not str or not re.fullmatch("[0-9a-f]{64}", report_sha256):
        raise ValueError("Explicit literal producer report SHA256 required")
    report_path = directory / "report.json"
    raw_report = ordinary(report_path, 4 * 1024**2)
    if identity(raw_report)["sha256"] != report_sha256:
        raise ValueError("Selected literal producer report differs")
    report = json.loads(raw_report, object_pairs_hook=unique_object)
    if (
        type(report) is not dict
        or report.get("schema") != "poulsback-mesa-pack-literal-check-v1"
        or report.get("status")
        != "PASS_CPU_ACTUAL_MESA_NIR_LITERAL_SOURCE_AND_RETAINED_PACKET_ONLY"
        or report.get("profile") != "pack-literal-thresholds"
        or report.get("actual_mesa_producer") is not True
        or report.get("ordinary_context_or_DRI_path") is not False
        or report.get("inputs_libraries_and_toolchain_unchanged") is not True
    ):
        raise ValueError("Completed diagnostic Mesa literal producer required")
    commands = report.get("commands")
    if (
        type(commands) is not list
        or not commands
        or any(
            type(row) is not dict
            or type(row.get("exit_code")) is not int
            or row["exit_code"] != 0
            or row.get("status") != "PASS"
            or row.get("output_closed") is not True
            or row.get("process_group_stopped") is not True
            for row in commands
        )
    ):
        raise ValueError("Literal producer command closure is incomplete")
    prefix = "pack-literal-w64-h64-thresholds."
    extents = {**SPANS, "program": 16 * 28, "source-tap": 288}
    selected, pins = {}, []
    for flavor in ("strict-cases", "ubsan-cases"):
        folder = directory / flavor
        if folder.is_symlink() or not folder.is_dir():
            raise ValueError("Ordinary literal case directory required")
        if {p.name for p in folder.iterdir() if p.name.startswith(prefix)} != {
            prefix + key + ".original.bin" for key in extents
        }:
            raise ValueError("Exactly six original literal records required")
        for key, bound in extents.items():
            path = folder / (prefix + key + ".original.bin")
            raw = ordinary(path, bound)
            if (key != "program" and len(raw) != bound) or (
                key in selected and raw != selected[key]
            ):
                raise ValueError("Literal extent or strict/UBSAN disagreement: " + key)
            selected[key] = raw
            pins.append(dict(path=str(path), **identity(raw)))
    count = literal_program_count(selected["program"])
    if type(report.get("source_instruction_count")) is not int or (
        report["source_instruction_count"] != count
    ):
        raise ValueError("Literal instruction count differs from source")
    if selected["indices"] != struct.pack("<6I", *range(6)):
        raise ValueError("Literal ordinal indices required")
    if selected["window-conventions"] != struct.pack(
        "<6I6f", 1, 1, 1, 0xFFFFFFFF, 1, 0, 32, -32, 0.5, 32, 32, 0.5
    ):
        raise ValueError("Literal raw1 GL window conventions required")
    tap = b"".join(
        selected["clip"][16 * i : 16 * (i + 1)] + selected["vertices"][32 * i : 32 * (i + 1)]
        for i in range(6)
    )
    if selected["source-tap"] != tap:
        raise ValueError("Literal source tap differs from geometry")
    manifest, manifest_sha, header = encode_source(
        selected, report_sha256, raw_report, literal_count=count
    )
    if ordinary(report_path, 4 * 1024**2) != raw_report:
        raise ValueError("Literal producer report changed during selection")
    return dict(
        header=header,
        source_manifest=manifest,
        pins=pins,
        receipt=dict(
            scope="MESA_CPU_GENERIC_NIR_LITERAL_INPUTS_ONLY",
            report=str(report_path),
            report_sha256=report_sha256,
            header_sha256=identity(header)["sha256"],
            source_manifest_sha256=manifest_sha,
            scene="pack-literal-thresholds",
            width=64,
            height=64,
            vertex_count=6,
            index_count=6,
            fragment_instructions=count,
            packet_version=15,
            packet_profile=14,
            outer_version=7,
            half_pixel_center=1,
            bottom_edge_rule=1,
            min_samples=1,
        ),
    )


def literal_program_count(raw):
    """Verify the complete scalar graph; no output may depend on COLOR0."""
    if len(raw) % 28 or not 9 <= len(raw) // 28 <= 16:
        raise ValueError("Bounded literal scalar graph required")
    instructions = list(struct.iter_unpack("<7I", raw))
    count, inputs = len(instructions), 0
    rgba = (0x3F000000, 0x3F010101, 0x3EFDFDFE, 0x3F800000)
    for i, (op, immediate, index, component, a, b, c) in enumerate(instructions):
        if i >= count - 4:
            color = i - (count - 4)
            if (op, immediate, index, component, b, c) != (8, 0, color, 0, 0, 0):
                raise ValueError("Literal final RGBA output required")
            if a >= i or instructions[a] != (0, rgba[color], 0, 0, 0, 0, 0):
                raise ValueError("Literal output must reference its exact constant")
        elif op == 0:
            if (index, component, a, b, c) != (0, 0, 0, 0, 0) or immediate not in (0, *rgba):
                raise ValueError("Unexpected literal scalar constant")
        elif (op, immediate, index, component, a, b, c) == (1, 0, 0, 3, 0, 0, 0):
            inputs += 1
        else:
            raise ValueError("Unexpected literal scalar operation")
    if inputs != 1:
        raise ValueError("Exactly one unused COLOR0 alpha declaration required")
    return count
