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
import tempfile
from datetime import datetime, timezone

from check import compare
from generate_image import generate

PROFILE_SECONDS = 10
STARTER_REPOSITORY = "https://github.com/ucb-152/fa26-lab5"

PATTERNS = {"curves", "vertical-curves", "straight", "shapes", "texture"}
SOURCE_ITEMS = ("src", "include", "app", "scripts", "examples", "Makefile", "workloads.yaml")


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
            raise ValueError(f"COMPARE experiment {token} is not a successful experiment.")
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


def prepare_starter(experiments, command, environment):
    starter = experiments / "starter"
    starter.mkdir(exist_ok=True)
    source = starter / "source"
    if not source.exists():
        with tempfile.TemporaryDirectory(prefix=".starter-", dir=experiments) as temporary:
            checkout = Path(temporary) / "source"
            run_logged(["git", "clone", "--depth", "1", STARTER_REPOSITORY, checkout],
                       starter / "clone.log", env=environment)
            checkout.rename(source)
    run_logged(command, starter / "build.log", source, environment)


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


def format_table(headers, rows, left_columns=1):
    """Align plain-text tables without depending on terminal escape sequences."""
    widths = [max(len(row[i]) for row in [headers, *rows]) for i in range(len(headers))]

    def line(row):
        return "  ".join(value.ljust(width) if i < left_columns else value.rjust(width)
                         for i, (value, width) in enumerate(zip(row, widths))).rstrip()

    return "\n".join([line(headers), *(line(row) for row in rows)])


def speedup(before, after):
    return f"{before / after:.3f}x" if after > 0 else "n/a"


def workload_report(timings, versions, runs):
    """Put the overall comparison before the current version's stage breakdown."""
    current = timings["current"]
    after = current["total"]["mean"]
    references = len(timings) > 1
    headers = ["Version", "Exp.", "Mean (ms)" if runs > 1 else "Time (ms)", "SD (ms)"]
    if references:
        headers += ["Current speedup"]
    rows = []
    for label, stages in timings.items():
        total = stages["total"]
        row = [label, "-" if label == "starter" else versions[label].name, f"{total['mean'] * 1000:.3f}",
               f"{total['stdev'] * 1000:.3f}" if runs > 1 else "-"]
        if references:
            row += [speedup(total["mean"], after) if label != "current" else "-"]
        rows.append(row)
    sections = ["Total kernel time", format_table(headers, rows, left_columns=2)]

    rows = []
    for stage, stats in current.items():
        if stage == "total":
            continue
        share = f"{stats['mean'] / after:.1%}" if after > 0 else "n/a"
        rows.append([stage, f"{stats['mean'] * 1000:.3f}",
                     f"{stats['stdev'] * 1000:.3f}" if runs > 1 else "-", share])
    sections += ["", "Current stage breakdown",
                 format_table(["Stage", headers[2], "SD (ms)", "% of total"], rows)]
    return "\n".join(sections)


def summary_report(all_timings, versions, runs):
    references = [label for label in versions if label != "current"]
    headers = ["Workload", "Mean (ms)" if runs > 1 else "Time (ms)", "SD (ms)"]
    headers += [f"vs {versions[label].name}" for label in references]
    rows = []
    for name, timings in all_timings.items():
        total = timings["current"]["total"]
        rows.append([name, f"{total['mean'] * 1000:.3f}",
                     f"{total['stdev'] * 1000:.3f}" if runs > 1 else "-",
                     *(speedup(timings[label]["total"]["mean"], total["mean"])
                       for label in references)])
    return format_table(headers, rows)


def collect_profile(experiment, name, runs):
    """Record a workload batch with the timing run count, then generate its APerf report."""
    folder = experiment / "profiles" / name
    folder.mkdir(parents=True)
    with (folder / "record.log").open("w") as log:
        recorder = subprocess.Popen(
            ["aperf", "record", "-r", str(folder / "recording"),
             "-p", str(PROFILE_SECONDS), "--profile"],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            run_logged([experiment / "bin/pgm2svg", "--benchmark", runs,
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


def benchmark(root, workloads, runs=None, note="", preset="quick", profile=False, compare_versions=""):
    if runs is None:
        runs = 40 if profile else 7
    if runs < 1:
        raise ValueError("RUNS must be a positive integer.")
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
        versions = {"current": current, "starter": experiments / "starter"}
        if previous:
            versions["previous"] = previous[-1]
        for version in extra:
            if version not in versions.values():
                versions[f"version-{version.name}"] = version

        say(f"Experiment {current.name} ({runs} {'runs' if runs != 1 else 'run'})")
        if note:
            say(f"Note: {note}")
        try:
            say("Preparing inputs...")
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
            command += ["../bin/pgm2svg", "../bin/test_pipeline"]
            info["build_command"] = command
            persist()
            say("Building...")
            run_logged(command, current / "build.log", current / "source", environment)
            prepare_starter(experiments, command, environment)
            say("Running tests...")
            run_logged([sys.executable, "scripts/check.py", "../bin"],
                       current / "check.log", current / "source", environment)
            results = current / "results"
            results.mkdir()
            # Finish all correctness checks before collecting any timings.
            for item in workloads:
                say(f"Checking {item['name']}...")
                folder = results / item["name"]
                folder.mkdir()
                for label, version in versions.items():
                    run_logged([version / "bin/test_pipeline", "--dump",
                                inputs / f"{item['name']}.pgm", folder / f"{label}.dump"],
                               folder / f"{label}-check.txt")
                    if label != "current":
                        mismatch = compare(folder / "current.dump", folder / f"{label}.dump")
                        if mismatch:
                            raise ValueError(f"Correctness failed for {item['name']} against {label} {version.name}: {mismatch}")
                # Keep the check result, not large transient binary dumps.
                for dump in folder.glob("*.dump"):
                    dump.unlink()
            say("Checks passed.")
            info["comparisons"] = {label: version.name for label, version in versions.items()}
            info["timings"] = {}
            for item in workloads:
                name = item["name"]
                say(f"\n{name}")
                folder = results / name
                timings = {}
                for label, version in versions.items():
                    description = label if label == "starter" else f"{label} {version.name}"
                    say(f"Benchmarking {description}...")
                    output = run_logged([version / "bin/pgm2svg", "--benchmark", runs,
                                         inputs / f"{name}.pgm", folder / label], folder / f"{label}-timings.txt")
                    timings[label] = parse_timings(output)
                info["timings"][name] = timings
                say("\n" + workload_report(timings, versions, runs))
                persist()
            if len(workloads) > 1:
                say("\nSummary")
                say(summary_report(info["timings"], versions, runs))
            if profile:
                info["profiling"] = {"runs": runs, "seconds": PROFILE_SECONDS, "status": "running"}
                persist()
                try:
                    for item in workloads:
                        say(f"\nProfiling {item['name']}...")
                        report_path = collect_profile(current, item["name"], runs)
                        say(f"APerf report: {report_path.relative_to(root)}")
                    info["profiling"]["status"] = "passed"
                except (ValueError, OSError) as error:
                    # Profiling failures must not discard valid correctness/timing results.
                    info["profiling"]["status"] = "failed"
                    info["profiling"]["error"] = str(error)
                    info["status"] = "passed"
                    raise
            info["status"] = "passed"
            say(f"\nResults: {current.relative_to(root) / 'results.txt'}")
        except BaseException as error:
            if info.get("status") != "passed":
                info["status"] = "failed"
            info["error"] = str(error) or type(error).__name__
            if info["status"] == "passed":
                say(f"\nProfiling failed: {info['error']}")
                say(f"Benchmark passed. Results: {current.relative_to(root) / 'results.txt'}")
            else:
                say(f"\nError: {info['error']}")
                say(f"Failed experiment: {current.relative_to(root)}/")
            raise
        finally:
            persist()
        return current


def main():
    root = Path(__file__).resolve().parents[1]
    try:
        runs = os.environ.get("LAB_BENCH_RUNS")
        if runs is not None:
            runs = int(runs)
        preset = os.environ.get("LAB_BENCH_WORKLOAD", "quick")
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
