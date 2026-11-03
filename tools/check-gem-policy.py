#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Execute the exact compiled Linux GEM entrypoint bodies with CPU policy mocks."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def identity(path):
    raw = path.read_bytes()
    return {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def extract(raw, name):
    # Mask C comments/strings, preserving offsets and newlines for braces.
    text = raw.decode("utf-8")
    masked = re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: ''.join('\n' if x == '\n' else ' ' for x in m[0]),
        text,
        flags=re.S,
    )
    pattern = r'^(?:static\s+)?(?:int|void)\s+' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{'
    matches = list(re.finditer(pattern, masked, re.M))
    if len(matches) != 1:
        raise ValueError("exact production function not unique: " + name)
    begin = matches[0].start()
    index = matches[0].end() - 1
    depth = 0
    for end in range(index, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if depth == 0:
            return text[begin : end + 1].encode() + b'\n'
    raise ValueError("unclosed production function: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if platform.machine() != 'i686':
        parser.error("native i386 VM required")
    build = args.kernel_build.resolve(strict=True)
    output = args.output.absolute()
    if (
        output.exists()
        or output.parent.resolve() != output.parent
        or output.is_relative_to(ROOT)
        or output.is_relative_to(build)
    ):
        parser.error("new output beneath a canonical parent outside source/build required")
    report = json.loads((build / 'report.json').read_text())
    if (
        report['status'] != 'PASS_COMPILE_ONLY_REQUALIFICATION_REQUIRED'
        or 'physical/poulsback.ko' not in report['modules']
    ):
        raise ValueError("fresh successful physical compilation required; never load module")
    stage = build / 'physical'
    for name in ('gem.c', 'gem.h', 'poulsback_gem_hold.inc'):
        if identity(stage / name) != report['staged_sources'][name]:
            raise ValueError("compiled stage differs: " + name)
    if identity(ROOT / 'kernel/integration/poulsback_gem_hold.inc') != identity(
        stage / 'poulsback_gem_hold.inc'
    ):
        raise ValueError("current HOLD policy differs from compiled stage")
    if identity(ROOT / 'kernel/integration/linux.patch') != report['integration_patch']:
        raise ValueError("current integration patch differs from compiled stage")
    output.mkdir()
    raw = (stage / 'gem.c').read_bytes()
    extracted = b'/* Exact compiled-stage production bodies; CPU mocks only. */\n'
    for name in ('psb_gem_pin', 'psb_gem_unpin', 'psb_gem_free_object'):
        extracted += extract(raw, name)
    (output / 'gem-entrypoints.inc').write_bytes(extracted)
    (output / 'poulsback_gem_hold.inc').write_bytes((stage / 'poulsback_gem_hold.inc').read_bytes())
    (output / 'drm').mkdir()
    (output / 'drm/drm_drv.h').write_text('/* Explicit CPU policy mocks supply the types. */\n')
    inputs = {
        str(path): identity(path)
        for path in (
            Path(__file__),
            ROOT / 'tests/kernel/test_legacy_gem.c',
            ROOT / 'kernel/integration/linux.patch',
            ROOT / 'kernel/integration/poulsback_gem_hold.inc',
            build / 'report.json',
            stage / 'gem.c',
            stage / 'gem.h',
            stage / 'poulsback_gem_hold.inc',
        )
    }
    result = {
        'schema': 'poulsback-legacy-gem-policy-v1',
        'state': 'RUNNING',
        'inputs': inputs,
        'compiled_functions_sha256': hashlib.sha256(extracted).hexdigest(),
        'commands': [],
        'hardware_accessed': False,
        'kernel_module_loaded': False,
        'scope': 'EXACT_COMPILED_PRODUCTION_BODIES_CPU_MOCK_REF_CACHE_LIFETIME_ONLY',
    }
    try:
        for flavor, extra in (
            ('strict', []),
            ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
        ):
            binary = output / flavor
            commands = [
                (
                    'compile-' + flavor,
                    [
                        'gcc',
                        '-std=gnu11',
                        '-O2',
                        '-march=i686',
                        '-mtune=generic',
                        '-Wall',
                        '-Wextra',
                        '-Werror',
                        *extra,
                        '-I' + str(output),
                        '-I' + str(ROOT / 'kernel/debug'),
                        str(ROOT / 'tests/kernel/test_legacy_gem.c'),
                        '-o',
                        str(binary),
                    ],
                ),
                ('run-' + flavor, [str(binary)]),
            ]
            for label, argv in commands:
                done = subprocess.run(argv, capture_output=True)
                (output / (label + '.stdout.original')).write_bytes(done.stdout)
                (output / (label + '.stderr.original')).write_bytes(done.stderr)
                result['commands'].append(
                    {'label': label, 'argv': argv, 'returncode': done.returncode}
                )
                if done.returncode:
                    raise RuntimeError(label + ': ' + done.stderr.decode(errors='replace'))
        for name, expected in inputs.items():
            if identity(Path(name)) != expected:
                raise ValueError('source/stage changed during policy checks: ' + name)
        result['state'] = 'PASS_COMPILED_GEM_POLICY_CPU_ONLY'
    except Exception as error:
        result['state'] = 'REFUSED'
        result['error'] = str(error)
        raise
    finally:
        (output / 'report.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    print(json.dumps({'state': result['state'], 'hardware_accessed': False}))


if __name__ == '__main__':
    main()
