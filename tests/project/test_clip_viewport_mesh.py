#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Portable complete CLIP15/viewport/BG7 constructor check; CPU only."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import time
from fractions import Fraction

UNITS = (
    'tests/unit/test_sgx535_clip_viewport_mesh.c',
    'src/raster/sgx535_clip_viewport_mesh.c',
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
    'src/raster/sgx535_window_mesh.c',
)


def identity(path):
    raw = path.read_bytes()
    return dict(path=str(path), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def command(out, name, argv):
    start = time.monotonic()
    with (
        (out / (name + '.stdout.original')).open('xb') as stdout,
        (out / (name + '.stderr.original')).open('xb') as stderr,
    ):
        child = subprocess.Popen(
            argv,
            stdout=stdout,
            stderr=stderr,
            env=dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'),
        )
        rc = child.wait()
    row = dict(
        name=name,
        argv=list(map(str, argv)),
        pid=child.pid,
        returncode=rc,
        elapsed_seconds=time.monotonic() - start,
    )
    (out / (name + '.child.original.json')).write_text(json.dumps(row, indent=2) + '\n')
    return row


def synthetic_header():
    xy = ((4, 4), (28, 4), (4, 28), (36.5, 36.5), (36.5, 60.5), (60.5, 60.5))

    def bits(v):
        return struct.unpack('<I', struct.pack('<f', v))[0]

    vertices, clip = [], []
    for i, (x, y) in enumerate(xy):
        rgba = (1, 0, 0, 1) if i < 3 else (0, 1, 0, 1)
        vertices += [bits(v) for v in (x, y, 0.5, 1, *rgba)]
        clip += [bits(v) for v in ((x - 32) / 32, (32 - y) / 32, 0, 1)]
    program = []
    # Enum fields are taken from the ordinary public compiler ABI.
    for i in range(4):
        program += [1, 0, 0, i, 0, 0, 0]
    for i in range(4):
        program += [8, 0, i, 0, i, 0, 0]
    spans = dict(
        clip=clip,
        vertices=vertices,
        indices=list(range(6)),
        program=program,
        window_conventions=[
            1,
            1,
            1,
            0xFFFFFFFF,
            1,
            0,
            0x42000000,
            0xC2000000,
            0x3F000000,
            0x42000000,
            0x42000000,
            0x3F000000,
        ],
    )
    text = '/* Synthetic CPU source only. */\n'
    for name, words in spans.items():
        text += 'static const uint32_t window_mesh_input_' + name + '[] = {'
        text += ','.join('0x%08xU' % word for word in words) + '};\n'
    return text.encode()


def independent_projection(cases, source_clip, source_vertices):
    """Check full raw image placement and coordinates, without a GPU result."""
    old = (cases / 'old-window6-pds.original.bin').read_bytes()
    actual = (cases / 'task15-pds.original.bin').read_bytes()
    assert len(old) == 768 and len(actual) == 1024
    vp = (0x42000000, 0x42000000, 0x42000000, 0xC2000000, 0x3F000000, 0x3F000000)
    expected = bytearray(1024)
    for start, size in ((0, 128), (128, 60), (192, 4), (208, 8), (224, 124), (352, 60), (416, 148)):
        expected[start : start + size] = old[start : start + size]
    struct.pack_into('<I', expected, 292, 0x81E0000F)
    fg = list(struct.unpack('<10I', old[576:616]))
    assert fg[0] == 0x54C1 and fg[8] == 0x10000 and fg[4] == 0x0C000000 and fg[7] == 0x08001800
    fg = fg[:7] + list(vp) + fg[7:]
    fg[0], fg[14] = 0x55C1, 0
    expected[576:640] = struct.pack('<16I', *fg)
    expected[640:684] = old[624:668]
    expected[688:704] = old[672:688]
    expected[704:772] = old[688:756]
    assert struct.unpack_from('<I', expected, 736)[0] == 0x0C02A203
    struct.pack_into('<I', expected, 736, 0x0C02A204)
    assert bytes(expected) == actual
    for i, (words, primary_bytes) in enumerate(((11, 48), (16, 64), (2, 16))):
        control = struct.unpack_from('<I', actual, 708 + i * 28)[0]
        dma = struct.unpack_from('<I', actual, 228 + i * 64)[0]
        assert ((dma & 15) + 1) * (((dma >> 4) & 15) + 1) == words
        assert (control & 255) * 16 == primary_bytes and primary_bytes >= words * 4
        assert ((control >> 26) & 63) * 16 == 48
    vu = bytearray((cases / 'old-window6-vuse.original.bin').read_bytes())
    struct.pack_into('<II', vu, 52, 0x28A1F001, 0xA0200800)
    assert vu == (cases / 'task15-vuse.original.bin').read_bytes()
    assert (cases / 'old-window6-fuse.original.bin').read_bytes() == (
        cases / 'task15-fuse.original.bin'
    ).read_bytes()
    assert (cases / 'old-bg3-bytes.original.bin').read_bytes() == (
        cases / 'bg7-bytes.original.bin'
    ).read_bytes()
    geometry = (cases / 'task15-geometry.original.bin').read_bytes()
    assert len(geometry) == 280
    clip = struct.unpack('<24I', source_clip)
    window = struct.unpack('<48I', source_vertices)
    coordinates = []

    def rational(word):
        return Fraction(struct.unpack('<f', struct.pack('<I', word))[0])

    for i in range(6):
        c = clip[4 * i : 4 * i + 4]
        v = window[8 * i : 8 * i + 8]
        uploaded = struct.unpack('<8I', geometry[32 * i : 32 * i + 32])
        assert uploaded == c + v[4:]
        matrix = (
            rational(c[0]) * 32 + 32,
            -rational(c[1]) * 32 + 32,
            rational(c[2]) / 2 + Fraction(1, 2),
            Fraction(1),
        )
        projected = tuple(struct.unpack('<I', struct.pack('<f', float(n)))[0] for n in matrix)
        assert projected == v[:4]
        assert all(rational(projected[k]) == matrix[k] for k in range(4))
        coordinates.append(
            dict(
                ordinal=i,
                clip=list(c),
                sidecar_window=list(v[:4]),
                exact_math_window=list(projected),
            )
        )
    meta = json.loads((cases / 'projection.original.json').read_text())
    assert (meta['task'], meta['profile'], meta['outer']) == (15, 14, 7)
    assert not meta['depth_enabled'] and not meta['physical_z_viewport_qualified']
    assert (
        len(meta['resource']) == 8
        and len(meta['sections']) == 16
        and len(meta['relocations']) == 40
    )
    bound = [
        bytearray(actual),
        bytearray(vu),
        bytearray((cases / 'task15-fuse.original.bin').read_bytes()),
    ]
    for kind, section, offset, role, target, size, align, mask, right, left in meta['relocations']:
        s = meta['sections'][section]
        resource = meta['resource'][role]
        va = resource['va'] + target
        assert target + size <= resource['bytes'] and va % align == 0 and offset + 4 <= s[3]
        if kind == 1:
            value = (va >> right) << left
        else:
            assert role in (1, 2)
            units = (va & 0x7FFFF) // 16
            value = (3, 4)[role - 1] | (units // 2048) * 16 | (units % 2048) * 256
        dest = bound[s[1]]
        at = s[2] + offset
        (before,) = struct.unpack_from('<I', dest, at)
        struct.pack_into('<I', dest, at, (before & ~mask) | (value & mask))
    for raw, name in zip(bound, ('pds', 'vuse', 'fuse')):
        assert raw == (cases / ('task15-bound-' + name + '.original.bin')).read_bytes()
    return dict(
        status='PASS_INDEPENDENT_CPU_BYTES_EXTENTS_COORDINATES_ONLY',
        checked_pds_bytes=1024,
        checked_use_bytes=80 + 1536,
        foreground_relocations=40,
        source_vertices=coordinates,
        original_window_image_bytes=768,
        background_offsets=[1024, 1088],
        maximum_pds_owner_extent=1092,
        pds_owner_bytes=4096,
        geometry_bytes=280,
        target_bytes=16384,
        TA_primary_allocation_bytes=[48, 64, 16],
        DMA_state_words=[11, 16, 2],
        PDS_data_bytes=[48, 48, 48],
        z_scope='ST mathematical sidecar projection only; emitted MTE Z effect UNKNOWN; depth disabled',
        edge_rule_qualified=False,
        retirement_qualified=False,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--base-root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--corpus', type=Path)
    parser.add_argument('--report-sha256')
    parser.add_argument('--target-i386', action='store_true')
    args = parser.parse_args()
    if (args.corpus is None) != (args.report_sha256 is None):
        parser.error('paired explicit producer corpus and report SHA256 required')
    roots = [p.resolve(strict=True) for p in (args.root, args.base_root) if p]

    def source(name):
        return next(p / name for p in roots if (p / name).is_file())

    out = args.output.absolute()
    out.mkdir(parents=True, exist_ok=False)
    selected_input = None
    if args.corpus:
        importer = source('tools/window_scene_inputs.py')
        spec = importlib.util.spec_from_file_location('clip_input_source', importer)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        selected_input = module.load(args.corpus.resolve(strict=True), args.report_sha256)
        header = selected_input['header']
        (out / 'source-inputs.original.json').write_text(
            json.dumps(selected_input['receipt'], indent=2) + '\n'
        )
        (out / 'source-pins.original.json').write_text(
            json.dumps(selected_input['pins'], indent=2) + '\n'
        )
        source_clip = (
            args.corpus / 'strict-cases/gl-window-w64-h64-v14.clip.original.bin'
        ).read_bytes()
        source_vertices = (
            args.corpus / 'strict-cases/gl-window-w64-h64-v14.vertices.original.bin'
        ).read_bytes()
    else:
        header = synthetic_header()
        import re

        def source_array(name):
            words = re.search(rb'window_mesh_input_' + name + rb'\[\] = \{([^}]+)', header)[1]
            return b''.join(
                struct.pack('<I', int(word.rstrip(b'U'), 16)) for word in words.split(b',')
            )

        source_clip, source_vertices = source_array(b'clip'), source_array(b'vertices')
    # The actual importer spells its public symbolic words u32. This is a
    # fixture typedef, not a source/header token rewrite.
    header = b'typedef uint32_t u32;\n' + header
    (out / 'poulsback_window_mesh_actor_input.generated.h').write_bytes(header)
    units = [source(name) for name in UNITS]
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    includes = [str(p / 'src/raster') for p in roots] + [str(p / 'src/compiler') for p in roots]
    includes += [str(p / 'tests/unit') for p in roots] + [str(out)]
    flags = [
        '-std=c11',
        '-O1',
        '-g',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wconversion',
        '-Wshadow',
        '-Wstrict-prototypes',
        '-Wmissing-prototypes',
        '-Wl,--wrap=calloc',
        *['-I' + p for p in includes],
    ]
    if args.target_i386:
        flags += ['-m32', '-march=i686', '-mtune=atom']
    selected = {
        Path(__file__).resolve(),
        *units,
        out / 'poulsback_window_mesh_actor_input.generated.h',
    }
    children = []
    for i, unit in enumerate(units):
        row = command(
            out, 'dependency-%02d' % i, [*compiler, *flags, '-MM', '-MT', 'selected', str(unit)]
        )
        children.append(row)
        if row['returncode']:
            return 1
        raw = (out / ('dependency-%02d.stdout.original' % i)).read_text().replace('\\\n', ' ')
        selected.update(Path(p).resolve() for p in shlex.split(raw.split(':', 1)[1]))
    if args.corpus:
        selected.add(importer)
    before = [identity(p) for p in sorted(selected)]
    (out / 'source-before.original.json').write_text(json.dumps(before, indent=2) + '\n')
    for flavor, extra in (
        ('strict', []),
        ('ubsan', ['-fsanitize=undefined', '-fno-sanitize-recover=all']),
    ):
        cases = out / (flavor + '-cases')
        cases.mkdir()
        binary = out / flavor
        for name, argv in (
            (flavor + '-compile', [*compiler, *flags, *extra, *map(str, units), '-o', str(binary)]),
            (flavor + '-run', [str(binary), str(cases)]),
        ):
            row = command(out, name, argv)
            children.append(row)
            if row['returncode']:
                return 1
        projected = independent_projection(cases, source_clip, source_vertices)
        (out / (flavor + '-independent-projection.original.json')).write_text(
            json.dumps(projected, indent=2) + '\n'
        )
    after = [identity(p) for p in sorted(selected)]
    (out / 'source-after.original.json').write_text(json.dumps(after, indent=2) + '\n')
    if before != after:
        raise ValueError('selected source changed')
    a = {p.name: identity(p) for p in (out / 'strict-cases').iterdir()}
    z = {p.name: identity(p) for p in (out / 'ubsan-cases').iterdir()}
    if set(a) != set(z) or any(
        (a[n]['bytes'], a[n]['sha256']) != (z[n]['bytes'], z[n]['sha256']) for n in a
    ):
        raise ValueError('strict/UBSAN raw images differ')
    if args.corpus:
        again = module.load(args.corpus.resolve(strict=True), args.report_sha256)
        if again['pins'] != selected_input['pins'] or again['header'] != selected_input['header']:
            raise ValueError('original producer changed')
    summary = dict(
        state='PASS_COMPLETE_CLIP_VIEWPORT_TASK15_CPU_ONLY',
        target_i386=args.target_i386,
        children=children,
        source_unchanged=True,
        records=a,
        genuine_source=bool(args.corpus),
        hardware_qualified=False,
        edge_rule_qualified=False,
        retirement_qualified=False,
        native_admission=False,
    )
    (out / 'summary.original.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(
        json.dumps(
            dict(
                state=summary['state'],
                records=len(a),
                source_paths=len(before),
                target_i386=args.target_i386,
            )
        )
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
