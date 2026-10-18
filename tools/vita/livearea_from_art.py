#!/usr/bin/env python3
"""Builds the Vita bubble icon and LiveArea images in vita/sce_sys/ from box art you supply.

The art is copyrighted: neither the sources nor the PNGs are committed (.gitignore), and the VPK gets them only when they exist.
Usage: python3 tools/vita/livearea_from_art.py <painting> <cover>
  painting: the box painting without the logo, 16:9 (e.g. 1456x819) -> bg.png and icon0.png
  cover:    the box cover with the logo (e.g. 301x266) -> startup.png
The Vita accepts only 8-bit palette PNGs here.
"""

import os
import sys

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "vita", "sce_sys")


def save(img, relative):
    path = os.path.normpath(os.path.join(OUT, relative))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
    print(f"wrote {path}")


painting = Image.open(sys.argv[1]).convert("RGB")
cover = Image.open(sys.argv[2]).convert("RGB")

width = round(painting.height * 840 / 500)
left = (painting.width - width) // 2
save(painting.crop((left, 0, left + width, painting.height)).resize((840, 500), Image.LANCZOS), "livearea/contents/bg.png")

# Square around the X-wing's engines, in 1456x819 painting coordinates.
scale = painting.width / 1456
save(painting.crop(tuple(round(v * scale) for v in (470, 150, 1130, 810))).resize((128, 128), Image.LANCZOS), "icon0.png")

height = round(cover.width * 158 / 280)
save(cover.crop((0, 0, cover.width, height)).resize((280, 158), Image.LANCZOS), "livearea/contents/startup.png")
