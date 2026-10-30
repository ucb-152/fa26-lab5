#!/usr/bin/env python3
"""Save, test, and benchmark a version of the lab without overwriting experiments."""

import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
from datetime import datetime, timezone

from compare_results import compare
from generate_image import generate

PROFILE_RUNS = 40
PROFILE_SECONDS = 10

PATTERNS = {"curves", "vertical-curves", "straight", "shapes", "texture"}
SOURCE_ITEMS = ("src", "include", "app", "tests", "scripts", "examples", "Makefile", "workloads.yaml")


def select_workloads(root, preset, input_path):
    if input_path:
        path = (root / input_path).resolve()
        if not path.is_file():
            raise ValueError(f"Input does not exist: {path}")
        return [{"name": "custom", "input": str(path)}]
    try:
        import yaml
    except ImportError as error:
        raise ValueError("PyYAML is missing. Rerun bash scripts/setup.sh on your instance.") from error
    try:
        config = yaml.safe_load((root / "workloads.yaml").read_text())
    except yaml.YAMLError as error:
        raise ValueError(f"Invalid workloads.yaml: {error}") from error
    presets = config.get("presets") if isinstance(config, dict) else None
    if not isinstance(presets, dict) or not presets:
        raise ValueError("workloads.yaml must contain a presets mapping.")
    if preset not in presets:
        raise ValueError(f"Unknown workload '{preset}'. Choose from: {', '.join(map(str, presets))}")
    entries = presets[preset]
    if not isinstance(entries, list) or not entries:
        raise ValueError(f"Preset '{preset}' must contain at least one image.")
    selected = []
    for item in entries:
        if not isinstance(item, dict) or set(item) != {"pattern", "width", "height"}:
            raise ValueError("Each workload needs pattern, width, and height.")
        pattern, width, height = item["pattern"], item["width"], item["height"]
        if not isinstance(pattern, str) or pattern not in PATTERNS or any(type(n) is not int or not 5 <= n <= 8192 for n in (width, height)):
            raise ValueError(f"Invalid workload: {item}; use a known pattern and dimensions from 5 to 8192.")
        selected.append(dict(item, name=f"{pattern}-{width}x{height}"))
    if len({item["name"] for item in selected}) != len(selected):
        raise ValueError(f"Preset '{preset}' contains duplicate images.")
    return selected


def save_source(root, experiment):
    source = experiment / "source"
    source.mkdir()
    for name in SOURCE_ITEMS:
        path = root / name
        if path.is_dir():
            shutil.copytree(path, source / name, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        else:
            shutil.copy2(path, source / name)


def saved_versions(experiments):
    versions = []
    for path in sorted(experiments.iterdir(), key=lambda p: int(p.name) if p.name.isdigit() else -1):
        if path.is_dir() and path.name.isdigit() and (path / "experiment.json").exists():
            info = json.loads((path / "experiment.json").read_text())
            if info.get("status") == "passed":
                versions.append(path)
    return versions


def extra_versions(previous, requested):
    """Resolve explicit experiment IDs against successful saved versions."""
    if not requested.strip():
        return []
    available = {int(path.name): path for path in previous}
    selected = []
    for token in requested.split(","):
        token = token.strip()
        if not token.isascii() or not token.isdecimal() or int(token) < 1:
            raise ValueError("COMPARE must be comma-separated positive experiment numbers, e.g. COMPARE=002,005.")
        version = available.get(int(token))
        if version is None:
            raise ValueError(f"COMPARE experiment {token} is not a successful saved experiment.")
        if version not in selected:
            selected.append(version)
    return selected


def run_logged(command, log, cwd=None, env=None):
    result = subprocess.run([str(part) for part in command], cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise ValueError(f"Command failed (exit {result.returncode}). See {log}")
    return result.stdout


def parse_timings(output):
    lines = output.strip().splitlines()
    header = lines[0].split() if lines else []
    scales = {"(s)": 1, "(ms)": 1e-3, "(us)": 1e-6, "(ns)": 1e-9}
    if len(header) < 3 or header[2] not in scales:
        raise ValueError("The executable did not print a recognized timing table.")
    scale = scales[header[2]]
    stages = {}
    for line in lines[1:]:
        name, *values = line.split()
        stats = [float(value) * scale for value in values]
        if len(stats) == 1:
            stats = [stats[0], 0, stats[0], stats[0]]
        if len(stats) != 4 or any(not math.isfinite(v) or v < 0 for v in stats):
            raise ValueError("Invalid timing statistics.")
        stages[name] = dict(zip(("mean", "stdev", "min", "max"), stats))
    if "total" not in stages:
        raise ValueError("Missing total kernel time.")
    return stages


def collect_profile(experiment, name):
    """Record one fixed workload batch, then generate its APerf report."""
    folder = experiment / "profiles" / name
    folder.mkdir(parents=True)
    with (folder / "record.log").open("w") as log:
        recorder = subprocess.Popen(
            ["aperf", "record", "-r", str(folder / "recording"),
             "-p", str(PROFILE_SECONDS), "--profile"],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            run_logged([experiment / "bin/pgm2svg", "--benchmark", PROFILE_RUNS,
                        experiment / "inputs" / f"{name}.pgm", folder / "drawing"],
                       folder / "workload.log")
            if recorder.wait() != 0:
                raise ValueError(f"APerf recording failed. See {folder / 'record.log'}")
        finally:
            if recorder.poll() is None:
                os.killpg(recorder.pid, signal.SIGINT)
                try:
                    recorder.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(recorder.pid, signal.SIGKILL)
                    recorder.wait()
    run_logged(["aperf", "report", "-r", folder / "recording", "-n", folder / "report"],
               folder / "report.log")
    report = folder / "report/index.html"
    if not report.is_file():
        raise ValueError(f"APerf did not create {report}. See {folder / 'report.log'}")
    return report


def benchmark(root, workloads, runs=7, note="", preset="directed", profile=False, compare_versions=""):
    experiments = root / "experiments"
    experiments.mkdir(exist_ok=True)
    # A second invocation must not select the same ID or disturb measurements.
    with (experiments / ".lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("Another benchmark is running in this repository.") from error
        previous = saved_versions(experiments)
        extra = extra_versions(previous, compare_versions)
        number = 1 + max((int(p.name) for p in experiments.iterdir() if p.name.isdigit()), default=0)
        current = experiments / f"{number:03d}"
        current.mkdir()
        info = {"status": "running", "created": datetime.now(timezone.utc).isoformat(),
                "note": note, "preset": preset, "runs": runs, "workloads": workloads,
                "profile": profile}
        manifest = current / "experiment.json"
        report = []

        def say(message):
            print(message, flush=True)
            report.append(message)
            (current / "results.txt").write_text("\n".join(report) + "\n")

        def persist():
            manifest.write_text(json.dumps(info, indent=2) + "\n")

        persist()
        say(f"Experiment {current.name}: {current.relative_to(root)}")
        if note:
            say(f"Note: {note}")
        try:
            save_source(root, current)
            inputs = current / "inputs"
            inputs.mkdir()
            for item in workloads:
                path = inputs / f"{item['name']}.pgm"
                if "input" in item:
                    shutil.copy2(item["input"], path)
                else:
                    width, height = item["width"], item["height"]
                    path.write_bytes(f"P5\n{width} {height}\n255\n".encode() + generate(width, height, item["pattern"]))
            environment = os.environ.copy()
            environment.pop("MAKEFLAGS", None)
            environment.pop("MAKEOVERRIDES", None)
            command = ["make", "BUILD=../bin"]
            for key in ("CC", "CFLAGS", "CPPFLAGS", "LDFLAGS", "LDLIBS"):
                if f"LAB_BENCH_{key}" in environment:
                    command.append(f"{key}={environment[f'LAB_BENCH_{key}']}")
            command += ["../bin/pgm2svg", "../bin/dump_result", "../bin/test_edge_paths"]
            info["build_command"] = command
            persist()
            say("Building saved source and running tests...")
            run_logged(command, current / "build.log", current / "source", environment)
            for script, arguments in (("check_outputs", ["../bin"]),
                                      ("test_results", ["../bin"]),
                                      ("test_benchmark", [])):
                run_logged([sys.executable, f"tests/{script}.py", *arguments],
                           current / f"{script}.log", current / "source", environment)
            versions = {"current": current}
            if previous:
                versions["starter"] = previous[0]
                if previous[-1] != previous[0]:
                    versions["previous"] = previous[-1]
                say(f"Comparing with starter {previous[0].name} and previous {previous[-1].name}.")
            else:
                say("First successful run establishes the starter version. Run this before editing code.")
            for version in extra:
                if version not in versions.values():
                    versions[f"version-{version.name}"] = version
                    say(f"Also comparing with experiment {version.name}.")
            results = current / "results"
            results.mkdir()
            # Finish all correctness checks before collecting any timings.
            for item in workloads:
                folder = results / item["name"]
                folder.mkdir()
                for label, version in versions.items():
                    run_logged([version / "bin/dump_result", inputs / f"{item['name']}.pgm", folder / f"{label}.dump"],
                               folder / f"{label}-check.txt")
                    if label != "current":
                        mismatch = compare(folder / "current.dump", folder / f"{label}.dump")
                        if mismatch:
                            raise ValueError(f"Correctness failed for {item['name']} against {label} {version.name}: {mismatch}")
                # Keep the check result, not large transient binary dumps.
                for dump in folder.glob("*.dump"):
                    dump.unlink()
            say("Correctness checks passed.")
            info["comparisons"] = {label: version.name for label, version in versions.items()}
            info["timings"] = {}
            for item in workloads:
                name = item["name"]
                folder = results / name
                timings = {}
                for label, version in versions.items():
                    output = run_logged([version / "bin/pgm2svg", "--benchmark", runs,
                                         inputs / f"{name}.pgm", folder / label], folder / f"{label}-timings.txt")
                    timings[label] = parse_timings(output)
                info["timings"][name] = timings
                say(f"\n{name}: current stage timings\n{(folder / 'current-timings.txt').read_text().rstrip()}")
                say("Version       Mean (ms)   Stdev (ms)   Speedup of current")
                after = timings["current"]["total"]["mean"]
                for label, stages in timings.items():
                    total = stages["total"]
                    speedup = f"{total['mean'] / after:.3f}x" if after > 0 and label != "current" else "—"
                    say(f"{label:12} {total['mean'] * 1000:10.3f} {total['stdev'] * 1000:12.3f} {speedup:>20}")
                persist()
            if profile:
                info["profiling"] = {"runs": PROFILE_RUNS, "seconds": PROFILE_SECONDS, "status": "running"}
                persist()
                say(f"\nCollecting APerf profiles ({PROFILE_RUNS} iterations per input)...")
                try:
                    for item in workloads:
                        report_path = collect_profile(current, item["name"])
                        say(f"APerf report: {report_path.relative_to(root)}")
                    info["profiling"]["status"] = "passed"
                except (ValueError, OSError) as error:
                    # Profiling failures must not discard valid correctness/timing results.
                    info["profiling"]["status"] = "failed"
                    info["profiling"]["error"] = str(error)
                    info["status"] = "passed"
                    say(f"Profiling failed: {error}. Benchmark results are saved.")
                    raise
            info["status"] = "passed"
            say(f"\nSaved source, executables, inputs, and results in {current.relative_to(root)}")
        except BaseException as error:
            if info.get("status") != "passed":
                info["status"] = "failed"
            info["error"] = str(error) or type(error).__name__
            say(f"FAILED: {info['error']}\nSaved attempt: {current.relative_to(root)}")
            raise
        finally:
            persist()
        return current


def main():
    root = Path(__file__).resolve().parents[1]
    try:
        runs = int(os.environ.get("LAB_BENCH_RUNS", "7"))
        if runs < 1:
            raise ValueError("RUNS must be a positive integer.")
        preset = os.environ.get("LAB_BENCH_WORKLOAD", "directed")
        workloads = select_workloads(root, preset, os.environ.get("LAB_BENCH_INPUT", ""))
        profile = os.environ.get("LAB_BENCH_PROFILE", "0")
        if profile not in {"0", "1"}:
            raise ValueError("PROFILE must be 0 or 1.")
        benchmark(root, workloads, runs, os.environ.get("LAB_BENCH_NOTE", ""), preset, profile == "1",
                  compare_versions=os.environ.get("LAB_BENCH_COMPARE", ""))
    except (ValueError, OSError) as error:
        print(f"benchmark: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
