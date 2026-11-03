#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Independent complete logical color/depth oracle for the CPU cube fixture.

No depth byte layout or conversion rule is qualified. Triangle barycentrics
are checked against an independent orthographic ray/box intersection; named
D16/D24 conversion models are prospective logical planes, never GPU captures.
"""

import argparse
from fractions import Fraction as F
import hashlib
import json
from pathlib import Path
import struct

COLORS = [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1), (1, 1, 0, 1), (1, 0, 1, 1), (0, 1, 1, 1)]


def f32(word):
    sign = -1 if word >> 31 else 1
    exponent, mantissa = (word >> 23) & 255, word & 0x7FFFFF
    assert exponent != 255
    if exponent:
        mantissa |= 1 << 23
    return sign * F(mantissa) * F(2) ** ((exponent or 1) - 150)


def edge(a, b, p):
    return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])


def triangles(vertices, point):
    hits = []
    for ordinal in range(12):
        v = vertices[ordinal * 3 : ordinal * 3 + 3]
        area = edge(v[0], v[1], v[2])
        assert area
        weights = [
            edge(v[1], v[2], point) / area,
            edge(v[2], v[0], point) / area,
            edge(v[0], v[1], point) / area,
        ]
        if min(weights) < 0:
            continue
        assert min(weights) > 0, 'fixture intentionally avoids sample-center edges'
        z = sum(w * p[2] for w, p in zip(weights, v))
        color = tuple(v[0][4:])
        assert all(tuple(p[4:]) == color for p in v)
        hits.append((z, color, ordinal))
    return hits


def ray_box(x, y, width, height):
    # Normalize the viewport to the independent 16-unit ray/box basis.
    u = (F(2 * x + 1, 2) - F(width, 2)) * F(16, width)
    v = (F(2 * y + 1, 2) - F(height, 2)) * F(16, height)
    lower = [-3 - v / 2 - 3 * u / 4, v - 3, -3 - v / 2 + 3 * u / 4]
    upper = [3 - v / 2 - 3 * u / 4, v + 3, 3 - v / 2 + 3 * u / 4]
    lo, hi = max(lower), min(upper)
    if lo >= hi:
        return F(1), (0, 0, 0, 1)
    assert lower.count(lo) == 1
    face = [0, 3, 4][lower.index(lo)]
    return F(1, 2) + lo / 16, COLORS[face]


def quantize(value, bits, model):
    value *= (1 << bits) - 1
    n, r = divmod(value.numerator, value.denominator)
    if model == 'floor':
        return n
    if model == 'half_up':
        return n + (2 * r >= value.denominator)
    assert model == 'rne'
    return n + (2 * r > value.denominator or (2 * r == value.denominator and n % 2))


def decode(clip_raw, vertices_raw, indices_raw, geometry_raw, width, height):
    assert len(clip_raw) == 576 and len(vertices_raw) == 1152 and len(indices_raw) == 144
    assert len(geometry_raw) == 1300
    words = list(struct.iter_unpack('<8I', vertices_raw))
    clips = list(struct.iter_unpack('<4I', clip_raw))
    indices = struct.unpack('<36I', indices_raw)
    assert indices == tuple(range(36))
    v = [tuple(map(f32, w)) for w in words]
    for c, p in zip(clips, v):
        c = tuple(map(f32, c))
        assert c[3] == p[3] == 1
        assert p[:3] == (
            c[0] * F(width, 2) + F(width, 2),
            c[1] * F(-height, 2) + F(height, 2),
            c[2] / 2 + F(1, 2),
        )
        assert all(k in (0, 1) for k in p[4:]) and p[7] == 1
    # Independent geometry packing, including full mask and foreground index order.
    mask = b''.join(
        struct.pack('<4f', x, y, 1, 1)
        for x, y in [(0, 0), (width, 0), (0, height), (width, height)]
    )
    expected = vertices_raw + mask + struct.pack('<42H', 0, 1, 2, 3, 2, 1, *indices)
    assert geometry_raw == expected
    assert sorted(tuple(v[i][4:]) for i in range(0, 36, 6)) == sorted(COLORS)
    return v


def observe(vertices, width, height):
    rows = []
    models = {
        name: []
        for name in ['d16_floor', 'd16_rne', 'd16_half_up', 'd24_floor', 'd24_rne', 'd24_half_up']
    }
    for y in range(height):
        for x in range(width):
            hits = triangles(vertices, (F(2 * x + 1, 2), F(2 * y + 1, 2)))
            expected_z, expected_color = ray_box(x, y, width, height)
            z, color, winning = F(1), (0, 0, 0, 1), None
            for candidate, rgba, ordinal in hits:
                if candidate < z:
                    z, color, winning = candidate, rgba, ordinal
            assert (z, color) == (expected_z, expected_color)
            assert len(hits) == (2 if winning is not None else 0)
            codes = {}
            for bits in [16, 24]:
                for model in ['floor', 'rne', 'half_up']:
                    depth = (1 << bits) - 1
                    quantized_color = (0, 0, 0, 1)
                    for candidate, rgba, _ in hits:
                        code = quantize(candidate, bits, model)
                        if code < depth:
                            depth, quantized_color = code, rgba
                    assert quantized_color == expected_color
                    name = f'd{bits}_{model}'
                    models[name].append(depth)
                    codes[name] = depth
            rows.append(
                dict(
                    x=x,
                    y=y,
                    source_strict_triangles=[h[2] for h in hits],
                    normalized_depth=str(z),
                    RGBA=[int(c * 255) for c in color],
                    depth_codes=codes,
                )
            )
    assert sum(r['normalized_depth'] != '1' for r in rows) == 3 * width * height // 16
    return rows, models


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    outputs = []
    cases = []
    resolutions = []
    mutations = 0
    for width, height in [(16, 16), (64, 64)]:
        canonical = None
        for fmt in [1, 2]:
            for order in range(3):
                prefix = (
                    f'cube-f{fmt}-o{order}'
                    if width == 16
                    else f'cube-w{width}-h{height}-f{fmt}-o{order}'
                )
                raw = [
                    (a.corpus / f'{prefix}.{suffix}.original.bin').read_bytes()
                    for suffix in ['clip', 'vertices', 'indices', 'geometry']
                ]
                vertices = decode(*raw, width, height)
                rows, models = observe(vertices, width, height)
                stable = [(r['normalized_depth'], r['RGBA'], r['depth_codes']) for r in rows]
                if canonical is not None:
                    assert stable == canonical
                # Mutations are rejected by provenance, order and complete byte checks.
                for field, at in [(0, 0), (1, 35 * 32 + 16), (2, 35 * 4), (3, 1299)]:
                    bad = list(raw)
                    changed = bytearray(bad[field])
                    changed[at] ^= 1
                    bad[field] = bytes(changed)
                    try:
                        decode(*bad, width, height)
                    except AssertionError:
                        pass
                    else:
                        raise AssertionError(('accepted mutation', field, at))
                    mutations += 1
                cases.append(
                    {
                        'source': prefix,
                        'width': width,
                        'height': height,
                        'all_positions_checked': len(rows),
                        'equal_canonical_logical_plane': True,
                        'strict_triangle_ordinals_sha256': hashlib.sha256(
                            json.dumps(
                                [r['source_strict_triangles'] for r in rows], separators=(',', ':')
                            ).encode()
                        ).hexdigest(),
                        'source_records': [
                            {
                                'suffix': suffix,
                                'bytes': len(b),
                                'sha256': hashlib.sha256(b).hexdigest(),
                            }
                            for suffix, b in zip(['clip', 'vertices', 'indices', 'geometry'], raw)
                        ],
                    }
                )
                if canonical is not None:
                    continue
                canonical = stable
                drawn = 3 * width * height // 16
                resolutions.append(
                    {
                        'width': width,
                        'height': height,
                        'strict_drawn': drawn,
                        'clear': width * height - drawn,
                        'sample_edges': 0,
                        'cases_checked': 6,
                    }
                )
                data = {
                    'schema': 'depth-cube-full-logical-CPU-oracle-v2',
                    'source': prefix,
                    'scope': 'CPU_INPUT_AND_PROSPECTIVE_MATHEMATICAL_MODELS_ONLY',
                    'width': width,
                    'height': height,
                    'strict_drawn': drawn,
                    'clear': width * height - drawn,
                    'sample_edges': 0,
                    'depth_test': 'LESS/write, clear1; triangle scan independently agrees with ray/box intersection',
                    'depth_layout_qualified': False,
                    'depth_conversion_qualified': False,
                    'hardware_qualified': False,
                    'API_qualified': False,
                    'rows': rows,
                }
                target = a.output / f'{prefix}.logical-reference.json'
                target.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n')
                color = a.output / f'{prefix}.color-rgba8.cpu-model.bin'
                color.write_bytes(bytes(c for r in rows for c in r['RGBA']))
                outputs += [target, color]
                for name, codes in models.items():
                    target = a.output / f'{prefix}.{name}.logical-codes.cpu-model.bin'
                    target.write_bytes(struct.pack(f'<{width * height}I', *codes))
                    outputs.append(target)
    wide = []
    for fmt in [1, 2]:
        prefix = f'cube-w1280-h800-f{fmt}-o0'
        paths = [
            a.corpus / f'{prefix}.{suffix}.original.bin'
            for suffix in ['clip', 'vertices', 'indices', 'geometry']
        ]
        if not any(p.exists() for p in paths):
            continue
        raw = [p.read_bytes() for p in paths]
        decode(*raw, 1280, 800)
        wide.append(
            {
                'source': prefix,
                'width': 1280,
                'height': 800,
                'scope': 'CPU_GEOMETRY_PACKING_ONLY_NO_FULL_PIXEL_OR_DEPTH_PLANE',
                'geometry_sha256': hashlib.sha256(raw[3]).hexdigest(),
            }
        )
    manifest = {
        'scope': 'CPU_ORACLE_ONLY_NO_PHYSICAL_DEPTH_LAYOUT_OR_GPU_RESULT',
        'resolutions': resolutions,
        'cases': cases,
        'wide_construction': wide,
        'source_mutations_rejected': mutations,
        'qualifications': False,
        'outputs': [
            {
                'path': p.name,
                'bytes': p.stat().st_size,
                'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
            }
            for p in outputs
        ],
    }
    (a.output / 'manifest.original.json').write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + '\n'
    )
    print(
        'cube oracle: all256 and all4096 positions; 48/768 drawn,208/3328 clear; twelve order/format cases; CPU models only'
    )


if __name__ == '__main__':
    main()
