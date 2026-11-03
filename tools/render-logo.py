#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors
# SPDX-License-Identifier: MIT

"""Deterministic, font-free poulsback logo renderer.

Core shapes and keylines use integer, half-open rectangles. The optional halo
is a separate layer underneath the unchanged binary core. Logo cores never
use image generation, font rasterization, or screenshot tracing. The cube grid comes from the project's independent CPU cube reference,
checked against the C geometry before generation.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from hashlib import sha256
import importlib.util
import os
import shlex
import struct
import subprocess
import tempfile
import html
import json
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageChops, ImageDraw, ImageFilter, __version__ as PILLOW_VERSION

ROOT = Path(__file__).resolve().parents[1]
DESIGN = ROOT / "assets/logo"
REFERENCE = ROOT / "tests/project/depth_cube_reference.py"
CUBE_HEADER = ROOT / "tests/unit/depth_cube_reference.h"
TYPE = json.loads((DESIGN / "lettering.json").read_text(encoding="utf-8"))
LAYOUT = json.loads((DESIGN / "layout.json").read_text(encoding="utf-8"))
THEMES = ("dark", "light")
FINISHES = ("crisp", "soft")
OUTLINES = ("black", "white")


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int
    color: str
    group: str


@dataclass
class Scene:
    width: int
    height: int
    rects: list[Rect]
    theme: str
    kind: str
    cube_origin: tuple[int, int]
    cube_cell: int
    outline: str | None = None


def rgb(value: str) -> tuple[int, int, int]:
    return tuple(int(value[i : i + 2], 16) for i in (1, 3, 5))


def align_up(value: int, unit: int) -> int:
    return (value + unit - 1) // unit * unit


def bitmap_rects(rows: list[str], marker: str = "#") -> list[tuple[int, int, int, int]]:
    """Merge identical horizontal runs on adjacent rows; preserve every cell."""
    active: dict[tuple[int, int], list[int]] = {}
    out: list[tuple[int, int, int, int]] = []
    for y, row in enumerate(rows):
        runs: list[tuple[int, int]] = []
        x = 0
        while x < len(row):
            if row[x] != marker:
                x += 1
                continue
            end = x + 1
            while end < len(row) and row[end] == marker:
                end += 1
            runs.append((x, end - x))
            x = end
        current = set(runs)
        for key in list(active):
            if key not in current:
                out.append(tuple(active.pop(key)))
        for x, w in runs:
            if (x, w) in active:
                active[(x, w)][3] += 1
            else:
                active[(x, w)] = [x, y, w, 1]
    out.extend(tuple(r) for r in active.values())
    return sorted(out, key=lambda r: (r[1], r[0], r[2], r[3]))


def cube_reference():
    spec = importlib.util.spec_from_file_location("poulsback_logo_cube", REFERENCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def canonical_cube() -> dict:
    """Crop the existing 64x64 CPU reference; keep no second hand-drawn grid."""
    reference = cube_reference()
    codes = {(0, 0, 0, 1): ".", (1, 0, 0, 1): "R", (1, 0, 1, 1): "M", (1, 1, 0, 1): "Y"}
    image = [
        "".join(codes[reference.ray_box(x, y, 64, 64)[1]] for x in range(64)) for y in range(64)
    ]
    if any(
        image[y][x] != "."
        for y in range(64)
        for x in range(64)
        if not (16 <= x < 48 and 16 <= y < 48)
    ):
        raise ValueError("Cube reference exceeds the selected logo crop")
    rows = [row[16:48] for row in image[16:48]]
    if any(sum(row.count(code) for row in rows) != 256 for code in "RMY"):
        raise ValueError("Cube reference no longer matches the three-face design")
    rectangles = [(x, y, w, h, code) for code in "RMY" for x, y, w, h in bitmap_rects(rows, code)]
    rectangles.sort(key=lambda r: (r[1], r[0], r[2], r[3]))
    return dict(
        width=32,
        height=32,
        rows=rows,
        rectangles=rectangles,
        palette={"R": "#FF0000", "M": "#FF00FF", "Y": "#FFFF00"},
    )


CUBE = canonical_cube()


def verify_cube_math() -> dict:
    """Compare all pixels/depth with the actual compiled C test geometry.

    This is a host CPU reference check, with no module, device or GPU access.
    Exact binary-fraction input and sample positions need no tolerance.
    """
    source = '''#include <stdint.h>
#include <stdio.h>
#include <float.h>
#include "depth_cube_reference.h"
_Static_assert(sizeof(float)==4 && FLT_RADIX==2 && FLT_MANT_DIG==24,
               "The cube fixture requires binary32");
int main(void) {
    for (uint32_t i=0; i<DEPTH_CUBE_VERTICES; i++) {
        float clip[4], color[4];
        depth_cube_input(i,clip,color);
        if (fwrite(clip,sizeof(float),4,stdout)!=4 ||
            fwrite(color,sizeof(float),4,stdout)!=4) return 1;
    }
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="poulsback-logo-") as directory:
        work = Path(directory)
        (work / "cube.c").write_text(source, encoding="utf-8")
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(CUBE_HEADER.parent),
            str(work / "cube.c"),
            "-o",
            str(work / "cube"),
        ]
        subprocess.run(command, check=True, capture_output=True, timeout=30)
        raw = subprocess.run(
            [str(work / "cube")], check=True, capture_output=True, timeout=5
        ).stdout
    if len(raw) != 36 * 8 * 4:
        raise ValueError("Expected all 36 original C cube vertices")
    reference = cube_reference()
    vertices = []
    for values in struct.iter_unpack("=8f", raw):
        x, y, z, w, *rgba = map(reference.F, values)
        if w != 1:
            raise ValueError("Logo fixture requires the selected W=1 projection")
        vertices.append((x * 32 + 32, y * -32 + 32, z / 2 + reference.F(1, 2), w, *rgba))
    for y in range(64):
        for x in range(64):
            hits = reference.triangles(
                vertices, (reference.F(2 * x + 1, 2), reference.F(2 * y + 1, 2))
            )
            actual = min(hits, key=lambda h: h[0])[:2] if hits else (reference.F(1), (0, 0, 0, 1))
            if actual != reference.ray_box(x, y, 64, 64):
                raise ValueError(f"C cube and ray/box reference disagree at {x},{y}")
    return dict(
        scope="HOST_CPU_REFERENCE_ONLY",
        pixels_checked=4096,
        logo_cells=1024,
        differing_pixels=0,
        c_geometry_sha256=sha256(CUBE_HEADER.read_bytes()).hexdigest(),
        independent_reference_sha256=sha256(REFERENCE.read_bytes()).hexdigest(),
        hardware_qualification=False,
    )


def text_geometry(
    text: str, font: dict, cell: int, extra_tracking: int = 0
) -> tuple[list[tuple[int, int, int, int]], int]:
    x = 0
    rects: list[tuple[int, int, int, int]] = []
    for i, character in enumerate(text):
        if character == " ":
            x += font["space_advance"] * cell
        else:
            glyph = font["glyphs"][character]
            rects.extend(
                (x + rx * cell, ry * cell, rw * cell, rh * cell)
                for rx, ry, rw, rh in bitmap_rects(glyph["rows"])
            )
            x += glyph["advance"] * cell
        if i + 1 < len(text):
            x += extra_tracking
    if text and text[-1] != " ":
        x -= font["tracking"] * cell
    return rects, x


def add_cube(rects: list[Rect], x: int, y: int, cell: int) -> None:
    colors = CUBE["palette"]
    for rx, ry, rw, rh, code in CUBE["rectangles"]:
        rects.append(Rect(x + rx * cell, y + ry * cell, rw * cell, rh * cell, colors[code], "cube"))


def add_brackets(
    rects: list[Rect],
    x: int,
    y: int,
    width: int,
    height: int,
    arm: int,
    thickness: int,
    color: str,
) -> None:
    for bx, right in ((x, False), (x + width - arm, True)):
        stem_x = bx + arm - thickness if right else bx
        for rx, ry, rw, rh in (
            (stem_x, y, thickness, height),
            (bx, y, arm, thickness),
            (bx, y + height - thickness, arm, thickness),
        ):
            rects.append(Rect(rx, ry, rw, rh, color, "lettering"))


def make_icon_scene(outline: str = "black") -> Scene:
    """The color cube with a black or white outline."""
    if outline not in OUTLINES:
        raise ValueError("Unknown icon outline")
    size = LAYOUT["icon"]["size_px"]
    cell = LAYOUT["icon"]["cube_cell_px"]
    cube_size = CUBE["width"] * cell
    cube_x = cube_y = (size - cube_size) // 2
    rects: list[Rect] = []
    add_cube(rects, cube_x, cube_y, cell)
    # Theme selects a preview background only. The icon outline is explicit.
    theme = "dark" if outline == "white" else "light"
    return Scene(size, size, rects, theme, "icon", (cube_x, cube_y), cell, outline)


def make_scene(theme: str, kind: str = "logo", outline: str | None = None) -> Scene:
    if theme not in THEMES or kind not in ("logo", "icon"):
        raise ValueError("Unknown theme or kind")
    if kind == "icon":
        return make_icon_scene(outline or ("white" if theme == "dark" else "black"))
    colors = LAYOUT["themes"][theme]
    rects: list[Rect] = []

    cell = LAYOUT["cube_cell_px"]
    cube_size = CUBE["width"] * cell
    margin = LAYOUT["margin_px"]
    cube_y = margin
    bracket_arm = LAYOUT["bracket_width_px"]
    gap = LAYOUT["bracket_to_cube_px"]
    group_width = 2 * (bracket_arm + gap) + cube_size
    cube_x = margin + bracket_arm + gap
    add_brackets(
        rects,
        margin,
        cube_y,
        group_width,
        cube_size,
        bracket_arm,
        LAYOUT["bracket_thickness_px"],
        colors["foreground"],
    )
    add_cube(rects, cube_x, cube_y, cell)

    word_x = margin + group_width + LAYOUT["cube_to_wordmark_px"]
    word_y = cube_y + LAYOUT["wordmark_top_offset_px"]
    word_cell = LAYOUT["wordmark_cell_px"]
    word_rects, word_width = text_geometry(LAYOUT["name"], TYPE["wordmark"], word_cell)
    rects.extend(
        Rect(word_x + x, word_y + y, w, h, colors["foreground"], "lettering")
        for x, y, w, h in word_rects
    )

    tagline_rects, tagline_width = text_geometry(
        LAYOUT["tagline"],
        TYPE["tagline"],
        LAYOUT["tagline_cell_px"],
        LAYOUT["tagline_extra_tracking_px"],
    )
    tag_x = word_x + (word_width - tagline_width) // 2
    tag_y = word_y + TYPE["wordmark"]["height"] * word_cell + LAYOUT["wordmark_to_tagline_px"]
    rects.extend(
        Rect(tag_x + x, tag_y + y, w, h, colors["tagline"], "tagline")
        for x, y, w, h in tagline_rects
    )
    cursor_x = word_x + word_width + LAYOUT["cursor_gap_px"]
    cursor_y = word_y + TYPE["wordmark"]["x_height_top"] * word_cell
    rects.append(
        Rect(
            cursor_x,
            cursor_y,
            LAYOUT["cursor_width_px"],
            TYPE["wordmark"]["x_height"] * word_cell,
            "#FF00FF",
            "cursor",
        )
    )
    right = cursor_x + LAYOUT["cursor_width_px"]
    width = align_up(right + margin, 16)
    height = cube_size + 2 * margin
    # Center by an integer translation, including the rounded-up canvas padding.
    shift = (width - (right + margin)) // 2
    if shift:
        rects = [Rect(r.x + shift, r.y, r.w, r.h, r.color, r.group) for r in rects]
        cube_x += shift
    return Scene(width, height, rects, theme, kind, (cube_x, cube_y), cell)


def mask_for(scene: Scene, group: str) -> Image.Image:
    mask = Image.new("L", (scene.width, scene.height), 0)
    draw = ImageDraw.Draw(mask)
    for r in scene.rects:
        if r.group == group:
            draw.rectangle((r.x, r.y, r.x + r.w - 1, r.y + r.h - 1), fill=255)
    return mask


def group_masks(scene: Scene) -> dict[str, Image.Image]:
    return {
        group: mask_for(scene, group)
        for group in ("cube", "lettering")
        if any(r.group == group for r in scene.rects)
    }


def keyline_masks(scene: Scene) -> dict[str, Image.Image]:
    mask = mask_for(scene, "cube")
    radius = LAYOUT["icon"]["keyline_px"] if scene.kind == "icon" else LAYOUT["keyline_px"]
    return {"cube": ImageChops.subtract(mask.filter(ImageFilter.MaxFilter(2 * radius + 1)), mask)}


def scene_colors(scene: Scene) -> dict:
    colors = dict(LAYOUT["themes"][scene.theme])
    if scene.kind == "icon":
        colors["cube_keyline"] = LAYOUT["icon"]["outlines"][scene.outline]
    return colors


def halo_mask(scene: Scene) -> Image.Image:
    union = Image.new("L", (scene.width, scene.height), 0)
    for mask in (*group_masks(scene).values(), *keyline_masks(scene).values()):
        union = ImageChops.lighter(union, mask)
    return union


def color_layer(size: tuple[int, int], color: str, alpha: Image.Image) -> Image.Image:
    layer = Image.new("RGBA", size, (*rgb(color), 255))
    layer.putalpha(alpha)
    return layer


def render_png(scene: Scene, finish: str = "crisp", scale: int = 1) -> Image.Image:
    if finish not in FINISHES or not isinstance(scale, int) or scale < 1:
        raise ValueError("Scale must be a positive integer; finish is crisp or soft")
    if scene.kind == "icon" and finish != "crisp":
        raise ValueError("Cube icons use the crisp finish")
    size = (scene.width * scale, scene.height * scale)
    image = Image.new("RGBA", size, (0, 0, 0, 0))
    colors = scene_colors(scene)
    if finish == "soft":
        halo = (
            halo_mask(scene)
            .resize(size, Image.Resampling.NEAREST)
            .filter(ImageFilter.GaussianBlur(LAYOUT["halo_radius_px"] * scale))
        )
        opacity = colors["halo_opacity"]
        halo = halo.point([round(a * opacity) for a in range(256)])
        image = Image.alpha_composite(image, color_layer(size, colors["halo"], halo))
    for group, mask in keyline_masks(scene).items():
        image = Image.alpha_composite(
            image,
            color_layer(size, colors["cube_keyline"], mask.resize(size, Image.Resampling.NEAREST)),
        )
    draw = ImageDraw.Draw(image)
    for r in scene.rects:
        draw.rectangle(
            (r.x * scale, r.y * scale, (r.x + r.w) * scale - 1, (r.y + r.h) * scale - 1),
            fill=(*rgb(r.color), 255),
        )
    return image


def mask_rectangles(mask: Image.Image) -> list[tuple[int, int, int, int]]:
    values = mask.tobytes()
    rows = [
        "".join("#" if v else "." for v in values[y * mask.width : (y + 1) * mask.width])
        for y in range(mask.height)
    ]
    return bitmap_rects(rows)


def path_d(rects: Iterable[tuple[int, int, int, int]]) -> str:
    return " ".join(f"M{x} {y}h{w}v{h}h{-w}Z" for x, y, w, h in rects)


def svg_text(scene: Scene, finish: str = "crisp", display_scale: int = 2) -> str:
    if finish not in FINISHES or not isinstance(display_scale, int) or display_scale < 1:
        raise ValueError("Unknown finish or invalid integer scale")
    if scene.kind == "icon" and finish != "crisp":
        raise ValueError("Cube icons use the crisp finish")
    width, height = scene.width, scene.height
    colors = scene_colors(scene)
    title = (
        f"poulsback — color cube, {scene.outline} outline"
        if scene.kind == "icon"
        else f"poulsback — terminal {scene.theme} {scene.kind}"
    )
    description = (
        "Original three-face cube on an integer pixel grid, with a solid outer outline."
        if scene.kind == "icon"
        else "The pulse is back. Original cube geometry and custom lettering on an integer pixel grid. All lettering is geometry; no external fonts."
    )
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width * display_scale}" height="{height * display_scale}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc" shape-rendering="crispEdges">',
        f'  <title id="title">{html.escape(title)}</title>',
        f'  <desc id="desc">{description}</desc>',
    ]
    if finish == "soft":
        lines.extend(
            [
                '  <defs>',
                f'    <filter id="halo" x="0" y="0" width="{width}" height="{height}" filterUnits="userSpaceOnUse" color-interpolation-filters="sRGB">',
                f'      <feGaussianBlur stdDeviation="{LAYOUT["halo_radius_px"]}"/>',
                '    </filter>',
                '  </defs>',
            ]
        )
        union = halo_mask(scene)
        lines.append(
            f'  <path data-layer="halo" d="{path_d(mask_rectangles(union))}" fill="{colors["halo"]}" opacity="{colors["halo_opacity"]}" filter="url(#halo)" shape-rendering="geometricPrecision"/>'
        )
    for group, mask in keyline_masks(scene).items():
        lines.append(
            f'  <path data-layer="keyline-{group}" d="{path_d(mask_rectangles(mask))}" fill="{colors["cube_keyline"]}"/>'
        )
    for group in ("cube", "lettering", "cursor", "tagline"):
        palette = list(dict.fromkeys(r.color for r in scene.rects if r.group == group))
        for color in palette:
            d = path_d(
                (r.x, r.y, r.w, r.h) for r in scene.rects if r.group == group and r.color == color
            )
            lines.append(f'  <path data-layer="{group}" d="{d}" fill="{color}"/>')
    lines.append('</svg>')
    return "\n".join(lines) + "\n"


def svg_assets():
    for theme in THEMES:
        for finish in FINISHES:
            yield f"poulsback-terminal-{theme}-{finish}", make_scene(theme), finish
    for outline in OUTLINES:
        yield f"poulsback-cube-color-outline-{outline}", make_icon_scene(outline), "crisp"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="Check C math and all saved SVGs without changing them"
    )
    parser.add_argument(
        "--output-dir", type=Path, default=DESIGN, help="Directory for the six SVG variants"
    )
    parser.add_argument(
        "--with-png", action="store_true", help="Also export PNGs at integer scales 1, 2 and 4"
    )
    parser.add_argument("--theme", choices=THEMES, default="dark")
    parser.add_argument("--finish", choices=FINISHES, default="crisp")
    parser.add_argument("--kind", choices=("logo", "icon"), default="logo")
    parser.add_argument("--outline", choices=OUTLINES)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument(
        "--output", type=Path, help="Render one .svg or .png instead of the complete set"
    )
    args = parser.parse_args()
    if args.scale < 1:
        parser.error("--scale must be a positive integer")
    if args.check and (args.output or args.with_png):
        parser.error("--check does not write assets")
    if args.output and args.kind == "icon" and args.finish != "crisp":
        parser.error("Cube icons use --finish crisp")
    if args.output and args.kind == "logo" and args.outline:
        parser.error("--outline applies to --kind icon")
    if args.output and args.output.suffix.lower() not in (".svg", ".png"):
        parser.error("--output must end in .svg or .png")
    proof = verify_cube_math()
    if args.check:
        expected = {
            stem + ".svg": svg_text(scene, finish).encode("utf-8")
            for stem, scene, finish in svg_assets()
        }
        if {p.name for p in args.output_dir.glob("*.svg")} != set(expected):
            raise ValueError("Saved SVG set differs from the six generated variants")
        for name, data in expected.items():
            if (args.output_dir / name).read_bytes() != data:
                raise ValueError("Stale logo asset: " + name)
        print(json.dumps(dict(status="PASS_LOGO_CPU_REFERENCE", svg_files=6, **proof), indent=2))
    elif args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        scene = make_scene(args.theme, args.kind, args.outline)
        if args.output.suffix.lower() == ".svg":
            args.output.write_text(svg_text(scene, args.finish, args.scale), encoding="utf-8")
        else:
            render_png(scene, args.finish, args.scale).save(
                args.output, optimize=False, compress_level=9
            )
        print(args.output)
    else:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for stem, scene, finish in svg_assets():
            (args.output_dir / (stem + ".svg")).write_text(
                svg_text(scene, finish), encoding="utf-8"
            )
            if args.with_png:
                for scale in (1, 2, 4):
                    render_png(scene, finish, scale).save(
                        args.output_dir / f"{stem}@{scale}x.png", optimize=False, compress_level=9
                    )
        print(
            json.dumps(
                dict(
                    output_dir=str(args.output_dir),
                    svg_files=6,
                    png_files=18 if args.with_png else 0,
                    pillow_version=PILLOW_VERSION,
                )
            )
        )


if __name__ == "__main__":
    main()
