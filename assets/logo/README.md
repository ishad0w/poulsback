# The `poulsback` logo

The name **poulsback** is a little wordplay. *Poulsbo* was the codename for the
platform behind the Intel GMA 500, and *pouls* is French for “pulse.” So
**THE PULSE IS BACK** is both a nod to the hardware and a way of saying what the
project is about: bringing it back to life.

The cube's three visible faces make it recognizable at small sizes. Red,
magenta and yellow distinguish the faces; the magenta cursor ties the cube to
the pixel-style lettering. Sharp edges and compact letters keep the design
consistent. Light and dark variants support different backgrounds.

The logo is an illustration. It does not depict GPU output or declare API
support.

## Assets and generation

The six self-contained SVGs need no fonts or external images. The terminal
wordmark has light/dark and crisp/soft variants; two cube-only icons use black
or white outlines. The main README uses the dark crisp terminal variant and
links to this guide.

[layout.json](layout.json) controls spacing and colors;
[lettering.json](lettering.json) holds the glyph grids. The optional
[generator](../../tools/render-logo.py) needs Python 3, a C11 compiler and
Pillow. Driver builds do not require Pillow.

```sh
# Check the geometry and existing SVGs without changing them.
python3 -I -B tools/render-logo.py --check

# Regenerate SVGs in assets/logo.
python3 -I -B tools/render-logo.py

# Export SVGs and PNGs outside the source assets.
python3 -I -B tools/render-logo.py --output-dir build/logo --with-png
```

Use integer display scales for crisp pixel edges. The geometry checks live
with the [cube reference](../../tests/project/depth_cube_reference.py)
and generator. The root [license](../../LICENSE) covers these project assets.
The [name and logo policy](../../TRADEMARK.md) explains how to identify upstream
and independent derivatives without implying endorsement; it preserves that
copyright permission.
