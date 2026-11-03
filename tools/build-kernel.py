#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Stage and compile the physical kernel module from current source and exact inputs.

This command never installs or loads a module. Build outputs must be new and
outside input trees.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import time
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_targets

ROOT = Path(__file__).resolve().parents[1]
LICENSE_PATHS = (
    "LICENSE",
    "NOTICES.md",
    "LICENSES/GPL-2.0-only.txt",
    "LICENSES/Linux-syscall-note.txt",
)
# Project objects linked beside the patched gma500 sources.
MODULE_OBJECTS = (
    "poulsback_engine.o poulsback_overlap.o poulsback_teardown.o poulsback_context.o "
    "poulsback_scene.o "
    "poulsback_submit.o "
    "poulsback_regtable.o poulsback_twod.o sgx535_2d_encoder.o "
    "poulsback_file.o poulsback_bo.o poulsback_bo_use.o poulsback_vm.o poulsback_vm_layout.o "
    "poulsback_display.o poulsback_framebuffer.o poulsback_display_gtt.o "
    "poulsback_display_psb_source.o sgx535_debug.o sgx535_source_epoch.o "
    "sgx535_engine_irq.o sgx535_engine_irq_lock.o sgx535_engine121.o sgx535_use_code_bank.o "
    "sgx535_spm.o sgx535_spm_state.o"
)
# Entry points of the module's own work: file hooks and ioctls, the BO's GEM,
# mmap and dma-buf callbacks, the engine's scheduler callbacks and work items
# (job starts, raster, OOM, 2D feeding, QoS relax), the IRQ handler, the typed
# set_config and the page flip (typed and legacy), framebuffer callbacks and work items,
# the display fence release, the debugfs files and the diagnostics' work item
# and proc file. A callback missing here is not bounded.
STACK_ROOTS = (
    "poulsback_file_open",
    "poulsback_file_close",
    "poulsback_bo_create_ioctl",
    "poulsback_bo_info_ioctl",
    "poulsback_context_create_ioctl",
    "poulsback_context_destroy_ioctl",
    "poulsback_submit_ioctl",
    "poulsback_submit_2d_ioctl",
    "poulsback_display_info_ioctl",
    "poulsback_prime_handle_to_fd",
    "poulsback_prime_fd_to_handle",
    "poulsback_prime_import",
    "bo_open",
    "bo_close",
    "bo_free",
    "bo_fault",
    "bo_mmap",
    "bo_export",
    "bo_get_sg_table",
    "engine_run_job",
    "engine_free_job",
    "engine_timedout_job",
    "engine_start_work",
    "engine_raster_work",
    "engine_oom_work",
    "engine_twod_work",
    "engine_qos_relax",
    "gma_irq_handler",
    "poulsback_display_psb_set_config",
    "gma_crtc_page_flip",
    "psb_flip_release_work",
    "framebuffer_destroy",
    "framebuffer_create_handle",
    "framebuffer_retire_work",
    "fence_release",
    "poulsback_display_debugfs_init",
    "psb_display_show",
    "psb_position_show",
    "psb_flips_show",
    "engine_show",
    "emit",
    "open_cache",
    "close_cache",
)
# Indirect calls the aggregate follows: the IRQ route's executor handler.
STACK_CALLBACKS = {"sgx535_engine_irq_route_locked": ("engine_irq",)}


def identity(path):
    raw = path.read_bytes()
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def tool_path(program, env):
    found = shutil.which(program, path=env.get("PATH"))
    if not found:
        raise ValueError("Unresolved build tool: " + program)
    path = Path(found).resolve(strict=True)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError("Build tool must be an ordinary executable: " + program)
    return path


def collect_toolchain(output, target, env, report):
    """Retain selected executables and query output, not a transitive sysroot seal."""
    result = {
        "schema": "poulsback-kernel-toolchain-v1",
        "tools": {},
        "scope": "SELECTED_EXECUTABLES_NOT_COMPLETE_HEADER_OR_DYNAMIC_LIBRARY_CLOSURE",
        "environment": {
            name: env[name]
            for name in (
                "PATH",
                "MAKEFLAGS",
                "MFLAGS",
                "GNUMAKEFLAGS",
                "ARCH",
                "CROSS_COMPILE",
                "LD",
                "AS",
                "AR",
                "NM",
                "OBJCOPY",
                "OBJDUMP",
                "READELF",
                "STRIP",
                "LLVM",
                "LLVM_IAS",
                "KBUILD_OUTPUT",
                "KBUILD_EXTRA_SYMBOLS",
                "KBUILD_USERCFLAGS",
                "KBUILD_USERLDFLAGS",
                "HOSTCFLAGS",
                "HOSTCXXFLAGS",
                "HOSTLDFLAGS",
                "CPATH",
                "C_INCLUDE_PATH",
                "CPLUS_INCLUDE_PATH",
                "COMPILER_PATH",
                "LIBRARY_PATH",
                "GCC_EXEC_PREFIX",
                "SOURCE_DATE_EPOCH",
                "KBUILD_BUILD_TIMESTAMP",
                "KBUILD_BUILD_USER",
                "KBUILD_BUILD_HOST",
                "LC_ALL",
            )
            if name in env
        },
    }
    report["toolchain"] = result

    def retain(name, program):
        if not program or "\0" in program or "\n" in program:
            raise ValueError("Invalid compiler tool response: " + name)
        path = tool_path(program, env)
        result["tools"][name] = {"requested": program, "path": str(path), **identity(path)}
        return path

    def query(name, argv):
        row = {"target": "toolchain", "argv": argv, "returncode": None}
        report["commands"].append(row)
        stdout = stderr = b""
        started = time.monotonic()
        try:
            child = subprocess.run(argv, capture_output=True, timeout=15, env=env)
            stdout, stderr = child.stdout, child.stderr
            row["returncode"] = child.returncode
            if child.returncode:
                raise RuntimeError("Build tool query failed: " + name)
            return stdout
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b"", error.stderr or b""
            row["timed_out"] = True
            raise RuntimeError("Build tool query timed out: " + name) from error
        except OSError as error:
            row["error"] = str(error)
            raise
        finally:
            row["seconds"] = time.monotonic() - started
            for stream, raw in (("stdout", stdout), ("stderr", stderr)):
                path = output / ("toolchain-" + name + "." + stream)
                path.write_bytes(raw)
                row[stream] = {"path": path.name, **identity(path)}

    compiler = retain("gcc", target["toolchain"]["c"])
    make = retain("make", "make")
    query("gcc-version", [str(compiler), "--version"])
    query("make-version", [str(make), "--version"])
    result["gcc_target"] = query("gcc-target", [str(compiler), "-dumpmachine"]).decode().strip()
    for name in ("cc1", "as", "ld"):
        program = query(name, [str(compiler), "-print-prog-name=" + name]).decode().strip()
        retain(name, program)
    return result


def verify_toolchain(toolchain, env):
    for name, expected in toolchain["tools"].items():
        path = tool_path(expected["requested"], env)
        if str(path) != expected["path"] or identity(path) != {
            key: expected[key] for key in ("bytes", "sha256")
        }:
            raise ValueError("Build tool changed during build: " + name)


def kernel_stack_policy(lock):
    config = lock["abi_inputs"][".config"]
    policy = lock.get("stack_policy")
    if (
        not isinstance(policy, dict)
        or set(policy)
        != {
            "schema",
            "architecture",
            "thread_size_bytes",
            "config_frame_warn_bytes",
            "fatal_frame_bytes",
            "basis",
        }
        or policy["schema"] != "poulsback-kernel-stack-policy-v1"
        or policy["architecture"] != "x86_32"
        or type(policy["thread_size_bytes"]) is not int
        or policy["thread_size_bytes"] != 8192
        or type(policy["config_frame_warn_bytes"]) is not int
        or type(policy["fatal_frame_bytes"]) is not int
        or not 0
        < policy["config_frame_warn_bytes"]
        < policy["fatal_frame_bytes"]
        <= policy["thread_size_bytes"] // 2
        or not isinstance(policy["basis"], str)
        or not policy["basis"].strip()
    ):
        raise ValueError("No reviewed x86_32 kernel stack policy for this ABI configuration")
    if (
        not isinstance(config, dict)
        or set(config) != {"bytes", "sha256"}
        or type(config["bytes"]) is not int
        or config["bytes"] <= 0
        or not isinstance(config["sha256"], str)
        or not re.fullmatch(r"[0-9a-f]{64}", config["sha256"])
    ):
        raise ValueError("Invalid stack policy ABI configuration identity")
    return {
        **policy,
        "abi_config": config,
        "scope": "Per-function admission only; cumulative call/IRQ stack safety is not proven",
    }


def kernel_object_function_symbols(path):
    """Read defined ELF32/i386 functions and bindings without executing tools."""
    raw = path.read_bytes()
    if (
        len(raw) < 52
        or raw[:7] != b"\x7fELF\x01\x01\x01"
        or struct.unpack_from("<HH", raw, 16) != (1, 3)
    ):
        raise ValueError("ordinary ELF32 i386 relocatable object required")
    offset = struct.unpack_from("<I", raw, 32)[0]
    entry_size, count = struct.unpack_from("<HH", raw, 46)
    if entry_size != 40 or not count or offset + count * 40 > len(raw):
        raise ValueError("bounded ELF32 section table required")
    found = False
    functions = []
    for index in range(count):
        section = struct.unpack_from("<10I", raw, offset + index * 40)
        if section[1] != 2:  # SHT_SYMTAB
            continue
        found = True
        start, size, stride = section[4], section[5], section[9]
        if stride != 16 or size % stride or start + size > len(raw):
            raise ValueError("bounded ELF32 symbol table required")
        if section[6] >= count:
            raise ValueError("bounded ELF32 symbol string table required")
        strings = struct.unpack_from("<10I", raw, offset + section[6] * 40)
        if strings[1] != 3 or strings[4] + strings[5] > len(raw):
            raise ValueError("ordinary ELF32 symbol string table required")
        names = raw[strings[4] : strings[4] + strings[5]]
        for position in range(start, start + size, stride):
            name_offset, _value, _size, info, _other, defined = struct.unpack_from(
                "<IIIBBH", raw, position
            )
            if info & 15 in (2, 10) and defined:  # STT_FUNC / STT_GNU_IFUNC
                end = names.find(b"\0", name_offset)
                if name_offset >= len(names) or end < 0:
                    raise ValueError("bounded ELF32 function name required")
                functions.append(
                    {"name": names[name_offset:end].decode("ascii"), "binding": info >> 4}
                )
    if not found:
        raise ValueError("ELF32 symbol table required for empty stack report")
    return functions


def kernel_object_has_functions(path):
    return bool(kernel_object_function_symbols(path))


def kernel_stack_symbol(name):
    """GCC's .su names retain clone kinds but omit their terminal ordinal."""
    return re.sub(r"\.(constprop|isra)\.\d+$", r".\1", name)


def kernel_section_relocations(raw):
    """Name the function behind each i386 PC32/PLT32 relocation against a section.

    Keys are (relocated section, offset); the REL addend in place plus the 4-byte
    displacement gives the destination offset inside the symbol's section.
    """
    shoff = struct.unpack_from("<I", raw, 32)[0]
    count, names_index = struct.unpack_from("<HH", raw, 48)
    sections = [struct.unpack_from("<10I", raw, shoff + i * 40) for i in range(count)]

    def string(table, offset):
        start = sections[table][4] + offset
        return raw[start : raw.index(b"\0", start)].decode("ascii")

    result = {}
    for rel in sections:
        if rel[1] != 9 or rel[7] >= count or rel[6] >= count:  # SHT_REL
            continue
        symtab = sections[rel[6]]
        symbols = [
            struct.unpack_from("<IIIBBH", raw, symtab[4] + j * 16) for j in range(symtab[5] // 16)
        ]
        relocated = sections[rel[7]]
        for k in range(rel[5] // 8):
            offset, info = struct.unpack_from("<II", raw, rel[4] + k * 8)
            if info & 0xFF not in (2, 4) or (info >> 8) >= len(symbols):
                continue
            section_symbol = symbols[info >> 8]
            if section_symbol[3] & 15 != 3:  # STT_SECTION
                continue
            destination = struct.unpack_from("<i", raw, relocated[4] + offset)[0] + 4
            for symbol in symbols:
                if (
                    symbol[3] & 15 == 2
                    and symbol[5] == section_symbol[5]
                    and symbol[1] <= destination < symbol[1] + symbol[2]
                ):
                    key = (string(names_index, relocated[0]), offset)
                    result[key] = string(symtab[6], symbol[0])
                    break
    return result


def kernel_stack_calls(path, env):
    """Join emitted i386 functions to real call/jump relocation targets."""
    if path.is_symlink() or not path.is_file():
        raise ValueError("Ordinary aggregate object required: " + path.name)
    before = identity(path)
    kernel_object_has_functions(path)  # Also validates ELF32/i386 relocatable ABI.
    tool = tool_path(env.get("OBJDUMP", "objdump"), env)
    tool_pin = identity(tool)
    argv = [str(tool), "-drw", "--no-show-raw-insn", "-Mintel", str(path)]
    child = subprocess.run(argv, capture_output=True, timeout=30, env=env)
    if child.returncode or identity(path) != before or identity(tool) != tool_pin:
        raise ValueError("Aggregate disassembly failed or inputs changed: " + path.name)
    sections = kernel_section_relocations(path.read_bytes())
    functions = {}
    current = section = None
    for line in child.stdout.decode("utf-8", errors="strict").splitlines():
        heading = re.fullmatch(r"Disassembly of section (\S+):", line)
        if heading:
            section = heading[1]
            continue
        header = re.fullmatch(r"[0-9a-f]+ <([^>]+)>:", line)
        if header:
            emitted = header[1]
            current = kernel_stack_symbol(emitted)
            if current in functions:
                raise ValueError("Ambiguous emitted stack symbol: " + path.name + ":" + current)
            functions[current] = {"emitted": emitted, "edges": []}
            continue
        if current is None:
            continue
        instruction = re.match(r"\s*([0-9a-f]+):\s+(call|jmp)\s+(.+)", line)
        if instruction is None:
            continue
        relocation = re.search(r"([0-9a-f]+):\s+R_386_(?:PC32|PLT32)\s+([^\s]+)", instruction[3])
        target = re.search(r"<([^>]+)>", instruction[3])
        name = relocation[2] if relocation else target[1] if target else None
        if relocation and name.startswith("."):
            name = sections.get((section, int(relocation[1], 16)), name)
        if name is None:
            functions[current]["edges"].append(
                {
                    "target": "<unresolved-indirect>",
                    "offset": instruction[1],
                    "instruction": instruction[2],
                    "relocation": False,
                }
            )
            continue
        # Offsets name intra-function branches, never a second emitted frame;
        # a cold part shares its function's frame.
        name = re.sub(r"[+-]0x[0-9a-f]+$", "", name)
        name = kernel_stack_symbol(name.removesuffix(".cold"))
        if name != current or instruction[2] == "call":
            functions[current]["edges"].append(
                {
                    "target": name,
                    "offset": instruction[1],
                    "instruction": instruction[2],
                    "relocation": bool(relocation),
                }
            )
    if not functions:
        raise ValueError("No emitted aggregate functions: " + path.name)
    return functions, {
        "object": path.name,
        **before,
        "argv": argv,
        "returncode": child.returncode,
        "tool": {"path": str(tool), **tool_pin},
        "stdout": {"bytes": len(child.stdout), "sha256": hashlib.sha256(child.stdout).hexdigest()},
        "stderr": {"bytes": len(child.stderr), "sha256": hashlib.sha256(child.stderr).hexdigest()},
    }


def license_bytes():
    inputs = {}
    for relative in LICENSE_PATHS:
        path = ROOT
        for part in Path(relative).parts:
            path /= part
            if path.is_symlink():
                raise ValueError("License input symlink is not allowed: " + relative)
        if not path.is_file():
            raise ValueError("Missing ordinary license input: " + relative)
        inputs[relative] = path.read_bytes()
    return inputs


def kernel_module_stack_usage(stage, policy, frames, env=None):
    """Conservative module-frame bound for every module entry point.

    Each root's deepest chain of emitted direct calls inside the module, plus
    the IRQ route's executor callback, is summed from GCC .su frames. Other
    indirect calls and kernel frames use the explicit reserve, not measured.
    """
    reserve = 2048
    result = {
        "status": "FAIL",
        "scope": "module-entry-points",
        "thread_size_bytes": policy["thread_size_bytes"],
        "kernel_reserve_bytes": reserve,
        "module_limit_bytes": policy["thread_size_bytes"] - reserve,
        "basis": "Qualified i386 8192-byte THREAD_SIZE minus explicit 2048-byte kernel reserve "
        "policy; conservative GCC .su module-frame sums over emitted direct calls",
        "paths": [],
        "objects": [],
        "kernel_external_calls": [],
        "indirect_calls": [],
        "errors": [],
    }
    env = dict(os.environ if env is None else env)
    index = {}
    for row in frames:
        key = (Path(row["report"]).stem, kernel_stack_symbol(row["location"].rsplit(":", 1)[-1]))
        index.setdefault(key, []).append(row)
    try:
        if policy["architecture"] != "x86_32" or policy["thread_size_bytes"] != 8192:
            raise ValueError("No qualified module aggregate stack policy for this target")
        graphs, definitions = {}, {}
        for obj in sorted(stage.glob("*.o")):
            if not obj.with_suffix(".c").is_file() or obj.name.endswith(".mod.o"):
                continue
            graph, receipt = kernel_stack_calls(obj, env)
            result["objects"].append(receipt)
            # A hot/cold split shares one frame; the cold part joins its parent.
            for name in [name for name in graph if name.endswith(".cold")]:
                parent = kernel_stack_symbol(name.removesuffix(".cold"))
                if parent not in graph:
                    raise ValueError("Cold part without its function: " + obj.name + ":" + name)
                graph[parent]["edges"] += graph.pop(name)["edges"]
            graphs[obj.stem] = graph
            for symbol in kernel_object_function_symbols(obj):
                if symbol["binding"] in (1, 2):  # STB_GLOBAL / STB_WEAK
                    name = kernel_stack_symbol(symbol["name"])
                    definitions.setdefault(name, []).append((obj.stem, name))

        def frame(key):
            rows = index.get(key, [])
            if len(rows) != 1 or rows[0]["qualifier"] not in ("static", "dynamic,bounded"):
                raise ValueError("Missing, ambiguous or unbounded .su frame: " + ":".join(key))
            return rows[0]["bytes"]

        def emitted(name):
            owners = [(stem, name) for stem in graphs if name in graphs[stem]]
            if len(owners) != 1:
                raise ValueError("Missing or ambiguous emitted function: " + name)
            return owners[0]

        cache = {}

        def deepest(node, active=()):
            if node in active:
                raise ValueError("Recursive module call path: " + ":".join(node))
            if node in cache:
                return cache[node]
            targets = set()
            for edge in graphs[node[0]][node[1]]["edges"]:
                target = edge["target"]
                # Retpoline thunks and static-call trampolines (tracepoints)
                # jump through a pointer: indirect calls.
                if target == "<unresolved-indirect>" or target.startswith(
                    ("__x86_indirect_thunk_", "__SCT__")
                ):
                    row = ":".join(node)
                    if node[1] not in STACK_CALLBACKS and row not in result["indirect_calls"]:
                        result["indirect_calls"].append(row)
                elif target in graphs[node[0]] and not (
                    edge["relocation"] and len(definitions.get(target, [])) > 1
                ):
                    targets.add((node[0], target))
                elif edge["relocation"] and target in definitions:
                    if len(definitions[target]) != 1:
                        raise ValueError("Ambiguous module definition: " + target)
                    targets.add(definitions[target][0])
                elif edge["relocation"] and not target.startswith("."):
                    if target not in result["kernel_external_calls"]:
                        result["kernel_external_calls"].append(target)
                else:
                    raise ValueError("Missing module call target: " + ":".join(node) + " " + target)
            for name in STACK_CALLBACKS.get(node[1], ()):
                targets.add(emitted(name))
            targets.discard(node)
            best = (frame(node), [node])
            for child in sorted(targets):
                size, path = deepest(child, (*active, node))
                if frame(node) + size > best[0]:
                    best = (frame(node) + size, [node] + path)
            cache[node] = best
            return best

        for name in STACK_ROOTS:
            size, path = deepest(emitted(name))
            result["paths"].append(
                {
                    "name": name,
                    "conservative_su_bytes": size,
                    "frames": [
                        {"object": stem + ".o", "symbol": symbol, "bytes": frame((stem, symbol))}
                        for stem, symbol in path
                    ],
                }
            )
        for receipt in result["objects"]:
            if identity(stage / receipt["object"]) != {
                key: receipt[key] for key in ("bytes", "sha256")
            }:
                raise ValueError("Aggregate object changed after disassembly: " + receipt["object"])
        result["max_conservative_su_bytes"] = max(
            row["conservative_su_bytes"] for row in result["paths"]
        )
        if result["max_conservative_su_bytes"] > result["module_limit_bytes"]:
            raise ValueError(
                "Module aggregate stack exceeds reserved budget: "
                + str(result["max_conservative_su_bytes"])
                + " > "
                + str(result["module_limit_bytes"])
            )
        result["status"] = "PASS_CONSERVATIVE_MODULE_BOUND"
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result["errors"].append(str(error))
    return result


def kernel_stack_usage(stage, policy, stdout, stderr, *, env=None):
    """Retain all frame warnings and reject unsafe or incomplete GCC stack data."""
    result = {"status": "FAIL", "warnings": [], "reports": [], "above_warn": [], "errors": []}
    fatal = policy["fatal_frame_bytes"]
    for stream, raw in (("stdout", stdout), ("stderr", stderr)):
        for line in raw.decode("utf-8", errors="replace").splitlines():
            if "warning:" not in line or "frame-larger-than" not in line:
                continue
            match = re.search(r"frame size of (\d+) bytes is larger than (\d+) bytes", line)
            row = {"stream": stream, "diagnostic": line}
            result["warnings"].append(row)
            if match is None:
                result["errors"].append("Unrecognized frame warning: " + line)
                continue
            size, limit = map(int, match.groups())
            row.update(frame_bytes=size, warning_limit_bytes=limit)
            if size >= fatal:
                result["errors"].append("Fatal compiler frame warning: " + line)

    objects = {path.stem for path in stage.glob("*.o") if path.with_suffix(".c").is_file()}
    reports = {path.stem: path for path in stage.glob("*.su")}
    for stem in sorted(objects - reports.keys()):
        result["errors"].append("Missing stack-usage report for C object: " + stem)
    if not objects:
        result["errors"].append("No compiled C objects with stack-usage coverage")
    frames = []
    for stem, path in sorted(reports.items()):
        if path.is_symlink() or not path.is_file() or stem not in objects:
            result["errors"].append("Unmatched ordinary stack-usage report required: " + path.name)
            continue
        before = identity(path)
        rows = path.read_text().splitlines()
        if not rows:
            obj = path.with_suffix(".o")
            try:
                object_pin = identity(obj)
                if kernel_object_has_functions(obj) or identity(obj) != object_pin:
                    raise ValueError("empty report for defined functions or changed object")
            except (OSError, ValueError) as error:
                result["errors"].append(f"Incomplete stack usage: {path.name}: {error}")
        for number, line in enumerate(rows, 1):
            match = re.fullmatch(r"(.+:\d+:\d+:[^\t]+)\t(\d+)\t(static|dynamic(?:,bounded)?)", line)
            if match is None:
                result["errors"].append(f"Malformed stack-usage row: {path.name}:{number}")
                continue
            location, size, qualifier = match.groups()
            row = {
                "report": path.name,
                "location": location,
                "bytes": int(size),
                "qualifier": qualifier,
            }
            frames.append(row)
            if int(size) > policy["config_frame_warn_bytes"]:
                result["above_warn"].append(row)
            if int(size) >= fatal or qualifier == "dynamic":
                result["errors"].append(f"Unsafe stack usage: {path.name}:{number}: {line}")
        if identity(path) != before:
            result["errors"].append("Stack-usage report changed during validation: " + path.name)
        result["reports"].append({"path": path.name, **before, "rows": len(rows)})
    if not frames:
        result["errors"].append("No function stack-usage rows")
    result.update(
        c_objects=len(objects),
        functions=len(frames),
        max_stack_usage_bytes=max((row["bytes"] for row in frames), default=0),
    )
    aggregate = kernel_module_stack_usage(stage, policy, frames, env=env)
    result["module_aggregate"] = aggregate
    result["errors"].extend(aggregate["errors"])
    for receipt in result["reports"]:
        if identity(stage / receipt["path"]) != {key: receipt[key] for key in ("bytes", "sha256")}:
            result["errors"].append(
                "Stack-usage report changed after aggregate validation: " + receipt["path"]
            )
    if not result["errors"]:
        result["status"] = "PASS_PER_FUNCTION_LIMIT_ONLY"
    return result


def validate_inputs(headers, source, *, lock_path):
    lock = json.loads(lock_path.read_text())
    for relative, expected in lock["abi_inputs"].items():
        if identity(headers / relative) != expected:
            raise ValueError("Kernel ABI input differs: " + relative)
    for relative, expected in lock["linux_source_files"].items():
        if identity(source / relative) != expected:
            raise ValueError("External Linux source differs: " + relative)
    return lock


def project_sources():
    for path in sorted((ROOT / "kernel").rglob("*")):
        if path.is_symlink():
            raise ValueError(
                "Project source symlink is not allowed: " + str(path.relative_to(ROOT))
            )
        if not path.is_file() or path.suffix not in (".c", ".h", ".inc"):
            continue
        if "test" in path.relative_to(ROOT).parts or "_test." in path.name:
            continue
        yield path
    for name in ("poulsback_bo_uapi.h", "poulsback_exec_uapi.h"):
        yield ROOT / "include/uapi/poulsback" / name
    for name in (
        "raster/sgx535_engine121",
        "raster/sgx535_2d_encoder",
        "compiler/sgx535_use_code_bank",
        "raster/sgx535_spm",
        "raster/sgx535_spm_state",
    ):
        yield ROOT / "src" / (name + ".c")
        yield ROOT / "src" / (name + ".h")


class KernelBuildPlan(NamedTuple):
    """Resolved files, Kbuild and receipt; no admission or device authority.

    Resolved once before output creation. Staging rechecks every source byte
    against this plan before copying, then executes its fixed patch.
    """

    build_target: str
    source: Path
    files: tuple
    patch: tuple
    kbuild: str
    license_inputs: str


def make_build_plan(source, build_target):
    lock_path = build_targets.input_path(build_target, "kernel_lock")
    patch_path = build_targets.input_path(build_target, "kernel_patch")
    notices = license_bytes()
    notice_pins = {
        name: {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        for name, raw in notices.items()
    }
    files = []
    names = set()

    def add_file(name, path, expected, project=False):
        if name in names:
            raise ValueError("Source basename collision: " + name)
        names.add(name)
        files.append((name, path, expected["bytes"], expected["sha256"], project))

    for name in notices:
        add_file("licensing/" + name, ROOT / name, notice_pins[name])
    stream = io.StringIO()
    prefix = "drivers/gpu/drm/gma500/"
    for relative, expected in sorted(json.loads(lock_path.read_text())["linux_source_files"].items()):
        path = Path(relative)
        if path.parent.as_posix() == prefix.rstrip("/") and (
            path.suffix in (".c", ".h") or path.name == "Makefile"
        ):
            add_file(path.name, source / relative, expected)
    for path in project_sources():
        add_file(path.name, path, identity(path), project=True)
    stream.write(
        "\npoulsback-y += " + MODULE_OBJECTS + "\nccflags-y += -I$(src)\n"
        "ccflags-y += -DPOULSBACK_SELFCHECK=0\n"
    )
    for name, *_ in files:
        if name.endswith(".c") and name[:-2] + ".o" in MODULE_OBJECTS.split():
            stream.write("CFLAGS_" + name[:-2] + ".o += -Werror=unused-value\n")
    patch_pin = identity(patch_path)
    return KernelBuildPlan(
        json.dumps(build_target, sort_keys=True),
        source,
        tuple(files),
        (patch_path, patch_pin["bytes"], patch_pin["sha256"]),
        stream.getvalue(),
        json.dumps(notice_pins, sort_keys=True),
    )


def stage_sources(destination, source, report, *, build_target, plan):
    if (
        type(plan) is not KernelBuildPlan
        or plan.source != source
        or plan.build_target != json.dumps(build_target, sort_keys=True)
    ):
        raise ValueError("Kernel staging plan and requested inputs disagree")
    if json.loads(plan.license_inputs) != report["license_inputs"]:
        raise ValueError("License inputs changed before staging")
    external = source / "drivers/gpu/drm/gma500"
    expected = {path.name for _, path, _, _, _ in plan.files if path.parent == external}
    actual = {
        path.name
        for path in external.iterdir()
        if path.is_file() and (path.suffix in (".c", ".h") or path.name == "Makefile")
    }
    if actual != expected:
        raise ValueError("External source inventory changed before staging")
    observed = []
    for name, path, size, digest, project in plan.files:
        raw = path.read_bytes()
        if {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()} != {
            "bytes": size,
            "sha256": digest,
        }:
            raise ValueError("Source changed before staging: " + str(path))
        observed.append((name, raw, path, project))
    patch_raw = plan.patch[0].read_bytes()
    if dict(bytes=len(patch_raw), sha256=hashlib.sha256(patch_raw).hexdigest()) != dict(
        bytes=plan.patch[1], sha256=plan.patch[2]
    ):
        raise ValueError("Integration patch changed before staging")
    destination.mkdir()
    for name, raw, path, project in observed:
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(raw)
        if project:
            report["inputs"].append(
                dict(
                    source=str(path.relative_to(ROOT)),
                    bytes=len(raw),
                    sha256=hashlib.sha256(raw).hexdigest(),
                )
            )
    patch_command = ["patch", "--batch", "--fuzz=0", "--no-backup-if-mismatch", "-p1", "--input=-"]
    (destination.parent / "physical-patch.stdin").write_bytes(patch_raw)
    patched = subprocess.run(patch_command, input=patch_raw, cwd=destination, capture_output=True)
    (destination.parent / "physical-patch.stdout").write_bytes(patched.stdout)
    (destination.parent / "physical-patch.stderr").write_bytes(patched.stderr)
    report["commands"].append(
        dict(
            target="physical-patch",
            argv=patch_command,
            returncode=patched.returncode,
            stdin=dict(bytes=len(patch_raw), sha256=hashlib.sha256(patch_raw).hexdigest()),
        )
    )
    patched.check_returncode()
    with (destination / "Makefile").open("a") as stream:
        stream.write(plan.kbuild)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-target", required=True)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--kernel-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", choices=("physical",), required=True)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    try:
        build_target = build_targets.load(args.build_target)
        target_receipt = build_targets.receipt(build_target)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    lock_path = build_targets.input_path(build_target, "kernel_lock")
    patch_path = build_targets.input_path(build_target, "kernel_patch")
    headers = args.headers.resolve(strict=True)
    source = args.kernel_source.resolve(strict=True)
    try:
        plan = make_build_plan(source, build_target)
    except ValueError as error:
        parser.error(str(error))
    output = args.output.absolute()
    if output.parent.resolve() != output.parent or output.exists() or output.is_symlink():
        parser.error("--output requires a new directory beneath an existing canonical parent")
    if any(output.is_relative_to(tree) for tree in (ROOT, headers, source)):
        parser.error("--output must be outside source and header trees")
    if platform.machine() != build_target["toolchain"]["native_machine"]:
        parser.error("The selected reference ABI requires a native i686 build environment")
    if args.jobs < 1 or args.jobs > 64:
        parser.error("--jobs must be between 1 and 64")
    lock = validate_inputs(headers, source, lock_path=lock_path)
    stack_policy = kernel_stack_policy(lock)
    metadata = {
        str(path.relative_to(ROOT)): identity(path)
        for path in (lock_path, Path(__file__), patch_path)
    }
    notices = license_bytes()
    notice_pins = {
        name: {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
        for name, raw in notices.items()
    }
    metadata.update(notice_pins)
    output.mkdir()
    report = {
        "schema": "poulsback-kernel-build-v1",
        "status": "BUILDING",
        "target": args.target,
        "build_target": target_receipt,
        "kernel_release": lock["kernel_release"],
        "inputs": [],
        "commands": [],
        "modules": {},
        "hardware_accessed": False,
        "module_loaded": False,
        "gpu_operations_qualified": False,
        "kernel_stack_policy": stack_policy,
        "kernel_stack_usage": {},
        "license_selection": "PER_FILE_TERMS_ROOT_MIT_WITH_GPL_EXCEPTIONS",
        "license_inputs": notice_pins,
        "lock": identity(lock_path),
        "recipe": identity(Path(__file__)),
        "integration_patch": identity(patch_path),
        "external_source_pins": lock["linux_source_files"],
        "abi_pins": lock["abi_inputs"],
    }
    env = os.environ.copy()
    env.update(
        KBUILD_BUILD_TIMESTAMP=build_target["build_metadata"]["timestamp"],
        KBUILD_BUILD_USER=build_target["build_metadata"]["user"],
        KBUILD_BUILD_HOST=build_target["build_metadata"]["host"],
        LC_ALL="C",
    )
    try:
        report["toolchain"] = collect_toolchain(output, build_target, env, report)
        stage = output / "physical"
        stage_sources(stage, source, report, build_target=build_target, plan=plan)
        command = [
            "make",
            "-C",
            str(headers),
            "M=" + str(stage),
            "ARCH=" + build_target["toolchain"]["kernel_arch"],
            "CC=" + build_target["toolchain"]["c"],
            "HOSTCC=" + build_target["toolchain"]["c"],
            "-j" + str(args.jobs),
            "KCFLAGS=" + " ".join(build_target["architecture"]["c_flags"]) + " -fstack-usage "
            "-ffile-prefix-map=" + str(stage) + "=/poulsback-kernel",
            "modules",
        ]
        result = subprocess.run(command, capture_output=True, env=env)
        (output / "physical.stdout").write_bytes(result.stdout)
        (output / "physical.stderr").write_bytes(result.stderr)
        report["commands"].append(
            {"target": "physical", "argv": command, "returncode": result.returncode}
        )
        stack_usage = kernel_stack_usage(stage, stack_policy, result.stdout, result.stderr, env=env)
        report["kernel_stack_usage"]["physical"] = stack_usage
        if result.returncode:
            raise RuntimeError("Build failed: physical\n" + result.stderr.decode()[-8000:])
        if stack_usage["status"] != "PASS_PER_FUNCTION_LIMIT_ONLY":
            raise RuntimeError(
                "Kernel stack gate failed: physical\n" + "\n".join(stack_usage["errors"])
            )
        report["staged_sources"] = {
            path.name: identity(path)
            for path in sorted(stage.iterdir())
            if path.is_file()
            and (path.suffix in (".c", ".h", ".inc") or path.name == "Makefile")
            and not path.name.endswith(".mod.c")
        }
        for name, expected in notice_pins.items():
            if identity(stage / "licensing" / name) != expected:
                raise ValueError("Staged license differs: " + name)
        for module in stage.glob("*.ko"):
            elf = build_targets.check_elf(module)
            if elf["type"] != 1:
                raise ValueError("Kernel module must be an ELF32 relocatable object")
            report.setdefault("module_elf", {})[str(module.relative_to(output))] = elf
            report["modules"][str(module.relative_to(output))] = identity(module)
        for item in report["inputs"]:
            if identity(ROOT / item["source"]) != {k: item[k] for k in ("bytes", "sha256")}:
                raise ValueError("Project source changed during build: " + item["source"])
        if license_bytes() != notices:
            raise ValueError("License inputs changed during build")
        for relative, expected in metadata.items():
            if identity(ROOT / relative) != expected:
                raise ValueError("Build recipe changed during build: " + relative)
        if build_targets.receipt(build_target) != target_receipt:
            raise ValueError("Build target or its inputs changed during build")
        verify_toolchain(report["toolchain"], env)
        report["status"] = "PASS_COMPILE_ONLY_REQUALIFICATION_REQUIRED"
    except Exception as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "modules": report["modules"]}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
