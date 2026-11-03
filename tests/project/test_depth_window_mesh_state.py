#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Check complete version2 depth-state packets without a device or Mesa SDK."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
UNITS = (
    'tests/unit/test_sgx535_depth_window_mesh_state.c',
    'src/raster/sgx535_depth_state.c',
    'src/raster/sgx535_depth_clear.c',
    'src/raster/sgx535_depth_window_mesh.c',
    'src/raster/sgx535_window_mesh.c',
    'src/raster/sgx535_task_packet.c',
    'src/raster/sgx535_window_vertex.c',
    'src/raster/sgx535_task_launch.c',
    'src/raster/sgx535_normalized_color0_mask_list.c',
    'src/raster/sgx535_clip_w1.c',
    'src/raster/sgx535_engine121.c',
    'src/raster/sgx535_task_image.c',
    'src/raster/sgx535_pbe_state.c',
    'src/raster/sgx535_pixel_event.c',
    'src/raster/sgx535_heap_layout.c',
    'src/compiler/sgx535_vertex_dma.c',
    'src/compiler/sgx535_vertex_pds.c',
    'src/compiler/sgx535_vertex_use.c',
    'src/compiler/sgx535_pds_use_link.c',
    'src/compiler/sgx535_use_code_bank.c',
    'src/raster/sgx535_varying_packet.c',
    'src/raster/sgx535_compiled_color0_packet.c',
    'src/compiler/bitwise.c',
    'src/compiler/fragment.c',
    'src/compiler/fragment_normalized.c',
    'src/raster/sgx535_normalized_color0_task.c',
    'src/compiler/usse.c',
    'src/compiler/iterator.c',
    'src/compiler/pack.c',
    'src/compiler/pds_fragment.c',
    'src/compiler/texture2d.c',
    'src/raster/sgx535_background_object.c',
    'src/raster/sgx535_background_pair.c',
)


def execute(output, target=False):
    output.mkdir(parents=True, exist_ok=False)
    paths = [ROOT / name for name in UNITS]
    paths += sorted((ROOT / 'src/raster').glob('*.h')) + sorted((ROOT / 'src/raster').glob('*.inc'))
    paths += sorted((ROOT / 'src/compiler').glob('*.h'))
    paths += [
        ROOT / 'tests/unit' / name
        for name in (
            'test_sgx535_depth_window_mesh.c',
            'test_sgx535_window_mesh.c',
            'normalized_mask_list_reference.h',
        )
    ]

    def identities():
        return [
            dict(path=str(p.relative_to(ROOT)), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
            for p in paths
        ]

    before = identities()
    (output / 'source-before.json').write_text(json.dumps(before, indent=2) + '\n')
    common = [
        os.environ.get('CC', 'cc'),
        '-std=c11',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wconversion',
        '-Wshadow',
        '-Wstrict-prototypes',
        '-Wmissing-prototypes',
        '-O1',
        '-g',
        '-fno-builtin-memcpy',
        '-Wl,--wrap=calloc',
        '-I' + str(ROOT / 'src/compiler'),
        '-I' + str(ROOT / 'src/raster'),
        '-I' + str(ROOT / 'tests/unit'),
    ]
    if target:
        common += ['-m32', '-march=i686', '-mtune=atom']
    rows = []
    for flavor, extra in (
        ('strict', []),
        ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
    ):
        binary = output / flavor
        artifacts = output / (flavor + '-cases')
        artifacts.mkdir()
        for name, argv in (
            (
                flavor + '-compile',
                common + extra + [str(ROOT / p) for p in UNITS] + ['-o', str(binary)],
            ),
            (flavor + '-run', [str(binary), str(artifacts)]),
        ):
            with (
                (output / (name + '.stdout')).open('xb') as stdout,
                (output / (name + '.stderr')).open('xb') as stderr,
            ):
                child = subprocess.Popen(
                    argv,
                    cwd=ROOT,
                    stdout=stdout,
                    stderr=stderr,
                    env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'),
                )
                rc = child.wait(timeout=120)
            row = dict(name=name, pid=child.pid, returncode=rc, argv=argv)
            rows.append(row)
            (output / (name + '.json')).write_text(json.dumps(row, indent=2) + '\n')
            if rc:
                return rc
    after = identities()
    (output / 'source-after.json').write_text(json.dumps(after, indent=2) + '\n')
    if before != after:
        raise ValueError('selected packet inputs changed')
    a = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (output / 'strict-cases').iterdir()
    }
    b = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (output / 'ubsan-cases').iterdir()
    }
    if a != b or len(a) != 32:
        raise ValueError('complete strict/UBSAN state packet bytes differ')
    summary = dict(
        state='PASS_COMPLETE_DEPTH_STATE_PACKETS_CPU_ONLY',
        target_i386=target,
        children=rows,
        packet_records=a,
        source_unchanged=True,
        hardware_qualified=False,
        native_admission=False,
    )
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)
    return 0


class DepthState(unittest.TestCase):
    def test_complete_checked_state_packets(self):
        with tempfile.TemporaryDirectory(prefix='poulsback-depth-state-') as temporary:
            self.assertEqual(execute(Path(temporary) / 'output'), 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--target-i386', action='store_true')
    args = parser.parse_args()
    if args.output:
        raise SystemExit(execute(args.output.resolve(), args.target_i386))
    unittest.main(argv=[__file__])
