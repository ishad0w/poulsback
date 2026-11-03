#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""The clip-W1 component's vectors with their exact rational expectations.

usage: clip_w1_reference.py OUTPUT. Each record is ten little-endian words:
clip x, y, z, w, viewport width, height, then the expected window x, y, z and
w, each rounded once to binary32 (nearest, ties to even). test_clip_w1.c
reads OUTPUT; build-userspace.py runs this before it.
"""

from fractions import Fraction
from pathlib import Path
import random
import struct
import sys


def rational(bits):
    exponent, fraction = (bits >> 23) & 255, bits & 0x7FFFFF
    significand, power = (
        (fraction, -149) if exponent == 0 else (fraction | 0x800000, exponent - 150)
    )
    value = Fraction(significand) * (Fraction(2) ** power)
    return -value if bits & 0x80000000 else value


def rounded(value):
    # Host conversion provides a nearby candidate only. Exact Fraction distance
    # to its adjacent binary32 values chooses nearest, with LSB-even tie break.
    # Thus a double rounding in the host conversion cannot become our oracle.
    (nearby,) = struct.unpack("<I", struct.pack("<f", float(value)))
    candidates = {nearby, nearby + 1}
    if nearby:
        candidates.add(nearby - 1)
    return min(candidates, key=lambda word: (abs(rational(word) - value), word & 1))


def vectors():
    special = [
        0,
        1,
        0x7FFFFF,
        0x800000,
        0x800001,
        0x2FFFFFFF,
        0x30000000,
        0x337FFFFF,
        0x33800000,
        0x33800001,
        0x3EAAAAAB,
        0x3EFFFFFF,
        0x3F000000,
        0x3F000001,
        0x3F7FFFFE,
        0x3F7FFFFF,
        0x3F800000,
    ]
    special += [value | 0x80000000 for value in special]
    for extent in range(1, 4097):
        # Includes every allowed extent and both near-one cancellation ends.
        for value in (0x3F7FFFFF, 0xBF7FFFFF, 1, 0x80000001):
            yield (value, value, value, 0x3F800000, extent, 4097 - extent)
    for value in special:
        for width in (1, 2, 3, 31, 64, 255, 1280, 4095, 4096):
            yield (value, value ^ 0x80000000, value, 0x3F800000, width, width)
    rng = random.Random(0x535121)
    for _ in range(10000):
        values = [rng.randrange(0x3F800001) | (rng.randrange(2) << 31) for _ in range(3)]
        yield (*values, 0x3F800000, rng.randrange(1, 4097), rng.randrange(1, 4097))
    for value in ((0xBF000000, 0x3F000000), (0x3F000000, 0x3F000000), (0xBF000000, 0xBEF00000)):
        yield (*value, 0, 0x3F800000, 64, 64)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    raw = bytearray()
    for x, y, z, w, width, height in vectors():
        expected = [
            rounded((rational(x) + 1) * width / 2),
            rounded((1 - rational(y)) * height / 2),
            rounded((rational(z) + 1) / 2),
            w,
        ]
        raw += struct.pack("<10I", x, y, z, w, width, height, *expected)
    Path(sys.argv[1]).write_bytes(raw)
    print(f"{len(raw) // 40} clip-W1 vectors")


if __name__ == "__main__":
    main()
