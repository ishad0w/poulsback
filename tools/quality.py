#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Standing quality gates: build lists, dead, test-only and unused code, globals
that should be static, warnings, clones and size. Current hits live in the debt
list (quality-debts.txt); a hit outside it, a count above it, or an entry that no
longer hits fails. docs/tests.md#quality-gates owns the rules.

usage: quality.py check [--gates G,G] [--workspace DIR] [--hits]   (make check)
       quality.py reach [--gates G,G] [--workspace DIR] [--hits]   (make check-reach)
       quality.py ratchet --base REV      (pre-commit: counts never rise, no new CR entries)
       quality.py reconcile [--workspace DIR]   (merge maintenance: deletes and lowers only)
       quality.py inside PLAN             (internal: the builds, in the pinned i686 image)

The builds use a prepared root's pinned i686 image (--workspace;
default $POULSBACK_WORKSPACE, else the directory above the driver's git
checkout): the Mesa archive, the DDX and the kernel module for the warnings,
then for `reach` the real libgallium and DDX links with --gc-sections, the
module's relocation graph from its loader-visible sections and LTO links that
fold the product's constant flags.
"""

import argparse
import collections
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DEBTS = "quality-debts.txt"
# make check's gates (QC-stale runs in check-project.py) and the slow ones that need the
# prepared build root's full links (make check-reach, make coverage).
CHECK_GATES = ("sources", "test-only", "tools", "static", "warning", "clones", "size")
REACH_GATES = ("dead", "test-reach", "never-runs")
# Every gate the list may name: these, QC-stale (check-project.py), QC-coverage (coverage.py).
ALL_GATES = (*CHECK_GATES, *REACH_GATES, "stale", "coverage")
COUNTED = {"clones", "warning"}
# The files whose change changes what the gates find (the ratchet's exception).
GATE_TOOLS = ("tools/quality.py", "tools/coverage.py", "tools/check-project.py")
PRODUCT_DIRS = ("kernel", "src")
SUFFIXES = frozenset((".c", ".h", ".inc", ".cpp"))
FILE_LINES, FUNCTION_LINES, CLONE_TOKENS = 1500, 200, 50
# A debt ends with a closure label, such as CR1-M01 or P4c-6.
ENDS = re.compile(r"CR\d+-[KMXTL]\d\d|[A-Za-z0-9][A-Za-z0-9.]*(?:-[A-Za-z0-9.]+)*")
WARNINGS = ("-Wmissing-prototypes", "-Wmissing-declarations", "-Wunused", "-Wshadow")
# Without inlining, the linker's section graph is the source call graph: a global
# whose callers were all inlined is not mistaken for dead code.
GRAPH = ("-fno-inline",)
# QC-dead (f): an LTO link of the whole component from its real roots, still without
# inlining or identical-code folding, so a function it drops is one the product's
# constant arguments make unreachable.
LTO = ("-O2", "-flto", "-fno-inline", "-fno-ipa-icf")
WARNING_KINDS = re.compile(r"-W(?:missing-prototypes|missing-declarations|shadow(?:=[\w-]+)?|unused[\w-]*)")
CONTAINER_SOURCE, CONTAINER_OUT = "/source", "/out"
PATCH = "kernel/integration/linux.patch"


class Debt:
    __slots__ = ("gate", "subject", "count", "ends", "reason", "line")

    def __init__(self, gate, subject, count, ends, reason, line):
        self.gate, self.subject, self.count = gate, subject, count
        self.ends, self.reason, self.line = ends, reason, line


def load_debts(root, merged=False):
    """Entries `GATE SUBJECT[=COUNT] ENDS REASON` and `closed: ID ...` lines. `merged`: a
    union-merged list (the ratchet's base right after a merge) counts a duplicate once,
    at its higher count; the list a commit stages must have none."""
    debts, closed = {}, set()
    for number, raw in enumerate((root / DEBTS).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("closed:"):
            closed.update(line.split()[1:])
            continue
        fields = line.split(None, 3)
        if len(fields) != 4:
            raise ValueError(f"{DEBTS}:{number}: expected GATE SUBJECT ENDS REASON")
        gate, subject, ends, reason = fields
        if gate not in ALL_GATES:
            raise ValueError(f"{DEBTS}:{number}: unknown gate {gate}")
        count = 1
        if gate in COUNTED:
            subject, _, value = subject.rpartition("=")
            if not subject or not value.isdigit() or int(value) < 1:
                raise ValueError(f"{DEBTS}:{number}: {gate} needs SUBJECT=COUNT")
            count = int(value)
        if not ENDS.fullmatch(ends):
            raise ValueError(f"{DEBTS}:{number}: ENDS must be a CR ID or a plan step: {ends}")
        if (gate, subject) in debts:
            if not merged:
                raise ValueError(f"{DEBTS}:{number}: duplicate entry {gate} {subject}")
            debts[gate, subject].count = max(debts[gate, subject].count, count)
            continue
        debts[gate, subject] = Debt(gate, subject, count, ends, reason, number)
    return debts, closed


def compare(gates, hits, debts, closed):
    """Errors for hits outside the list, counts that changed and entries that no longer hit."""
    errors = []
    for gate in gates:
        found = hits.get(gate, {})
        listed = {d.subject: d for d in debts.values() if d.gate == gate}
        for subject, (count, detail) in sorted(found.items()):
            debt = listed.get(subject)
            if debt is None:
                errors.append(f"{gate} {subject}: new hit ({detail})")
            elif count > debt.count:
                errors.append(f"{gate} {subject}: {count} hits, the list allows {debt.count} ({detail})")
            elif count < debt.count:
                errors.append(f"{DEBTS}:{debt.line}: {gate} {subject} fell to {count}: lower it to ={count}")
        for subject, debt in sorted(listed.items()):
            if subject not in found:
                errors.append(f"{DEBTS}:{debt.line}: {gate} {subject} no longer hits: delete the entry")
            elif debt.ends in closed:
                errors.append(f"{DEBTS}:{debt.line}: {gate} {subject} names {debt.ends}, which is closed")
    return errors


def debt_lines(gate, found, debts):
    """The current hits in the list's format (known entries keep ENDS and REASON)."""
    for subject, (count, detail) in sorted(found.items()):
        debt = next((d for d in debts.values() if d.gate == gate and d.subject == subject), None)
        shown = f"{subject}={count}" if gate in COUNTED else subject
        yield f"{gate} {shown} {debt.ends if debt else '?'} {debt.reason if debt else detail}"



def reconcile_text(text, hits, computed):
    """The list with duplicate entries merged (the first line kept, with the highest count),
    entries of the `computed` gates that no longer hit deleted and counts lowered to the
    hits. It never adds an entry or raises a count, and a second run changes nothing."""
    out, first, changes = [], {}, collections.Counter()
    for line in text.splitlines(keepends=True):
        fields = line.split(None, 3)
        if len(fields) != 4 or fields[0] not in ALL_GATES:
            out.append(line)  # comments, `closed:` and anything load_debts refuses stay as they are
            continue
        gate, subject, ends, reason = fields
        count = 1
        if gate in COUNTED:
            subject, _, value = subject.rpartition("=")
            count = int(value) if value.isdigit() else 1
        if (gate, subject) in first:
            entry = first[gate, subject]
            entry[2] = max(entry[2], count)
            changes["duplicates merged"] += 1
            continue
        entry = [gate, subject, count, ends, reason.rstrip("\n")]
        first[gate, subject] = entry
        out.append(entry)
    result = []
    for item in out:
        if isinstance(item, str):
            result.append(item)
            continue
        gate, subject, count, ends, reason = item
        if gate in computed:
            if subject not in hits.get(gate, {}):
                changes["stale entries deleted"] += 1
                continue
            if hits[gate][subject][0] < count:
                count = hits[gate][subject][0]
                changes["counts lowered"] += 1
        shown = f"{subject}={count}" if gate in COUNTED else subject
        result.append(f"{gate} {shown} {ends} {reason}\n")
    return "".join(result), changes


# ---------------------------------------------------------------- C text


SCRUB = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
C_TOKEN = re.compile(
    r"(?P<id>[A-Za-z_]\w*)|(?P<num>\.?\d(?:[eEpP][+-]|[\w.])*)|(?P<str>\"\"|'')"
    r"|(?P<op>->|\+\+|--|<<=|>>=|<<|>>|[-+*/%&|^!=<>]=|&&|\|\||\.\.\.|::|\S)"
)
KEYWORDS = frozenset(
    "auto break case char const continue default do double else enum extern float for goto if "
    "inline int long register restrict return short signed sizeof static struct switch typedef "
    "union unsigned void volatile while _Bool _Static_assert bool true false NULL".split()
)


def code_text(text):
    """Comments removed, strings emptied, preprocessor lines blanked; lines kept."""

    def scrub(match):
        value = match.group()
        if value[0] in "\"'":
            return value[0] * 2
        return "\n" * value.count("\n") if value.startswith("/*") else ""

    lines, directive = SCRUB.sub(scrub, text).split("\n"), False
    for index, line in enumerate(lines):
        if directive or line.lstrip().startswith("#"):
            directive = line.rstrip().endswith("\\")
            lines[index] = ""
    return "\n".join(lines)


def c_tokens(text):
    """(token, kind, line) of code text; identifiers and literals keep their spelling."""
    tokens = []
    for number, line in enumerate(code_text(text).split("\n"), 1):
        for match in C_TOKEN.finditer(line):
            tokens.append((match.group(), match.lastgroup, number))
    return tokens


def functions(tokens):
    """(name, first line, last line) of every top-level function definition."""
    found, depth, start, index = [], 0, 0, 0
    while index < len(tokens):
        value = tokens[index][0]
        if value == "{" and depth == 0:
            name = None
            if index and tokens[index - 1][0] == ")" and "=" not in (t[0] for t in tokens[start:index]):
                level, back = 0, index - 1
                while back >= 0:
                    level += {")": 1, "(": -1}.get(tokens[back][0], 0)
                    if level == 0:
                        break
                    back -= 1
                if back > 0 and tokens[back - 1][1] == "id" and tokens[back - 1][0] not in KEYWORDS:
                    name = tokens[back - 1]
            close, level = index, 0
            while close < len(tokens):
                level += {"{": 1, "}": -1}.get(tokens[close][0], 0)
                if level == 0:
                    break
                close += 1
            if name:
                found.append((name[0], name[2], tokens[min(close, len(tokens) - 1)][2]))
            index, start = close + 1, close + 1
            continue
        if value == "{":
            depth += 1
        elif value == "}":
            depth -= 1
            if depth == 0:
                start = index + 1
        elif value == ";" and depth == 0:
            start = index + 1
        index += 1
    return found


def product_files(root):
    """Product sources: kernel/ and src/, without the overlay's test_* fixtures."""
    return [
        path.relative_to(root).as_posix()
        for top in PRODUCT_DIRS
        for path in sorted((root / top).rglob("*"))
        if path.is_file() and path.suffix in SUFFIXES and not path.name.startswith("test_")
    ]


def is_product(relative):
    path = PurePosixPath(relative)
    return path.parts[0] in PRODUCT_DIRS and path.suffix in SUFFIXES and not path.name.startswith("test_")


# ---------------------------------------------------------------- host gates


def gate_size(root, texts):
    hits = {}
    for relative, text in texts.items():
        lines = text.count("\n") + (not text.endswith("\n"))
        if lines > FILE_LINES:
            hits[relative] = (1, f"{lines} lines > {FILE_LINES}")
        longest = {}
        for name, first, last in functions(c_tokens(text)):
            longest[name] = max(longest.get(name, 0), last - first + 1)
        for name, length in longest.items():
            if length > FUNCTION_LINES:
                hits[f"{relative}:{name}"] = (1, f"{length} lines > {FUNCTION_LINES}")
    return hits


def gate_clones(root, texts, window=CLONE_TOKENS):
    """Type-2 clones: windows of `window` tokens equal after identifiers and literals
    are normalized. A file's count is its maximal cloned regions."""
    index = collections.defaultdict(list)
    for relative, text in texts.items():
        sequence = [
            value if kind == "op" or value in KEYWORDS else kind.upper()
            for value, kind, _ in c_tokens(text)
        ]
        for start in range(len(sequence) - window + 1):
            index[hash(tuple(sequence[start : start + window]))].append((relative, start))
    cloned = collections.defaultdict(set)
    partners = collections.defaultdict(set)
    for places in index.values():
        if len(places) < 2:
            continue
        for relative, start in places:
            others = [(r, s) for r, s in places if r != relative or abs(s - start) >= window]
            if others:
                cloned[relative].update(range(start, start + window))
                partners[relative].update(r for r, _ in others)
    hits = {}
    for relative, positions in cloned.items():
        ordered = sorted(positions)
        regions = 1 + sum(b != a + 1 for a, b in zip(ordered, ordered[1:]))
        shown = ", ".join(sorted(partners[relative] - {relative})[:3]) or "itself"
        hits[relative] = (regions, f"{regions} cloned regions of >= {window} tokens; with {shown}")
    return hits


def load_tool(root, name):
    path = root / "tools" / name
    spec = importlib.util.spec_from_file_location("poulsback_" + path.stem.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(path.parent))
    return module


INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)


class BuildLists:
    """What each builder names: module, Mesa archive, DDX (products); host suites and
    the overlay's Meson tests (tests). Includes are followed by name."""

    def __init__(self, root):
        self.root = root
        kernel = load_tool(root, "build-kernel.py")
        staged = {path.name: path.relative_to(root).as_posix() for path in kernel.project_sources()}
        self.kernel_objects = kernel.MODULE_OBJECTS.split()
        self.kernel_staged = staged
        module = [staged[name[:-2] + ".c"] for name in self.kernel_objects]
        patch = (root / "kernel/integration/linux.patch").read_text(encoding="utf-8")
        patched = [staged[n] for n in re.findall(r'^\+[ \t]*#[ \t]*include[ \t]*"([^"]+)"', patch, re.M) if n in staged]
        meson = root / "src/gallium/mesa/poulsback/meson.build"
        text = meson.read_text(encoding="utf-8")
        block = re.search(r"libpoulsback\s*=\s*static_library\((.*?)\n\)", text, re.S).group(1)
        names = re.findall(r"'([^']+\.c)'", re.search(r"\s*'poulsback',\s*\[(.*?)\]", block, re.S).group(1))
        self.mesa_c_args = re.findall(r"'([^']+)'", re.search(r"c_args\s*:\s*\[(.*?)\]", block, re.S).group(1))
        self.mesa = [self.meson_path(name) for name in names]
        self.ddx = sorted(p.relative_to(root).as_posix() for p in (root / "src/xorg").glob("*.c"))
        meson_all = {self.meson_path(n) for n in re.findall(r"([\w./-]+\.(?:cpp|c))(?![\w.])", meson_expanded(text))}
        userspace = load_tool(root, "build-userspace.py")
        self.components = {}
        host = set()
        for name, (sources, test) in userspace.COMPONENTS.items():
            paths = [userspace.component_source(s) for s in sources] + [userspace.component_test(test)]
            relatives = [p.relative_to(root).as_posix() for p in paths]
            self.components[name] = (relatives, userspace.component_options(name))
            host.update(relatives)
        self.userspace = userspace
        self.product_seeds = {"module": module + patched, "mesa": self.mesa, "ddx": self.ddx}
        # Test builders: every C file under tests/ and the overlay's fixtures, and the
        # product files a Python builder in tools/ or tests/ names.
        tests = [p for p in (root / "tests").rglob("*") if p.is_file() and p.suffix in SUFFIXES]
        tests += sorted((root / "src/gallium/mesa/poulsback").glob("test_*"))
        named = set()
        for script in [*(root / "tools").glob("*.py"), *(root / "tests").rglob("*.py")]:
            text = script.read_text(encoding="utf-8", errors="replace")
            named.update(PurePosixPath(n).name for n in re.findall(r"[\w./-]+\.(?:c|cpp|h|inc)\b", text))
        scripted = [r for r in product_files(root) if PurePosixPath(r).name in named]
        self.test_seeds = sorted(host | (meson_all - set(self.mesa)) | {p.relative_to(root).as_posix() for p in tests} | set(scripted))

    def meson_path(self, name):
        if name.startswith(("compiler/", "raster/")):
            return "src/" + name
        local = "src/gallium/mesa/poulsback/" + name
        if (self.root / local).is_file():
            return local
        found = sorted(p.relative_to(self.root).as_posix() for p in (self.root / "tests").rglob(name))
        return found[0] if found else local

    def closure(self, seeds):
        by_name = collections.defaultdict(list)
        for top in ("kernel", "src", "include", "tests"):
            for path in (self.root / top).rglob("*"):
                if path.is_file() and path.suffix in SUFFIXES:
                    by_name[path.name].append(path.relative_to(self.root).as_posix())
        seen, stack = set(seeds), list(seeds)
        while stack:
            relative = stack.pop()
            text = (self.root / relative).read_text(encoding="utf-8", errors="replace")
            for name in INCLUDE.findall(text):
                local = (PurePosixPath(relative).parent / name).as_posix()
                if (self.root / local).is_file():
                    targets = [os.path.normpath(local)]
                else:
                    candidates = by_name.get(PurePosixPath(name).name, [])
                    targets = [c for c in candidates if c.endswith(name)]
                for target in targets:
                    if target not in seen:
                        seen.add(target)
                        stack.append(target)
        return seen


def gate_sources(root, lists, products):
    built = set()
    for seeds in lists.product_seeds.values():
        built |= lists.closure(seeds)
    tested = lists.closure(lists.test_seeds)
    sources, test_only = {}, {}
    for relative in products:
        if relative in built:
            continue
        if relative in tested:
            test_only[relative] = (1, "only test builds compile or include it")
        else:
            sources[relative] = (1, "no build list names it and no built file includes it")
    return sources, test_only, built



GENERIC_NAMES = frozenset(("README.md", "MANIFEST.json", "Makefile", "__init__.py", "meson.build"))
NAMED_SUFFIXES = (".py", ".c", ".cpp", ".h", ".inc", ".json", ".jsonl", ".bin", ".frag", ".vert", ".txt", ".sh")


class References:
    """The tools/ and tests/ files a text names: by path, by a file name no other file
    has, or by `import NAME`. A named directory that runs nothing (data such as
    tests/compiler, or headers such as tests/kernel/vm-mocks) names its files."""

    def __init__(self, files):
        self.files = files
        self.by_name = collections.defaultdict(list)
        for name in files:
            self.by_name[PurePosixPath(name).name].append(name)
        self.modules = {PurePosixPath(n).stem: n for n in files if n.endswith(".py")}
        # A directory whose whole subtree runs nothing (data, or headers an -I names).
        subtree = collections.defaultdict(set)
        for name in files:
            for parent in PurePosixPath(name).parents:
                subtree[str(parent)].add(PurePosixPath(name).suffix)
        runnable = {".py", ".sh", ".c", ".cpp"}
        self.directories = sorted(d for d, suffixes in subtree.items() if "/" in d and not suffixes & runnable)

    def __call__(self, text, own=None):
        found = {name for name in self.files if name in text}
        for directory in self.directories:
            if re.search(re.escape(directory) + r"(?![\w.-])", text):
                found.update(n for n in self.files if n.startswith(directory + "/"))
        for token in set(re.findall(r"[\w.-]+", text)):
            owners = self.by_name.get(token, ())
            if len(owners) == 1 and token not in GENERIC_NAMES and token.endswith(NAMED_SUFFIXES):
                found.add(owners[0])
        for module in re.findall(r"^\s*(?:import|from)\s+(\w+)", text, re.M):
            if module in self.modules:
                found.add(self.modules[module])
        found.discard(own)
        return found


def meson_expanded(text):
    """Meson text plus the file names its `foreach` loops build by concatenation
    (`'test_nir_fragment_' + variant + '.c'`)."""
    built = []
    loop = r"^\s*foreach\s+(\w+)\s*:\s*\[([^\]]*)\]\s*$(.*?)^\s*endforeach"
    for variable, values, body in re.findall(loop, text, re.M | re.S):
        items = re.findall(r"'([^']*)'", values)
        for prefix, suffix in re.findall(r"'([^']*)'\s*\+\s*" + variable + r"\s*\+\s*'([^']*)'", body):
            built += [prefix + item + suffix for item in items]
    return text + "\n" + "\n".join(built)


def lab_commands(workspace):
    """The external runner scripts (the build root's tracked eng/lab and ops) with comments
    stripped: what they run, not what they mention."""
    listed = subprocess.run(["git", "-C", str(workspace), "ls-files", "eng/lab", "ops"], capture_output=True, text=True)
    lines = []
    for relative in listed.stdout.split():
        path = workspace / relative
        if path.suffix in (".py", ".sh", "") and path.is_file() and path.stat().st_size < 2_000_000:
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                lines.append(re.sub(r"(?:^|\s)#.*$", "", line))
    return "\n".join(lines)


def gate_tools(root, lists, product, lab_text):
    """QC-dead (e). A tool is used when a make recipe, AGENTS.md's command table, a
    ledger runner (spec/) or an external runner script runs it, or a used tool loads
    it; what a used tool names (helpers, oracles, data) is used too. A test runs from a
    build list, unittest discovery or an external runner script, and is used only when
    its subject is a used tool or live product code (`product`: built sources and the
    product's other inputs, such as linux.patch); data and headers are used when used
    code names or includes them."""
    files = sorted(
        p.relative_to(root).as_posix() for top in ("tools", "tests") for p in (root / top).rglob("*")
        if p.is_file() and "__pycache__" not in p.parts and p.suffix != ".md"
    )  # fmt: skip
    names = References(files)
    text = {f: (root / f).read_text(encoding="utf-8", errors="replace") for f in files}
    edges = {f: names(text[f], f) for f in files}
    agents = (root / "AGENTS.md").read_text(encoding="utf-8")
    sources = [(root / "Makefile").read_text(encoding="utf-8"), agents[agents.find("## Commands") :], lab_text]
    sources += [p.read_text(encoding="utf-8") for p in sorted((root / "spec").rglob("*.json"))]
    roots = set().union(*(names(source) for source in sources))
    used, stack = set(), [f for f in roots if f.startswith("tools/")]
    while stack:
        name = stack.pop()
        if name not in used:
            used.add(name)
            # A used tool's helpers, oracles and data; tests still need a used subject.
            stack += [g for g in edges[name] if not PurePosixPath(g).name.startswith("test_")]
    meson = (root / "src/gallium/mesa/poulsback/meson.build").read_text(encoding="utf-8")
    runners = {f for f in files if re.fullmatch(r"tests/(?:project|unit)/test_[^/]*\.py", f)}
    runners |= {f for f in roots if f.startswith("tests/")} | names(meson_expanded(meson))
    subjects = {}
    for relatives, _ in lists.components.values():
        for relative in relatives:
            if relative.startswith("tests/"):
                subjects.setdefault(relative, set()).update(r for r in relatives if is_product(r))
    product_names = {PurePosixPath(n).name: n for n in product if PurePosixPath(n).name not in GENERIC_NAMES}
    closures = {}

    def includes(name):
        """What a C test's compile includes (a test it includes counts with its subject)."""
        if name not in closures:
            closures[name] = lists.closure([name]) if name.endswith((".c", ".cpp")) else set()
        return closures[name]

    def subject(name):
        if not name.endswith((".py", ".c", ".cpp")):
            return True
        named = subjects.get(name) or {product_names[n] for n in re.findall(r"[\w.-]+", text[name]) if n in product_names}
        named |= {n for n in product if n in text[name]} | (includes(name) & product)
        return bool(named & product) or any(g in used and g.startswith("tools/") for g in edges[name])

    changed = True
    while changed:
        changed = False
        for name in files:
            if name in used or name.startswith("tools/"):
                continue
            run = name in runners or any(name in edges[g] for g in used)
            if run and subject(name):
                used.add(name)
                changed = True
        # What a used test's compile includes (a mock header a product file names).
        for name in [n for n in used if n.startswith("tests/") and n.endswith((".c", ".cpp"))]:
            for included in includes(name):
                if included.startswith("tests/") and included not in used:
                    used.add(included)
                    changed = True
    return {f: (1, "nothing runs it, or it serves only unused tools or dead code") for f in files if f not in used}


# ---------------------------------------------------------------- ELF


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, capture_output=True, text=True, **kwargs).stdout


def elf_sections(path):
    sections = {}
    for match in re.finditer(r"^\s*\[\s*(\d+)\]\s+(\S+)", run(["readelf", "-SW", str(path)]), re.M):
        sections[int(match.group(1))] = match.group(2)
    return sections


def elf_symbols(path):
    """Num -> (name, type, bind, visibility, section index or None)."""
    symbols = {}
    for line in run(["readelf", "-sW", str(path)]).splitlines():
        fields = line.split()
        if len(fields) < 7 or not fields[0].endswith(":") or not fields[0][:-1].isdigit():
            continue
        name = fields[7] if len(fields) > 7 else ""
        ndx = int(fields[6]) if fields[6].isdigit() else None
        symbols[int(fields[0][:-1])] = (name, fields[3], fields[4], fields[5], ndx)
    return symbols


def elf_relocations(path):
    """Target section name -> referenced symbol numbers."""
    edges, current = collections.defaultdict(list), None
    for line in run(["readelf", "-rW", str(path)]).splitlines():
        match = re.match(r"Relocation section '\.rela?(\.[^']*|[^'.][^']*)' at", line)
        if match:
            current = match.group(1)
            continue
        fields = line.split()
        if current and len(fields) >= 3 and re.fullmatch(r"[0-9a-f]{8,16}", fields[0]):
            info = int(fields[1], 16)
            edges[current].append(info >> (32 if len(fields[1]) > 8 else 8))
    return edges


def symbol_pairs(path):
    """(function name, its source file as DWARF names it) of every function (nm -l). A
    linked library may hold two static functions of one name."""
    pairs = []
    for line in run(["nm", "-l", "--defined-only", str(path)]).splitlines():
        fields = line.split("\t")
        head = fields[0].split()
        if len(head) == 3 and head[1] in "TtWw" and len(fields) > 1:
            pairs.append((head[2], fields[1].rsplit(":", 1)[0]))
    return pairs


def symbol_files(path):
    """Function name -> source file, for one object (its names are unique)."""
    return dict(symbol_pairs(path))


CLONE_SUFFIX = re.compile(r"\.(?:cold|part|isra|constprop|lto_priv|localalias)(?:\.\d+)*$")


def base_name(symbol):
    while True:
        stripped = CLONE_SUFFIX.sub("", symbol)
        if stripped == symbol:
            return symbol
        symbol = stripped


class ObjectFacts:
    """Functions, globals and undefined references of one relocatable object."""

    def __init__(self, path, source, prefix_map):
        self.path, self.source = path, source
        self.sections = elf_sections(path)
        self.symbols = elf_symbols(path)
        self.files = symbol_files(path)
        locations = {name: prefix_map(file) for name, file in self.files.items()}
        self.functions = {}  # symbol -> (section name, repository path)
        self.globals, self.undefined = {}, set()
        for name, kind, bind, _, ndx in self.symbols.values():
            if ndx is None:
                if name and kind == "NOTYPE" and bind in ("GLOBAL", "WEAK"):
                    self.undefined.add(name)
                continue
            if kind == "FUNC" and not name.startswith("__x86."):
                self.functions[name] = (self.sections.get(ndx, ""), locations.get(name, source))
            if kind in ("FUNC", "OBJECT") and bind in ("GLOBAL", "WEAK"):
                self.globals[name] = locations.get(name, source)


def facts_of(objects):
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        return list(pool.map(lambda item: ObjectFacts(*item), objects))


def link_dropped(facts, gc_log, link_map, archive=None):
    """Functions the real link keeps: {(path, function base): kept}. A function is
    dropped when its archive member is never pulled or --gc-sections removes its section."""
    removed = set()
    for match in re.finditer(r"removing unused section '([^']+)' in file '([^']+)'", gc_log):
        file = match.group(2)
        member = re.search(r"\(([^()]+)\)$", file)
        removed.add((member.group(1) if member else Path(file).name, match.group(1)))
    included = None
    if archive:
        head = link_map.split("Allocating common symbols")[0].split("Discarded input sections")[0]
        included = set(re.findall(r"^\S*" + re.escape(archive) + r"\(([^()]+)\)\s*$", head, re.M))
    state = {}
    for fact in facts:
        member = fact.path.name
        for name, (section, location) in fact.functions.items():
            if not location:
                continue
            kept = (included is None or member in included) and (member, section) not in removed
            base = base_name(name)
            old = state.get((location, base))
            state[location, base] = kept or bool(old)
    return state


def cref_references(link_map):
    """Symbol -> files that reference it, from the map's --cref table."""
    table = link_map.split("Cross Reference Table", 1)
    references, symbol = collections.defaultdict(list), None
    for line in (table[1].splitlines()[3:] if len(table) > 1 else []):
        if not line.strip():
            continue
        if not line[0].isspace():
            parts = line.split()
            symbol = parts[0]
            references[symbol] = []  # the first file is the definition
            continue
        if symbol:
            references[symbol].append(line.strip())
    return references


# Sections the module loader reads (the module's roots), and metadata sections
# that list code without calling it (never roots, never followed).
KERNEL_ROOTS = re.compile(
    r"\.gnu\.linkonce\.this_module|__param|__ksymtab\w*|__kcrctab\w*|_ftrace_events|"
    r"__tracepoints_ptrs|__bpf_raw_tp_map|_ftrace_eval_map|\.init_array|\.ctors|__dyndbg\w*|"
    r"\.export_symbol"
)
KERNEL_METADATA = re.compile(
    r"\.debug\w*|\.eh_frame|__mcount_loc|__patchable_function_entries|__bug_table|__ex_table|"
    r"__jump_table|\.altinstructions|\.altinstr_replacement|\.altinstr_aux|\.smp_locks|"
    r"\.parainstructions|\.static_call_sites|\.retpoline_sites|\.return_sites|\.call_sites|"
    r"\.ibt_endbr_seal|\.orc_unwind\w*|\.discard\.\w*|\.note\.\w*|\.comment|__tracepoints_strings|"
    r"\.modinfo|__versions|\.static_call\.text|__tracepoint_str|\.data\.once"
)
KERNEL_NODES = re.compile(r"\.(?:text|data|rodata|bss|init|exit|ref|ro_after_init)|__tracepoints$")


def section_edges(facts):
    """(object, section) -> the (object, section)s its relocations reach, for every section
    with relocations but metadata (which lists code without calling it)."""
    defined = {}
    for index, fact in enumerate(facts):
        for name, kind, bind, _, ndx in fact.symbols.values():
            if ndx is not None and bind in ("GLOBAL", "WEAK") and name:
                defined.setdefault(name, (index, fact.sections.get(ndx, "")))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        relocations = list(pool.map(lambda fact: elf_relocations(fact.path), facts))
    edges = collections.defaultdict(set)
    for index, (fact, sections) in enumerate(zip(facts, relocations)):
        for section, numbers in sections.items():
            if KERNEL_METADATA.match(section):
                continue
            targets = edges[index, section]
            for number in numbers:
                name, kind, bind, _, ndx = fact.symbols.get(number, ("", "", "", "", None))
                if ndx is not None:
                    targets.add((index, fact.sections.get(ndx, "")))
                elif kind != "SECTION" and name in defined:
                    targets.add(defined[name])
    return edges


def closure(edges, roots):
    seen, stack = set(roots), list(roots)
    while stack:
        for target in edges.get(stack.pop(), ()):
            if target not in seen:
                seen.add(target)
                stack.append(target)
    return seen


def kernel_reach(facts, edges=None):
    """Reachable (object, section) from the loader-visible sections through relocations."""
    edges = section_edges(facts) if edges is None else edges
    roots = [node for node in edges if KERNEL_ROOTS.fullmatch(node[1])]
    unknown = {s for _, s in edges if not KERNEL_ROOTS.fullmatch(s) and not KERNEL_NODES.match(s)}
    if unknown:
        raise ValueError("Unclassified kernel sections with relocations: " + ", ".join(sorted(unknown)))
    if not roots:
        raise ValueError("No module roots (.gnu.linkonce.this_module) among the kernel objects")
    return closure(edges, roots)


def classify_dropped(state, nodes, graphs, test_names):
    """dead and test-reach hits of the functions the product links drop. A dropped function
    is test-reached when a test names it, or when a test-named dropped function reaches it
    through the relocation graph (`graphs`: component -> section edges; `nodes`: function ->
    its (component, object, section)s); such a callee's hit names that caller, whose ENDS it
    takes. Nothing a product link drops is ever clean."""
    dropped = sorted(key for key, kept in state.items() if not kept and (key[0] == PATCH or is_product(key[0])))
    seeds = [key for key in dropped if key[1] in test_names]
    origin, queue = {}, collections.deque()
    for key in seeds:
        for node in sorted(nodes.get(key, ())):
            if node not in origin:
                origin[node] = key
                queue.append(node)
    while queue:
        node = queue.popleft()
        for target in sorted(graphs.get(node[0], {}).get(node[1:], ())):
            if (node[0], *target) not in origin:
                origin[node[0], *target] = origin[node]
                queue.append((node[0], *target))
    dead, test_reach, seeded = {}, {}, set(seeds)
    for key in dropped:
        subject = f"{key[0]}:{key[1]}"
        caller = next((origin[n] for n in sorted(nodes.get(key, ())) if n in origin), None)
        if key in seeded:
            test_reach[subject] = (1, "only tests reach it; the product link drops it")
        elif caller is not None:
            test_reach[subject] = (1, f"only tests reach it, through {caller[1]}; the product link drops it")
        else:
            dead[subject] = (1, "nothing reaches it; the product link drops it")
    return dead, test_reach


# ---------------------------------------------------------------- builds


def workspace_inputs(workspace, root):
    """The pinned image and SDKs; the Linux source subset of the version root's
    kernel lock pins (ops/build/linux-VERSION, such as linux-6.1.190, else ops/build/linux)."""
    image = re.search(r'IMAGE = "(sha256:[0-9a-f]{64})"', (workspace / "eng/lab/labimage.py").read_text())
    version = json.loads((root / "kernel/integration/linux.lock.json").read_text())["linux_version"]
    linux = workspace / f"ops/build/linux-{version}"
    inputs = {
        "image": image.group(1) if image else None,
        "mesa_sdk": workspace / "ops/build/mesa-sdk",
        "xorg_sdk": workspace / "ops/build/xorg-sdk",
        "linux": linux if linux.is_dir() else workspace / "ops/build/linux",
    }
    missing = [k for k, v in inputs.items() if not v or (isinstance(v, Path) and not v.is_dir())]
    if missing:
        raise ValueError(f"Workspace {workspace} lacks the build environment: {', '.join(missing)}")
    return inputs


def mesa_plan(root, lists, sdk):
    """Mesa's flags for libpoulsback from the SDK's compile_commands entry,
    plus libgallium's link line (docs/building.md#mesa)."""
    entries = json.loads((sdk / "build/compile_commands.json").read_text())
    entry = next(e for e in entries if e["output"].endswith("libpoulsback.a.p/poulsback_context.c.o"))
    package = str(Path(entry["directory"]).parent)
    flags, skip = [], False
    for arg in shlex.split(entry["command"])[1:]:
        if skip:
            skip = False
        elif arg in ("-o", "-MQ", "-MF", "-MT"):
            skip = True
        elif arg in ("-c", "-MD") or arg.startswith(("-I", "-fdiagnostics-color")):
            pass
        elif arg.startswith(("-D", "-W", "-f", "-m", "-std=", "-O", "-g", "-pthread")):
            flags.append(arg)
    mesa, build = package + "/mesa-26.2.4", package + "/build"
    system = [
        f"{build}/src/compiler/nir", f"{mesa}/src/compiler/nir", f"{build}/include", f"{mesa}/include",
        f"{build}/src", f"{mesa}/src", f"{build}/src/compiler", f"{mesa}/src/compiler",
        f"{build}/src/util", f"{build}/src/util/format", f"{mesa}/src/gallium/include",
        f"{mesa}/src/gallium/auxiliary", "/usr/include/libdrm",
    ]  # fmt: skip
    flags += [
        *lists.mesa_c_args, "-m32", f"-fdebug-prefix-map={CONTAINER_SOURCE}=driver",
        *(f"-I{CONTAINER_SOURCE}/{d}" for d in ("src/gallium/mesa/poulsback", "src/compiler", "src/raster", "include/uapi/poulsback")),
        *(x for p in system for x in ("-isystem", p)), *WARNINGS, "-Wunused-function", *GRAPH,
    ]  # fmt: skip
    gallium = "src/gallium/targets/dri/libgallium-26.2.4.so"
    text = run(["ninja", "-C", str(sdk / "build"), "-t", "commands", gallium])
    link = shlex.split(text.strip().splitlines()[-1])
    link[link.index("-o") + 1] = CONTAINER_OUT + "/mesa/libgallium.so"
    link[link.index("src/gallium/drivers/poulsback/libpoulsback.a")] = CONTAINER_OUT + "/mesa/libpoulsback.a"
    libdir = "/usr/lib/i386-linux-gnu/"
    runtime = {"expat": 1, "z": 1, "zstd": 1, "unwind": 8, "xcb": 1, "xcb-randr": 0, "xcb-dri3": 0,
               "xcb-present": 0, "xcb-sync": 1, "xshmfence": 1, "xcb-xfixes": 0}  # fmt: skip
    versioned = {f"{libdir}lib{n}.so": f"{libdir}lib{n}.so.{m}" for n, m in runtime.items()}
    link = [versioned.get(a, a) for a in link]
    link += ["-Wl,--print-gc-sections", f"-Wl,-Map={CONTAINER_OUT}/mesa/link.map", "-Wl,--cref"]
    return package, {"sources": lists.mesa, "flags": flags, "link": link, "workdir": build}


DDX_FLAGS = [
    # Xorg SDK flags, plus sections for the gc link and the gate's warnings.
    "-std=gnu11", "-O2", "-g", "-fPIC", "-fvisibility=hidden", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
    "-I/tmp/sdk/usr/include/xorg", "-I/tmp/sdk/usr/include/pixman-1", "-I/tmp/sdk/usr/include",
    "-I/usr/include/libdrm", "-ffunction-sections", "-fdata-sections",
    f"-fdebug-prefix-map={CONTAINER_SOURCE}=driver", *WARNINGS, "-Wno-error", *GRAPH,
]  # fmt: skip


def build_in_image(root, inputs, lists, out, jobs, lto=False):
    """One container run: Mesa archive + libgallium link, DDX link, module objects;
    with `lto`, the Mesa archive and the DDX once more as LTO links from their roots."""
    package, mesa = mesa_plan(root, lists, inputs["mesa_sdk"])
    release = json.loads((root / "kernel/integration/linux.lock.json").read_text())["kernel_release"]
    plan = {
        "mesa": mesa,
        "ddx": {"sources": lists.ddx, "flags": DDX_FLAGS},
        "kernel": {"headers": f"/usr/src/linux-headers-{release}", "flags": ["-ffunction-sections", "-fdata-sections", *WARNINGS, *GRAPH]},
        "jobs": jobs,
        "lto": lto,
    }  # fmt: skip
    (out / "plan.json").write_text(json.dumps(plan, indent=1) + "\n")
    command = [
        "docker", "--host=unix:///var/run/docker.sock", "run", "--rm", "--pull", "never",
        "--network", "none", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
        "--user", f"{os.getuid()}:{os.getgid()}", "--workdir", CONTAINER_OUT,
        "--mount", f"type=bind,source={root},target={CONTAINER_SOURCE},readonly",
        "--mount", f"type=bind,source={inputs['mesa_sdk']},target={package},readonly",
        "--mount", f"type=bind,source={inputs['xorg_sdk']},target=/xorg,readonly",
        "--mount", f"type=bind,source={inputs['linux']},target=/linux,readonly",
        "--mount", f"type=bind,source={out},target={CONTAINER_OUT}",
        inputs["image"], "setarch", "i686", "python3", "-I", "-B",
        f"{CONTAINER_SOURCE}/tools/quality.py", "inside", f"{CONTAINER_OUT}/plan.json",
    ]  # fmt: skip
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("Pinned-image build failed:\n" + (result.stdout + result.stderr)[-6000:])
    return json.loads((out / "inside.json").read_text())


def inside(plan_path):
    """In the pinned image: compile and link; diagnostics go to *.stderr files."""
    plan = json.loads(Path(plan_path).read_text())
    out, source, jobs = Path(CONTAINER_OUT), Path(CONTAINER_SOURCE), plan["jobs"]
    env = dict(os.environ, LANG="C", LC_ALL="C")
    pool = concurrent.futures.ThreadPoolExecutor(jobs)
    result = {}

    def compile_all(name, units, flags):
        directory = out / name
        directory.mkdir(exist_ok=True)

        def one(relative):
            obj = directory / (relative.replace("/", "_")[:-2] + ".o")
            done = subprocess.run(["cc", *flags, "-c", str(source / relative), "-o", str(obj)], env=env, capture_output=True, text=True)  # fmt: skip
            return relative, obj, done

        compiled = list(pool.map(one, units))
        (directory / "compile.stderr").write_text("".join(d.stderr for _, _, d in compiled))
        failed = [r for r, _, d in compiled if d.returncode]
        if failed:
            raise RuntimeError(f"{name}: compile failed: {failed}\n" + (directory / "compile.stderr").read_text()[-4000:])
        return [(r, str(o)) for r, o, _ in compiled]

    def defined_in(objects):
        names = set()
        for _, obj in objects:
            for line in subprocess.run(["nm", "--defined-only", "-g", obj], capture_output=True, text=True).stdout.splitlines():
                names.add(line.split()[-1])
        return names

    def lto_link(name, sources, flags, roots, libraries):
        """`roots` None: the exports the objects' visibility makes (the DDX)."""
        objects = compile_all(name, sources, [*flags, *LTO])
        script = []
        if roots is not None:
            (out / name / "roots.ver").write_text("{ global: " + "".join(f"{r}; " for r in roots) + "local: *; };\n")
            script = [f"-Wl,--version-script={out / name / 'roots.ver'}"]
        link = subprocess.run(
            ["cc", "-m32", "-shared", "-fPIC", *LTO, f"-flto={jobs}", "-g", "-ffunction-sections", "-Wl,--gc-sections",
             *script, "-o", str(out / name / "lto.so"), *(o for _, o in objects), *libraries],
            env=env, capture_output=True, text=True)  # fmt: skip
        (out / name / "link.stderr").write_text(link.stderr)
        if link.returncode:
            raise RuntimeError(f"{name}: LTO link failed\n" + link.stderr[-4000:])

    def mesa():
        spec = plan["mesa"]
        objects = compile_all("mesa", spec["sources"], spec["flags"])
        subprocess.run(["ar", "rcs", str(out / "mesa/libpoulsback.a"), *(o for _, o in objects)], check=True)
        link = subprocess.run(spec["link"], cwd=spec["workdir"], env=env, capture_output=True, text=True)
        (out / "mesa/link.stderr").write_text(link.stderr)
        if link.returncode:
            raise RuntimeError("mesa: libgallium link failed\n" + link.stderr[-4000:])
        (out / "mesa/libgallium.so").unlink()
        if plan["lto"]:
            # The roots: what libgallium's other inputs reference in the archive.
            ours = defined_in(objects)
            references = cref_references((out / "mesa/link.map").read_text(errors="replace"))
            roots = sorted(
                s for s, files in references.items()
                if s in ours and not s.startswith("__x86.") and any("libpoulsback.a(" not in f for f in files)
            )  # fmt: skip
            lto_link("mesa-lto", spec["sources"], spec["flags"], roots, [])
        return {"objects": objects}

    def ddx():
        spec = plan["ddx"]
        subprocess.run("mkdir -p /tmp/sdk && for d in /xorg/*.deb; do dpkg-deb -x \"$d\" /tmp/sdk; done", shell=True, check=True)  # fmt: skip
        objects = compile_all("ddx", spec["sources"], spec["flags"])
        link = subprocess.run(
            ["cc", "-shared", "-fPIC", "-Wl,--gc-sections", "-Wl,--print-gc-sections",
             f"-Wl,-Map={out}/ddx/link.map", "-Wl,--cref", "-o", str(out / "ddx/poulsback_drv.so"),
             *(o for _, o in objects), "-ldrm"], env=env, capture_output=True, text=True)  # fmt: skip
        (out / "ddx/link.stderr").write_text(link.stderr)
        if link.returncode:
            raise RuntimeError("ddx: link failed\n" + link.stderr[-4000:])
        if plan["lto"]:
            lto_link("ddx-lto", spec["sources"], spec["flags"], None, ["-ldrm"])
        return {"objects": objects}

    def kernel():
        spec = plan["kernel"]
        builder = load_tool(source, "build-kernel.py")
        target = builder.build_targets.load("debian-12-i686")
        staging = builder.make_build_plan(Path("/linux"), target)
        report = {"license_inputs": json.loads(staging.license_inputs), "inputs": [], "commands": []}
        stage = out / "kernel"
        builder.stage_sources(stage, Path("/linux"), report, build_target=target, plan=staging)
        flags = [*target["architecture"]["c_flags"], *spec["flags"], f"-ffile-prefix-map={stage}=/poulsback-kernel"]
        command = ["make", "-C", spec["headers"], "M=" + str(stage), "ARCH=" + target["toolchain"]["kernel_arch"],
                   "CC=gcc", "HOSTCC=gcc", f"-j{jobs}", "KCFLAGS=" + " ".join(flags), "modules"]  # fmt: skip
        built = subprocess.run(command, env=env, capture_output=True, text=True)
        (out / "kernel.stderr").write_text(built.stderr)
        if built.returncode:
            raise RuntimeError("kernel: module build failed\n" + built.stderr[-4000:])
        return {"staged": {name: str(path.relative_to(source)) for name, path, *_ , project in staging.files if project}}

    jobs_pool = concurrent.futures.ThreadPoolExecutor(3)
    futures = {name: jobs_pool.submit(job) for name, job in (("kernel", kernel), ("mesa", mesa), ("ddx", ddx))}  # fmt: skip
    errors = []
    for name, future in futures.items():
        try:
            result[name] = future.result()
        except Exception as error:  # noqa: BLE001 - every job's failure is reported
            errors.append(f"{name}: {error}")
    (out / "inside.json").write_text(json.dumps(result, indent=1) + "\n")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


def host_objects(root, lists, out, jobs):
    """The strict host suites' translation units at -O0 with the gate's warnings:
    their objects (references from tests) and diagnostics."""
    common = ["-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wshadow", "-Wstrict-prototypes", *WARNINGS]  # fmt: skip
    include = [f"-I{root}/src/{d}" for d in ("compiler", "raster", "video")]
    include += [f"-I{root}/kernel/{d}" for d in ("source", "integration", "debug", "memory", "engine")]
    units = {}
    for relatives, options in lists.components.values():
        for relative in relatives:
            units.setdefault((relative, tuple(options)), None)
    directory = out / "host"
    directory.mkdir()

    def one(index_unit):
        index, (relative, options) = index_unit
        obj = directory / f"{index}_{Path(relative).stem}.o"
        done = subprocess.run(["cc", *common, *include, *options, "-c", str(root / relative), "-o", str(obj)], capture_output=True, text=True)  # fmt: skip
        if done.returncode:
            raise RuntimeError(f"host compile failed: {relative}\n{done.stderr[-3000:]}")
        return relative, obj, done.stderr

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        compiled = list(pool.map(one, enumerate(units)))
    return [(r, o) for r, o, _ in compiled], "".join(s for _, _, s in compiled)


DIAGNOSTIC = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:\d+:)? warning: (?P<text>.*?) \[(?P<flag>-W[^\]]+)\]$", re.M)  # fmt: skip


def gate_warning(texts_by_build, path_of):
    seen, hits = set(), collections.defaultdict(int)
    for text in texts_by_build:
        for match in DIAGNOSTIC.finditer(text):
            flag = match.group("flag").split("=")[0]
            if not WARNING_KINDS.fullmatch(match.group("flag")):
                continue
            relative = path_of(match.group("file"))
            if not relative or not is_product(relative):
                continue
            key = (relative, int(match.group("line")), flag)
            if key not in seen:
                seen.add(key)
                hits[relative, flag] += 1
    return {f"{r}:{flag}": (count, f"{count} {flag} warnings") for (r, flag), count in hits.items()}


def gate_static(facts, roots):
    """Globals no other translation unit references (QC-warnings' static rule).
    Names starting with `__` come from macros (tracepoints, device tables)."""
    references = collections.Counter()
    for fact in facts:
        references.update(fact.undefined)
    hits = {}
    for fact in facts:
        for name, location in fact.globals.items():
            if name.startswith("__") or name in roots or references[name]:
                continue
            if location and is_product(location):
                hits[f"{location}:{name}"] = (1, "global named by no other translation unit: make it static")
    return hits


def keep_destination(keep, roots):
    """--keep's directory: new, beneath an existing canonical parent, and outside the
    checkout, the workspace and the build inputs, so keeping the outputs never replaces
    or deletes anything."""
    keep = keep.absolute()
    if keep.exists() or keep.is_symlink() or not keep.parent.is_dir() or keep.parent.resolve() != keep.parent:
        raise ValueError(f"--keep {keep}: needs a new directory beneath an existing canonical parent")
    for tree in roots:
        if keep.is_relative_to(Path(tree).resolve()):
            raise ValueError(f"--keep {keep}: inside {tree}; keep the build outputs outside the source and input trees")
    return keep


def build_gates(root, lists, workspace, jobs, keep=None, build_dir=None, lto=False):
    """dead, test-reach, static and warning hits from the pinned-image builds and the host
    TUs, and {(path, function): kept by a product build} (QC-coverage's product functions)."""
    inputs = workspace_inputs(workspace, root)
    if keep:
        keep = keep_destination(keep, [root, workspace, *(v for v in inputs.values() if isinstance(v, Path))])
    build_dir = root / (build_dir or "build")
    build_dir.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="quality-", dir=build_dir))
    try:
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            image = pool.submit(build_in_image, root, inputs, lists, scratch, jobs, lto)
            host = pool.submit(host_objects, root, lists, scratch, jobs)
            built, (host_units, host_stderr) = image.result(), host.result()
        return analyse(root, lists, scratch, built, host_units, host_stderr, inputs["linux"])
    finally:
        if keep:
            shutil.move(scratch, keep)  # a new directory: nothing else is replaced
        shutil.rmtree(scratch, ignore_errors=True)


def analyse(root, lists, scratch, built, host_units, host_stderr, linux):
    staged = built["kernel"]["staged"]

    def path_of(file):
        """Repository path of a diagnostic or DWARF file name; None outside the driver."""
        file = file.strip()
        for prefix in (CONTAINER_OUT + "/driver/", CONTAINER_SOURCE + "/", "driver/", str(root) + "/"):
            if file.startswith(prefix):
                return file[len(prefix) :]
        for prefix in ("/poulsback-kernel/", CONTAINER_OUT + "/kernel/", str(scratch / "kernel") + "/"):
            if file.startswith(prefix):
                return staged.get(file[len(prefix) :])
        return None

    def mapped(container_path):
        return scratch / Path(container_path).relative_to(CONTAINER_OUT)

    mesa = facts_of([(mapped(o), r, path_of) for r, o in built["mesa"]["objects"]])
    ddx = facts_of([(mapped(o), r, path_of) for r, o in built["ddx"]["objects"]])
    kernel_dir = scratch / "kernel"
    kernel_objects = sorted(p for p in kernel_dir.glob("*.o") if not p.name.endswith(".mod.o") or p.name == "poulsback.mod.o")  # fmt: skip
    kernel_objects = [p for p in kernel_objects if p.name != "poulsback.o"]
    kernel = facts_of([(p, staged.get(p.stem + ".c"), path_of) for p in kernel_objects])
    host = facts_of([(o, r, path_of) for r, o in host_units])

    mesa_map = (scratch / "mesa/link.map").read_text(errors="replace")
    ddx_map = (scratch / "ddx/link.map").read_text(errors="replace")
    nodes = collections.defaultdict(set)
    for tag, facts in (("mesa", mesa), ("ddx", ddx)):
        for index, fact in enumerate(facts):
            for name, (section, location) in fact.functions.items():
                if location:
                    nodes[location, base_name(name)].add((tag, index, section))
    state, linked = {}, set()
    for key, kept in link_dropped(mesa, (scratch / "mesa/link.stderr").read_text(), mesa_map, "libpoulsback.a").items():
        state[key] = state.get(key, False) or kept
        linked |= {key} if kept else set()
    for key, kept in link_dropped(ddx, (scratch / "ddx/link.stderr").read_text(), ddx_map).items():
        state[key] = state.get(key, False) or kept
        linked |= {key} if kept else set()
    kernel_edges = section_edges(kernel)
    reached, in_module = kernel_reach(kernel, kernel_edges), set()
    project = {name[:-2] for name in lists.kernel_objects}
    pristine = {}
    for index, fact in enumerate(kernel):
        upstream = linux / "drivers/gpu/drm/gma500" / (fact.path.stem + ".c")
        if fact.path.stem not in project and upstream.is_file():
            pristine[index] = set(re.findall(r"\w+", upstream.read_text(errors="replace")))
        for name, (section, location) in fact.functions.items():
            base = base_name(name)
            if not (location and is_product(location)):
                # A gma500 function linux.patch adds (defined in the gma500 file, which
                # upstream never names) is product code.
                own = PurePosixPath(fact.files.get(name, "")).name == fact.path.stem + ".c"
                if index not in pristine or not own or base in pristine[index]:
                    continue
                location = PATCH
            kept = (index, section) in reached
            state[location, base] = state.get((location, base), False) or kept
            nodes[location, base].add(("kernel", index, section))
            if kept:
                in_module.add((location, base))

    test_tokens = set()
    for path in [*(root / "tests").rglob("*"), *(root / "src/gallium/mesa/poulsback").glob("test_*")]:
        if path.is_file() and path.suffix in SUFFIXES:
            test_tokens.update(re.findall(r"\w+", path.read_text(errors="replace")))
    graphs = {"mesa": section_edges(mesa), "ddx": section_edges(ddx), "kernel": kernel_edges}
    dead, test_reach = classify_dropped(state, nodes, graphs, test_tokens)

    exported = set()
    for line in run(["readelf", "--dyn-syms", "-W", str(scratch / "ddx/poulsback_drv.so")]).splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[4] == "GLOBAL" and fields[6] != "UND":
            exported.add(fields[7])
    outside = set()
    for link_map, own in ((mesa_map, "libpoulsback.a("), (ddx_map, "/ddx/")):
        for symbol, files in cref_references(link_map).items():
            if any(own not in f for f in files):
                outside.add(symbol)
    roots = exported | outside | {"init_module", "cleanup_module", "__this_module"}
    static = gate_static([*mesa, *ddx, *kernel, *host], roots)
    stderrs = [host_stderr, (scratch / "kernel.stderr").read_text(errors="replace")]
    stderrs += [(scratch / d / "compile.stderr").read_text(errors="replace") for d in ("mesa", "ddx")]
    hits = {"dead": dead, "test-reach": test_reach, "static": static, "warning": gate_warning(stderrs, path_of)}
    if (scratch / "mesa-lto/lto.so").is_file():
        runnable = set()
        for name in ("mesa-lto", "ddx-lto"):
            for function, file in symbol_pairs(scratch / name / "lto.so"):
                location = path_of(file)
                if location and is_product(location):
                    runnable.add((location, base_name(function)))
        never = sorted(key for key in linked - runnable - in_module if is_product(key[0]))
        hits["never-runs"] = {f"{r}:{f}": (1, "linked, but the LTO link from the roots drops it: it can never run") for r, f in never}
        for key in never:
            state[key] = False
    return hits, state


# ---------------------------------------------------------------- commands


def collect(root, gates, workspace, jobs, keep=None, build_dir=None):
    texts = {r: (root / r).read_text(encoding="utf-8", errors="replace") for r in product_files(root)}
    hits = {}
    if "size" in gates:
        hits["size"] = gate_size(root, texts)
    if "clones" in gates:
        hits["clones"] = gate_clones(root, texts)
    builds = {"static", "warning", *REACH_GATES} & set(gates)
    if {"sources", "test-only", "tools"} & set(gates) or builds:
        lists = BuildLists(root)
        hits["sources"], hits["test-only"], built = gate_sources(root, lists, list(texts))
        if "tools" in gates:
            dead_files = set(hits["sources"]) | set(hits["test-only"])
            inputs = {
                p.relative_to(root).as_posix() for top in (*PRODUCT_DIRS, "include", "support")
                for p in (root / top).rglob("*") if p.is_file() and p.suffix not in SUFFIXES
            }  # fmt: skip
            hits["tools"] = gate_tools(root, lists, (built - dead_files) | inputs, lab_commands(workspace))
        if builds:
            hits.update(build_gates(root, lists, workspace, jobs, keep, build_dir, "never-runs" in gates)[0])
    return {gate: hits.get(gate, {}) for gate in gates}


def default_workspace():
    """$POULSBACK_WORKSPACE, else the directory above the checkout's git directory
    (right in snapshot worktrees too), else the driver's parent."""
    if os.environ.get("POULSBACK_WORKSPACE"):
        return Path(os.environ["POULSBACK_WORKSPACE"])
    common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                            capture_output=True, text=True).stdout.strip()  # fmt: skip
    if common and (Path(common).parents[1] / "eng/lab/labimage.py").is_file():
        return Path(common).parents[1]
    return ROOT.parent


def check(args):
    root = args.root.resolve()
    allowed = CHECK_GATES if args.command == "check" else REACH_GATES
    gates = args.gates.split(",") if args.gates else list(allowed)
    unknown = set(gates) - set(allowed)
    if unknown:
        raise ValueError(f"Unknown {args.command} gate: " + ", ".join(sorted(unknown)))
    debts, closed = load_debts(root)
    begin = time.monotonic()
    hits = collect(root, gates, (args.workspace or default_workspace()).resolve(), args.jobs, args.keep, args.build_dir)
    if args.hits:
        for gate in gates:
            print("\n".join(debt_lines(gate, hits[gate], debts)))
        return 0
    errors = compare(gates, hits, debts, closed)
    counts = ", ".join(f"{g} {sum(c for c, _ in hits[g].values())}" for g in gates)
    seconds = time.monotonic() - begin
    label = "quality gates" if args.command == "check" else "reach gates"
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print(f"FAIL: {label} ({counts}; {len(errors)} errors, {seconds:.1f} s). "
              f"See docs/tests.md#quality-gates; `tools/quality.py {args.command} --hits` prints the list.", file=sys.stderr)  # fmt: skip
        return 1
    print(f"PASS: {label}, every hit listed ({counts}; {seconds:.1f} s)")
    return 0


def moved_from(debt, removed):
    """The one removed entry a new review entry continues: same gate and ENDS, and its
    function name equals the new one or is contained in it (foo -> poulsback_context_foo).
    A name that pairs with none, or with two, is a new debt."""
    name = debt.subject.rsplit(":", 1)[-1]
    candidates = [d for d in removed if d.gate == debt.gate and d.ends == debt.ends]
    for same in (True, False):
        matches = [d for d in candidates if (d.subject.rsplit(":", 1)[-1] == name) == same and d.subject.rsplit(":", 1)[-1] in name]
        if matches:
            return matches[0] if len(matches) == 1 else None
    return None


def ratchet(args):
    """Against BASE's list: no count rises, and no new entry ends with a CR ID unless it
    continues one entry the same commit removes (a moved or renamed function)."""
    root = args.root.resolve()
    debts, _ = load_debts(root)
    with tempfile.TemporaryDirectory() as temporary:
        repo = (args.repo or root).resolve()
        raw = subprocess.run(["git", "-C", str(repo), "show", f"{args.base}:{DEBTS}"], capture_output=True)
        Path(temporary, DEBTS).write_bytes(raw.stdout if raw.returncode == 0 else b"")
        base, _ = load_debts(Path(temporary), merged=True)
    changed = [
        name for name in GATE_TOOLS
        if subprocess.run(["git", "-C", str(repo), "show", f"{args.base}:{name}"], capture_output=True).stdout
        != ((root / name).read_bytes() if (root / name).is_file() else b"")
    ]  # fmt: skip
    if changed:
        # New or changed gates find new hits of old code: the list's rules apply from the next commit.
        print(f"ratchet: {', '.join(changed)} changed; the gates' new hits may enter the list")
        return 0
    errors, removed = [], [d for key, d in base.items() if key not in debts]
    for key, debt in sorted(debts.items(), key=lambda item: item[1].line):
        old = base.get(key)
        if old is None and debt.ends.startswith("CR") and base:
            previous = moved_from(debt, removed)
            if previous is None:
                errors.append(f"{DEBTS}:{debt.line}: new entry for {debt.ends}: review debts only shrink")
            else:
                removed.remove(previous)  # one removed entry continues at most one new one
        elif old is not None and debt.count > old.count:
            errors.append(f"{DEBTS}:{debt.line}: {debt.gate} {debt.subject} rose from {old.count} to {debt.count}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


def reconcile(args):
    """After a series merged into the list: the computed gates' stale entries deleted, counts
    lowered, duplicates merged; coverage entries are only deduplicated (make coverage
    judges them). The gates judge the result; reconcile never adds."""
    root = args.root.resolve()
    workspace = (args.workspace or default_workspace()).resolve()
    hits = collect(root, [*CHECK_GATES, *REACH_GATES], workspace, args.jobs, None, args.build_dir)
    spec = importlib.util.spec_from_file_location("poulsback_check_project", root / "tools/check-project.py")
    project = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(project)
    hits["stale"] = project.stale_names(root)
    path = root / DEBTS
    text, changes = reconcile_text(path.read_text(encoding="utf-8"), hits, {*CHECK_GATES, *REACH_GATES, "stale"})
    path.write_text(text, encoding="utf-8")
    print("reconcile: " + (", ".join(f"{n} {what}" for what, n in sorted(changes.items())) or "nothing to change"))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, gates in (("check", CHECK_GATES), ("reach", REACH_GATES)):
        checking = commands.add_parser(name)
        checking.add_argument("--gates", help="comma-separated subset of " + ",".join(gates))
        checking.add_argument("--workspace", type=Path, help="default: $POULSBACK_WORKSPACE or above the git checkout")
        checking.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 4))
        checking.add_argument("--hits", action="store_true", help="print the current hits in the list's format")
        checking.add_argument("--keep", type=Path,
                              help="keep the build outputs in this new directory, outside the checkout and workspace")
        checking.add_argument("--build-dir", type=Path, default=Path("build"), help="scratch parent (relative to the root)")
    reconciling = commands.add_parser("reconcile")
    reconciling.add_argument("--workspace", type=Path, help="default: $POULSBACK_WORKSPACE or above the git checkout")
    reconciling.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 4))
    reconciling.add_argument("--build-dir", type=Path, default=Path("build"), help="scratch parent (relative to the root)")
    ratcheting = commands.add_parser("ratchet")
    ratcheting.add_argument("--base", default="HEAD")
    ratcheting.add_argument("--repo", type=Path, help="the git checkout BASE is read from (default: the root)")
    inner = commands.add_parser("inside")
    inner.add_argument("plan")
    args = parser.parse_args(argv)
    try:
        if args.command == "inside":
            return inside(args.plan)
        if args.command == "reconcile":
            return reconcile(args)
        return ratchet(args) if args.command == "ratchet" else check(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, StopIteration) as error:
        detail = getattr(error, "stderr", "") or ""
        print(f"quality: {error}\n{detail}"[-6000:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
