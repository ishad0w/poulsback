# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Build requirements are strict data and confer no runtime admission."""

import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "poulsback_build_targets", ROOT / "tools/build_targets.py"
)
targets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(targets)


class BuildTargets(unittest.TestCase):
    def test_explicit_discovery_adds_data_without_a_registry_or_script_execution(self):
        original = targets.load("debian-12-i686")
        candidate = copy.deepcopy(original)
        candidate.update(id="example-system-i686", kind="candidate")
        candidate["toolchain"].update(c="gcc-12", cpp="g++-12")
        candidate["graphics"]["c_flags"] = ["-march=i686", "-mtune=generic"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for target, system in ((original, "debian-12"), (candidate, "example-system")):
                directory = root / "support" / system
                directory.mkdir(parents=True)
                (directory / "i686.json").write_text(json.dumps(target))
            marker = root / "executed"
            (root / "support/example-system/script.py").write_text(
                "raise RuntimeError('must never execute')\n"
            )
            for relative in original["inputs"].values():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((ROOT / relative).read_bytes())
            with mock.patch.object(targets, "ROOT", root):
                self.assertEqual(set(targets.discover()), {original["id"], candidate["id"]})
                self.assertEqual(targets.load(candidate["id"]), candidate)
                self.assertEqual(
                    targets.receipt(candidate)["manifest"]["path"],
                    "support/example-system/i686.json",
                )
                with self.assertRaisesRegex(ValueError, "Unknown build target"):
                    targets.load("not-installed-i686")
                with self.assertRaises(TypeError):
                    targets.load()
                self.assertFalse(marker.exists())
            second = root / "support/example-system/i686.json"
            second.write_text(json.dumps(original))
            with (
                mock.patch.object(targets, "ROOT", root),
                self.assertRaisesRegex(ValueError, "Duplicate build-target ID"),
            ):
                targets.discover()

    def test_reference_inputs_and_options_are_unchanged(self):
        target = targets.load("debian-12-i686")
        self.assertEqual(target["id"], "debian-12-i686")
        self.assertEqual(target["architecture"]["c_flags"], ["-march=i686", "-mtune=generic"])
        self.assertEqual(target["graphics"]["c_flags"], ["-march=i686", "-mtune=atom"])
        pin = targets.receipt(target)
        self.assertEqual(
            pin["inputs"]["kernel_lock"],
            targets._identity(ROOT / "kernel/integration/linux.lock.json"),
        )
        self.assertEqual(
            pin["inputs"]["graphics_lock"],
            targets._identity(ROOT / "src/gallium/graphics.lock.json"),
        )
        self.assertIn("NOT_RUNTIME_OR_HARDWARE_ADMISSION", pin["scope"])

    def test_unknown_fields_and_unsupported_abis(self):
        changes = [
            ((), "shell", "echo forbidden"),
            ((), "schema", "future"),
            (("architecture",), "pointer_bits", True),
            (("architecture",), "elf_machine", 62),
            (("architecture",), "abi", "x32"),
            (("architecture",), "c_flags", ["-march=native"]),
            (("toolchain",), "c", "sh -c anything"),
            (("graphics",), "pkg_config_dirs", ["usr/lib/pkgconfig", "usr/lib/pkgconfig"]),
            (("graphics",), "libdir", "../lib"),
            (("inputs",), "kernel_lock", "/tmp/lock.json"),
            (("inputs",), "graphics_lock", "src/../other.json"),
        ]
        for path, field, value in changes:
            with self.subTest(path=path, field=field):
                target = copy.deepcopy(targets.load("debian-12-i686"))
                obj = target
                for part in path:
                    obj = obj[part]
                obj[field] = value
                with self.assertRaises(ValueError):
                    targets.validate(target)

    def test_id_is_not_a_file_or_command(self):
        for value in ("../test", "/tmp/test", "foo;sh", "", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                targets.load(value)

    def test_duplicate_oversized_and_mismatched_manifests(self):
        original = targets.load("debian-12-i686")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "support/debian-12").mkdir(parents=True)
            path = root / "support/debian-12/i686.json"
            for raw in (
                '{"schema":"x","schema":"y"}',
                " " * (targets.MAX_MANIFEST + 1),
                json.dumps(dict(original, id="different-id")),
            ):
                path.write_text(raw)
                with mock.patch.object(targets, "ROOT", root), self.assertRaises(ValueError):
                    targets.load("debian-12-i686")

    def test_missing_symlink_and_changed_inputs(self):
        original = targets.load("debian-12-i686")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "support/debian-12/i686.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(original))
            with mock.patch.object(targets, "ROOT", root), self.assertRaises(ValueError):
                targets.load("debian-12-i686")
            for name in original["inputs"].values():
                item = root / name
                item.parent.mkdir(parents=True, exist_ok=True)
                item.write_text("input")
            first = root / original["inputs"]["kernel_lock"]
            first.unlink()
            first.symlink_to(ROOT / original["inputs"]["kernel_lock"])
            with mock.patch.object(targets, "ROOT", root), self.assertRaises(ValueError):
                targets.load("debian-12-i686")
        changed = copy.deepcopy(original)
        changed["graphics"]["libdir"] = "lib"
        with self.assertRaisesRegex(ValueError, "changed"):
            targets.receipt(changed)

    def test_elf_rejects_amd64_and_x32(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "output"
            header = bytearray(52)
            header[:7] = b"\x7fELF\x01\x01\x01"
            struct.pack_into("<HHI", header, 16, 1, 3, 1)
            struct.pack_into("<H", header, 40, 52)
            path.write_bytes(header)
            self.assertEqual(targets.check_elf(path)["machine"], 3)
            for cls, machine in ((2, 62), (1, 62)):
                header[4] = cls
                struct.pack_into("<H", header, 18, machine)
                path.write_bytes(header)
                with self.assertRaises(ValueError):
                    targets.check_elf(path)

    def test_compile_assertions_cover_userspace_not_uname(self):
        for cpp in (False, True):
            source = targets.compiler_source(cpp)
            for term in (
                "__GLIBC__",
                "__i386__",
                "__i686__",
                "sizeof(void *) == 4",
                "sizeof(long) == 4",
            ):
                self.assertIn(term, source)
            self.assertNotIn("uname", source)


if __name__ == "__main__":
    unittest.main()
