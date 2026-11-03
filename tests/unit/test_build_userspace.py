# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Actual tiny C builds exercise graph reuse, profiles and refused stale work."""

import ctypes
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which("cc")


def module(path):
    spec = importlib.util.spec_from_file_location("poulsback_builder_fixture", path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


@unittest.skipUnless(CC, "actual C compiler required")
class BuildGraphTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="poulsback-object-graph-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("tools", "src/compiler", "src/gallium", "tests/unit"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        for name in ("build-userspace.py", "compiler_inputs.py"):
            shutil.copyfile(ROOT / "tools" / name, self.root / "tools" / name)
        (self.root / "src/gallium/graphics.lock.json").write_text("{}\n")
        self.header = self.root / "src/compiler/value.h"
        self.header.write_text("#define FACTOR 1\n")
        self.source = self.root / "src/compiler/value.c"
        self.source.write_text('#include "value.h"\nint value(void){return 5*FACTOR;}\n')
        self.fixture = self.root / "tests/unit/test_value.c"
        self.fixture.write_text(
            "#include <stdio.h>\nint value(void);\nint main(void){"
            'printf("VALUE=%d\\n",value());return 0;}\n'
        )
        self.builder = module(self.root / "tools/build-userspace.py")
        self.builder.COMPONENTS = {
            "one": (["compiler/value.c"], "test_value.c"),
            "two": (["compiler/value.c"], "test_value.c"),
        }
        self.output = self.root / "out"

    def run_builder(self, *extra, compiler=CC):
        argv = [
            "build-userspace.py",
            "--build-target",
            "host",
            "--output",
            str(self.output),
            "--cc",
            compiler,
        ]
        with mock.patch.object(sys, "argv", [*argv, *extra]):
            result = self.builder.main()
        report = json.loads((self.output / "report.json").read_bytes())
        self.assertEqual(report["status"], "PASS" if result == 0 else "FAIL")
        return result, report

    def values(self, report):
        return [
            (self.output / entry["stdout"]).read_text()
            for entry in report["commands"]
            if entry["phase"] == "test"
        ]

    def test_shared_objects_and_noop_execute_every_test(self):
        rc, clean = self.run_builder()
        self.assertEqual(rc, 0)
        self.assertEqual(clean["unique_object_configurations"], 4)
        self.assertEqual(clean["build_target"]["id"], "host")
        self.assertIn("HOST_CPU_ONLY", clean["build_target"]["scope"])
        self.assertEqual(len(clean["target_outputs"]), 4)
        self.assertTrue(all(row["elf_class"] in (32, 64) for row in clean["target_outputs"]))
        self.assertEqual(clean["object_compiles"], 4)
        self.assertEqual(self.values(clean), ["VALUE=5\n"] * 4)
        rc, incremental = self.run_builder()
        self.assertEqual(rc, 0)
        self.assertEqual(incremental["object_compiles"], 0)
        self.assertEqual(incremental["object_reuses"], 4)
        self.assertEqual(self.values(incremental), self.values(clean))
        self.assertEqual(sum("-M" in r["argv"] for r in incremental["commands"]), 8)
        self.assertEqual(sum("-dM" in r["argv"] for r in incremental["commands"]), 1)
        self.assertEqual(sum("-v" in r["argv"] for r in incremental["commands"]), 1)
        self.assertEqual(sum(r["phase"] == "prepare" for r in incremental["commands"]), 13)
        self.assertEqual(sum(r["phase"] == "link" for r in incremental["commands"]), 4)
        self.assertTrue(Path(clean["run_directory"], "report.json").is_file())

    def test_source_and_header_incremental_match_new_code(self):
        self.assertEqual(self.run_builder()[0], 0)
        self.source.write_text(self.source.read_text().replace("5*FACTOR", "6*FACTOR"))
        rc, source = self.run_builder()
        self.assertEqual((rc, source["object_compiles"]), (0, 2))
        self.assertEqual(self.values(source), ["VALUE=6\n"] * 4)
        self.header.write_text("#define FACTOR 3\n")
        rc, header = self.run_builder()
        self.assertEqual((rc, header["object_compiles"]), (0, 2))
        self.assertEqual(self.values(header), ["VALUE=18\n"] * 4)

    def test_added_and_removed_optional_header_invalidate_old_closure(self):
        self.source.write_text(
            '#include "value.h"\n#if __has_include("optional.h")\n'
            '#include "optional.h"\n#else\n#define EXTRA 0\n#endif\n'
            "int value(void){return 5*FACTOR+EXTRA;}\n"
        )
        self.assertEqual(self.run_builder()[0], 0)
        optional = self.header.with_name("optional.h")
        optional.write_text("#define EXTRA 3\n")
        rc, added = self.run_builder()
        self.assertEqual((rc, added["object_compiles"]), (0, 2))
        self.assertEqual(self.values(added), ["VALUE=8\n"] * 4)
        optional.unlink()
        rc, removed = self.run_builder()
        self.assertEqual((rc, removed["object_compiles"]), (0, 2))
        self.assertEqual(self.values(removed), ["VALUE=5\n"] * 4)

    def test_symlink_retarget_changes_actual_dependency(self):
        a, b = self.header.with_name("a.h"), self.header.with_name("b.h")
        a.write_text("#define FACTOR 1\n")
        b.write_text("#define FACTOR 2\n")
        self.header.unlink()
        self.header.symlink_to(a.name)
        self.assertEqual(self.run_builder()[0], 0)
        self.header.unlink()
        self.header.symlink_to(b.name)
        rc, report = self.run_builder()
        self.assertEqual((rc, report["object_compiles"]), (0, 2))
        self.assertEqual(self.values(report), ["VALUE=10\n"] * 4)

    def test_optional_header_created_during_cached_test_refuses_final_closure(self):
        optional = self.header.with_name("optional.h")
        marker = self.root / "create-optional"
        self.source.write_text(
            '#include "value.h"\n#if __has_include("optional.h")\n'
            '#include "optional.h"\n#else\n#define EXTRA 0\n#endif\n'
            "int value(void){return 5*FACTOR+EXTRA;}\n"
        )
        self.fixture.write_text(
            '#include <stdio.h>\nint value(void);\nint main(void){FILE *p=fopen('
            + json.dumps(str(marker))
            + ',"rb");if(p){fclose(p);p=fopen('
            + json.dumps(str(optional))
            + ',"wb");if(!p)return 2;fputs("#define EXTRA 7\\n",p);fclose(p);}'
            'printf("VALUE=%d\\n",value());return 0;}\n'
        )
        self.assertEqual(self.run_builder("--flavor", "strict")[0], 0)
        marker.touch()
        rc, report = self.run_builder("--flavor", "strict")
        self.assertEqual(rc, 1)
        self.assertEqual(self.values(report), ["VALUE=5\n"] * 2)
        self.assertTrue(all(row["reused"] for row in report["objects"]))
        self.assertEqual(len(report["objects"]), 2)
        self.assertIn("dependency closure changed during CPU checks", report["error"])
        self.assertEqual(report["commands"][-1]["phase"], "prepare")
        self.assertIn("-M", report["commands"][-1]["argv"])

    def test_deleted_header_does_not_run_old_binary(self):
        self.assertEqual(self.run_builder()[0], 0)
        old = (self.output / "one-strict").read_bytes()
        self.header.unlink()
        rc, report = self.run_builder()
        self.assertEqual(rc, 1)
        self.assertEqual(report["commands"][-1]["phase"], "prepare")
        self.assertNotEqual(report["commands"][-1]["exit_code"], 0)
        self.assertEqual(self.values(report), [])
        self.assertEqual((self.output / "one-strict").read_bytes(), old)

    def test_compile_failure_keeps_old_binary_and_actual_child(self):
        self.assertEqual(self.run_builder()[0], 0)
        old = (self.output / "one-strict").read_bytes()
        self.source.write_text('int value(void){return "invalid";}\n')
        rc, report = self.run_builder()
        self.assertEqual(rc, 1)
        child = report["commands"][-1]
        self.assertEqual(child["phase"], "compile")
        self.assertNotEqual(child["exit_code"], 0)
        self.assertFalse(child["timed_out"])
        self.assertEqual(self.values(report), [])
        self.assertEqual((self.output / "one-strict").read_bytes(), old)

    def test_corrupted_object_is_recompiled(self):
        self.assertEqual(self.run_builder()[0], 0)
        path = next((self.output / "objects/strict").glob("*.o"))
        path.write_bytes(b"corrupt")
        rc, report = self.run_builder()
        self.assertEqual((rc, report["object_compiles"]), (0, 1))
        self.assertEqual(self.values(report), ["VALUE=5\n"] * 4)

    def test_component_options_and_flavors_are_not_shared(self):
        # The debug component compiles as gnu11 (its mocks need GNU types), the
        # others as strict c11: one object per option set and flavor.
        self.fixture.write_text(
            "#include <stdio.h>\nint value(void);\n"
            "#ifdef __STRICT_ANSI__\n#define MODE 1\n#else\n#define MODE 0\n#endif\n"
            'int main(void){printf("VALUE=%d MODE=%d\\n",value(),MODE);return 0;}\n'
        )
        self.builder.COMPONENTS = {
            name: (["compiler/value.c"], "test_value.c") for name in ("one", "debug")
        }
        rc, report = self.run_builder()
        self.assertEqual((rc, report["object_compiles"]), (0, 8))
        self.assertEqual(
            self.values(report),
            ["VALUE=5 MODE=1\n", "VALUE=5 MODE=0\n"] * 2,
        )

    def test_wrapper_link_options_remain_effective(self):
        self.fixture.write_text(
            "#include <assert.h>\n#include <stdlib.h>\n#include <stdio.h>\n"
            "void *__real_calloc(size_t,size_t);void __real_free(void*);\n"
            "static unsigned allocations,releases;\n"
            "void *__wrap_calloc(size_t n,size_t b){allocations++;return __real_calloc(n,b);}\n"
            "void __wrap_free(void *p){releases++;__real_free(p);}\n"
            "int main(void){void *(*volatile alloc)(size_t,size_t)=calloc;"
            "void (*volatile drop)(void*)=free;void *p=alloc(1,4);assert(p);drop(p);"
            'assert(allocations==1&&releases==1);puts("WRAPPERS=1/1");return 0;}\n'
        )
        (self.root / "tests/unit/test_fragment_normalized.c").write_text(
            "/* Included source record. */\n"
        )
        self.builder.COMPONENTS = {"fragment-normalized": (["compiler/value.c"], "test_value.c")}
        rc, report = self.run_builder()
        self.assertEqual(rc, 0)
        self.assertEqual(self.values(report), ["WRAPPERS=1/1\n"] * 2)
        links = [r["argv"] for r in report["commands"] if r["phase"] == "link"]
        self.assertTrue(all("-Wl,--wrap=calloc" in a and "-Wl,--wrap=free" in a for a in links))

    def test_no_cached_pass_when_fixture_runtime_fails(self):
        marker = self.root / "refuse"
        self.fixture.write_text(
            "#include <stdio.h>\nint main(void){FILE *f=fopen("
            + json.dumps(str(marker))
            + ',"r");if(f){fclose(f);return 7;}puts("RUN");return 0;}\n'
        )
        self.assertEqual(self.run_builder()[0], 0)
        marker.write_text("fail\n")
        rc, report = self.run_builder()
        self.assertEqual(rc, 1)
        self.assertTrue(all(r["reused"] for r in report["objects"]))
        self.assertEqual(report["commands"][-1]["exit_code"], 7)
        self.assertEqual(report["commands"][-1]["phase"], "test")

    def test_timeout_reaps_actual_test_process_group(self):
        source, binary = self.root / "hang.c", self.root / "hang"
        source.write_text(
            "#define _POSIX_C_SOURCE 200809L\n#include <signal.h>\n#include <stdio.h>\n"
            "#include <unistd.h>\nint main(void){pid_t p=fork();if(p<0)return 1;"
            "if(!p){signal(SIGTERM,SIG_IGN);close(1);close(2);for(;;)pause();}"
            'printf("GRANDCHILD=%ld\\n",(long)p);fflush(stdout);for(;;)pause();}\n'
        )
        subprocess.run([CC, "-std=c11", str(source), "-o", str(binary)], check=True)
        directory = self.output / "runs/000001"
        directory.mkdir(parents=True)
        report = {"commands": [], "phase_seconds": {}}
        runner = self.builder.CommandRunner(
            directory, report, lambda: None, cwd=self.root, timeout=0.15
        )
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            runner.run([str(binary)], "timeout", "test")
        child = report["commands"][-1]
        self.assertTrue(child["timed_out"])
        self.assertIsNotNone(child["exit_code"])
        self.assertTrue(child["reaped"])
        self.assertTrue(child["output_complete"])
        self.assertEqual([r["signal"] for r in child["cleanup"]], ["SIGTERM", "SIGKILL"])
        pid = int((directory / "timeout.stdout.txt").read_text().split("=")[1])
        for _ in range(100):
            try:
                state = Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0]
            except FileNotFoundError:
                break
            if state in ("Z", "X"):
                break
            time.sleep(0.01)
        else:
            self.fail("Timed-out descendant still active")

    @unittest.skipUnless(sys.platform == "linux", "Linux subreaper required for fixture cleanup")
    def test_timeout_bounds_detached_descendant_inherited_pipes(self):
        source, binary = self.root / "detached.c", self.root / "detached"
        pid_file = self.root / "detached.pid"
        source.write_text(
            "#define _POSIX_C_SOURCE 200809L\n#include <signal.h>\n#include <stdio.h>\n"
            '#include <unistd.h>\nstatic void term(int s){(void)s;write(1,"TERM\\n",5);'
            'write(2,"TERM-ERROR\\n",11);_exit(0);}\nint main(void){signal(SIGTERM,term);'
            'puts("LEADER");fflush(stdout);fputs("LEADER-ERROR\\n",stderr);'
            "pid_t p=fork();if(p<0)return 1;if(!p){if(setsid()<0)return 2;"
            'FILE *f=fopen("detached.pid","w");if(!f)return 3;'
            'fprintf(f,"%ld\\n",(long)getpid());fclose(f);'
            'puts("DETACHED");fflush(stdout);fputs("DETACHED-ERROR\\n",stderr);'
            "sleep(3);return 0;}sleep(4);return 0;}\n"
        )
        subprocess.run([CC, "-std=c11", str(source), "-o", str(binary)], check=True)
        directory = self.output / "runs/000001"
        directory.mkdir(parents=True)
        report = {"commands": [], "phase_seconds": {}}
        runner = self.builder.CommandRunner(
            directory, report, lambda: None, cwd=self.root, timeout=0.15
        )
        # Adopt this deliberately detached fixture so teardown can reap it.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        try:
            begin = time.monotonic()
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                runner.run([str(binary)], "detached", "test")
            self.assertLess(time.monotonic() - begin, 2.0)
            child = report["commands"][-1]
            self.assertTrue(child["timed_out"])
            self.assertTrue(child["reaped"])
            self.assertFalse(child["output_complete"])
            self.assertIsNotNone(child["exit_code"])
            self.assertEqual([r["signal"] for r in child["cleanup"]], ["SIGTERM", "SIGKILL"])
            self.assertEqual(
                (directory / "detached.stdout.txt").read_bytes(), b"LEADER\nDETACHED\nTERM\n"
            )
            self.assertEqual(
                (directory / "detached.stderr.txt").read_bytes(),
                b"LEADER-ERROR\nDETACHED-ERROR\nTERM-ERROR\n",
            )
            pid = int(pid_file.read_text())
            state = Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0]
            self.assertNotIn(state, ("Z", "X"), "Runner killed a detached fixture")
        finally:
            try:
                if pid_file.exists():
                    pid = int(pid_file.read_text())
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    for _ in range(100):
                        if os.waitpid(pid, os.WNOHANG)[0] == pid:
                            break
                        time.sleep(0.01)
                    else:
                        self.fail("Detached fixture was not reaped during bounded teardown")
            finally:
                self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)

    def test_timeout_reports_unreaped_child_separately_from_incomplete_output(self):
        directory = self.output / "runs/000001"
        directory.mkdir(parents=True)
        report = {"commands": [], "phase_seconds": {}}
        runner = self.builder.CommandRunner(
            directory, report, lambda: None, cwd=self.root, timeout=0.15
        )
        child = mock.Mock(pid=1234, returncode=None)
        child.communicate.side_effect = [
            subprocess.TimeoutExpired(["fixture"], 0.15, output=b"before"),
            subprocess.TimeoutExpired(["fixture"], 0.2, output=b"before-after", stderr=b"error"),
        ]
        child.wait.side_effect = subprocess.TimeoutExpired(["fixture"], 0.2)
        with (
            mock.patch.object(self.builder.subprocess, "Popen", return_value=child),
            mock.patch.object(self.builder.os, "killpg"),
            mock.patch.object(self.builder.time, "sleep"),
        ):
            self.assertIsNone(runner.run(["fixture"], "unreaped", "test", required=False))
        command = report["commands"][-1]
        self.assertTrue(command["timed_out"])
        self.assertFalse(command["reaped"])
        self.assertFalse(command["output_complete"])
        self.assertIsNone(command["exit_code"])
        self.assertEqual((directory / "unreaped.stdout.txt").read_bytes(), b"before-after")
        self.assertEqual((directory / "unreaped.stderr.txt").read_bytes(), b"error")
        child.stdout.close.assert_called_once_with()
        child.stderr.close.assert_called_once_with()
        child.wait.assert_called_once_with(timeout=0.2)

    @unittest.skipUnless(shutil.which("clang"), "actual Clang fallback required")
    def test_other_compiler_uses_unchanged_uncached_route(self):
        rc, first = self.run_builder("--flavor", "strict", compiler=shutil.which("clang"))
        self.assertEqual(rc, 0)
        self.assertFalse(first["object_cache_supported"])
        self.assertEqual(first["dependency_manifest_scope"], "LEGACY_DECLARED_INPUTS_ONLY_UNCACHED")
        self.assertEqual(first["objects"], [])
        self.assertEqual(self.values(first), ["VALUE=5\n"] * 2)
        rc, second = self.run_builder("--flavor", "strict", compiler=shutil.which("clang"))
        self.assertEqual(rc, 0)
        self.assertEqual(sum(r["phase"] == "uncached-build" for r in second["commands"]), 2)
        self.assertEqual(self.values(second), self.values(first))

    def test_output_lock_rejects_competing_invocation(self):
        self.output.mkdir()
        with (self.output / ".build.lock").open("a+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(SystemExit) as result:
                self.run_builder()
        self.assertEqual(result.exception.code, 2)
        self.assertFalse((self.output / "runs").exists())

    def compiler_wrapper(self, body):
        path = self.root / "tools/compiler-wrapper.py"
        path.write_text(
            "import os,sys\nfrom pathlib import Path\n"
            + body
            + "\nos.execv("
            + repr(CC)
            + ",["
            + repr(CC)
            + ",*sys.argv[1:]])\n"
        )
        return shlex.join([sys.executable, str(path)])

    def test_compiler_wrapper_identity_invalidates_object_reuse(self):
        cc = self.compiler_wrapper("# First immutable wrapper.\n")
        self.assertEqual(self.run_builder(compiler=cc)[0], 0)
        rc, cached = self.run_builder(compiler=cc)
        self.assertEqual((rc, cached["object_compiles"]), (0, 0))
        path = self.root / "tools/compiler-wrapper.py"
        path.write_text(path.read_text() + "# Changed compiler input.\n")
        rc, changed = self.run_builder(compiler=cc)
        self.assertEqual((rc, changed["object_compiles"]), (0, 4))
        self.assertEqual(self.values(changed), ["VALUE=5\n"] * 4)
        self.assertTrue(all(row["gcc_toolchain"] for row in [changed["object_toolchain"]]))

    def test_header_appearing_only_during_compile_refuses_cache_commit(self):
        self.assertEqual(self.run_builder()[0], 0)
        old = (self.output / "one-strict").read_bytes()
        optional = self.header.with_name("optional.h")
        self.source.write_text(
            '#include "value.h"\n#if __has_include("optional.h")\n'
            '#include "optional.h"\n#else\n#define EXTRA 0\n#endif\n'
            'int value(void){return 5*FACTOR+EXTRA;}\n'
        )
        cc = self.compiler_wrapper(
            'if "-c" in sys.argv:Path('
            + repr(str(optional))
            + ').write_text("#define EXTRA 7\\n")\n'
        )
        rc, report = self.run_builder(compiler=cc)
        self.assertEqual(rc, 1)
        self.assertIn("dependency closure changed", report["error"])
        self.assertEqual(report["commands"][-1]["phase"], "compile")
        self.assertEqual(report["commands"][-1]["exit_code"], 0)
        self.assertEqual(self.values(report), [])
        self.assertEqual((self.output / "one-strict").read_bytes(), old)

    def test_failed_optional_cache_probe_retains_legacy_compiler_behavior(self):
        cc = self.compiler_wrapper('if "-dM" in sys.argv:sys.exit(2)\n')
        rc, report = self.run_builder(compiler=cc)
        self.assertEqual(rc, 0)
        self.assertFalse(report["object_cache_supported"])
        failed = [r for r in report["commands"] if r["exit_code"]]
        self.assertEqual(len(failed), 1)
        self.assertFalse(failed[0]["required"])
        self.assertEqual(failed[0]["exit_code"], 2)
        self.assertEqual(self.values(report), ["VALUE=5\n"] * 4)

    def test_spawn_error_records_attempt_without_fabricated_child(self):
        directory = self.output / "runs/000001"
        directory.mkdir(parents=True)
        report = {"commands": [], "phase_seconds": {}}
        runner = self.builder.CommandRunner(directory, report, lambda: None, cwd=self.root)
        with self.assertRaises(FileNotFoundError):
            runner.run([str(self.root / "missing-executable")], "missing", "compile")
        child = report["commands"][-1]
        self.assertIsNone(child["pid"])
        self.assertIsNone(child["exit_code"])
        self.assertIn("No such file", child["spawn_error"])


if __name__ == "__main__":
    unittest.main()
