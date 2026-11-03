#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Developer-only formatting and Python lint; explicit paths or staged sources."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VERSIONS = {'RUFF': ('ruff', '0.11.13'), 'CLANG_FORMAT': ('clang-format', '23.1.1')}
C_SUFFIXES = frozenset(('.c', '.h', '.inc'))
CPP_SUFFIXES = frozenset(
    ('.cc', '.cpp', '.cxx', '.c++', '.C', '.hh', '.hpp', '.hxx', '.h++', '.H', '.ipp', '.tpp')
)
C_FAMILY_SUFFIXES = C_SUFFIXES | CPP_SUFFIXES


def selected_paths(names):
    if not names:
        raw = subprocess.check_output(
            ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z'], cwd=ROOT
        )
        names = [os.fsdecode(name) for name in raw.split(b'\0') if name]
    paths = []
    for name in names:
        path = Path(name)
        path = path if path.is_absolute() else ROOT / path
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(ROOT) or resolved != path or not path.is_file():
            raise ValueError('Select an ordinary file inside the checkout: ' + name)
        if path.suffix == '.py' or path.suffix in C_FAMILY_SUFFIXES:
            paths.append(path)
    return sorted(set(paths))


def tool(key):
    default, version = VERSIONS[key]
    name = os.environ.get(key, default)
    executable = shutil.which(name)
    if not executable:
        raise ValueError(f'{default} {version} is required; set {key} to its executable path')
    result = subprocess.run(
        [executable, '--version'], check=True, capture_output=True, text=True, timeout=10
    )
    if not re.search(r'(?<![\d.])' + re.escape(version) + r'(?![\d.])', result.stdout):
        raise ValueError(f'Expected {default} {version}; received {result.stdout.strip()!r}')
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('lint', 'format', 'format-check'))
    parser.add_argument('paths', nargs='*')
    args = parser.parse_args()
    paths = selected_paths(args.paths)
    python = [str(p) for p in paths if p.suffix == '.py']
    c = [p for p in paths if p.suffix in C_FAMILY_SUFFIXES]
    if not paths:
        print('No selected source files. Pass FILES="path ..." or stage the intended patch.')
        return
    commands = []
    if python:
        ruff = tool('RUFF')
        if args.action == 'lint':
            commands.append([ruff, 'check', '--no-fix', '--no-cache', '--', *python])
        else:
            commands.append(
                [
                    ruff,
                    'format',
                    '--no-cache',
                    *(['--check'] if args.action == 'format-check' else []),
                    '--',
                    *python,
                ]
            )
    if c and args.action != 'lint':
        formatter = tool('CLANG_FORMAT')
        for path in c:
            commands.append(
                [
                    formatter,
                    '--assume-filename='
                    + str(path if path.suffix in CPP_SUFFIXES else path.with_suffix('.c')),
                    *(['-i'] if args.action == 'format' else ['--dry-run', '--Werror']),
                    str(path),
                ]
            )
    for command in commands:
        subprocess.run(command, cwd=ROOT, check=True)
    if c and args.action == 'lint':
        print(
            'C/C++ diagnostics belong to the selected strict build/tests; no C/C++ source was rewritten.'
        )


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1) from error
