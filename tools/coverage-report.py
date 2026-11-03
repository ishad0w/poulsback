#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Count the coverage ledger's cases, runners, runs and passes per domain.

`check` validates the ledger: every case in scope declares a runner and
criteria, every case list parses and pins its source, every exclusion is
accepted by a maintainer. `ids --source SOURCE` maps a suite's test names (stdin)
to ledger case IDs for runner adapters. `report` reads run records (`cases.jsonl`, schema
poulsback-case-result-v1) and counts, per domain, the cases, the cases whose
runner exists, the cases run at the commit and the cases passed. It never runs
a test and never turns a missing result into a pass.
"""

import argparse
import fnmatch
import hashlib
import json
import math
import re
import subprocess
import sys
import tarfile
from pathlib import Path, PurePosixPath

LIST_SCHEMA = 'poulsback-case-list-v1'
RESULT_SCHEMA = 'poulsback-case-result-v1'
EXCLUSION_SCHEMA = 'poulsback-exclusions-v1'
VERDICTS = {'pass', 'fail', 'crash', 'timeout', 'skip', 'xfail'}
# Run classes: tests/acceptance/README.md defines them.
CLASSES = {'CHAIN', 'EXPERIMENT', 'BENCH', 'SOAK', 'NIGHTLY', 'HOST'}
REPOSITORIES = {'driver', 'workspace'}
# The product a HARDWARE result belongs to; CPU and source results also bind
# the tests. A docs-only commit keeps the results of the commit before it.
PRODUCT_PATHS = (
    'kernel',
    'src',
    'include',
    'support',
    'tools/build-kernel.py',
    'tools/build-userspace.py',
    'tools/build_targets.py',
)
TEST_PATHS = ('tests',)
LOCAL_ID = re.compile(r'[^\s/][^\x00-\x1f]*')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def finite_only(constant):
    """json's parse_constant: NaN and the infinities are not measurements."""
    raise ValueError(f'non-finite JSON constant {constant}')


def load_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, f'duplicate JSON key {key!r} in {path}')
            result[key] = value
        return result

    return json.loads(
        Path(path).read_text(encoding='utf-8'), object_pairs_hook=pairs, parse_constant=finite_only
    )


def text(value, label):
    require(type(value) is str and value.strip() and '\n' not in value, label + ' text')


def relative_path(value, label):
    text(value, label)
    path = PurePosixPath(value)
    require(
        not path.is_absolute() and '..' not in path.parts and path.as_posix() == value,
        label + ' path',
    )


def runner(value, label):
    require(type(value) is dict, label + ' runner')
    require(
        set(value) == {'entrypoint', 'arguments', 'backend', 'repository', 'builder'},
        label + ' runner fields',
    )
    relative_path(value['entrypoint'], label + ' runner entrypoint')
    require(
        type(value['arguments']) is list and all(type(x) is str for x in value['arguments']),
        label + ' runner arguments',
    )
    require(value['repository'] in REPOSITORIES, label + ' runner repository')
    text(value['builder'], label + ' runner builder')


def bound(value, label):
    require(type(value) is dict, label + ' bound')
    require(set(value) in ({'min', 'unit'}, {'max', 'unit'}), label + ' bound fields')
    limit = value.get('min', value.get('max'))
    require(
        type(limit) in (int, float) and math.isfinite(limit) and limit >= 0, label + ' bound value'
    )
    text(value['unit'], label + ' bound unit')


class Ledger:
    """The catalogue, its case lists and exclusions, expanded to cases."""

    def __init__(self, root, workspace):
        self.root = root
        self.workspace = workspace
        self.catalog = load_json(root / 'spec/dod.json')
        policy = self.catalog['coverage_policy']
        self.m1_domains = policy['m1_domains']
        domains = {d['id']: d for d in self.catalog['domains']}
        milestones = {m['id']: m for m in self.catalog['integration_milestones']}
        needed, pending = [], [m for d in self.m1_domains for m in domains[d]['required_milestones']]
        while pending:
            ident = pending.pop()
            if ident not in needed:
                needed.append(ident)
                pending.extend(milestones[ident]['prerequisites'])
        self.m1_milestones = [m for m in milestones if m in needed]
        self.families = []  # (domain row, family node)
        for ident in self.m1_domains:
            for requirement in domains[ident]['requirements']:
                if not requirement['mandatory']:
                    continue
                for case in requirement['cases']:
                    self.families.append((ident, requirement, case))
        for ident in self.m1_milestones:
            for case in milestones[ident]['cases']:
                self.families.append(('integration', milestones[ident], case))
        self.lists = {}
        self.cases = []  # dicts: id, domain, family, runner, criteria, bound, external
        self.external = []  # dicts: family, domain, list, runner, criteria
        for domain, node, case in self.families:
            criteria = case['criteria_ref']
            if case['case_list'] is None:
                self.cases.append(
                    dict(
                        id=case['id'],
                        domain=domain,
                        family=case['id'],
                        runner=case['runner_suite'],
                        criteria=criteria,
                        bound=case['parameters'].get('bound'),
                    )
                )
                continue
            listing = self.case_list(case)
            default_runner = listing['runner'] or case['runner_suite']
            default_criteria = listing['criteria'] or criteria
            if listing['external'] is not None:
                self.external.append(
                    dict(
                        family=case['id'],
                        domain=domain,
                        list=listing['external'],
                        runner=default_runner,
                        criteria=default_criteria,
                    )
                )
            for item in listing['cases']:
                if 'retired' in item:
                    continue
                self.cases.append(
                    dict(
                        id=case['id'] + '/' + item['id'],
                        domain=domain,
                        family=case['id'],
                        runner=item.get('runner') or default_runner,
                        criteria=item.get('criteria') or default_criteria,
                        bound=item.get('bound'),
                        derive=item.get('derive'),
                    )
                )
        self.exclusions = self.load_exclusions(policy['exclusions_ref'])

    def case_list(self, case):
        path = case['case_list']
        relative_path(path, case['id'] + ' case list')
        require(
            path.startswith('spec/cases/') and path.endswith('.json'), case['id'] + ' list path'
        )
        listing = load_json(self.root / path)
        require(
            set(listing)
            == {'schema', 'family', 'source', 'criteria', 'runner', 'external', 'cases'},
            path + ' fields',
        )
        require(listing['schema'] == LIST_SCHEMA, path + ' schema')
        require(listing['family'] == case['id'], path + ' family')
        text(listing['source'], path + ' source')
        if listing['criteria'] is not None:
            text(listing['criteria'], path + ' criteria')
        if listing['runner'] is not None:
            runner(listing['runner'], path)
        external = listing['external']
        if external is not None:
            require(
                set(external)
                == {
                    'source',
                    'member',
                    'select',
                    'count',
                    'sha256',
                    'source_sha256',
                    'builder',
                    'note',
                },
                path + ' external fields',
            )
            text(external['source'], path + ' external source')
            require(external['member'] is None or type(external['member']) is str, path)
            require(
                type(external['select']) is list
                and external['select']
                and all(type(x) is str and x for x in external['select']),
                path + ' external selection',
            )
            enumerated = external['count'] is not None
            require(
                enumerated == (external['sha256'] is not None)
                and enumerated == (external['source_sha256'] is not None),
                path + ' pins',
            )
            if enumerated:
                require(type(external['count']) is int and external['count'] > 0, path)
                for key in ('sha256', 'source_sha256'):
                    require(re.fullmatch(r'[0-9a-f]{64}', external[key]), path + ' ' + key)
            text(external['builder'], path + ' external builder')
            text(external['note'], path + ' external note')
        require(type(listing['cases']) is list, path + ' cases')
        require(listing['cases'] or external is not None, path + ' empty list')
        seen = set()
        for item in listing['cases']:
            require(type(item) is dict and 'id' in item, path + ' case')
            require(
                set(item) <= {'id', 'criteria', 'runner', 'bound', 'parameters', 'derive', 'retired'},
                path + ' keys',
            )
            ident = item['id']
            require(
                type(ident) is str and LOCAL_ID.fullmatch(ident) and ident == ident.strip(),
                path + ' case ID ' + repr(ident),
            )
            require(ident not in seen, path + ' duplicate case ' + ident)
            seen.add(ident)
            if 'criteria' in item:
                text(item['criteria'], path + ' ' + ident + ' criteria')
            if 'runner' in item:
                runner(item['runner'], path + ' ' + ident)
            if 'bound' in item:
                bound(item['bound'], path + ' ' + ident)
            if 'parameters' in item:
                require(type(item['parameters']) is dict, path + ' ' + ident + ' parameters')
            if 'retired' in item:
                # A listed case is retired with its reason, never deleted.
                text(item['retired'], path + ' ' + ident + ' retired reason')
            if 'derive' in item:
                derive = item['derive']
                require(
                    type(derive) is dict
                    and set(derive) == {'family', 'match'}
                    and type(derive['match']) is list
                    and all(type(x) is str and x for x in derive['match']),
                    path + ' ' + ident + ' derive',
                )
        self.lists[path] = listing
        return listing

    def load_exclusions(self, path):
        relative_path(path, 'exclusions')
        data = load_json(self.root / path)
        require(set(data) == {'schema', 'exclusions'}, 'exclusions fields')
        require(data['schema'] == EXCLUSION_SCHEMA, 'exclusions schema')
        result = []
        for entry in data['exclusions']:
            require(
                set(entry)
                == {'match', 'pattern', 'reason', 'evidence', 'accepted_by', 'accepted_on'},
                'exclusion fields',
            )
            require(entry['match'] in ('exact', 'prefix'), 'exclusion match')
            for key in ('pattern', 'reason', 'evidence'):
                text(entry[key], 'exclusion ' + key)
            # Only a maintainer accepts an exclusion.
            require(entry['accepted_by'] == 'maintainer', 'exclusion must be accepted by a maintainer')
            require(
                re.fullmatch(r'\d{4}-\d{2}-\d{2}', entry['accepted_on'] or ''), 'exclusion date'
            )
            result.append(entry)
        return result

    def excluded(self, ident):
        for entry in self.exclusions:
            pattern = entry['pattern']
            if ident == pattern if entry['match'] == 'exact' else ident.startswith(pattern):
                return entry
        return None


def check(ledger):
    """The make-check rule: no case in scope without a runner or criteria."""
    root = ledger.root
    for domain, node, case in ledger.families:
        require(case['runner_suite'] is not None, 'case without a runner: ' + case['id'])
        runner(case['runner_suite'], case['id'])
    for case in ledger.cases:
        require(case['runner'] is not None, 'case without a runner: ' + case['id'])
        require(case['criteria'], 'case without criteria: ' + case['id'])
    for entry in ledger.external:
        require(entry['runner'] is not None, 'list without a runner: ' + entry['family'])
    external = {e['family'] for e in ledger.external}
    for case in ledger.cases:
        if case.get('derive'):
            require(case['derive']['family'] in external, 'derived from no list: ' + case['id'])
    ids = [c['id'] for c in ledger.cases]
    require(len(ids) == len(set(ids)), 'duplicate case IDs')
    listed = {p.relative_to(root).as_posix() for p in (root / 'spec/cases').glob('*.json')}
    referenced = {
        c['case_list']
        for d in ledger.catalog['domains']
        for r in d['requirements']
        for c in r['cases']
        if c['case_list']
    }
    referenced |= {
        c['case_list'] for m in ledger.catalog['integration_milestones'] for c in m['cases']
    } - {None}
    require(not listed - referenced, 'unreferenced case lists: ' + ', '.join(sorted(listed - referenced)))
    known = set(ids) | {e['family'] for e in ledger.external}
    for entry in ledger.exclusions:
        hit = (
            entry['pattern'] in known
            if entry['match'] == 'exact'
            else any(i.startswith(entry['pattern']) for i in known)
        )
        require(hit, 'exclusion names no case: ' + entry['pattern'])
    return dict(
        status='PASS_LEDGER',
        families=len(ledger.families),
        cases=len(ledger.cases),
        external_lists=len(ledger.external),
        exclusions=len(ledger.exclusions),
    )


def source_names(ledger, spec, cache):
    """Every name of an external list's source, or None when it is not readable here."""
    if ledger.workspace is None:
        return None
    source = ledger.workspace / spec['source']
    key = (str(source), spec['member'])
    if key not in cache:
        names = None
        if source.is_file() and spec['member'] and tarfile.is_tarfile(source):
            with tarfile.open(source) as archive:
                try:
                    raw = archive.extractfile(spec['member']).read().decode('utf-8')
                except (KeyError, AttributeError):
                    raw = None
            names = None if raw is None else [x for x in raw.splitlines() if x.strip()]
        elif source.is_file() and spec['member'] is None and source.suffix == '.txt':
            names = [x for x in source.read_text(encoding='utf-8').splitlines() if x.strip()]
        cache[key] = names
    return cache[key]


def external_names(ledger, entry, cache):
    """The selected member names of an external list, or None when unavailable."""
    spec = entry['list']
    names = source_names(ledger, spec, cache)
    if names is None:
        return None
    siblings = [
        e
        for e in ledger.external
        if (e['list']['source'], e['list']['member']) == (spec['source'], spec['member'])
    ]
    claimed = [p for e in siblings if e is not entry for p in e['list']['select'] if p != '*']

    def selected(name, patterns):
        return any(fnmatch.fnmatchcase(name, p) for p in patterns)

    if spec['select'] == ['*']:
        return sorted(n for n in names if not selected(n, claimed))
    return sorted(n for n in names if selected(n, spec['select']))


def case_ids(ledger, source, member, names):
    """The ledger case ID of each test name of an external list (for runner adapters)."""
    entries = [
        e
        for e in ledger.external
        if e['list']['source'] == source and e['list']['member'] == member
    ]
    require(entries, f'no external list has source {source} and member {member}')
    full = source_names(ledger, entries[0]['list'], {})
    known = None if full is None else set(full)
    result = []
    for name in names:
        if known is not None and name not in known:
            result.append(None)
            continue
        hits = [
            e
            for e in entries
            if e['list']['select'] != ['*']
            and any(fnmatch.fnmatchcase(name, p) for p in e['list']['select'])
        ]
        rest = [e for e in entries if e['list']['select'] == ['*']]
        family = (hits or rest or [None])[0]
        result.append(family['family'] + '/' + name if family else None)
    return result


def list_sha256(names):
    return hashlib.sha256(''.join(n + '\n' for n in sorted(names)).encode()).hexdigest()


class Git:
    def __init__(self, root):
        self.root = root
        self.cache = {}

    def identity(self, commit, paths):
        key = (commit, paths)
        if key not in self.cache:
            out = subprocess.run(
                ['git', '-C', str(self.root), 'ls-tree', commit, '--', *paths],
                check=False,
                capture_output=True,
                text=True,
            )
            self.cache[key] = tuple(out.stdout.split('\n')) if out.returncode == 0 else None
        return self.cache[key]

    def head(self, rev):
        out = subprocess.run(
            ['git', '-C', str(self.root), 'rev-parse', '--verify', rev + '^{commit}'],
            check=True,
            capture_output=True,
            text=True,
        )
        return out.stdout.strip()


def result_files(workspace, extra):
    files = []
    if workspace is not None:
        runs = workspace / 'runs'
        for pattern in ('*/*/cases.jsonl', 'keep/*/cases.jsonl', 'keep/*/*/cases.jsonl'):
            files.extend(sorted(runs.glob(pattern)))
    for path in extra:
        path = Path(path)
        if path.is_dir():
            files.extend(sorted(path.glob('**/cases.jsonl')))
        else:
            files.append(path)
    return list(dict.fromkeys(files))


def read_results(files, problems):
    results = {}
    for path in files:
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if not line.strip():
                continue
            where = f'{path}:{number}'
            try:
                row = json.loads(line, parse_constant=finite_only)
                require(type(row) is dict and row.get('schema') == RESULT_SCHEMA, 'schema')
                require(type(row.get('case')) is str and row['case'], 'case')
                require(re.fullmatch(r'[0-9a-f]{40}', row.get('commit', '')), 'commit')
                require(row.get('tree') in ('clean', 'dirty'), 'tree')
                require(row.get('class') in CLASSES, 'class')
                require(row.get('verdict') in VERDICTS, 'verdict')
                text(row.get('run', ''), 'run')
                if 'value' in row:
                    value = row['value']
                    require(type(value) in (int, float) and math.isfinite(value), 'value')
                if 'list_sha256' in row:
                    require(re.fullmatch(r'[0-9a-f]{64}', row['list_sha256']), 'list_sha256')
            except (ValueError, json.JSONDecodeError) as error:
                problems.append(f'{where}: {error}')
                continue
            results.setdefault(row['case'], []).append(row)
    return results


def runner_available(ledger, spec, cache={}):
    """The entrypoint exists; a suite runner (workspace, [SUITE, STEP]) also
    lists the step in `<entrypoint>.steps.json` ({"SUITE": ["STEP", ...]})."""
    base = ledger.root if spec['repository'] == 'driver' else ledger.workspace
    if base is None or not (base / spec['entrypoint']).is_file():
        return False
    if spec['repository'] != 'workspace' or len(spec['arguments']) != 2:
        return True
    manifest = base / (spec['entrypoint'] + '.steps.json')
    if manifest not in cache:
        cache[manifest] = load_json(manifest) if manifest.is_file() else {}
    suite, step = spec['arguments']
    return step in cache[manifest].get(suite, [])


def judge(case, rows, git, head_ids, pin=None):
    """pass, a failing verdict, or None when no result counts at this commit.

    A listed suite's result counts only from the pinned list (`list_sha256`)."""
    paths = PRODUCT_PATHS + (TEST_PATHS if case['runner']['backend'] != 'HARDWARE' else ())
    want = head_ids[paths]
    counted = [
        r
        for r in rows
        if r['tree'] == 'clean'
        and (pin is None or r.get('list_sha256') == pin)
        and git.identity(r['commit'], paths) == want
    ]
    if not counted:
        return None
    verdicts = []
    for row in counted:
        verdict = row['verdict']
        limit = case.get('bound')
        if verdict == 'pass' and limit is not None:
            value = row.get('value')
            if value is None or not math.isfinite(value) or row.get('unit') != limit['unit']:
                verdict = 'fail'
            elif 'min' in limit and value < limit['min'] or 'max' in limit and value > limit['max']:
                verdict = 'fail'
        verdicts.append(verdict)
    # One pass among failures is a flaky case, not a pass.
    failing = [v for v in verdicts if v != 'pass']
    return failing[0] if failing else 'pass'


def report(ledger, files, commit, repository):
    git = Git(repository)
    head = git.head(commit)
    head_ids = {
        p: git.identity(head, p) for p in (PRODUCT_PATHS, PRODUCT_PATHS + TEST_PATHS)
    }
    problems = []
    results = read_results(files, problems)
    rows = {}
    order = ledger.m1_domains + ['integration']

    def row(domain):
        return rows.setdefault(
            domain,
            dict(
                cases=0,
                with_runner=0,
                run=0,
                passed=0,
                failed=0,
                excluded=0,
                not_enumerated=0,
            ),
        )

    missing_runners = {}
    failures = []

    def missing(spec, family, count):
        key = ' '.join([spec['builder'], '|', spec['entrypoint'], *spec['arguments']])
        entry = missing_runners.setdefault(key, dict(cases=0, families=[]))
        entry['cases'] += count
        if family not in entry['families']:
            entry['families'].append(family)

    cache = {}
    verdicts, listed = {}, {}  # per external family: name -> verdict; its names
    for entry in ledger.external:
        spec = entry['list']
        counts = row(entry['domain'])
        family = entry['family']
        verdicts[family] = {}
        if spec['count'] is None:
            # An unenumerated list is one open case: the domain cannot complete.
            counts['cases'] += 1
            counts['not_enumerated'] += 1
            missing(
                dict(builder=spec['builder'], entrypoint='list', arguments=[spec['source']]),
                family,
                1,
            )
            continue
        names = external_names(ledger, entry, cache)
        full = source_names(ledger, spec, cache)
        if full is not None and list_sha256(full) != spec['source_sha256']:
            problems.append(f'{family}: source list {spec["source"]} differs from its pin')
        elif names is not None and (
            len(names) != spec['count'] or list_sha256(names) != spec['sha256']
        ):
            problems.append(f'{family}: selection differs from its pin')
        counts['cases'] += spec['count']
        prefix = family + '/'
        reported = {k[len(prefix) :]: v for k, v in results.items() if k.startswith(prefix)}
        if names is not None:
            stray = set(reported) - set(names)
            if stray:
                problems.append(f'{family}: {len(stray)} results name no listed case')
            reported = {k: v for k, v in reported.items() if k in set(names)}
        listed[family] = names
        excluded = sum(1 for n in (names or reported) if ledger.excluded(prefix + n))
        counts['excluded'] += excluded
        if not runner_available(ledger, entry['runner']):
            missing(entry['runner'], family, spec['count'] - excluded)
            continue
        counts['with_runner'] += spec['count'] - excluded
        case = dict(runner=entry['runner'], bound=None)
        passed_names = []
        for name, found in reported.items():
            if ledger.excluded(prefix + name):
                continue
            verdict = judge(case, found, git, head_ids, spec['source_sha256'])
            verdicts[family][name] = verdict
            if verdict is None:
                continue
            counts['run'] += 1
            if verdict == 'pass':
                counts['passed'] += 1
                passed_names.append(name)
            else:
                counts['failed'] += 1
                failures.append((prefix + name, verdict))
        if names is None and len(passed_names) == spec['count']:
            if list_sha256(passed_names) != spec['sha256']:
                problems.append(f'{family}: passed names differ from the pinned list')

    def derived(spec):
        """A view on a listed family: pass when every matching test passed."""
        found = verdicts.get(spec['family'], {})
        names = listed.get(spec['family']) or list(found)
        matched = [
            n
            for n in names
            if any(fnmatch.fnmatchcase(n, p) for p in spec['match'])
            and not ledger.excluded(spec['family'] + '/' + n)
        ]
        seen = [found.get(n) for n in matched]
        if not matched:
            # No test targets it: a failure once the family has run, never a pass.
            return 'fail' if any(v is not None for v in found.values()) else None
        failing = [v for v in seen if v not in (None, 'pass')]
        if failing:
            return failing[0]
        return 'pass' if all(v == 'pass' for v in seen) else None

    known = set()
    for case in ledger.cases:
        known.add(case['id'])
        counts = row(case['domain'])
        counts['cases'] += 1
        if ledger.excluded(case['id']):
            counts['excluded'] += 1
            continue
        if not runner_available(ledger, case['runner']):
            missing(case['runner'], case['family'], 1)
            continue
        counts['with_runner'] += 1
        if case.get('derive'):
            verdict = derived(case['derive'])
        else:
            verdict = judge(case, results.get(case['id'], []), git, head_ids)
        if verdict is None:
            continue
        counts['run'] += 1
        if verdict == 'pass':
            counts['passed'] += 1
        else:
            counts['failed'] += 1
            failures.append((case['id'], verdict))
    stray = [
        k
        for k in results
        if k not in known and not any(k.startswith(e['family'] + '/') for e in ledger.external)
    ]
    if stray:
        problems.append(f'{len(stray)} results name no ledger case (first: {sorted(stray)[0]})')
    total = dict(cases=0, with_runner=0, run=0, passed=0, failed=0, excluded=0, not_enumerated=0)
    for domain in order:
        for key, value in row(domain).items():
            total[key] += value
    complete = (
        total['not_enumerated'] == 0
        and total['passed'] + total['excluded'] == total['cases']
        and not problems
    )
    return dict(
        commit=head,
        result_files=len(files),
        domains={d: rows[d] for d in order},
        m1=total,
        m1_complete=complete,
        missing_runners=dict(sorted(missing_runners.items())),
        failures=failures,
        problems=problems,
    )


def print_report(data, show_missing):
    head = ('domain', 'cases', 'runner', 'run', 'passed', 'failed', 'excluded', 'unlisted')
    keys = ('cases', 'with_runner', 'run', 'passed', 'failed', 'excluded', 'not_enumerated')
    print(f'coverage at driver {data["commit"][:12]} ({data["result_files"]} result files)')
    print('{:<14}'.format(head[0]) + ''.join(f'{h:>10}' for h in head[1:]))
    for domain, counts in list(data['domains'].items()) + [('M1 total', data['m1'])]:
        print(f'{domain:<14}' + ''.join(f'{counts[k]:>10}' for k in keys))
    m1 = data['m1']
    share = 100.0 * m1['passed'] / m1['cases'] if m1['cases'] else 0.0
    state = 'COMPLETE' if data['m1_complete'] else 'NOT COMPLETE'
    print(f'M1 {state}: {m1["passed"]} of {m1["cases"]} cases passed ({share:.2f} %), ', end='')
    print(f'{m1["excluded"]} excluded (shown apart, never passes), ', end='')
    print(f'{m1["not_enumerated"]} lists not enumerated')
    for problem in data['problems']:
        print('problem: ' + problem)
    if show_missing:
        print('cases without a runner yet (builder | runner: cases, families):')
        for key, entry in data['missing_runners'].items():
            print(f'  {key}: {entry["cases"]}, ' + ' '.join(entry['families']))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('check', 'report', 'ids'), nargs='?', default='report')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--workspace', type=Path, help='default: the matching prepared root next to the checkout')
    parser.add_argument('--results', action='append', default=[], help='cases.jsonl or a directory')
    parser.add_argument('--commit', default='HEAD')
    parser.add_argument('--repo', type=Path, help='git repository of the commits (default: root)')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--missing-runners', action='store_true')
    parser.add_argument('--source', help='ids: an external list source; test names on stdin')
    parser.add_argument('--member', help='ids: the member of an archive source')
    args = parser.parse_args(argv)
    root = args.root.resolve()
    workspace = args.workspace
    if workspace is None and (root.parent / 'eng/lab').is_dir():
        workspace = root.parent
    try:
        ledger = Ledger(root, workspace.resolve() if workspace else None)
        if args.command == 'ids':
            names = [line.strip() for line in sys.stdin if line.strip()]
            for name, ident in zip(names, case_ids(ledger, args.source, args.member, names)):
                print(ident or 'NONE ' + name)
            return 0
        if args.command == 'check':
            data = check(ledger)
        else:
            check(ledger)
            files = result_files(ledger.workspace, args.results)
            data = report(ledger, files, args.commit, (args.repo or root).resolve())
    except (ValueError, OSError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(json.dumps(dict(status='INVALID', error=str(error))))
        return 1
    if args.json or args.command == 'check':
        print(json.dumps(data, indent=2))
    else:
        print_report(data, args.missing_runners)
    return 0


if __name__ == '__main__':
    sys.exit(main())
