# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Explicit and staged style selection must dispatch every supported source."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("poulsback_style", ROOT / "tools/style.py")
STYLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STYLE)


class SourceStyle(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory(prefix="poulsback-style-")
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name).resolve()
        self.patch = mock.patch.object(STYLE, "ROOT", self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def source(self, name, raw=b"template<typename T>struct Box{T value;};\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
        return path

    def test_explicit_cpp_suffixes_reach_shared_selection(self):
        paths = [self.source("source" + suffix) for suffix in STYLE.CPP_SUFFIXES]
        paths += [self.source("input" + suffix) for suffix in (".py", ".c", ".h", ".inc")]
        ignored = self.source("README.md", b"# prose\n")
        self.assertEqual(
            STYLE.selected_paths([str(p) for p in paths] + [str(ignored)]), sorted(paths)
        )

    def test_staged_cpp_selection_does_not_discover_unselected_sources(self):
        selected = self.source("src/selected.cpp")
        self.source("src/unselected.cpp")
        self.source("benchmark/OpenGL/vendor/source.cpp")
        with mock.patch.object(
            STYLE.subprocess, "check_output", return_value=b"src/selected.cpp\0"
        ) as git:
            self.assertEqual(STYLE.selected_paths([]), [selected])
        git.assert_called_once_with(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"], cwd=self.root
        )

    def test_explicit_cpp_dispatch_keeps_cpp_language_and_check_is_readonly(self):
        selected = self.source("src/requested.cpp")
        original = selected.read_bytes()
        with (
            mock.patch.object(sys, "argv", ["style.py", "format-check", str(selected)]),
            mock.patch.object(STYLE, "tool", return_value="/clang-format") as tool,
            mock.patch.object(STYLE.subprocess, "run") as run,
        ):
            STYLE.main()
        tool.assert_called_once_with("CLANG_FORMAT")
        run.assert_called_once_with(
            [
                "/clang-format",
                "--assume-filename=" + str(selected),
                "--dry-run",
                "--Werror",
                str(selected),
            ],
            cwd=self.root,
            check=True,
        )
        self.assertEqual(selected.read_bytes(), original)

    def test_cpp_headers_and_c_fragments_use_their_existing_language_route(self):
        cpp = self.source("types.hpp")
        fragment = self.source("fragment.inc", b"int value;\n")
        with (
            mock.patch.object(sys, "argv", ["style.py", "format", str(cpp), str(fragment)]),
            mock.patch.object(STYLE, "tool", return_value="/clang-format"),
            mock.patch.object(STYLE.subprocess, "run") as run,
        ):
            STYLE.main()
        self.assertEqual(
            run.call_args_list,
            [
                mock.call(
                    [
                        "/clang-format",
                        "--assume-filename=" + str(fragment.with_suffix(".c")),
                        "-i",
                        str(fragment),
                    ],
                    cwd=self.root,
                    check=True,
                ),
                mock.call(
                    ["/clang-format", "--assume-filename=" + str(cpp), "-i", str(cpp)],
                    cwd=self.root,
                    check=True,
                ),
            ],
        )

    def test_cpp_lint_reports_strict_build_scope_instead_of_no_sources(self):
        cpp = self.source("input.cc")
        output = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["style.py", "lint", str(cpp)]),
            mock.patch.object(STYLE, "tool") as tool,
            mock.patch.object(STYLE.subprocess, "run") as run,
            contextlib.redirect_stdout(output),
        ):
            STYLE.main()
        self.assertIn(
            "C/C++ diagnostics belong to the selected strict build/tests", output.getvalue()
        )
        self.assertNotIn("No selected source", output.getvalue())
        tool.assert_not_called()
        run.assert_not_called()

    def test_mixed_python_cpp_check_dispatches_both_tools(self):
        cpp, python = self.source("input.cxx"), self.source("helper.py", b"pass\n")
        with (
            mock.patch.object(sys, "argv", ["style.py", "format-check", str(cpp), str(python)]),
            mock.patch.object(STYLE, "tool", side_effect=["/ruff", "/clang-format"]),
            mock.patch.object(STYLE.subprocess, "run") as run,
        ):
            STYLE.main()
        self.assertEqual(
            run.call_args_list,
            [
                mock.call(
                    ["/ruff", "format", "--no-cache", "--check", "--", str(python)],
                    cwd=self.root,
                    check=True,
                ),
                mock.call(
                    [
                        "/clang-format",
                        "--assume-filename=" + str(cpp),
                        "--dry-run",
                        "--Werror",
                        str(cpp),
                    ],
                    cwd=self.root,
                    check=True,
                ),
            ],
        )

    def test_explicit_cpp_keeps_ordinary_checkout_file_guard(self):
        source = self.source("real.cpp")
        link = self.root / "alias.cpp"
        link.symlink_to(source)
        with self.assertRaisesRegex(ValueError, "ordinary file inside the checkout"):
            STYLE.selected_paths([str(link)])
        with tempfile.TemporaryDirectory() as outside:
            external = Path(outside) / "outside.cpp"
            external.write_text("int value;\n")
            with self.assertRaisesRegex(ValueError, "ordinary file inside the checkout"):
                STYLE.selected_paths([str(external)])

    def test_pinned_formatter_checks_actual_cpp_and_only_requested_file(self):
        formatter = os.environ.get("CLANG_FORMAT")
        if not formatter:
            self.skipTest("CLANG_FORMAT is not set to the pinned development formatter")
        selected = self.source("requested.cpp")
        unselected = self.source("benchmark/OpenGL/vendor/unselected.cpp")
        original = unselected.read_bytes()
        shutil.copyfile(ROOT / ".clang-format", self.root / ".clang-format")
        with (
            mock.patch.object(sys, "argv", ["style.py", "format-check", str(selected)]),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            STYLE.main()
        with mock.patch.object(sys, "argv", ["style.py", "format", str(selected)]):
            STYLE.main()
        with mock.patch.object(sys, "argv", ["style.py", "format-check", str(selected)]):
            STYLE.main()
        self.assertEqual(unselected.read_bytes(), original)
        self.assertIn(b"template <typename T>", selected.read_bytes())


if __name__ == "__main__":
    unittest.main()
