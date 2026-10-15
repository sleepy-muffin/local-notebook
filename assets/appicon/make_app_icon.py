#!/usr/bin/env python3
"""Generate the Local Notebook application icon.

The monochrome composer icons in ../icons/ are authored as SVG and rasterised from
their polygon paths. The application icon is different: it is full colour and needs
to be legible from a 16px taskbar thumbnail up to a 512px launcher tile, so it is
composed here from primitives with Pillow -- this script is its source of truth.

Re-run from anywhere after changing the geometry or palette:

    python3 assets/appicon/make_app_icon.py

It writes ``app-<size>.png`` beside this file and a multi-resolution ``app.ico`` for
Windows builds. Requires Pillow.

The palette is Catppuccin Macchiato (the default theme), so the icon sits naturally
next to the interface. The mark is a spiral-bound notebook with a bookmark, drawn on
a rounded tile so it reads on both light and dark docks.
"""

import os

from PIL import Image, ImageDraw, ImageFilter

# --- Palette (Catppuccin Macchiato) -----------------------------------------
TILE_TOP = (48, 52, 76)      # a touch above surface, so the tile is not flat
TILE_BOTTOM = (24, 25, 38)   # crust
PAGE = (231, 236, 253)       # a clean paper white
COVER = (138, 173, 244)      # blue
COVER_EDGE = (108, 145, 226) # the cover's shaded right edge
RING = (202, 211, 245)       # text
RIBBON = (237, 135, 150)     # red

# The artwork is authored on a 1024x1024 grid and scaled to each output size.
GRID = 1024.0
# Every size is drawn this many times larger and then downscaled, so small icons
# stay smooth instead of aliasing.
SUPERSAMPLE = 4

SIZES = [16, 24, 32, 48, 64, 128, 256, 512]
ICO_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

# --- Geometry, on the 1024 grid ---------------------------------------------
TILE_BOX = (64, 64, 960, 960)
TILE_RADIUS = 208

PAGES_BOX = (352, 250, 748, 782)
PAGES_RADIUS = 36

COVER_BOX = (300, 226, 696, 758)
COVER_RADIUS = 40

RING_Y = [300, 378, 456, 534, 612, 690]
RING_HALF_W = 34
RING_HALF_H = 18
RING_STROKE = 15

RIBBON_POINTS = [(590, 226), (650, 226), (650, 846), (620, 806), (590, 846)]

# The footprint the drop shadow follows (the book, including the hanging ribbon).
SHADOW = (300, 226, 748, 846)


def render(size):
    """Draw the icon at ``size`` pixels, supersampled and downscaled."""
    px = size * SUPERSAMPLE
    scale = px / GRID

    def s(value):
        return value * scale

    def box(bounds):
        return tuple(s(value) for value in bounds)

    image = Image.new("RGBA", (px, px), (0, 0, 0, 0))

    # The tile: a vertical gradient clipped to a rounded square.
    gradient = Image.new("RGB", (1, px))
    for y in range(px):
        t = y / max(1, px - 1)
        gradient.putpixel(
            (0, y),
            tuple(round(TILE_TOP[i] + (TILE_BOTTOM[i] - TILE_TOP[i]) * t) for i in range(3)),
        )
    gradient = gradient.resize((px, px))

    tile_mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(tile_mask).rounded_rectangle(
        box(TILE_BOX), radius=s(TILE_RADIUS), fill=255
    )
    image.paste(gradient, (0, 0), tile_mask)

    # A soft shadow lifts the notebook off the tile.
    shadow = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (s(SHADOW[0]), s(SHADOW[1] + 14), s(SHADOW[2]), s(SHADOW[3] + 14)),
        radius=s(64),
        fill=(0, 0, 0, 130),
    )
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s(20))))

    draw = ImageDraw.Draw(image)


    # Spiral binding: rings straddling the cover's left edge. Drawn after the
    # cover so each loop sits on top of it.
    stroke = max(1, round(s(RING_STROKE)))
    for y in RING_Y:
        draw.ellipse(
            box(
                (
                    COVER_BOX[0] - RING_HALF_W,
                    y - RING_HALF_H,
                    COVER_BOX[0] + RING_HALF_W,
                    y + RING_HALF_H,
                )
            ),
            outline=RING,
            width=stroke,
        )

    # The page block peeks out along the right and bottom of the cover.
    draw.rounded_rectangle(box(PAGES_BOX), radius=s(PAGES_RADIUS), fill=PAGE)

    # The cover, with a shaded right edge for a little depth.
    draw.rounded_rectangle(box(COVER_BOX), radius=s(COVER_RADIUS), fill=COVER)
    draw.rounded_rectangle(
        (
            s(COVER_BOX[2] - 34),
            s(COVER_BOX[1] + 6),
            s(COVER_BOX[2]),
            s(COVER_BOX[3] - 6),
        ),
        radius=s(18),
        fill=COVER_EDGE,
    )

    # The bookmark ribbon, hanging below the book with a notched tail.
    draw.polygon([(s(x), s(y)) for x, y in RIBBON_POINTS], fill=RIBBON)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    directory = os.path.dirname(os.path.abspath(__file__))

    for size in SIZES:
        out = os.path.join(directory, f"app-{size}.png")
        render(size).save(out)
        print("wrote", os.path.relpath(out, directory))

    # A multi-resolution .ico for Windows builds, sourced from the 256px render.
    ico = os.path.join(directory, "app.ico")
    render(256).save(ico, sizes=ICO_SIZES)
    print("wrote", os.path.relpath(ico, directory))


if __name__ == "__main__":
    main()
