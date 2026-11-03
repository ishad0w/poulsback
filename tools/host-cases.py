#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Write the ledger results of one host build-userspace run.

Reads the report.json of `tools/build-userspace.py` and prints one
poulsback-case-result-v1 line per ledger case whose runner is this tool
(`component/<name>` in a spec/cases list). A component passes when every one
of its test and oracle commands exited 0 in every selected flavor. The run is
`clean` only when every input the report hashed equals its blob at the commit,
so a shared checkout with other edits never yields a result for the commit.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ENTRYPOINT = 'tools/host-cases.py'
REPOSITORY = [ROOT]  # --repo: the git repository when ROOT is an exported tree


def git(*args, binary=False):
    out = subprocess.run(
        ['git', '-C', str(REPOSITORY[0]), *args], check=True, capture_output=True
    )
    return out.stdout if binary else out.stdout.decode().strip()


def clean(report, commit):
    for item in report['inputs']:
        try:
            blob = git('cat-file', 'blob', f'{commit}:{item["path"]}', binary=True)
        except subprocess.CalledProcessError:
            return False
        if hashlib.sha256(blob).hexdigest() != item['sha256']:
            return False
    return True


def cases():
    """The ledger case ID of every component this runner reports."""
    catalog = json.loads((ROOT / 'spec/dod.json').read_text(encoding='utf-8'))
    families = [c for d in catalog['domains'] for r in d['requirements'] for c in r['cases']]
    families += [c for m in catalog['integration_milestones'] for c in m['cases']]
    result = {}
    for family in families:
        if not family['case_list']:
            continue
        listing = json.loads((ROOT / family['case_list']).read_text(encoding='utf-8'))
        default = listing['runner'] or family['runner_suite'] or {}
        for item in listing['cases']:
            entry = (item.get('runner') or default).get('entrypoint')
            if entry == ENTRYPOINT and item['id'].startswith('component/'):
                result[item['id'][len('component/') :]] = family['id'] + '/' + item['id']
    return result


def verdicts(report):
    flavors = report['selected_flavors']
    seen = {}
    for command in report['commands']:
        if command['phase'] not in ('test', 'oracle'):
            continue
        label = Path(command['stdout']).name.split('.')[0]
        ok = command['exit_code'] == 0 and not command['timed_out']
        seen.setdefault(label, []).append(ok)
    result = {}
    for component in report['selected_components']:
        runs = [seen.get(f'{component}-{f}-test') for f in flavors]
        oracle = [v for k, v in seen.items() if k.startswith(component + '-') and 'oracle' in k]
        if any(r is None for r in runs):
            result[component] = 'fail'
        else:
            result[component] = 'pass' if all(all(r) for r in runs + oracle) else 'fail'
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--commit', default='HEAD')
    parser.add_argument('--out', type=Path, help='default: stdout')
    parser.add_argument('--repo', type=Path, help='git repository (default: this tree)')
    args = parser.parse_args(argv)
    if args.repo:
        REPOSITORY[0] = args.repo.resolve()
    report = json.loads(args.report.read_text(encoding='utf-8'))
    commit = git('rev-parse', '--verify', args.commit + '^{commit}')
    tree = 'clean' if clean(report, commit) else 'dirty'
    ids = cases()
    lines = []
    for component, verdict in sorted(verdicts(report).items()):
        if component not in ids:
            continue
        lines.append(
            json.dumps(
                dict(
                    schema='poulsback-case-result-v1',
                    case=ids[component],
                    commit=commit,
                    tree=tree,
                    run=str(args.report.parent),
                    verdict=verdict,
                    **{'class': 'HOST'},
                ),
                sort_keys=True,
            )
        )
    text = ''.join(line + '\n' for line in lines)
    if args.out:
        args.out.write_text(text, encoding='utf-8')
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
