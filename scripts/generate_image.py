#!/usr/bin/env python3
"""Generate single-image lab workloads without third-party image libraries."""

import argparse
import math
from pathlib import Path


def generate(width, height, pattern):
    pixels = bytearray(width * height)
    state = 1
    wave = [round(12 * math.sin(x / 35)) for x in range(width)]
    vertical_wave = [round(12 * math.sin(y / 35)) for y in range(height)]
    for y in range(height):
        for x in range(width):
            if pattern == "straight":
                # Long straight paths check linear scaling of block processing.
                value = 220 if (y // 32) % 2 else 30
            elif pattern == "vertical-curves":
                # Paths cross rows and gather edge_pixels interleaved with other paths.
                value = 220 if ((x + vertical_wave[y]) // 32) % 2 else 30
            elif pattern == "curves":
                # Long gently winding boundaries; useful for path reductions.
                value = 220 if ((y + wave[x]) // 32) % 2 else 30
            elif pattern == "shapes":
                # Separated disks and rectangles provide loops and corners.
                cell_x, cell_y = x % 96, y % 96
                if (x // 96 + y // 96) % 2:
                    inside = (cell_x - 48) ** 2 + (cell_y - 48) ** 2 <= 30 ** 2
                else:
                    inside = 18 <= cell_x < 78 and 24 <= cell_y < 72
                value = 220 if inside else 30
            else:
                # Fixed-seed texture produces many short, irregular paths.
                state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
                value = state >> 24
            pixels[y * width + x] = value
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=1024)
    parser.add_argument("--pattern", choices=["curves", "vertical-curves", "straight", "shapes", "texture"],
                        default="shapes")
    args = parser.parse_args()
    if not (5 <= args.width <= 8192 and 5 <= args.height <= 8192):
        parser.error("dimensions must be between 5 and 8192")
    pixels = generate(args.width, args.height, args.pattern)
    args.output.write_bytes(f"P5\n{args.width} {args.height}\n255\n".encode() + pixels)
    print(f"Wrote {args.width} x {args.height} {args.pattern} image to {args.output}")


if __name__ == "__main__":
    main()
