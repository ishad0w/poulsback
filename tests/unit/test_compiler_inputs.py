#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Use actual compiler depfiles to check escaping, headers and path identities."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools/compiler_inputs.py"
spec = importlib.util.spec_from_file_location("poulsback_compiler_inputs_test", TOOL)
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class CompilerInputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_gcc_actual_make_escapes_are_not_shell_quotes(self):
        names = [
            "space name.h",
            "hash#name.h",
            "dollar$name.h",
            r"back\slash.h",
            r"slash\ space.h",
            r"slash\#hash.h",
            r"two\\slashes.h",
            "single'quote.h",
            'double"quote.h',
            "colon:name.h",
        ]
        for i, name in enumerate(names):
            (self.root / name).write_text("#define VALUE_%d %d\n" % (i, i))
        source = self.root / "unit.c"
        source.write_text("".join("#include <" + name + ">\n" for name in names))
        compilers = list(
            dict.fromkeys(shutil.which(name) for name in ("cc",) if shutil.which(name))
        )
        self.assertTrue(compilers, "A real C preprocessor is required")
        for i, cc in enumerate(compilers):
            with self.subTest(cc=cc):
                dep = self.root / (str(i) + ".d")
                child = subprocess.run(
                    [
                        cc,
                        "-M",
                        "-MT",
                        inputs.TARGET,
                        "-MF",
                        str(dep),
                        "-I" + str(self.root),
                        str(source),
                    ],
                    capture_output=True,
                    timeout=10,
                )
                self.assertEqual(child.returncode, 0, child.stderr.decode())
                result = inputs.parse_make_dependencies(dep.read_bytes(), self.root)
                for name in names:
                    self.assertIn(self.root / name, result)
                self.assertIn(source, result)

    def test_clang_supported_filename_spelling_is_literal(self):
        cc = shutil.which("clang")
        if cc is None:
            self.skipTest("Clang is optional; selected target recipe uses GCC")
        names = ["space name.h", "hash#name.h", "dollar$name.h", "single'quote.h", 'double"quote.h']
        for name in names:
            (self.root / name).write_text("/* input */\n")
        source = self.root / "unit.c"
        source.write_text("".join("#include <" + name + ">\n" for name in names))
        dep = self.root / "clang.d"
        child = subprocess.run(
            [cc, "-M", "-MT", inputs.TARGET, "-MF", str(dep), "-I" + str(self.root), str(source)],
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        parsed = inputs.parse_make_dependencies(dep.read_bytes(), self.root)
        for name in names:
            self.assertIn(self.root / name, parsed)
            inputs.file_identity(self.root / name)

    def test_clang_normalized_backslash_dependency_cannot_be_reconstructed(self):
        cc = shutil.which("clang")
        if cc is None:
            self.skipTest("Clang is optional; selected target recipe uses GCC")
        header = self.root / r"back\slash.h"
        header.write_text("/* literal backslash input */\n")
        source = self.root / "unit.c"
        source.write_text("#include <back\\slash.h>\n")
        dep = self.root / "clang.d"
        child = subprocess.run(
            [cc, "-M", "-MT", inputs.TARGET, "-MF", str(dep), "-I" + str(self.root), str(source)],
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(child.returncode, 0, child.stderr.decode())
        parsed = inputs.parse_make_dependencies(dep.read_bytes(), self.root)
        # Do not turn slash-normalized compiler metadata into guessed bytes.
        self.assertNotIn(header, parsed)
        with self.assertRaises(FileNotFoundError):
            for path in parsed:
                inputs.file_identity(path)

    def test_fixed_single_rule_and_control_refusals(self):
        for raw in (
            b"",
            b"other: a.h\n",
            b"poulsback_dependencies: a.h\nsecond: b.h\n",
            b"poulsback_dependencies: a.h\0",
            b"poulsback_dependencies: $(call shell,x)\n",
            b"poulsback_dependencies: a#comment\n",
            b"poulsback_dependencies: a.h\\\n",
            b"poulsback_dependencies: bad\x01.h\n",
            b"poulsback_dependencies:\n",
        ):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                inputs.parse_make_dependencies(raw, self.root)
        with self.assertRaises(ValueError):
            inputs.parse_make_dependencies(b"poulsback_dependencies: a.h\n", Path("relative"))

    def test_ambiguous_trailing_backslash_or_space_refuses(self):
        for raw in (
            b"poulsback_dependencies: trailing\\ \n",
            b"poulsback_dependencies: trailing\\\\ other.h\n",
        ):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                inputs.parse_make_dependencies(raw, self.root)

    def test_lexical_symlink_path_is_retained_and_retarget_is_distinct(self):
        for name in ("first", "second"):
            (self.root / name).mkdir()
            (self.root / name / "header.h").write_text("same bytes\n")
        link = self.root / "include"
        link.symlink_to("first", target_is_directory=True)
        path = self.root / "include/header.h"
        parsed = inputs.parse_make_dependencies(
            b"poulsback_dependencies: include/header.h\n", self.root
        )
        self.assertEqual(parsed, [path])
        first = inputs.file_identity(path)
        link.unlink()
        link.symlink_to("second", target_is_directory=True)
        second = inputs.file_identity(path)
        self.assertEqual(first["path"], second["path"])
        self.assertEqual(first["sha256"], second["sha256"])
        self.assertNotEqual(first["resolved"], second["resolved"])
        self.assertNotEqual(first, second)

    def test_atime_change_is_ignored_but_rewrite_is_detected(self):
        path = self.root / "header.h"
        path.write_text("#define VALUE 1\n")
        first = inputs.file_identity(path)
        old = path.stat()
        os.utime(path, ns=(old.st_atime_ns + 1_000_000_000, old.st_mtime_ns))
        # Explicit utime changes ctime, unlike ordinary reads. Compare the
        # stable-read behavior with a later read, rather than hiding ctime.
        after_utime = inputs.file_identity(path)
        self.assertEqual(after_utime, inputs.file_identity(path))
        self.assertEqual(first["sha256"], after_utime["sha256"])
        path.write_text("#define VALUE 2\n")
        self.assertNotEqual(after_utime, inputs.file_identity(path))

    def test_nonregular_and_missing_inputs_refuse(self):
        with self.assertRaises(ValueError):
            inputs.file_identity(self.root)
        with self.assertRaises(FileNotFoundError):
            inputs.file_identity(self.root / "missing.h")


if __name__ == "__main__":
    unittest.main()
