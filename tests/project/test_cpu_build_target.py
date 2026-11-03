# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""CPU target selection refuses unqualified ABI/search paths before compiling."""

import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/build-userspace.py"
SPEC = importlib.util.spec_from_file_location("cpu_target_builder_test", SCRIPT)
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class CompilerSelectionTests(unittest.TestCase):
    def test_selection_is_required_before_output_creation(self):
        with tempfile.TemporaryDirectory(prefix="poulsback-no-cpu-target-") as directory:
            output = Path(directory) / "unused"
            child = subprocess.run(
                [sys.executable, "-I", "-B", str(SCRIPT), "--output", str(output)],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(child.returncode, 2)
            self.assertIn("--build-target", child.stderr)
            self.assertFalse(output.exists())

    def test_direct_builder_has_no_implicit_host_fallback(self):
        with tempfile.TemporaryDirectory(prefix="poulsback-direct-cpu-target-") as directory:
            output = Path(directory)
            args = SimpleNamespace(component=["pack"], flavor=["strict"], skip_tool_tests=True)
            self.assertEqual(BUILDER.build(args, output, ["gcc"]), 1)
            report = json.loads((output / "report.json").read_bytes())
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("--build-target is required", report["error"])
            self.assertEqual(report["commands"], [])

    def test_native_and_multilib_arguments(self):
        for arguments in (["gcc"], ["/usr/bin/gcc", "-m32"]):
            with self.subTest(arguments=arguments):
                BUILDER.target_compiler_arguments(arguments, {})

    def test_unqualified_arguments_refuse(self):
        for option in (
            "-m64",
            "-mx32",
            "-march=native",
            "-march=i686",
            "-mtune=atom",
            "--sysroot=/other",
            "-I/host",
            "-L/host",
            "@compiler-options",
            "gcc",
        ):
            with self.subTest(option=option), self.assertRaisesRegex(ValueError, "optional -m32"):
                BUILDER.target_compiler_arguments(["gcc", option], {})

    def test_duplicate_abi_refuses(self):
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            BUILDER.target_compiler_arguments(["gcc", "-m32", "-m32"], {})

    def test_search_overrides_refuse(self):
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
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, name):
                BUILDER.target_compiler_arguments(["gcc", "-m32"], {name: "/other"})
        BUILDER.target_compiler_arguments(["gcc"], {"CPATH": ""})

    def refused_build(self, compiler, *, overrides=None):
        with tempfile.TemporaryDirectory(prefix="poulsback-cpu-target-") as directory:
            environment = dict(os.environ)
            environment.update(overrides or {})
            child = subprocess.run(
                [
                    sys.executable,
                    "-I",
                    "-B",
                    str(SCRIPT),
                    "--build-target",
                    "debian-12-i686",
                    "--cc",
                    compiler,
                    "--component",
                    "pack",
                    "--output",
                    directory,
                ],
                env=environment,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(child.returncode, 1, child.stdout + child.stderr)
            report = json.loads((Path(directory) / "report.json").read_text())
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["commands"], [])
            self.assertFalse(report["hardware_dispatch"])
            return report

    def test_failure_is_recorded_before_compilation(self):
        report = self.refused_build("gcc -m64")
        self.assertIn("optional -m32", report["error"])
        self.assertEqual(report["build_target"]["id"], "debian-12-i686")

    def test_header_leakage_is_recorded_before_compilation(self):
        report = self.refused_build("gcc -m32", overrides={"CPATH": "/host/include"})
        self.assertIn("CPATH", report["error"])

    def test_sysroot_probe_refuses_before_link(self):
        calls = []

        def run(argv, tag, phase="identity"):
            calls.append((argv, tag, phase))
            return "/unqualified\n"

        targets = BUILDER.build_target_module()
        with tempfile.TemporaryDirectory(prefix="poulsback-cpu-probe-") as directory:
            with self.assertRaisesRegex(ValueError, "sysroot"):
                BUILDER.target_compiler_probe(
                    targets, targets.load("debian-12-i686"), ["gcc", "-m32"], Path(directory), run
                )
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], "target-compiler-sysroot")


class HostAbiRecordTests(unittest.TestCase):
    def test_compiled_elf_class_endian_and_machine_are_recorded(self):
        with tempfile.TemporaryDirectory(prefix="poulsback-host-abi-") as directory:
            path = Path(directory) / "fixture"
            for cls, order, machine in ((32, "little", 3), (64, "little", 62), (64, "big", 21)):
                raw = bytearray(52 if cls == 32 else 64)
                raw[:7] = b"\x7fELF" + bytes(
                    (1 if cls == 32 else 2, 1 if order == "little" else 2, 1)
                )
                struct.pack_into("<HHI" if order == "little" else ">HHI", raw, 16, 3, machine, 1)
                offset = 40 if cls == 32 else 52
                raw[offset : offset + 2] = len(raw).to_bytes(2, order)
                path.write_bytes(raw)
                self.assertEqual(
                    BUILDER.host_output_abi(path),
                    dict(elf_class=cls, endian=order, machine=machine, type=3),
                )

    def test_malformed_executable_refuses(self):
        with tempfile.TemporaryDirectory(prefix="poulsback-host-bad-abi-") as directory:
            path = Path(directory) / "fixture"
            for raw in (
                b"#!/bin/sh\n",
                b"\x7fELF\x03" + bytes(60),
                b"\x7fELF\x02\x01\x01" + bytes(57),
            ):
                path.write_bytes(raw)
                with self.assertRaises(ValueError):
                    BUILDER.host_output_abi(path)


if __name__ == "__main__":
    unittest.main()
