#!/usr/bin/env python3
"""Convert a raw RGBA32F reference frame to a tone-mapped PNG preview.

The capture writes tightly packed little-endian float32 pixels:
    (height, width, 4) = RGB radiance + alpha coverage.

Examples:
    python raw_reference_to_png.py reference_000000_pose_000000.bin preview.png
    python raw_reference_to_png.py reference_000000_pose_000000.bin preview.png \
        --width 1920 --height 1080 --exposure 0.5 --tonemap aces --no-alpha
"""

from __future__ import annotations

import argparse
import sys

import numpy as np
from PIL import Image


def reinhard(x: np.ndarray) -> np.ndarray:
    return x / (1.0 + x)


def aces_narkowicz(x: np.ndarray) -> np.ndarray:
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Raw RGBA32F reference -> PNG preview")
    parser.add_argument("input", help="input .bin")
    parser.add_argument("output", help="output .png")
    parser.add_argument("--width", type=int, default=2560)
    parser.add_argument("--height", type=int, default=1440)
    parser.add_argument("--exposure", type=float, default=1.0)
    parser.add_argument("--tonemap", choices=("reinhard", "aces", "linear"), default="reinhard")
    parser.add_argument("--gamma", type=float, default=2.2)
    parser.add_argument("--no-alpha", action="store_true", help="write RGB only (ignore coverage alpha)")
    parser.add_argument("--scale", type=float, default=1.0, help="resize the preview after tone mapping")
    args = parser.parse_args()

    expected = args.width * args.height * 4
    data = np.fromfile(args.input, dtype="<f4")
    if data.size != expected:
        print(
            f"size mismatch: got {data.size} floats, expected {expected} "
            f"for {args.width}x{args.height}x4",
            file=sys.stderr,
        )
        return 1

    image = data.reshape(args.height, args.width, 4)
    rgb = np.maximum(image[..., :3].astype(np.float64) * args.exposure, 0.0)

    if args.tonemap == "reinhard":
        rgb = reinhard(rgb)
    elif args.tonemap == "aces":
        rgb = aces_narkowicz(rgb)
    else:
        rgb = np.clip(rgb, 0.0, 1.0)

    if args.gamma > 0.0:
        rgb = np.power(np.clip(rgb, 0.0, 1.0), 1.0 / args.gamma)

    rgb8 = np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)

    if args.no_alpha:
        out = rgb8
    else:
        alpha = np.clip(image[..., 3], 0.0, 1.0)
        alpha8 = (alpha * 255.0 + 0.5).astype(np.uint8)
        out = np.dstack([rgb8, alpha8])

    preview = Image.fromarray(out, mode="RGB" if args.no_alpha else "RGBA")
    if args.scale != 1.0:
        preview = preview.resize(
            (max(1, int(preview.width * args.scale)), max(1, int(preview.height * args.scale))),
            Image.LANCZOS,
        )
    preview.save(args.output)
    print(f"wrote {args.output} ({preview.width}x{preview.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
