#!/usr/bin/env python3
"""Regenerate the dark placeholder images for the Flutter web UI dark mode.

The dark mode itself is generated at runtime by src/slic3r/GUI/FlutterWebDark.cpp,
which remaps the colors of the compiled Flutter bundle and its SVG icons. Raster
images cannot be remapped there, so the few placeholder images drawn on a light
background are converted here, once, and committed to resources/web/flutter_web_dark/.
manifest.txt records the CRC-32 of each source image; FlutterWebDark.cpp only uses a
dark copy while its source is unchanged, so rerun this script after a flutter_web
update changes one of them.

Usage: flutter_web_dark.py [resources/web/flutter_web] [resources/web/flutter_web_dark]
Needs Pillow.
"""

import sys
import zlib
from pathlib import Path

from PIL import Image

# Keep in sync with DARK_IMAGES in FlutterWebDark.cpp (paths below assets/assets/).
DARK_IMAGES = [
    "images/controlDefault.png", "images/deviceNotConnected.webp",
    "images/printtaskDefault.png", "images/defaultGcode.webp",
    "images/gcodeCover.png", "images/defaultEmpty.png",
]

# Keep in sync with neutral_curve() in FlutterWebDark.cpp: (light, dark) on 0..255.
NEUTRAL_CURVE = [(0, 255), (36, 228), (54, 190), (107, 135), (128, 120),
                 (158, 106), (172, 100), (200, 86), (220, 72), (235, 62),
                 (245, 54), (255, 45)]


def neutral_curve(v):
    for (x0, y0), (x1, y1) in zip(NEUTRAL_CURVE, NEUTRAL_CURVE[1:]):
        if v <= x1:
            return y0 + (y1 - y0) * (v - x0) / (x1 - x0)
    return NEUTRAL_CURVE[-1][1]


def map_pixel(p, cache={}):
    """Remap neutral (grey) pixels only, so tinted artwork keeps its colors.

    Only the smooth curve is used here (not the exact Prepare table), which keeps
    gradients free of banding.
    """
    if p not in cache:
        r, g, b, a = p
        if max(r, g, b) - min(r, g, b) <= 24:
            v = round(neutral_curve(0.299 * r + 0.587 * g + 0.114 * b))
            r, g, b = v, v, min(255, v + (4 if v < 128 else 1))
        cache[p] = (r, g, b, a)
    return cache[p]


def main():
    root = Path(__file__).resolve().parent.parent
    web = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "resources/web/flutter_web"
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "resources/web/flutter_web_dark"
    lines = []
    for rel in DARK_IMAGES:
        src = web / "assets" / "assets" / rel
        img = Image.open(src)
        rgba = img.convert("RGBA")
        data = rgba.get_flattened_data() if hasattr(rgba, "get_flattened_data") else rgba.getdata()
        rgba.putdata([map_pixel(p) for p in data])
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        out = rgba if img.mode == "RGBA" else rgba.convert("RGB")
        if dst.suffix == ".webp":
            out.save(dst, lossless=True)
        else:
            out.save(dst, optimize=True)
        lines.append("%08x %s\n" % (zlib.crc32(src.read_bytes()), rel))
    (out_dir / "manifest.txt").write_text("".join(lines), encoding="utf-8")
    print("flutter_web_dark: wrote %d images to %s" % (len(lines), out_dir))


if __name__ == "__main__":
    main()
