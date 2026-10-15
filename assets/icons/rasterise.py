#!/usr/bin/env python3
"""Rasterise the monochrome SVG icons in this directory to PNG.

The application embeds the PNGs, not the SVGs: Qt's PNG support is built into
QtGui, while decoding SVG needs the optional Qt SVG plugin. The PNGs are
recoloured to the active theme at runtime (see ui::resource_icon), so the
artwork itself is plain black on transparent.

The SVGs remain the editable source of truth. After editing one, re-run this
script from anywhere:

    python3 assets/icons/rasterise.py

It reads each SVG's polygon `d` attribute directly, so the two forms cannot
drift apart. Requires Pillow.
"""

import glob
import os
import re

from PIL import Image, ImageDraw

# The embedded size, and the factor the artwork is drawn at before downscaling
# (supersampling keeps the edges smooth).
SIZE = 64
SUPERSAMPLE = 4

# Every icon is authored on the same 24x24 grid.
VIEW_BOX = 24.0


def polygon_points(svg_text):
    """The vertices of the single polygon path an icon file declares."""
    match = re.search(r'd="([^"]+)"', svg_text)

    if match is None:
        raise ValueError("no path `d` attribute found")

    numbers = [float(n) for n in re.findall(r"-?\d+(?:\.\d+)?", match.group(1))]

    return list(zip(numbers[0::2], numbers[1::2]))


def main():
    directory = os.path.dirname(os.path.abspath(__file__))
    scale = SIZE * SUPERSAMPLE / VIEW_BOX

    for svg in sorted(glob.glob(os.path.join(directory, "*.svg"))):
        with open(svg, encoding="utf-8") as handle:
            points = polygon_points(handle.read())

        size = SIZE * SUPERSAMPLE
        image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        ImageDraw.Draw(image).polygon(
            [(x * scale, y * scale) for x, y in points], fill=(0, 0, 0, 255)
        )
        image = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)

        out = os.path.splitext(svg)[0] + ".png"
        image.save(out)
        print("wrote", os.path.relpath(out, directory))


if __name__ == "__main__":
    main()
