#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Build local graphics sources or run one selected benchmark case."""

import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
OUTPUTS = ROOT.parent / 'build/benchmark'
GROUPS = ('2D', '3D', 'OpenGL')
DEFAULT_SIZE = '640x640'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def local_path(value):
    path = Path(value).expanduser().resolve()
    if path == ROOT or path.is_relative_to(ROOT):
        raise ValueError('Keep generated output outside the benchmark source directory')
    return path


def cases():
    result = {}
    for group in GROUPS:
        for case in json.loads((ROOT / group / 'cases.json').read_text()):
            name = group + '/' + case['id']
            if name in result or Path(case['binary']).name != case['binary']:
                raise ValueError('Invalid or duplicate case: ' + name)
            result[name] = dict(case, group=group)
    return result


def command(argv, folder, *, cwd=None, seconds=600, scheduled_stop=False):
    """Keep raw output and actual status, including spawn failures and interrupts.

    A driver-blocked process is recorded and left for its hardware controller.
    SIGINT is the only automatic stop request; no kill, reboot or retry follows.
    """
    folder.mkdir(parents=True, exist_ok=False)
    record = {
        'command': list(map(str, argv)),
        'cwd': str(cwd or ROOT),
        'seconds': seconds,
        'actual_returncode': None,
        'state': 'STARTING',
        'scope': 'APPLICATION_ONLY',
        'environment': {
            key: os.environ[key]
            for key in (
                'DISPLAY',
                'LD_LIBRARY_PATH',
                'LIBGL_DRIVERS_PATH',
                'LIBGL_ALWAYS_SOFTWARE',
                'MESA_LOADER_DRIVER_OVERRIDE',
                'MESA_GL_VERSION_OVERRIDE',
                'MESA_GLES_VERSION_OVERRIDE',
                'vblank_mode',
            )
            if key in os.environ
        },
    }
    report = folder / 'command.json'
    write_json(report, record)
    start = time.monotonic()
    child = None
    interrupted = False
    try:
        with (folder / 'stdout.txt').open('wb') as stdout:
            with (folder / 'stderr.txt').open('wb') as stderr:
                child = subprocess.Popen(
                    record['command'],
                    cwd=cwd or ROOT,
                    stdout=stdout,
                    stderr=stderr,
                    start_new_session=True,
                )
                record.update(pid=child.pid, state='RUNNING')
                write_json(report, record)
                try:
                    child.wait(timeout=seconds)
                    record['state'] = 'EXITED' if child.returncode == 0 else 'FAILED'
                except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                    interrupted = isinstance(error, KeyboardInterrupt)
                    record.update(
                        state='INTERRUPTED' if interrupted else 'TIMED_OUT', stop_request='SIGINT'
                    )
                    if scheduled_stop and not interrupted:
                        record['state'] = 'STOPPED'
                    try:
                        os.killpg(child.pid, signal.SIGINT)
                    except ProcessLookupError:
                        pass
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        record['state'] = 'STILL_RUNNING'
    except OSError as error:
        record.update(state='SPAWN_FAILED', error=str(error))
    finally:
        record.update(
            elapsed_s=time.monotonic() - start,
            actual_returncode=child.poll() if child else None,
        )
        if record['state'] == 'STOPPED' and record['actual_returncode'] not in (0, -signal.SIGINT):
            record['state'] = 'FAILED'
        write_json(report, record)
    if interrupted:
        raise KeyboardInterrupt
    return record


def checked(argv, folder, **kwargs):
    record = command(argv, folder, **kwargs)
    if record['state'] != 'EXITED':
        raise RuntimeError(f"{record['state']}: inspect {folder / 'command.json'}")


def run_case(args):
    case = cases()[args.case]
    bundle = Path(args.bundle).expanduser().resolve()
    relative = 'bin/' + case['group'] + '/' + case['binary']
    binary = bundle / relative
    entry = json.loads((bundle / 'manifest.json').read_text())['binaries'][relative]
    if binary.stat().st_size != entry['bytes'] or digest(binary) != entry['sha256']:
        raise ValueError('Binary differs from the build manifest')
    display = os.environ.get('DISPLAY')
    if not display:
        raise ValueError('Set DISPLAY to the already admitted X11 session')
    if args.size and case['window'] != '{size}':
        raise ValueError(f"{args.case} has no native size option; window={case['window']}")
    size = args.size or DEFAULT_SIZE
    argv = [
        str(binary),
        *[arg.replace('{display}', display).replace('{size}', size) for arg in case['args']],
    ]
    if case['binary'].startswith('glmark2'):
        manifest = json.loads((bundle / 'manifest.json').read_text())
        for name, expected in manifest['resources'].items():
            if digest(bundle / name) != expected:
                raise ValueError('Modified glmark2 data: ' + name)
        argv += ['--data-path', str(bundle / 'share/glmark2')]
    # Unchanged demos may use default SIGINT handling. Keep their line output
    # before termination rather than leaving FPS/info buffered until process exit.
    stdbuf = shutil.which('stdbuf')
    if not stdbuf:
        raise ValueError('GNU coreutils stdbuf is required to retain live application output')
    result = command(
        [stdbuf, '-oL', '-eL', *argv],
        local_path(args.results),
        seconds=args.seconds,
        scheduled_stop=case['kind'] == 'interactive',
    )
    result.update(
        case=args.case,
        binary_sha256=entry['sha256'],
        kind=case['kind'],
        window_setting=case['window'].replace('{size}', size),
        window_size_observed=None,
        runner_sha256=digest(Path(__file__)),
        catalog_sha256=digest(ROOT / case['group'] / 'cases.json'),
        bundle_manifest_sha256=digest(bundle / 'manifest.json'),
    )
    write_json(local_path(args.results) / 'command.json', result)
    print(json.dumps(result, indent=2))
    return 0 if result['state'] in ('EXITED', 'STOPPED') else 1


def positive_seconds(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('seconds must be finite and positive')
    return value


def window_size(text):
    if not re.fullmatch(r'[1-9][0-9]*x[1-9][0-9]*', text):
        raise argparse.ArgumentTypeError('size must be WIDTHxHEIGHT, for example 640x640')
    return text


def source_records():
    return {
        str(p.relative_to(ROOT)): digest(p)
        for group in GROUPS
        for p in sorted((ROOT / group).rglob('*'))
        if p.is_file()
    }


def bundle_notices():
    source = (
        ROOT / 'NOTICES.md' if (ROOT / 'manifest.json').exists() else ROOT.parent / 'NOTICES.md'
    )
    heading = '## Benchmark tools\n'
    _, found, section = source.read_text().partition(heading)
    if not found:
        raise ValueError('Root NOTICES.md is missing the benchmark tools section')
    section = (heading + section.split('\n## ', 1)[0]).replace('benchmark/', '')
    return '# Licensing and source notices\n\n' + section.rstrip() + '\n'


def copy_build_info(work, metadata):
    metadata.mkdir(parents=True)
    shutil.copy2(work / 'compile_commands.json', metadata / 'compile_commands.json')
    for name in ('intro-dependencies.json', 'intro-buildoptions.json'):
        shutil.copy2(work / 'meson-info' / name, metadata / name)
    compilers = work / 'meson-info/intro-compilers.json'
    if compilers.is_file():
        shutil.copy2(compilers, metadata / compilers.name)
    else:
        # Debian 12's Meson 1.0 records compiler detection in this log.
        shutil.copy2(work / 'meson-logs/meson-log.txt', metadata / 'meson-log.txt')


def build_meson(name, source, output, logs, jobs, options=()):
    work = OUTPUTS / '.work' / name
    setup = [
        'meson',
        'setup',
        str(work),
        str(source),
        '--buildtype=release',
        '--wrap-mode=nodownload',
        '--prefix=' + str(output),
        *options,
    ]
    if (work / 'build.ninja').exists():
        setup.insert(2, '--reconfigure')
    checked(setup, logs / (name + '-setup'))
    checked(['meson', 'compile', '-C', work, '-j', str(jobs), '-v'], logs / (name + '-build'))
    checked(['meson', 'install', '-C', work, '--no-rebuild'], logs / (name + '-install'))
    copy_build_info(work, output / 'build-info' / name)


def build(args):
    output = local_path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    logs = output / 'logs'
    logs.mkdir()
    sources = source_records()
    build_meson('native', ROOT, output, logs, args.jobs)
    build_meson(
        'glmark2-x11',
        ROOT / 'OpenGL/glmark2',
        output,
        logs,
        args.jobs,
        ('-Dflavors=x11-gl,x11-glesv2', '-Ddata-path=glmark2-data', '--bindir=bin/OpenGL'),
    )
    if source_records() != sources:
        raise ValueError('Sources changed during the build; this bundle is incomplete')
    for name in (*GROUPS, 'tools'):
        shutil.copytree(
            ROOT / name, output / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc')
        )
    shutil.copy2(ROOT / 'meson.build', output / 'meson.build')
    (output / 'README.md').write_text(
        (ROOT / 'README.md').read_text().replace('](../NOTICES.md#', '](NOTICES.md#')
    )
    (output / 'NOTICES.md').write_text(bundle_notices())
    # Standalone bundles carry the existing root MIT text, not a second source licence.
    licence = ROOT / 'LICENSE' if (ROOT / 'LICENSE').exists() else ROOT.parent / 'LICENSE'
    shutil.copy2(licence, output / 'LICENSE')
    binaries = {}
    for group in GROUPS:
        for path in sorted((output / 'bin' / group).iterdir()):
            if path.name == 'cases.json':
                continue
            binaries[str(path.relative_to(output))] = {
                'bytes': path.stat().st_size,
                'sha256': digest(path),
                'elf': subprocess.check_output(['readelf', '-h', path], text=True),
                'dynamic': subprocess.check_output(['readelf', '-d', path], text=True),
            }
    architectures = {
        tuple(
            entry['elf'].split(field)[1].splitlines()[0].strip() for field in ('Class:', 'Machine:')
        )
        for entry in binaries.values()
    }
    if len(architectures) != 1:
        raise ValueError('Mixed ELF architectures')
    write_json(
        output / 'manifest.json',
        {
            'scope': 'BUILD_ONLY',
            'sources': sources,
            'binaries': binaries,
            'builder_sha256': digest(Path(__file__)),
            'resources': {
                str(p.relative_to(output)): digest(p)
                for p in sorted((output / 'share').rglob('*'))
                if p.is_file()
            },
        },
    )
    with (output / 'SHA256SUMS').open('w') as sums:
        for path in sorted(output.rglob('*')):
            if path.is_file() and path.name != 'SHA256SUMS':
                sums.write(digest(path) + '  ' + str(path.relative_to(output)) + '\n')
    print(f'Built {len(binaries)} executables in {output}. No graphics test was run.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    builder = commands.add_parser('build', help='Compile local sources; no downloads or test runs')
    builder.add_argument('--jobs', type=int, choices=range(1, 9), default=2)
    builder.add_argument('--output', default=str(OUTPUTS / 'bundle'))
    listing = commands.add_parser('list', help='List commands without executing tests')
    listing.add_argument('--group', choices=GROUPS)
    runner = commands.add_parser('run', help='Run ONE case in an existing X11 session')
    runner.add_argument('case', choices=tuple(cases()))
    runner.add_argument(
        '--bundle', default=str(ROOT if (ROOT / 'manifest.json').exists() else OUTPUTS / 'bundle')
    )
    runner.add_argument('--results', required=True, help='A new directory for logs')
    runner.add_argument('--seconds', type=positive_seconds, default=30)
    runner.add_argument(
        '--size', type=window_size, help='Window size, default 640x640 where supported'
    )
    args = parser.parse_args()
    if args.action == 'list':
        for name, case in cases().items():
            if not args.group or args.group == case['group']:
                size = case['window'].replace('{size}', DEFAULT_SIZE)
                print(f"{name}: {case['kind']}: window={size}: {case['description']}")
        return 0
    if args.action == 'run':
        return run_case(args)
    OUTPUTS.mkdir(parents=True, exist_ok=True)
    with (OUTPUTS / '.build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        build(args)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1) from error
    except KeyboardInterrupt:
        raise SystemExit(130) from None
