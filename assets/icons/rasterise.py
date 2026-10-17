#!/usr/bin/env python3
"""Rasterise the monochrome SVG icons in this directory to PNG.

The application embeds the PNGs, not the SVGs: Qt's PNG support is built into
QtGui, while decoding SVG needs the optional Qt SVG plugin. The PNGs are
recoloured to the active theme at runtime (see ui::resource_icon), so the
artwork itself is plain black on transparent.

The SVGs remain the editable source of truth. After editing one, re-run this
script from anywhere:

    python3 assets/icons/rasterise.py

Every icon is authored with straight-line path commands only, on a shared 24x24
grid, so the two forms cannot drift apart: the script reads each SVG's own `d`
attribute. A path may hold several subpaths; they are filled with the even-odd
rule, so a nested subpath becomes a hole (a gear's centre, an outlined sheet).
Requires Pillow.
"""

import glob
import os
import re

from PIL import Image, ImageChops, ImageDraw

# The embedded size, and the factor the artwork is drawn at before downscaling
# (supersampling keeps the edges smooth).
SIZE = 64
SUPERSAMPLE = 4

# Every icon is authored on the same 24x24 grid.
VIEW_BOX = 24.0


def parse_path(d):
    """The subpaths of an SVG path `d`, as lists of (x, y) points.

    Supports the move, line, horizontal, vertical and close commands, absolute
    and relative (M/m, L/l, H/h, V/v, Z/z); the icons use straight lines only.
    Each subpath is one contour.
    """
    tokens = re.findall(r"[MmLlHhVvZz]|-?\d+(?:\.\d+)?", d)

    subpaths = []
    current = []
    x = y = 0.0
    start = (0.0, 0.0)
    command = None
    index = 0

    def flush():
        if current:
            subpaths.append(list(current))
            current.clear()

    while index < len(tokens):
        token = tokens[index]

        if token.isalpha():
            command = token
            index += 1

            if command in "Zz":
                flush()
                x, y = start

            continue

        if command is None:
          raise

        if command in "Mm":
            nx, ny = float(tokens[index]), float(tokens[index + 1])
            index += 2

            if command == "m":
                nx, ny = x + nx, y + ny

            flush()
            x, y = nx, ny
            start = (x, y)
            current.append((x, y))
            # Further coordinate pairs after a move are implicit line-tos.
            command = "L" if command == "M" else "l"
        elif command in "Ll":
            nx, ny = float(tokens[index]), float(tokens[index + 1])
            index += 2

            if command == "l":
                nx, ny = x + nx, y + ny

            x, y = nx, ny
            current.append((x, y))
        elif command in "Hh":
            nx = float(tokens[index])
            index += 1

            if command == "h":
                nx = x + nx

            x = nx
            current.append((x, y))
        elif command in "Vv":
            ny = float(tokens[index])
            index += 1

            if command == "v":
                ny = y + ny

            y = ny
            current.append((x, y))
        else:
            raise ValueError(f"unsupported path command: {command!r}")

    flush()
    return subpaths


def render(subpaths):
    """A 64x64 black-on-transparent image of the subpaths, even-odd filled."""
    scale = SIZE * SUPERSAMPLE / VIEW_BOX
    size = SIZE * SUPERSAMPLE

    # Even-odd fill: a pixel covered by an odd number of contours is opaque, so a
    # contour nested inside another becomes a hole. XOR-ing the per-contour masks
    # computes exactly that parity.
    mask = Image.new("1", (size, size), 0)

    for points in subpaths:
        layer = Image.new("1", (size, size), 0)
        ImageDraw.Draw(layer).polygon(
            [(px * scale, py * scale) for px, py in points], fill=1
        )
        mask = ImageChops.logical_xor(mask, layer)

    alpha = mask.convert("L").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    image.paste(Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255)), (0, 0), alpha)
    return image


def main():
    directory = os.path.dirname(os.path.abspath(__file__))

    for svg in sorted(glob.glob(os.path.join(directory, "*.svg"))):
        with open(svg, encoding="utf-8") as handle:
            text = handle.read()

        match = re.search(r'\bd="([^"]+)"', text)

        if match is None:
            raise ValueError(f"no path `d` attribute in {os.path.basename(svg)}")

        image = render(parse_path(match.group(1)))

        out = os.path.splitext(svg)[0] + ".png"
        image.save(out)
        print("wrote", os.path.relpath(out, directory))


if __name__ == "__main__":
    main()
