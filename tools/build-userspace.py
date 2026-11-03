#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Build and test the CPU-only libraries with strict warnings and UBSAN."""

import argparse
import fcntl
import importlib.util
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
COLOR0_PACKET_SOURCES = [
    "compiler/bitwise.c",
    "compiler/fragment.c",
    "compiler/usse.c",
    "compiler/iterator.c",
    "compiler/pack.c",
    "raster/sgx535_varying_packet.c",
    "raster/sgx535_compiled_color0_packet.c",
    "compiler/fragment_normalized.c",
    "raster/sgx535_normalized_color0_task.c",
    "raster/sgx535_normalized_color0_mask_list.c",
]
EXECUTOR_SOURCES = [
    "raster/sgx535_task_launch.c",
    "raster/sgx535_background_object.c",
    "raster/sgx535_background_pair.c",
    "raster/sgx535_task_packet.c",
    "raster/sgx535_window_vertex.c",
    "raster/sgx535_clip_w1.c",
    "raster/sgx535_engine121.c",
    "compiler/sgx535_vertex_dma.c",
    "compiler/sgx535_vertex_use.c",
    "compiler/sgx535_vertex_pds.c",
    "raster/sgx535_pbe_state.c",
    "raster/sgx535_pixel_event.c",
    "raster/sgx535_heap_layout.c",
    "compiler/sgx535_use_code_bank.c",
    "compiler/sgx535_pds_use_link.c",
] + COLOR0_PACKET_SOURCES
WINDOW_MESH_SOURCES = [
    "raster/sgx535_window_mesh.c",
    "compiler/pds_fragment.c",
    "compiler/texture2d.c",
]
COMPONENTS = {
    "vm-layout": (
        ["kernel/memory/poulsback_vm_layout.c"],
        "kernel/test_poulsback_vm_layout.c",
    ),
    "vm": (["kernel/memory/poulsback_vm_layout.c"], "kernel/test_poulsback_vm.c"),
    "scene": (
        ["kernel/memory/poulsback_vm_layout.c", "raster/sgx535_engine121.c"],
        "kernel/test_poulsback_scene.c",
    ),
    "uapi": ([], "kernel/test_uapi.c"),
    "page-address": ([], "kernel/test_page_address.c"),
    "foreign-mmu": ([], "kernel/test_foreign_mmu.c"),
    "debug": ([], "kernel/test_debug.c"),
    "display-vblank": ([], "kernel/test_display_vblank.c"),
    "regtable": (
        ["kernel/engine/poulsback_regtable.c", "kernel/memory/poulsback_vm_layout.c"],
        "kernel/test_poulsback_regtable.c",
    ),
    "overlap": (["kernel/engine/poulsback_overlap.c"], "kernel/test_poulsback_overlap.c"),
    "teardown": (["kernel/engine/poulsback_teardown.c"], "kernel/test_poulsback_teardown.c"),
    "context": ([], "kernel/test_poulsback_context.c"),
    "bo-use": (["kernel/gem/poulsback_bo_use.c"], "kernel/test_poulsback_bo_use.c"),
    "twod-plan": (
        [
            "kernel/engine/poulsback_twod.c",
            "kernel/memory/poulsback_vm_layout.c",
            "raster/sgx535_2d_encoder.c",
        ],
        "kernel/test_poulsback_twod.c",
    ),
    "usse": (["compiler/usse.c", "compiler/pds.c"], "test_usse.c"),
    "iterator": (["compiler/iterator.c"], "test_iterator.c"),
    "texture2d-nearest": (["compiler/texture2d.c", "compiler/pack.c"], "test_sgx535_texture2d.c"),
    "pds-fragment": (
        [
            "compiler/iterator.c",
            "compiler/pds.c",
            "compiler/pds_fragment.c",
            "compiler/sgx535_pds_use_link.c",
            "compiler/sgx535_use_code_bank.c",
            "compiler/texture2d.c",
            "compiler/pack.c",
        ],
        "test_pds_fragment.c",
    ),
    "pds-secondary": (["compiler/pds_secondary.c"], "test_pds_secondary.c"),
    "pack": (["compiler/pack.c"], "test_pack.c"),
    "fragment": (
        [
            "compiler/usse.c",
            "compiler/iterator.c",
            "compiler/pack.c",
            "compiler/bitwise.c",
            "compiler/fragment.c",
        ],
        "test_fragment.c",
    ),
    "fragment-raw-sign": (
        [
            "compiler/usse.c",
            "compiler/iterator.c",
            "compiler/pack.c",
            "compiler/bitwise.c",
            "compiler/fragment.c",
        ],
        "test_fragment_raw_sign.c",
    ),
    "fragment-bool32-words": (
        [
            "compiler/usse.c",
            "compiler/iterator.c",
            "compiler/pack.c",
            "compiler/bitwise.c",
            "compiler/fragment.c",
        ],
        "test_fragment_bool32_words.c",
    ),
    "fragment-normalized": (
        [
            "compiler/usse.c",
            "compiler/iterator.c",
            "compiler/pack.c",
            "compiler/bitwise.c",
            "compiler/fragment.c",
            "compiler/fragment_normalized.c",
        ],
        "test_fragment_normalized.c",
    ),
    "fragment-raw-sign-normalized": (
        [
            "compiler/usse.c",
            "compiler/iterator.c",
            "compiler/pack.c",
            "compiler/bitwise.c",
            "compiler/fragment.c",
            "compiler/fragment_normalized.c",
        ],
        "test_fragment_raw_sign_normalized.c",
    ),
    "linear": (["compiler/usse.c", "compiler/linear.c"], "test_linear.c"),
    "native-2d": (
        ["raster/sgx535_2d_encoder_v1.c", "raster/sgx535_2d_encoder.c"],
        "test_sgx535_2d.c",
    ),
    "native-raster": (["raster/sgx535_2d_encoder.c"], "test_sgx535_2d_raster.c"),
    "native-raster-layouts": (
        ["raster/sgx535_2d_encoder.c"],
        "test_sgx535_2d_raster_layouts.c",
    ),
    "native-2d-encoder": (
        ["raster/sgx535_2d_encoder.c", "raster/sgx535_2d_encoder_v1.c"],
        "test_2d_encoder_vectors.c",
    ),
    "engine121": (["raster/sgx535_engine121.c"], "test_sgx535_engine121.c"),
    "normalized-color0-task": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_task.c",
    ),
    "normalized-color0-tile16": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_tile16.c",
    ),
    "normalized-color0-background": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_background.c",
    ),
    "normalized-color0-mask": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_mask.c",
    ),
    "normalized-color0-mask-list": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_mask_list.c",
    ),
    "normalized-color0-window-list": (
        EXECUTOR_SOURCES,
        "test_sgx535_normalized_color0_window_list.c",
    ),
    "window-mesh": (
        [*EXECUTOR_SOURCES, *WINDOW_MESH_SOURCES],
        "test_sgx535_window_mesh.c",
    ),
    "depth-window-mesh": (
        [
            *EXECUTOR_SOURCES,
            *WINDOW_MESH_SOURCES,
            "raster/sgx535_depth_state.c",
            "raster/sgx535_depth_clear.c",
            "raster/sgx535_depth_window_mesh.c",
        ],
        "test_sgx535_depth_window_mesh.c",
    ),
    # Its vectors and exact expectations come from tests/project/clip_w1_reference.py.
    "clip-w1": (["raster/sgx535_clip_w1.c"], "test_clip_w1.c"),
    "depth-cube-mesh": (
        [
            *EXECUTOR_SOURCES,
            *WINDOW_MESH_SOURCES,
            "raster/sgx535_depth_state.c",
            "raster/sgx535_depth_clear.c",
            "raster/sgx535_depth_window_mesh.c",
        ],
        "test_sgx535_depth_cube_mesh.c",
    ),
    "depth-state": (["raster/sgx535_depth_state.c"], "test_sgx535_depth_state.c"),
    "depth-clear": (
        [
            "raster/sgx535_depth_clear.c",
            "raster/sgx535_depth_state.c",
            "compiler/sgx535_vertex_dma.c",
            "compiler/sgx535_vertex_use.c",
            "compiler/sgx535_vertex_pds.c",
            "raster/sgx535_task_packet.c",
            "raster/sgx535_window_vertex.c",
            "raster/sgx535_clip_w1.c",
            "raster/sgx535_engine121.c",
            "raster/sgx535_pbe_state.c",
            "raster/sgx535_pixel_event.c",
        ],
        "test_sgx535_depth_clear.c",
    ),
    "heap-layout": (["raster/sgx535_heap_layout.c"], "test_sgx535_heap_layout.c"),
    "draw-mask": (["raster/sgx535_draw_mask.c"], "test_sgx535_draw_mask.c"),
    "zls": (["raster/sgx535_zls.c"], "test_sgx535_zls.c"),
    "task-image": (
        [
            "raster/sgx535_task_image.c",
            "raster/sgx535_clip_w1.c",
            "raster/sgx535_engine121.c",
            "compiler/sgx535_vertex_dma.c",
            "compiler/sgx535_vertex_use.c",
        ],
        "test_task_image.c",
    ),
    "task-pair": (EXECUTOR_SOURCES, "test_sgx535_task_pair.c"),
    "task-pair-mask-set": (EXECUTOR_SOURCES, "test_sgx535_task_pair_mask_set.c"),
    "background-object": (
        [
            "raster/sgx535_background_object.c",
            "raster/sgx535_background_pair.c",
            "compiler/sgx535_use_code_bank.c",
            "compiler/sgx535_pds_use_link.c",
            "raster/sgx535_task_packet.c",
            "raster/sgx535_window_vertex.c",
            "raster/sgx535_clip_w1.c",
            "raster/sgx535_engine121.c",
            "compiler/sgx535_vertex_dma.c",
            "compiler/sgx535_vertex_use.c",
            "compiler/sgx535_vertex_pds.c",
            "raster/sgx535_pbe_state.c",
            "raster/sgx535_pixel_event.c",
        ],
        "test_sgx535_background_object.c",
    ),
    "use-code-bank": (
        ["compiler/sgx535_use_code_bank.c"],
        "test_sgx535_use_code_bank.c",
    ),
    "pds-use-link": (
        ["compiler/sgx535_use_code_bank.c", "compiler/sgx535_pds_use_link.c"],
        "test_sgx535_pds_use_link.c",
    ),
    "vertex-dma": (["compiler/sgx535_vertex_dma.c"], "test_sgx535_vertex_dma.c"),
    "vertex-pds": (
        ["compiler/sgx535_vertex_pds.c", "compiler/sgx535_vertex_dma.c"],
        "test_sgx535_vertex_pds.c",
    ),
    "vertex": (["compiler/vertex.c"], "test_sgx535_vertex.c"),
    "pbe-state": (
        ["raster/sgx535_pbe_state.c", "raster/sgx535_pixel_event.c"],
        "test_sgx535_pbe_state.c",
    ),
    "pixel-event": (["raster/sgx535_pixel_event.c"], "test_sgx535_pixel_event.c"),
    "pixel-pds": (["raster/sgx535_pixel_pds.c"], "test_sgx535_pixel_pds.c"),
    "mte-state": (
        ["raster/sgx535_mte_state.c", "compiler/sgx535_vertex_use.c"],
        "test_sgx535_mte_state.c",
    ),
    "region-clip": (["raster/sgx535_region_clip.c"], "test_sgx535_region_clip.c"),
    "task-packet": (
        [
            "raster/sgx535_task_packet.c",
            "raster/sgx535_window_vertex.c",
            "raster/sgx535_clip_w1.c",
            "raster/sgx535_engine121.c",
            "compiler/sgx535_vertex_dma.c",
            "compiler/sgx535_vertex_use.c",
            "compiler/sgx535_vertex_pds.c",
            "raster/sgx535_pbe_state.c",
            "raster/sgx535_pixel_event.c",
        ],
        "test_sgx535_task_packet.c",
    ),
    "varying-packet": (
        [
            "raster/sgx535_varying_packet.c",
            "raster/sgx535_task_packet.c",
            "raster/sgx535_window_vertex.c",
            "raster/sgx535_clip_w1.c",
            "raster/sgx535_engine121.c",
            "raster/sgx535_pbe_state.c",
            "raster/sgx535_pixel_event.c",
            "compiler/sgx535_vertex_dma.c",
            "compiler/sgx535_vertex_use.c",
            "compiler/sgx535_vertex_pds.c",
            "compiler/usse.c",
            "compiler/pack.c",
        ],
        "test_sgx535_varying_packet.c",
    ),
    "task-launch": (EXECUTOR_SOURCES, "test_sgx535_task_launch.c"),
    "compiled-color0-packet": (
        EXECUTOR_SOURCES,
        "test_sgx535_compiled_color0_packet.c",
    ),
    "window-vertex": (["raster/sgx535_window_vertex.c"], "test_sgx535_window_vertex.c"),
    "window-color0-packet": (EXECUTOR_SOURCES, "test_sgx535_window_packet.c"),
    "vertex-use": (
        [
            "compiler/sgx535_vertex_use.c",
            "compiler/usse.c",
        ],
        "test_sgx535_vertex_use.c",
    ),
    "msvdx-owned": (["video/msvdx_owned.c"], "test_msvdx_owned.c"),
    "msvdx-firmware": (["video/msvdx_firmware.c"], "test_msvdx_firmware.c"),
    "msvdx-transport": (
        [
            "video/msvdx_proto.c",
            "video/msvdx_firmware.c",
            "video/msvdx_transport.c",
        ],
        "test_msvdx_transport.c",
    ),
    "mpeg2-dc-fixture": (["video/mpeg2_dc_fixture.c"], "test_mpeg2_dc_fixture.c"),
    "msvdx-proto": (["video/msvdx_proto.c"], "test_msvdx_proto.c"),
    "mpeg2-va029": (["video/mpeg2_va029.c"], "test_mpeg2_va029.c"),
    "texture-layout": (["raster/sgx535_texture_layout.c"], "test_sgx535_texture_layout.c"),
    "isp-words": (["raster/sgx535_isp_words.c"], "test_sgx535_isp_words.c"),
    "use-encode": (["compiler/use_encode.c"], "test_sgx535_use_encode.c"),
    "fs-backend": (
        ["compiler/use_encode.c", "compiler/fs_emit.c", "compiler/fs_epilogue.c"],
        "test_sgx535_fs.c",
    ),
    "spm": (["raster/sgx535_engine121.c", "raster/sgx535_spm.c"], "test_sgx535_spm.c"),
    "spm-state": (
        ["raster/sgx535_engine121.c", "raster/sgx535_spm.c", "raster/sgx535_spm_state.c"],
        "test_sgx535_spm_state.c",
    ),
    "texture-words": (
        ["raster/sgx535_texture_layout.c", "raster/sgx535_texture_words.c"],
        "test_sgx535_texture_words.c",
    ),
}

COMPONENTS["fragment-inot-words"] = (
    COMPONENTS["fragment-bool32-words"][0],
    "test_fragment_inot_words.c",
)
COMPONENTS["fragment-ieq-words"] = (
    COMPONENTS["fragment-bool32-words"][0],
    "test_fragment_ieq_words.c",
)


def identity(path):
    data = path.read_bytes()
    return {
        "path": str(path.relative_to(ROOT)),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def component_source(name):
    return ROOT / name if name.startswith("kernel/") else ROOT / "src" / name


def component_test(name):
    return ROOT / "tests" / name if name.startswith("kernel/") else ROOT / "tests/unit" / name


def component_options(name):
    if name in ("uapi", "twod-plan"):
        libdrm = subprocess.check_output(["pkg-config", "--cflags", "libdrm"], text=True)
        return [*shlex.split(libdrm), "-I" + str(ROOT / "include/uapi/poulsback")]
    if name == "debug":
        # The test includes sgx535_debug.c; its kernel headers resolve to CPU
        # stand-ins, which use GNU types (loff_t).
        return ["-std=gnu11", "-I" + str(ROOT / "tests/kernel/debug-mocks")]
    if name == "regtable":
        # Kernel sources include each other by name (Kbuild builds them in one directory).
        return ["-I" + str(ROOT / "kernel/memory")]
    if name == "vm":
        # The test includes poulsback_vm.c; its kernel headers resolve to CPU stand-ins.
        return ["-I" + str(ROOT / "tests/kernel/vm-mocks")]
    if name == "context":
        # As scene, with the context suite's stand-ins (device, file, ids,
        # scheduler entities) ahead of the vm suite's. The kernel file builds
        # under Kbuild's warnings: ioctl parameters it does not use, its int
        # handle into the UAPI's __u32, and vm-mocks' helpers it never calls.
        libdrm = subprocess.check_output(["pkg-config", "--cflags", "libdrm"], text=True)
        return ["-Wno-unused-parameter", "-Wno-sign-conversion", "-Wno-unused-function",
                "-I" + str(ROOT / "tests/kernel/context-mocks"),
                "-I" + str(ROOT / "tests/kernel/vm-mocks"), *shlex.split(libdrm),
                "-I" + str(ROOT / "include/uapi/poulsback")]
    if name == "scene":
        # As vm, plus poulsback_scene.c's UAPI header (libdrm's drm.h).
        libdrm = subprocess.check_output(["pkg-config", "--cflags", "libdrm"], text=True)
        return ["-I" + str(ROOT / "tests/kernel/vm-mocks"), *shlex.split(libdrm),
                "-I" + str(ROOT / "include/uapi/poulsback")]
    return []


def component_link_options(name):
    return [
        *(
            ["-Wl,--wrap=calloc"]
            if name
            in (
                "fragment",
                "fragment-normalized",
                "fragment-raw-sign-normalized",
                "compiled-color0-packet",
                "window-color0-packet",
                "normalized-color0-task",
                "normalized-color0-tile16",
                "normalized-color0-background",
                "normalized-color0-mask",
                "normalized-color0-mask-list",
                "normalized-color0-window-list",
                "window-mesh",
                "depth-window-mesh",
                "depth-cube-mesh",
            )
            else []
        ),
        *(
            ["-Wl,--wrap=free"]
            if name
            in (
                "fragment-normalized",
                "fragment-raw-sign-normalized",
            )
            else []
        ),
        *(["-Wl,--wrap=sgx535_clip_w1_encode"] if name == "window-color0-packet" else []),
        *(["-lm"] if name == "fs-backend" else []),
    ]


def compiler_input_module():
    path = ROOT / "tools/compiler_inputs.py"
    spec = importlib.util.spec_from_file_location("poulsback_compiler_inputs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_target_module():
    path = ROOT / "tools/build_targets.py"
    spec = importlib.util.spec_from_file_location("poulsback_cpu_build_targets", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def target_compiler_arguments(compiler, environment):
    """The first CPU target supports native GCC and its ordinary -m32 multilib."""
    if (
        not compiler
        or compiler[0].startswith(("-", "@"))
        or any(option != "-m32" for option in compiler[1:])
    ):
        raise ValueError("Target CPU compiler accepts only an executable and optional -m32")
    if compiler.count("-m32") > 1:
        raise ValueError("Duplicate target CPU ABI option")
    for name in (
        "CPATH",
        "C_INCLUDE_PATH",
        "CPLUS_INCLUDE_PATH",
        "OBJC_INCLUDE_PATH",
        "LIBRARY_PATH",
        "COMPILER_PATH",
        "GCC_EXEC_PREFIX",
        "PKG_CONFIG_PATH",
        "PKG_CONFIG_LIBDIR",
        "PKG_CONFIG_SYSROOT_DIR",
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "LD_AUDIT",
    ):
        if environment.get(name):
            raise ValueError("Target CPU build refuses inherited search override: " + name)


def target_compiler_probe(targets, target, compiler, run_directory, run):
    inputs = compiler_input_module()
    source = run_directory / "target-abi.c"
    binary = run_directory / "target-abi"
    depfile = run_directory / "target-abi.d"
    source.write_text(targets.compiler_source())
    sysroot = run([*compiler, "-print-sysroot"], "target-compiler-sysroot").strip()
    if sysroot not in ("", "/"):
        raise ValueError("Target CPU cross sysroot is not qualified")
    run(
        [
            *compiler,
            *target["architecture"]["c_flags"],
            "-std=c11",
            "-MD",
            "-MT",
            inputs.TARGET,
            "-MF",
            str(depfile),
            str(source),
            "-o",
            str(binary),
        ],
        "target-compiler-abi",
        "prepare",
    )
    elf = targets.check_elf(binary)
    return {
        "scope": "CPU_BUILD_AND_TEST_ONLY_NOT_MESA_KERNEL_OR_GPU_QUALIFICATION",
        "sysroot": sysroot,
        "elf": elf,
        "output": inputs.file_identity(binary),
        "headers": [
            inputs.file_identity(path)
            for path in inputs.parse_make_dependencies(depfile.read_bytes(), ROOT)
        ],
    }


def host_output_abi(path):
    """Record the compiled host ABI, independently of the running kernel name."""
    with path.open("rb") as source:
        data = source.read(64)
    if (
        len(data) < 52
        or data[:4] != b"\x7fELF"
        or data[4] not in (1, 2)
        or data[5] not in (1, 2)
        or data[6] != 1
    ):
        raise ValueError("CPU output must be an ELF32 or ELF64 executable")
    elf_class = 32 if data[4] == 1 else 64
    endian = "little" if data[5] == 1 else "big"
    kind, machine, version = struct.unpack_from("<HHI" if endian == "little" else ">HHI", data, 16)
    header_size = 52 if elf_class == 32 else 64
    size_offset = 40 if elf_class == 32 else 52
    if (
        len(data) < header_size
        or kind not in (2, 3)
        or version != 1
        or int.from_bytes(data[size_offset : size_offset + 2], endian) != header_size
    ):
        raise ValueError("Malformed CPU output ELF header")
    return {"elf_class": elf_class, "endian": endian, "machine": machine, "type": kind}


class CommandRunner:
    """Keep each actual child, including timeout cleanup, in its run report."""

    def __init__(self, directory, report, save, *, cwd, timeout=120):
        self.directory = directory
        self.report = report
        self.save = save
        self.cwd = cwd
        self.timeout = timeout
        self.environment = dict(os.environ)

    def run(self, argv, tag, phase, *, required=True):
        begin = time.monotonic()
        try:
            child = subprocess.Popen(
                argv,
                cwd=self.cwd,
                env=self.environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
        except OSError as error:
            self.report["commands"].append(
                {
                    "argv": argv,
                    "pid": None,
                    "exit_code": None,
                    "elapsed_seconds": time.monotonic() - begin,
                    "phase": phase,
                    "timed_out": False,
                    "spawn_error": str(error),
                    "cleanup": [],
                }
            )
            self.save()
            raise
        timed_out = False
        cleanup = []
        output_complete = True
        try:
            stdout, stderr = child.communicate(timeout=self.timeout)
        except subprocess.TimeoutExpired as timeout_error:
            timed_out = True
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(child.pid, sig)
                    cleanup.append({"signal": signal.Signals(sig).name, "error": None})
                except ProcessLookupError:
                    cleanup.append({"signal": signal.Signals(sig).name, "error": "already exited"})
                if sig == signal.SIGTERM:
                    # Do not reap the leader before the final group signal:
                    # its unreaped PID prevents signalling a recycled group.
                    time.sleep(0.2)
            try:
                stdout, stderr = child.communicate(timeout=0.2)
            except subprocess.TimeoutExpired as drain_error:
                # A detached descendant may retain the pipes after the owned
                # group dies. Keep captured bytes without waiting for its EOF.
                stdout = drain_error.output
                stderr = drain_error.stderr
                if stdout is None:
                    stdout = timeout_error.output or b""
                if stderr is None:
                    stderr = timeout_error.stderr or b""
                output_complete = False
                child.stdout.close()
                child.stderr.close()
                try:
                    child.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
        elapsed = time.monotonic() - begin
        out, err = (
            self.directory / (tag + ".stdout.txt"),
            self.directory / (tag + ".stderr.txt"),
        )
        out.write_bytes(stdout)
        err.write_bytes(stderr)
        self.report["commands"].append(
            {
                "argv": argv,
                "exit_code": child.returncode,
                "pid": child.pid,
                "stdout": str(out.relative_to(self.directory.parents[1])),
                "stderr": str(err.relative_to(self.directory.parents[1])),
                "elapsed_seconds": elapsed,
                "phase": phase,
                "timed_out": timed_out,
                "required": required,
                "cleanup": cleanup,
                "reaped": child.returncode is not None,
                "output_complete": output_complete,
            }
        )
        totals = self.report["phase_seconds"]
        totals[phase] = totals.get(phase, 0.0) + elapsed
        self.save()
        if timed_out or child.returncode:
            if not required:
                return None
            sys.stderr.buffer.write(stderr)
            sys.stderr.buffer.write(stdout)
            raise RuntimeError(f"CPU command {'timed out' if timed_out else 'failed'}: {tag}")
        if phase == "test":
            sys.stdout.buffer.write(stdout)
        return stdout.decode(errors="strict")


def gcc_toolchain(compiler, version, run, output, report):
    """Only the checked GCC depfile contract can authorize object reuse."""
    if "Free Software Foundation" not in version or any(word.startswith("@") for word in compiler):
        return None
    macros = run(
        [*compiler, "-dM", "-E", "-x", "c", "/dev/null"],
        "compiler-predefines",
        "prepare",
        required=False,
    )
    if macros is None or "#define __GNUC__ " not in macros or "#define __clang__ " in macros:
        return None
    if (
        run(
            [*compiler, "-v", "-E", "-x", "c", "/dev/null"],
            "compiler-driver",
            "prepare",
            required=False,
        )
        is None
    ):
        return None
    verbose = (output / report["commands"][-1]["stderr"]).read_bytes()
    drivers = re.findall(rb"^COLLECT_GCC=(.+)$", verbose, re.M)
    if len(drivers) != 1:
        return None
    paths = [os.fsdecode(drivers[0])]
    for kind in ("cc1", "as", "specs"):
        option = "-print-file-name=" if kind == "specs" else "-print-prog-name="
        value = run([*compiler, option + kind], "compiler-" + kind, "prepare", required=False)
        if value is None:
            return None
        value = value.strip()
        if kind != "specs" or value != "specs":
            paths.append(value)
    paths += [
        word.split("=", 1)[1]
        for word in compiler
        if word.startswith(("-specs=", "--specs=", "-fplugin="))
    ]
    inputs = compiler_input_module()
    try:
        files = [
            inputs.file_identity(Path(shutil.which(name) or name).absolute())
            for name in dict.fromkeys(paths)
        ]
    except (OSError, ValueError):
        return None
    # A ccache prefix's own bytes do not identify the actual GCC driver.
    if Path(files[0]["resolved"]).name == "ccache":
        return None
    return dict(
        files=files,
        predefines_sha256=hashlib.sha256(macros.encode()).hexdigest(),
        driver_query_sha256=hashlib.sha256(verbose).hexdigest(),
    )


class ObjectGraph:
    """Share exact TU configurations; test results and links are never cached."""

    def __init__(self, output, run_dir, compiler, version, command, report):
        self.output, self.run_dir = output, run_dir
        self.compiler, self.command, self.report = compiler, command, report
        self.inputs = compiler_input_module()
        self.finished = {}
        self.selections = {}
        self.observed = {}
        files = []
        for word in compiler:
            if word.startswith("-"):
                continue
            path = Path(shutil.which(word) or word)
            if path.is_file():
                files.append(self.inputs.file_identity(path.absolute()))
        environment = json.dumps(command.environment, sort_keys=True).encode()
        self.recipe = {
            "compiler": compiler,
            "compiler_version": version,
            "compiler_files": files,
            "cwd": str(ROOT),
            "environment_sha256": hashlib.sha256(environment).hexdigest(),
        }
        self.recipe.update(
            schema="poulsback-exact-object-v1",
            builder=identity(Path(__file__).resolve()),
            dependency_helper=identity(Path(self.inputs.__file__).resolve()),
        )
        self.recipe["gcc_toolchain"] = report["gcc_toolchain"]
        self.recipe["compiler_files"] += report["gcc_toolchain"]["files"]
        self.report["object_toolchain"] = self.recipe

    def dependencies(self, depfile):
        return [
            self.inputs.file_identity(path)
            for path in self.inputs.parse_make_dependencies(depfile.read_bytes(), ROOT)
        ]

    def object(self, source, flags, flavor):
        recipe = dict(self.recipe, source=str(source), options=flags, flavor=flavor)
        key = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
        if key in self.finished:
            return self.finished[key]
        store = self.output / "objects" / flavor
        store.mkdir(parents=True, exist_ok=True)
        obj, state = store / (key + ".o"), store / (key + ".json")
        pending = self.run_dir / (key + ".o")
        dep_scan = self.run_dir / (key + ".scan.d")
        dep_compile = self.run_dir / (key + ".compile.d")
        tag = flavor + "-" + key[:16]
        # Scan again even for an unchanged object: a new or shadowing header
        # can alter include resolution without changing any old dependency.
        self.command.run(
            [
                *self.compiler,
                *flags,
                "-M",
                "-MF",
                str(dep_scan),
                "-MT",
                "poulsback_dependencies",
                str(source),
            ],
            tag + "-dependencies",
            "prepare",
        )
        before = self.dependencies(dep_scan)
        if str(source.absolute()) not in {entry["path"] for entry in before}:
            raise RuntimeError("Compiler omitted the translation unit dependency")
        volatile = any(
            any(
                token in Path(entry["path"]).read_bytes()
                for token in (b"__TIME__", b"__DATE__", b"__TIMESTAMP__")
            )
            for entry in before
        )
        old = None
        try:
            old = json.loads(state.read_bytes())
        except (OSError, ValueError):
            pass
        reused = (
            not volatile
            and type(old) is dict
            and old.get("recipe") == recipe
            and old.get("dependencies") == before
            and obj.is_file()
            and old.get("object") == self.inputs.file_identity(obj)
        )
        if not reused:
            self.command.run(
                [
                    *self.compiler,
                    *flags,
                    "-MD",
                    "-MF",
                    str(dep_compile),
                    "-MT",
                    "poulsback_dependencies",
                    "-c",
                    str(source),
                    "-o",
                    str(pending),
                ],
                tag + "-compile",
                "compile",
            )
            if self.dependencies(dep_compile) != before:
                raise RuntimeError("Actual compiler dependency closure changed during compilation")
        if [self.inputs.file_identity(Path(entry["path"])) for entry in before] != before:
            raise RuntimeError("Compiler input changed during object selection")
        for entry in before:
            previous = self.observed.setdefault(entry["path"], entry)
            if previous != entry:
                raise RuntimeError("Compiler input changed between translation units")
        if not reused:
            pending.replace(obj)
            record = dict(
                recipe=recipe,
                dependencies=before,
                object=self.inputs.file_identity(obj),
                volatile=volatile,
            )
            temporary = self.run_dir / (key + ".json")
            temporary.write_text(json.dumps(record, indent=2) + "\n")
            temporary.replace(state)
        self.report["objects"].append(
            {
                "key": key,
                "source": str(source),
                "flavor": flavor,
                "options": flags,
                "reused": reused,
                "volatile": volatile,
                "dependency_count": len(before),
                "dependencies_sha256": hashlib.sha256(
                    json.dumps(before, sort_keys=True).encode()
                ).hexdigest(),
                "object": self.inputs.file_identity(obj),
            }
        )
        self.finished[key] = obj
        self.selections[key] = (source, flags, flavor, before)
        return obj

    def verify_inputs(self):
        for key, (source, flags, flavor, selected) in self.selections.items():
            depfile = self.run_dir / (key + ".final.d")
            self.command.run(
                [
                    *self.compiler,
                    *flags,
                    "-M",
                    "-MF",
                    str(depfile),
                    "-MT",
                    "poulsback_dependencies",
                    str(source),
                ],
                flavor + "-" + key[:16] + "-final-dependencies",
                "prepare",
            )
            if self.dependencies(depfile) != selected:
                raise RuntimeError("Compiler dependency closure changed during CPU checks")
        dependencies = list(self.observed.values())
        if [
            self.inputs.file_identity(Path(entry["path"])) for entry in dependencies
        ] != dependencies:
            raise RuntimeError("Actual compiler input changed during CPU checks")
        self.report["actual_compiler_dependencies"] = dependencies
        for entry in self.recipe["compiler_files"]:
            if self.inputs.file_identity(Path(entry["path"])) != entry:
                raise RuntimeError("Compiler executable changed during CPU checks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/userspace")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument(
        "--build-target",
        required=True,
        help="Explicit 'host' CPU ABI or a reviewed target ID; no implicit default",
    )
    parser.add_argument("--component", choices=sorted(COMPONENTS), action="append")
    parser.add_argument("--flavor", choices=("strict", "ubsan"), action="append")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc cannot be empty")
    with (output / ".build.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("output directory is already in use")
        return build(args, output, compiler)


def build(args, output, compiler):
    runs = output / "runs"
    runs.mkdir(exist_ok=True)
    run_dir = runs / str(
        max((int(p.name) for p in runs.iterdir() if p.name.isdigit()), default=0) + 1
    ).zfill(6)
    run_dir.mkdir(exist_ok=False)
    selected = args.component or list(COMPONENTS)
    # What a check reads. With gcc, verify_inputs also reruns -M for every object
    # and checks the actual closure; the uncached route (another compiler) has
    # only this list, so it takes every header and test source rather than a
    # per-component selection.
    source_inputs = {component_source(source) for name in selected for source in COMPONENTS[name][0]}
    for directory in ("src", "kernel", "include", "tests/unit", "tests/kernel"):
        source_inputs.update(
            path for path in (ROOT / directory).rglob("*") if path.suffix in (".h", ".inc")
        )
    source_inputs.update((ROOT / "tests/unit").glob("test_*.c"))
    # Data and references the suites read when they run.
    if "depth-cube-mesh" in selected:
        source_inputs.add(ROOT / "tests/project/depth_cube_reference.py")
    if "clip-w1" in selected:
        source_inputs.add(ROOT / "tests/project/clip_w1_reference.py")
    if {"msvdx-proto", "mpeg2-va029"}.intersection(selected):
        # The suites read the vectors relative to ROOT, the runner's directory.
        source_inputs.update((ROOT / "tests/vectors/msvdx-video").glob("*.txt"))
    if "native-2d-encoder" in selected:
        source_inputs.update((ROOT / "tests/vectors/2d-encoder").iterdir())
    source_inputs.add(Path(__file__).resolve())
    source_inputs = sorted(source_inputs)
    report = {
        "schema": "poulsback-cpu-build-v1",
        "hardware_dispatch": False,
        "inputs": [identity(path) for path in source_inputs],
        "commands": [],
        "compiler": compiler,
        "status": "RUNNING",
        "selected_components": selected,
        "selected_flavors": args.flavor or ["strict", "ubsan"],
        "run_directory": str(run_dir),
        "phase_seconds": {},
        "objects": [],
        "tests_always_run": True,
        "links_always_run": True,
    }
    report_path = output / "report.json"

    def save():
        raw = json.dumps(report, indent=2) + "\n"
        (run_dir / "report.json").write_text(raw)
        temporary = output / "report.json.tmp"
        temporary.write_text(raw)
        temporary.replace(report_path)

    command = CommandRunner(run_dir, report, save, cwd=ROOT)

    def run(argv, tag, phase="identity", *, required=True):
        return command.run(argv, tag, phase, required=required)

    try:
        target, targets = None, None
        selection = getattr(args, "build_target", None)
        if not isinstance(selection, str) or not selection:
            raise ValueError("--build-target is required: choose host or a reviewed target ID")
        if selection == "host":
            report["build_target"] = {
                "id": "host",
                "scope": "HOST_CPU_ONLY_NO_TARGET_ABI_MESA_KERNEL_OR_GPU_QUALIFICATION",
            }
        else:
            targets = build_target_module()
            target = targets.load(selection)
            report["build_target"] = targets.receipt(target)
            target_compiler_arguments(compiler, command.environment)
        report["compiler_version"] = run([*compiler, "--version"], "compiler-version")
        report["host"] = run(["uname", "-a"], "host-identity")
        common = [
            "-std=c11",
            "-O2",
            "-g",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-Wconversion",
            "-Wshadow",
            "-Wstrict-prototypes",
        ]
        if target is not None:
            common += target["architecture"]["c_flags"]
            report["target_compiler"] = target_compiler_probe(
                targets, target, compiler, run_dir, run
            )
        include = [
            "-I" + str(ROOT / "src" / directory) for directory in ("compiler", "raster", "video")
        ]
        include += [
            "-I" + str(ROOT / "kernel" / directory)
            for directory in ("source", "integration", "debug", "memory", "engine")
        ]
        report["gcc_toolchain"] = gcc_toolchain(
            compiler, report["compiler_version"], run, output, report
        )
        supported = report["gcc_toolchain"] is not None
        if target is not None and not supported:
            raise ValueError("The selected CPU build target requires checked GCC inputs")
        report["object_cache_supported"] = supported
        report["dependency_manifest_scope"] = (
            "ACTUAL_GCC_DEPENDENCY_CLOSURE" if supported else "LEGACY_DECLARED_INPUTS_ONLY_UNCACHED"
        )
        graph = (
            ObjectGraph(output, run_dir, compiler, report["compiler_version"], command, report)
            if supported
            else None
        )
        for flavor in args.flavor or ("strict", "ubsan"):
            extra = (
                []
                if flavor == "strict"
                # float-cast-overflow: GCC's undefined leaves it out (PUB-M01)
                else ["-O1", "-fsanitize=undefined,float-cast-overflow", "-fno-sanitize-recover=all"]
            )
            for name in selected:
                sources, test = COMPONENTS[name]
                flags = [*common, *extra, *include, *component_options(name)]
                binary = output / f"{name}-{flavor}"
                pending_binary = run_dir / f"{name}-{flavor}.pending"
                paths = [*[component_source(source) for source in sources], component_test(test)]
                if graph is not None:
                    objects = [graph.object(path, flags, flavor) for path in paths]
                    run(
                        [
                            *compiler,
                            *flags,
                            *map(str, objects),
                            *component_link_options(name),
                            "-o",
                            str(pending_binary),
                        ],
                        f"{name}-{flavor}-link",
                        "link",
                    )
                else:
                    # Preserve other compilers' former compile+link behavior;
                    # no unverifiable dependency record becomes a cache hit.
                    run(
                        [
                            *compiler,
                            *flags,
                            *map(str, paths),
                            *component_link_options(name),
                            "-o",
                            str(pending_binary),
                        ],
                        f"{name}-{flavor}-build",
                        "uncached-build",
                    )
                abi = (
                    targets.check_elf(pending_binary)
                    if target is not None
                    else host_output_abi(pending_binary)
                )
                report.setdefault("target_outputs", []).append(
                    {"component": name, "flavor": flavor, **abi}
                )
                pending_binary.replace(binary)
                if name == "clip-w1":
                    vectors = run_dir / f"{name}-{flavor}-vectors.bin"
                    run(
                        [
                            sys.executable,
                            "-I",
                            "-B",
                            str(ROOT / "tests/project/clip_w1_reference.py"),
                            str(vectors),
                        ],
                        f"{name}-{flavor}-oracle",
                        "oracle",
                    )
                    run([str(binary), str(vectors)], f"{name}-{flavor}-test", "test")
                elif name == "depth-cube-mesh":
                    corpus = run_dir / f"{name}-{flavor}-cases"
                    corpus.mkdir()
                    run([str(binary), str(corpus)], f"{name}-{flavor}-test", "test")
                    run(
                        [
                            sys.executable,
                            "-I",
                            "-B",
                            str(ROOT / "tests/project/depth_cube_reference.py"),
                            "--corpus",
                            str(corpus),
                            "--output",
                            str(run_dir / f"{name}-{flavor}-oracle"),
                        ],
                        f"{name}-{flavor}-oracle",
                        "oracle",
                    )
                else:
                    run([str(binary)], f"{name}-{flavor}-test", "test")
        if graph is not None:
            graph.verify_inputs()
        if target is not None and targets.receipt(target) != report["build_target"]:
            raise ValueError("Build-target inputs changed during CPU checks")
        report["object_compiles"] = sum(not row["reused"] for row in report["objects"])
        report["object_reuses"] = sum(row["reused"] for row in report["objects"])
        report["unique_object_configurations"] = len(report["objects"])
        for item in report["inputs"]:
            if identity(ROOT / item["path"]) != item:
                raise RuntimeError("Source changed during CPU checks")
        report["status"] = "PASS"
        save()
        print(f"PASS: {len(selected)} CPU suites; flavors={args.flavor or ['strict', 'ubsan']}")
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
        save()
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
