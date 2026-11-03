#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""QC-coverage, per function: every live product function is executed by a host
suite (gcov) or by a named Dell suite (its function records), or it is a debt with
a step. Line coverage is reported, never ratcheted. The run also checks the reach
gates (tools/quality.py reach), whose builds it shares. docs/tests.md#quality-gates
owns the rules.

usage: coverage.py check [--workspace DIR] [--dell DIR] [--report FILE] [--hits]

A live function is one a product link keeps and the LTO link does not drop
(QC-dead (b), (c), (f)). Dell records are `function-coverage-*.json` files under
--dell, a directory of device coverage records (`make coverage DELL_COVERAGE=DIR`; none
ship with a public checkout, and without one only the host suites count), schema
`poulsback-function-coverage-v1`: suite, component, source, driver_commit, tree,
run and the executed functions (bare names, or path:name). A clean record covers a
function while the function's file is unchanged since the record's commit.
"""

import argparse
import collections
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "poulsback-function-coverage-v1"
PRODUCT = ("kernel", "src", "include")
# The strict host flags (tools/build-userspace.py) at -O0, so every line maps to code.
FLAGS = ["-std=c11", "-O0", "-g", "--coverage", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow"]
TIMEOUT = 600


def quality(root):
    spec = importlib.util.spec_from_file_location("poulsback_quality", root / "tools/quality.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def suite_run(root, lists, name, directory):
    """Build one host suite with gcov in its own directory and run it."""
    relatives, options = lists.components[name]
    include = [f"-I{root}/src/{d}" for d in ("compiler", "raster", "video")]
    include += [f"-I{root}/kernel/{d}" for d in ("source", "integration", "debug", "memory", "engine")]
    directory.mkdir()
    objects = []
    for index, relative in enumerate(relatives):
        obj = directory / f"{index}_{Path(relative).stem}.o"
        subprocess.run(["cc", *FLAGS, *include, *options, "-c", str(root / relative), "-o", str(obj)],
                       check=True, capture_output=True)  # fmt: skip
        objects.append((relative, obj))
    binary = directory / name
    subprocess.run(["cc", *FLAGS, *(str(o) for _, o in objects), *lists.userspace.component_link_options(name),
                    "-o", str(binary)], check=True, capture_output=True)  # fmt: skip
    argv = [str(binary)]
    if name == "depth-cube-mesh":
        (directory / "cases").mkdir()
        argv.append(str(directory / "cases"))
    if name == "clip-w1":
        reference = root / "tests/project/clip_w1_reference.py"
        subprocess.run([sys.executable, "-I", "-B", str(reference), str(directory / "vectors.bin")],
                       check=True, capture_output=True)  # fmt: skip
        argv.append(str(directory / "vectors.bin"))
    done = subprocess.run(argv, cwd=root, capture_output=True, timeout=TIMEOUT)
    if done.returncode:
        raise RuntimeError(f"host suite {name} failed under gcov:\n{done.stderr.decode()[-2000:]}")
    return objects


def gcov(root, objects):
    """{path: {"functions": {name: count}, "lines": {line: count}}} over all suites."""
    merged = collections.defaultdict(lambda: {"functions": collections.Counter(), "lines": collections.Counter()})
    prefix = str(root) + "/"
    for _, obj in objects:
        data = obj.with_suffix(".gcda")
        if not data.is_file():
            continue
        raw = subprocess.run(["gcov", "--json-format", "--stdout", str(data)], cwd=obj.parent,
                             check=True, capture_output=True).stdout  # fmt: skip
        for document in raw.decode().splitlines():
            if not document.strip():
                continue
            for entry in json.loads(document)["files"]:
                path = entry["file"]
                if not path.startswith(prefix):
                    continue
                relative = path[len(prefix) :]
                for function in entry["functions"]:
                    merged[relative]["functions"][function["name"]] += function["execution_count"]
                for line in entry["lines"]:
                    merged[relative]["lines"][line["line_number"]] += line["count"]
    return merged


def product_blobs(repo, commit=None, root=None):
    """{path: blob} of kernel/, src/ and include/ at a commit, or in a checked tree."""
    if commit:
        listed = subprocess.run(["git", "-C", str(repo), "ls-tree", "-r", commit, "--", *PRODUCT],
                                capture_output=True, text=True)  # fmt: skip
        if listed.returncode:
            return None
        return {line.split("\t", 1)[1]: line.split()[2] for line in listed.stdout.splitlines()}
    paths = sorted(p.relative_to(root).as_posix() for top in PRODUCT for p in (root / top).rglob("*") if p.is_file())
    blobs = subprocess.run(["git", "hash-object", "--stdin-paths"], cwd=root, input="\n".join(paths),
                           capture_output=True, text=True, check=True).stdout.split()  # fmt: skip
    return dict(zip(paths, blobs))


def dell_records(directory, repo, root, live):
    """The live functions clean Dell records cover in this tree, and the records used. A
    record vouches for a function while the function's file is unchanged since the
    record's commit: `path:name` for that path, a bare name (the kernel's ftrace names)
    for every live function of that name whose file is unchanged."""
    covered, used, current = set(), [], product_blobs(repo, root=root)
    by_name = collections.defaultdict(list)
    for relative, function in live:
        by_name[function].append(relative)
    for path in sorted(directory.rglob("function-coverage-*.json")) if directory.is_dir() else ():
        record = json.loads(path.read_text())
        if record.get("schema") != SCHEMA or record.get("tree") != "clean":
            continue
        then = product_blobs(repo, record.get("driver_commit")) or {}
        unchanged = {p for p, blob in then.items() if current.get(p) == blob}
        before = len(covered)
        for entry in record["functions"]:
            relative, _, function = entry.rpartition(":")
            paths = [relative] if relative else by_name.get(function, [])
            covered.update((p, function) for p in paths if p in unchanged)
        used.append(f"{record['suite']}/{record['component']} ({record['run']}, {len(covered) - before} functions)")
    return covered, used


def debts(live, hosted, covered):
    """Live product functions no host suite executes and no counting Dell record covers."""
    return {
        f"{relative}:{function}": (1, "no host suite or Dell suite executes it")
        for relative, function in live
        if (relative, function) not in hosted and (relative, function) not in covered
    }


def lines(merged, tools):
    files = {r: d for r, d in merged.items() if tools.is_product(r)}
    report = {r: [sum(1 for c in d["lines"].values() if c), len(d["lines"])] for r, d in sorted(files.items())}
    return sum(h for h, _ in report.values()), sum(t for _, t in report.values()), report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--workspace", type=Path, help="default: $POULSBACK_WORKSPACE or above the git checkout")
    parser.add_argument("--dell", type=Path, help="a directory of device function records (DELL_COVERAGE); default: none")
    parser.add_argument("--repo", type=Path, help="the driver checkout with the records' commits (default: the root's, else WORKSPACE/driver)")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 4))
    parser.add_argument("--report", type=Path, help="write per-file line coverage and the unexecuted functions as JSON")
    parser.add_argument("--hits", action="store_true", help="print the coverage and reach hits in the list's format")
    parser.add_argument("--build-dir", type=Path, default=Path("build"), help="scratch parent (relative to the root)")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    begin = time.monotonic()
    try:
        tools = quality(root)
        workspace = (args.workspace or tools.default_workspace()).resolve()
        inside_git = subprocess.run(["git", "-C", str(root), "rev-parse", "--git-dir"], capture_output=True).returncode == 0
        repo = (args.repo or (root if inside_git else workspace / "driver")).resolve()
        lists = tools.BuildLists(root)
        (root / args.build_dir).mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix="coverage-", dir=root / args.build_dir))
        try:
            with concurrent.futures.ThreadPoolExecutor(2) as outer:
                product = outer.submit(tools.build_gates, root, lists, workspace, args.jobs, None, args.build_dir, True)
                with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
                    futures = [pool.submit(suite_run, root, lists, n, scratch / n) for n in lists.components]
                    objects = [item for future in futures for item in future.result()]
                merged = gcov(root, objects)
                hits, state = product.result()
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        live = sorted(key for key, kept in state.items() if kept)
        hosted = {(r, f) for r, d in merged.items() for f, c in d["functions"].items() if c}
        covered, records = dell_records(args.dell, repo, root, live) if args.dell else (set(), [])
        hits["coverage"] = debts(live, hosted, covered)
        gates = [*tools.REACH_GATES, "coverage"]
        listed, closed = tools.load_debts(root)
        hit, total, report = lines(merged, tools)
        if args.report:
            document = {"lines": report, "unexecuted": sorted(hits["coverage"]), "records": records}
            args.report.write_text(json.dumps(document, indent=1) + "\n")
        if args.hits:
            for gate in gates:
                print("\n".join(tools.debt_lines(gate, hits[gate], listed)))
            return 0
        errors = tools.compare(gates, hits, listed, closed)
        summary = (f"live product functions {len(live)}, {len(hits['coverage'])} unexecuted; host lines {hit}/{total} "
                   f"(reported, not ratcheted); Dell records {len(records)}; "
                   + ", ".join(f"{g} {len(hits[g])}" for g in tools.REACH_GATES) + f"; {time.monotonic() - begin:.0f} s")  # fmt: skip
        if errors:
            print("\n".join(errors), file=sys.stderr)
            print("FAIL: coverage (" + summary + ")", file=sys.stderr)
            return 1
        print("PASS: coverage (" + summary + ")")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"coverage: {error}\n{getattr(error, 'stderr', '') or ''}"[-6000:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
