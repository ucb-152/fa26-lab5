#!/usr/bin/env python3
"""Single test entry point: C unit checks and SVG output validation."""

from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def fixtures():
    """Small deterministic images for application integration tests."""
    width, height = 37, 29
    noise = bytearray()
    state = 1
    for _ in range(width * height):
        state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
        noise.append(state >> 24)
    cases = {
        "flat": (width, height, bytes([127]) * (width * height)),
        "rectangle": (
            width, height,
            bytes(220 if 7 <= x < 28 and 6 <= y < 23 else 20
                  for y in range(height) for x in range(width)),
        ),
        "ramp": (width, height, bytes(x * 255 // (width - 1)
                                      for y in range(height) for x in range(width))),
        "noise": (width, height, bytes(noise)),
        "minimum": (5, 5, bytes(range(0, 250, 10))),
    }
    # This checked-in example has a simple, canonical P5 header.
    valve = Path(__file__).resolve().parent.parent / "examples/valve.pgm"
    data = valve.read_bytes()
    header = b"P5\n500 375\n255\n"
    assert data.startswith(header) and len(data) == len(header) + 500 * 375
    cases["valve"] = (500, 375, data[len(header):])
    return cases


def run(*arguments, success=True):
    result = subprocess.run([str(argument) for argument in arguments],
                            capture_output=True, text=True)
    if success and result.returncode != 0:
        raise AssertionError(result.stderr)
    if not success and result.returncode == 0:
        raise AssertionError("Invalid invocation unexpectedly succeeded")
    return result


def check_timings(result, count):
    lines = result.stdout.splitlines()
    header = lines[0].split()
    unit = header[2]
    assert unit in {"(s)", "(ms)", "(us)", "(ns)"}
    expected_header = ["Stage", "Time", unit] if count == 1 else [
        "Stage", "Mean", unit, "Stdev", unit, "Min", unit, "Max", unit]
    assert header == expected_header
    rows = [line.split() for line in lines[1:]]
    assert [row[0] for row in rows] == [
        "setup", "blur", "sobel", "gradient", "suppression", "threshold",
        "hysteresis", "graph", "trace", "measure", "simplify", "cleanup", "total"]
    for stage, *stats in rows:
        if count == 1:
            assert len(stats) == 1 and float(stats[0]) >= 0, stage
            continue
        mean, stdev, minimum, maximum = map(float, stats)
        assert 0 <= minimum <= mean <= maximum and stdev >= 0, stage
        if count == 2:
            assert abs(stdev - (maximum - minimum) / 2) <= 0.0011, stage


def main():
    binaries = Path(sys.argv[1]).resolve()
    run(binaries / "test_edge_paths")
    namespace = {"svg": "http://www.w3.org/2000/svg"}
    with tempfile.TemporaryDirectory(prefix="edge-path-test-") as directory:
        temporary = Path(directory)
        for name, (width, height, pixels) in fixtures().items():
            source = temporary / f"{name}.pgm"
            source.write_bytes(f"P5\n{width} {height}\n255\n".encode() + pixels)
            for low, high in [(0, 0), (22, 45), (100, 200)]:
                prefix = temporary / "drawing"
                arguments = [source, prefix, low, high, 0, 0, 1]
                result = run(binaries / "pgm2svg", *arguments)
                svg = ET.parse(f"{prefix}.svg").getroot()
                assert svg.attrib["viewBox"] == f"0 0 {width} {height}"
                drawn = svg.findall(".//svg:path", namespace) + svg.findall(".//svg:circle", namespace)
                assert all(element.attrib["id"].startswith("path-") for element in drawn)
                assert not Path(f"{prefix}.edges.pgm").exists()
                assert not Path(f"{prefix}.paths.csv").exists()
                check_timings(result, 1)
                before = Path(f"{prefix}.svg").read_bytes()
                run(binaries / "pgm2svg", *arguments)
                after = Path(f"{prefix}.svg").read_bytes()
                assert before == after, "Output must be deterministic"

        source = temporary / "noise.pgm"
        run(binaries / "pgm2svg", source, temporary / "defaults")
        run(binaries / "pgm2svg", source, temporary / "explicit", 22, 45, 8, 0, 1)
        assert (temporary / "defaults.svg").read_bytes() == (temporary / "explicit.svg").read_bytes()
        for count in [1, 2, 3]:
            result = run(binaries / "pgm2svg", "--benchmark", count, source, temporary / "bench")
            check_timings(result, count)
            assert (temporary / "defaults.svg").read_bytes() == (temporary / "bench.svg").read_bytes()
        for count in ["0", "-1", "1.5", "abc", "999999999999999999999"]:
            run(binaries / "pgm2svg", "--benchmark", count, source, temporary / "bad", success=False)
        run(binaries / "pgm2svg", "--benchmark", success=False)
        run(binaries / "pgm2svg", source, temporary / "filtered", 22, 45, 1000000, 0, 1)
        filtered = ET.parse(temporary / "filtered.svg").getroot()
        assert not filtered.findall(".//svg:path", namespace)
        for parameters in [(100, 20, 8, 0, 1), (0.5, 45, 8, 0, 1),
                           (22, 45, -1, 0, 1), (22, 45, 8, 0, "nan"),
                           (22, 45, 8, 0, "inf"), (22, 45, 8, 0, "1e300")]:
            run(binaries / "pgm2svg", source, temporary / "bad", *parameters, success=False)
        run(binaries / "pgm2svg", source, temporary / "bad", 22, success=False)
        (temporary / "invalid.pgm").write_bytes(b"P5\n-1 5\n255\n")
        run(binaries / "pgm2svg", temporary / "invalid.pgm", temporary / "bad", success=False)
        (temporary / "small.pgm").write_bytes(b"P5\n4 5\n255\n" + bytes(20))
        run(binaries / "pgm2svg", temporary / "small.pgm", temporary / "bad", success=False)
        (temporary / "ascii.pgm").write_text("P2\n# comment\n5 5\n15\n" + "7 " * 25)
        run(binaries / "pgm2svg", temporary / "ascii.pgm", temporary / "ascii")
    print("Output tests passed: SVG-only output, repeatability, and arguments.")


if __name__ == "__main__":
    main()
