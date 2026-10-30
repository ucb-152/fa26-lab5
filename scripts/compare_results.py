#!/usr/bin/env python3
"""Compare complete pipeline results from two builds on the same image and parameters."""

import argparse
from itertools import zip_longest
from pathlib import Path
import struct
import subprocess
import tempfile


def fields(path):
    with path.open("rb") as stream:
        if stream.read(8) != b"L5OUT001":
            raise ValueError(f"Invalid result format: {path}")

        def value():
            data = stream.read(8)
            if len(data) != 8:
                raise ValueError(f"Truncated result: {path}")
            return struct.unpack("<Q", data)[0]

        sizes = {}
        for name in ("width", "height", "edge_pixel_count", "path_count", "point_count", "vertex_count"):
            sizes[name] = value()
            yield name, sizes[name]
        for i in range(sizes["edge_pixel_count"]):
            for name in ("x", "y", "strength", "neighbor_mask", "visited_mask", "degree"):
                yield f"edge_pixels[{i}].{name}", value()
        for i in range(sizes["width"] * sizes["height"]):
            yield f"pixel_to_edge[{i}]", value()
        for i in range(sizes["path_count"]):
            for name in ("first_point", "point_count", "closed", "retained", "first_vertex", "vertex_count"):
                yield f"paths[{i}].{name}", value()
        for name, count in (("point_ids", "point_count"), ("vertex_ids", "vertex_count")):
            for i in range(sizes[count]):
                yield f"{name}[{i}]", value()
        if stream.read(1):
            raise ValueError(f"Unexpected data after result: {path}")


def compare(reference, modified):
    left, right = fields(reference), fields(modified)
    try:
        for a, b in zip_longest(left, right):
            if a != b:
                if a and b and a[0] == b[0]:
                    return f"Mismatch at {a[0]}: reference={a[1]}, modified={b[1]}"
                return f"Result structure differs: reference={a}, modified={b}"
    finally:
        left.close()
        right.close()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="Saved reference directory")
    parser.add_argument("modified", type=Path, help="Modified build directory")
    parser.add_argument("input", type=Path, help="PGM input image")
    parser.add_argument("parameters", nargs="*", help="Optional LOW HIGH MIN_LENGTH MIN_STRENGTH TOLERANCE")
    args = parser.parse_args()
    if len(args.parameters) not in (0, 5):
        parser.error("Supply either no parameters or all five pipeline parameters.")
    try:
        with tempfile.TemporaryDirectory(prefix="lab5-results-") as temporary:
            outputs = []
            for label, build in (("reference", args.reference), ("modified", args.modified)):
                binary = build.resolve() / "dump_result"
                if not binary.is_file():
                    raise ValueError(f"Missing {binary}; build this version with the updated Makefile first.")
                output = Path(temporary) / label
                subprocess.run([str(binary), str(args.input.resolve()), str(output), *args.parameters], check=True)
                outputs.append(output)
            mismatch = compare(*outputs)
            if mismatch:
                parser.exit(1, mismatch + "\n")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    print("Pipeline results match (all fields, including ordering and IDs).")


if __name__ == "__main__":
    main()
