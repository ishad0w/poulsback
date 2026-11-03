# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Each quality gate fails on its planted case; the debt list only shrinks."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("poulsback_quality", ROOT / "tools/quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(QUALITY)


class Tree(unittest.TestCase):
    def setUp(self):
        storage = tempfile.TemporaryDirectory(prefix="poulsback-quality-")
        self.addCleanup(storage.cleanup)
        self.root = Path(storage.name)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text))
        return path

    def compile(self, relative, *flags):
        obj = self.root / (relative + ".o")
        subprocess.run(
            ["cc", "-c", "-O2", "-g", "-ffunction-sections", "-fdata-sections", *QUALITY.GRAPH,
             *flags, str(self.root / relative), "-o", str(obj)],
            check=True, capture_output=True,
        )  # fmt: skip
        return obj

    def facts(self, *pairs):
        def path_of(file):
            prefix = str(self.root) + "/"
            return file[len(prefix) :] if file.startswith(prefix) else None

        return QUALITY.facts_of([(obj, relative, path_of) for obj, relative in pairs])


LIBRARY = """\
    int used(int x);
    int planted_dead(int x);
    int api(int x);
    __attribute__((visibility("default"))) int api(int x) { return used(x) + 1; }
    int planted_dead(int x) { return x * 7; }
"""
HELPER = """\
    int used(int x);
    int planted_global = 3;
    int used(int x) { return x + planted_global; }
"""


class DeadAndStatic(Tree):
    def link(self):
        objects = [self.compile("src/lib.c", "-fPIC", "-fvisibility=hidden"),
                   self.compile("src/helper.c", "-fPIC", "-fvisibility=hidden")]  # fmt: skip
        link_map = self.root / "link.map"
        done = subprocess.run(
            ["cc", "-shared", "-Wl,--gc-sections", "-Wl,--print-gc-sections", f"-Wl,-Map={link_map}",
             "-Wl,--cref", "-o", str(self.root / "lib.so"), *map(str, objects)],
            check=True, capture_output=True, text=True,
        )  # fmt: skip
        facts = self.facts((objects[0], "src/lib.c"), (objects[1], "src/helper.c"))
        return facts, done.stderr, link_map.read_text()

    def test_planted_dead_function_is_dropped(self):
        self.write("src/lib.c", LIBRARY)
        self.write("src/helper.c", HELPER)
        facts, log, link_map = self.link()
        state = QUALITY.link_dropped(facts, log, link_map)
        self.assertFalse(state["src/lib.c", "planted_dead"])
        self.assertTrue(state["src/lib.c", "api"])
        self.assertTrue(state["src/helper.c", "used"])

    def test_planted_unused_global_must_be_static(self):
        self.write("src/lib.c", LIBRARY)
        self.write("src/helper.c", HELPER)
        facts, _, _ = self.link()
        hits = QUALITY.gate_static(facts, {"api"})
        self.assertIn("src/helper.c:planted_global", hits)
        self.assertIn("src/lib.c:planted_dead", hits)
        self.assertNotIn("src/helper.c:used", hits)
        self.assertNotIn("src/lib.c:api", hits)

    def test_callee_of_a_test_only_builder_is_test_reach_and_an_orphan_is_dead(self):
        self.write(
            "src/b.c",
            """\
            static int helper(int x) { return x * 2; }
            __attribute__((used)) static int orphan(int x) { return x - 1; }
            int builder(int x);
            int builder(int x) { return helper(x) + 1; }
            __attribute__((visibility("default"))) int api(int x);
            __attribute__((visibility("default"))) int api(int x) { return x; }
            """,
        )
        obj = self.compile("src/b.c", "-fPIC", "-fvisibility=hidden")
        facts = self.facts((obj, "src/b.c"))
        link_map = self.root / "b.map"
        done = subprocess.run(
            ["cc", "-shared", "-Wl,--gc-sections", "-Wl,--print-gc-sections", f"-Wl,-Map={link_map}",
             "-o", str(self.root / "b.so"), str(obj)], check=True, capture_output=True, text=True,
        )  # fmt: skip
        state = QUALITY.link_dropped(facts, done.stderr, link_map.read_text())
        nodes = {}
        for name, (section, location) in facts[0].functions.items():
            nodes.setdefault((location, QUALITY.base_name(name)), set()).add(("mesa", 0, section))
        dead, test_reach = QUALITY.classify_dropped(state, nodes, {"mesa": QUALITY.section_edges(facts)}, {"builder"})
        self.assertEqual(set(test_reach), {"src/b.c:builder", "src/b.c:helper"})
        self.assertIn("through builder", test_reach["src/b.c:helper"][1])
        self.assertEqual(set(dead), {"src/b.c:orphan"})

    def test_planted_never_running_function_leaves_the_lto_link(self):
        # The constant flag crosses a translation unit, so only the LTO link folds it.
        self.write("src/flag.c", "static int capture(int x) { return x * 3; }\n"
                                 "int work(int x, int flag);\nint work(int x, int flag) { return flag ? capture(x) : x + 1; }\n")
        self.write("src/api.c", "int work(int x, int flag);\n__attribute__((visibility(\"default\"))) int api(int x);\n"
                                "__attribute__((visibility(\"default\"))) int api(int x) { return work(x, 0); }\n")
        objects = [self.compile(n, "-fPIC", "-fvisibility=hidden") for n in ("src/flag.c", "src/api.c")]
        facts = self.facts((objects[0], "src/flag.c"), (objects[1], "src/api.c"))
        link_map = self.root / "normal.map"
        done = subprocess.run(
            ["cc", "-shared", "-Wl,--gc-sections", "-Wl,--print-gc-sections", f"-Wl,-Map={link_map}",
             "-o", str(self.root / "normal.so"), *map(str, objects)], check=True, capture_output=True, text=True,
        )  # fmt: skip
        self.assertTrue(QUALITY.link_dropped(facts, done.stderr, link_map.read_text())["src/flag.c", "capture"])
        lto = []
        for name in ("src/flag.c", "src/api.c"):
            lto.append(self.root / (name + ".lto.o"))
            subprocess.run(["cc", "-c", "-g", "-fPIC", "-fvisibility=hidden", *QUALITY.LTO, str(self.root / name),
                            "-o", str(lto[-1])], check=True, capture_output=True)  # fmt: skip
        subprocess.run(["cc", "-shared", "-fPIC", "-g", *QUALITY.LTO, "-o", str(self.root / "lto.so"), *map(str, lto)],
                       check=True, capture_output=True)  # fmt: skip
        runnable = {QUALITY.base_name(f) for f in QUALITY.symbol_files(self.root / "lto.so")}
        self.assertIn("api", runnable)
        self.assertNotIn("capture", runnable)

    def test_two_live_statics_of_one_name_both_stay_runnable(self):
        for name in ("a", "b"):
            self.write(f"src/{name}.c", f"static int same(int x) {{ return x + {len(name)}; }}\n"
                                        f"__attribute__((visibility(\"default\"))) int api_{name}(int x);\n"
                                        f"__attribute__((visibility(\"default\"))) int api_{name}(int x) {{ return same(x); }}\n")
        objects = []
        for name in ("src/a.c", "src/b.c"):
            objects.append(self.root / (name + ".lto.o"))
            subprocess.run(["cc", "-c", "-g", "-fPIC", "-fvisibility=hidden", *QUALITY.LTO, str(self.root / name),
                            "-o", str(objects[-1])], check=True, capture_output=True)  # fmt: skip
        subprocess.run(["cc", "-shared", "-fPIC", "-g", *QUALITY.LTO, "-o", str(self.root / "lto.so"), *map(str, objects)],
                       check=True, capture_output=True)  # fmt: skip
        found = {(Path(file).name, QUALITY.base_name(f)) for f, file in QUALITY.symbol_pairs(self.root / "lto.so")}
        self.assertLessEqual({("a.c", "same"), ("b.c", "same")}, found)

    def test_module_reach_follows_relocations_from_loader_sections(self):
        self.write(
            "kernel/mod.c",
            """\
            static int helper(int x) { return x + 1; }
            static int entry(void) { return helper(2); }
            static int planted(void) { return helper(3); }
            __attribute__((used, section(".gnu.linkonce.this_module"))) static void *module[] = {entry};
            __attribute__((used, section("__mcount_loc"))) static void *metadata[] = {planted};
            """,
        )
        facts = self.facts((self.compile("kernel/mod.c"), "kernel/mod.c"))
        reached = QUALITY.kernel_reach(facts)
        sections = {name: section for name, (section, _) in facts[0].functions.items()}
        self.assertIn((0, sections["entry"]), reached)
        self.assertIn((0, sections["helper"]), reached)
        self.assertNotIn((0, sections["planted"]), reached)

    def test_unknown_module_section_fails_loudly(self):
        self.write(
            "kernel/mod.c",
            """\
            static int entry(void) { return 1; }
            __attribute__((used, section(".gnu.linkonce.this_module"))) static void *module[] = {entry};
            __attribute__((used, section("mystery"))) static void *odd[] = {entry};
            """,
        )
        facts = self.facts((self.compile("kernel/mod.c"), "kernel/mod.c"))
        with self.assertRaisesRegex(ValueError, "mystery"):
            QUALITY.kernel_reach(facts)

    def test_warning_gate_counts_product_diagnostics_once(self):
        text = (
            "src/a.c:3:5: warning: declaration of 'x' shadows a parameter [-Wshadow]\n"
            "src/a.c:3:5: warning: declaration of 'x' shadows a parameter [-Wshadow]\n"
            "src/a.c:9:1: warning: no previous prototype for 'f' [-Wmissing-prototypes]\n"
            "/usr/include/x.h:1:1: warning: declaration of 'y' shadows a global [-Wshadow]\n"
            "src/a.c:12:1: warning: format string [-Wformat]\n"
        )
        hits = QUALITY.gate_warning([text, text], lambda f: f if f.startswith("src/") else None)
        self.assertEqual(hits["src/a.c:-Wshadow"][0], 1)
        self.assertEqual(hits["src/a.c:-Wmissing-prototypes"][0], 1)
        self.assertEqual(len(hits), 2)


class TextGates(Tree):
    def test_planted_long_function_and_file(self):
        body = "".join(f"    x += {i};\n" for i in range(QUALITY.FUNCTION_LINES))
        text = "int long_one(int x)\n{\n" + body + "    return x;\n}\nint short_one(void) { return 1; }\n"
        text += "/* filler */\n" * QUALITY.FILE_LINES
        hits = QUALITY.gate_size(self.root, {"src/big.c": text})
        self.assertIn("src/big.c", hits)
        self.assertIn("src/big.c:long_one", hits)
        self.assertNotIn("src/big.c:short_one", hits)

    def test_planted_clone_is_counted_in_both_files(self):
        body = "".join(f"    if (a[{i}] > b) total += a[{i}] * {i};\n" for i in range(6))
        first = "int sum(const int *a, int b)\n{\n    int total = 0;\n" + body + "    return total;\n}\n"
        second = first.replace("sum", "add").replace("total", "acc").replace("a[", "v[").replace("*a", "*v")
        hits = QUALITY.gate_clones(self.root, {"src/one.c": first, "src/two.c": second, "src/other.c": "int z;\n"})
        self.assertEqual(hits["src/one.c"][0], 1)
        self.assertEqual(hits["src/two.c"][0], 1)
        self.assertNotIn("src/other.c", hits)

    def test_planted_unused_tool_and_its_test(self):
        self.write("Makefile", "check:\n\tpython3 tools/used.py\n")
        self.write("AGENTS.md", "## Commands\n")
        self.write("tools/used.py", "import helper\n")
        self.write("tools/helper.py", "DATA = 'tests/vectors/set/one.bin'\n")
        self.write("tests/vectors/set/one.bin", "x")
        self.write("tools/unused.py", "print('nobody runs me')\n")
        self.write("tests/project/test_unused.py", "LOAD = 'tools/unused.py'\n")
        self.write("tests/project/test_used.py", "LOAD = 'tools/used.py'\n")
        self.write("tests/unit/test_dead_code.c", '#include "orphan.h"\n')
        self.write("tests/corpus/shader.frag", "void main() {}\n")  # a data directory an external runner script stages
        self.write("tests/unit/test_gen_x.c", '#include "live.c"\n')  # built by a meson foreach
        self.write(
            "src/gallium/mesa/poulsback/meson.build",
            "foreach v : ['x']\n  t = executable('t', 'test_gen_' + v + '.c')\nendforeach\n",
        )
        lists = QUALITY.BuildLists.__new__(QUALITY.BuildLists)
        lists.root, lists.components = self.root, {"dead": (["src/orphan.c", "tests/unit/test_dead_code.c"], [])}
        hits = QUALITY.gate_tools(self.root, lists, {"src/live.c"}, "cp -r driver/tests/corpus out\n")
        self.assertEqual(set(hits), {"tools/unused.py", "tests/project/test_unused.py", "tests/unit/test_dead_code.c"})

    def test_planted_unbuilt_and_test_only_files(self):
        self.write("kernel/a.c", '#include "a.h"\n')
        self.write("kernel/a.h", "int a;\n")
        self.write("kernel/orphan.c", "int orphan;\n")
        self.write("src/tested.c", "int tested;\n")
        self.write("tests/test_t.c", '#include "tested.c"\n')
        lists = QUALITY.BuildLists.__new__(QUALITY.BuildLists)
        lists.root = self.root
        lists.product_seeds = {"module": ["kernel/a.c"]}
        lists.test_seeds = ["tests/test_t.c"]
        sources, test_only, _ = QUALITY.gate_sources(self.root, lists, QUALITY.product_files(self.root))
        self.assertEqual(set(sources), {"kernel/orphan.c"})
        self.assertEqual(set(test_only), {"src/tested.c"})


class DebtList(Tree):
    def debts(self, text):
        self.write(QUALITY.DEBTS, text)
        return QUALITY.load_debts(self.root)

    def test_hits_and_entries_must_match(self):
        debts, closed = self.debts(
            "dead src/a.c:f CR1-M01 reason\n"
            "dead src/a.c:gone Q1 fixed already\n"
            "clones src/b.c=3 CR1-M14 copies\n"
            "size src/c.c CR1-M17 split\n"
            "closed: CR1-M17\n"
        )
        hits = {
            "dead": {"src/a.c:f": (1, ""), "src/a.c:new": (1, "")},
            "clones": {"src/b.c": (2, "")},
            "size": {"src/c.c": (1, "")},
        }
        errors = "\n".join(QUALITY.compare(["dead", "clones", "size"], hits, debts, closed))
        self.assertIn("dead src/a.c:new: new hit", errors)
        self.assertIn("src/a.c:gone no longer hits", errors)
        self.assertIn("fell to 2", errors)
        self.assertIn("CR1-M17, which is closed", errors)
        self.assertNotIn("src/a.c:f:", errors)

    def test_rising_count_fails(self):
        debts, closed = self.debts("warning src/a.c:-Wshadow=1 Q1 reason\n")
        errors = QUALITY.compare(["warning"], {"warning": {"src/a.c:-Wshadow": (2, "")}}, debts, closed)
        self.assertTrue(any("2 hits, the list allows 1" in e for e in errors))

    def test_ratchet_lets_a_move_or_rename_through_and_refuses_a_new_review_debt(self):
        import argparse

        def commit_list(text):
            self.write(QUALITY.DEBTS, text)
            subprocess.run(["git", "-C", str(self.root), "add", QUALITY.DEBTS], check=True)
            subprocess.run(["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base"],
                           check=True)  # fmt: skip

        def ratchet(text):
            self.write(QUALITY.DEBTS, text)
            args = argparse.Namespace(root=self.root, base="HEAD", repo=None)
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                return QUALITY.ratchet(args), errors.getvalue()

        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        commit_list("coverage src/a.c:fail_at CR1-M04 r\ncoverage src/a.c:other CR1-M04 r\n")
        moved = "coverage src/b.c:poulsback_context_fail_at CR1-M04 r\ncoverage src/b.c:other CR1-M04 r\n"
        self.assertEqual(ratchet(moved)[0], 0)
        planted = "coverage src/a.c:fail_at CR1-M04 r\ncoverage src/a.c:other CR1-M04 r\ncoverage src/c.c:brand_new CR1-M04 r\n"
        code, errors = ratchet(planted)
        self.assertEqual(code, 1)
        self.assertIn("new entry for CR1-M04", errors)
        doubled = "coverage src/b.c:fail_at CR1-M04 r\ncoverage src/c.c:fail_at CR1-M04 r\ncoverage src/a.c:other CR1-M04 r\n"
        self.assertEqual(ratchet(doubled)[0], 1)  # one removed entry continues at most one new one
        self.assertEqual(ratchet("coverage src/b.c:fail_at Q2-dellcov r\n")[0], 0)  # a later step may be new
        # A union merge leaves duplicates in the base: counted once, at the higher count.
        commit_list("clones src/x.c=3 Q1 r\nclones src/x.c=5 Q1 r\ndead src/y.c:f CR1-M01 r\ndead src/y.c:f CR1-M01 r\n")
        self.assertEqual(ratchet("clones src/x.c=5 Q1 r\ndead src/y.c:f CR1-M01 r\n")[0], 0)
        self.assertEqual(ratchet("clones src/x.c=6 Q1 r\ndead src/y.c:f CR1-M01 r\n")[0], 1)

    def test_reconcile_deletes_lowers_and_merges_only(self):
        merged = (
            "# header\nclosed: CR1-K01\n"
            "dead src/a.c:gone CR1-M01 fixed by the series\n"
            "dead src/a.c:kept CR1-M01 still there\n"
            "clones src/b.c=5 Q1 tip's count\n"
            "clones src/b.c=3 Q1 series' count\n"
            "warning src/c.c:-Wshadow=1 Q1 rises: reconcile must not raise it\n"
            "coverage src/d.c:f Q2-dellcov a\n"
            "coverage src/d.c:f Q2-dellcov a\n"
        )
        hits = {
            "dead": {"src/a.c:kept": (1, ""), "src/a.c:new": (1, "")},
            "clones": {"src/b.c": (4, "")},
            "warning": {"src/c.c:-Wshadow": (2, "")},
        }
        text, changes = QUALITY.reconcile_text(merged, hits, {"dead", "clones", "warning"})
        self.assertEqual(
            text,
            "# header\nclosed: CR1-K01\ndead src/a.c:kept CR1-M01 still there\nclones src/b.c=4 Q1 tip's count\n"
            "warning src/c.c:-Wshadow=1 Q1 rises: reconcile must not raise it\ncoverage src/d.c:f Q2-dellcov a\n",
        )
        self.assertEqual(changes, {"duplicates merged": 2, "stale entries deleted": 1, "counts lowered": 1})
        self.assertNotIn("src/a.c:new", text)  # a new hit is never added; check still fails on it
        self.assertEqual(QUALITY.reconcile_text(text, hits, {"dead", "clones", "warning"}), (text, {}))

    def test_malformed_entries_are_refused(self):
        for text in ("dead src/a.c:f\n", "nosuch src/a.c Q1 r\n", "clones src/a.c Q1 r\n",
                     "dead src/a.c:f !bad r\n", "dead x Q1 r\ndead x Q1 r\n"):  # fmt: skip
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.debts(text)


class Stale(Tree):
    def test_planted_stale_comment_and_path(self):
        spec = importlib.util.spec_from_file_location("poulsback_check_project", ROOT / "tools/check-project.py")
        project = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(project)
        self.write(
            "src/raster/a.c",
            """\
            /* poulsback_real() calls poulsback_gone() after old_helper(). */
            int poulsback_real(void) { return 0; }
            const char *poulsback_named = "POULSBACK_TRACE";
            """,
        )
        self.write("docs/x.md", "`poulsback_real` and `POULSBACK_TRACE`; see `src/raster/missing.c` and `src/raster/a.c`.\n")
        hits = project.stale_names(self.root)
        self.assertEqual(
            set(hits),
            {"src/raster/a.c:poulsback_gone", "src/raster/a.c:old_helper", "docs/x.md:src/raster/missing.c"},
        )


class Coverage(Tree):
    def test_planted_unexecuted_function_and_dell_records(self):
        spec = importlib.util.spec_from_file_location("poulsback_coverage", ROOT / "tools/coverage.py")
        coverage = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(coverage)
        live = [("src/a.c", "tested"), ("src/a.c", "planted"), ("kernel/k.c", "on_dell"), ("kernel/j.c", "by_path")]
        hits = coverage.debts(live, {("src/a.c", "tested")}, {("kernel/k.c", "on_dell"), ("kernel/j.c", "by_path")})
        self.assertEqual(set(hits), {"src/a.c:planted"})
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write("kernel/k.c", "int k;\n")
        self.write("kernel/j.c", "int j;\n")
        subprocess.run(["git", "-C", str(self.root), "add", "kernel"], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "k"], check=True)
        commit = subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        record = {"schema": coverage.SCHEMA, "suite": "smoke", "component": "kernel", "source": "ftrace",
                  "driver_commit": commit, "tree": "clean", "run": "r1", "functions": ["on_dell", "kernel/j.c:by_path"]}  # fmt: skip
        self.write("records/function-coverage-kernel.json", json.dumps(record))
        self.write("records/other/function-coverage-kernel.json", json.dumps(dict(record, tree="dirty", functions=["x"])))
        covered, used = coverage.dell_records(self.root / "records", self.root, self.root, live)
        self.assertEqual(covered, {("kernel/k.c", "on_dell"), ("kernel/j.c", "by_path")})
        self.assertEqual(used, ["smoke/kernel (r1, 2 functions)"])
        self.write("kernel/k.c", "int k2;\n")  # k.c changed: its functions no longer count, j.c's still do
        covered, _ = coverage.dell_records(self.root / "records", self.root, self.root, live)
        self.assertEqual(covered, {("kernel/j.c", "by_path")})


class Keep(Tree):
    """--keep never deletes: a synthetic failing build with an existing directory, the
    checkout or a path in it as --keep is refused before it builds, and a fresh
    directory receives the outputs."""

    def setUp(self):
        super().setUp()
        outside = tempfile.TemporaryDirectory(prefix="poulsback-quality-keep-")
        self.addCleanup(outside.cleanup)
        self.outside = Path(outside.name)
        for name in ("workspace_inputs", "build_in_image", "host_objects"):
            self.addCleanup(setattr, QUALITY, name, getattr(QUALITY, name))
        QUALITY.workspace_inputs = lambda *args: {}

        def failed_build(*args, **kwargs):
            raise RuntimeError("synthetic compile failure")

        QUALITY.build_in_image = failed_build
        QUALITY.host_objects = lambda *args: ([], "")

    def build_gates(self, keep):
        return QUALITY.build_gates(self.root, None, self.root, 1, keep, self.root / "scratch")

    def test_an_existing_directory_is_refused_and_kept(self):
        keep = self.outside / "existing"
        keep.mkdir()
        (keep / "sentinel.txt").write_text("not the tool's\n")
        with self.assertRaisesRegex(ValueError, "new directory"):
            self.build_gates(keep)
        self.assertEqual((keep / "sentinel.txt").read_text(), "not the tool's\n")

    def test_the_checkout_and_a_path_in_it_are_refused(self):
        self.write("src/kept.c", "int kept;\n")
        with self.assertRaisesRegex(ValueError, "new directory"):
            self.build_gates(self.root)
        with self.assertRaisesRegex(ValueError, "outside the source"):
            self.build_gates(self.root / "kept-outputs")
        self.assertTrue((self.root / "src/kept.c").is_file())
        self.assertFalse((self.root / "kept-outputs").exists())

    def test_a_fresh_directory_receives_the_outputs(self):
        keep = self.outside / "fresh"
        with self.assertRaisesRegex(RuntimeError, "synthetic"):
            self.build_gates(keep)
        self.assertTrue(keep.is_dir())
        self.assertEqual(sorted(p.name for p in self.outside.iterdir()), ["fresh"])


class Tokens(unittest.TestCase):
    def test_functions_skip_initializers_strings_and_comments(self):
        text = textwrap.dedent(
            """\
            static const int table[] = { 1, 2 };
            /* int fake(void) { } */
            static int real(int x)
            {
                const char *s = "}{";
                if (x) { return 1; }
                return 0;
            }
            #define MACRO(x) { x }
            struct s { int a; };
            """
        )
        self.assertEqual(QUALITY.functions(QUALITY.c_tokens(text)), [("real", 3, 8)])


if __name__ == "__main__":
    unittest.main()
