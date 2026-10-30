"""Exercise experiment preservation, comparisons, and workload selection."""

from contextlib import redirect_stdout
import fcntl
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark as bench

TABLE = "Stage Mean (ms) Stdev (ms) Min (ms) Max (ms)\nblur 1 0 1 1\ntotal 2 0.1 1.9 2.1\n"


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in bench.SOURCE_ITEMS:
            source = ROOT / name
            target = self.root / name
            if source.is_dir():
                target.mkdir()
                (target / "saved.txt").write_text("original")
            else:
                target.write_bytes(source.read_bytes())
        self.workloads = [{"name": "shapes-5x5", "pattern": "shapes", "width": 5, "height": 5}]

    def fake_run(self, command, log, cwd=None, env=None):
        if command[0] == "make":
            binary = cwd.parent / "bin"
            binary.mkdir()
            for name in ("pgm2svg", "dump_result", "test_edge_paths"):
                (binary / name).write_text("saved executable")
            text = "tests passed\n"
        elif str(command[1]).startswith("tests/"):
            text = "tests passed\n"
        elif "--benchmark" in map(str, command):
            self.measured.append(tuple(map(str, command)))
            text = TABLE
        else:
            Path(command[-1]).write_bytes(b"result")
            text = ""
        log.write_text(text)
        return text

    def run_experiment(self, **options):
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "compare", return_value=None):
            return bench.benchmark(self.root, self.workloads, 7, "reuse row offsets", **options)

    def test_preserves_every_version_and_reruns_saved_binaries(self):
        self.measured = []
        first = self.run_experiment()
        (self.root / "src/saved.txt").write_text("changed")
        second = self.run_experiment()
        third = self.run_experiment()
        self.assertEqual([p.name for p in (first, second, third)], ["001", "002", "003"])
        self.assertEqual((first / "source/src/saved.txt").read_text(), "original")
        self.assertEqual((second / "source/src/saved.txt").read_text(), "changed")
        info = json.loads((third / "experiment.json").read_text())
        self.assertEqual(info["comparisons"], {"current": "003", "starter": "001", "previous": "002"})
        self.assertEqual(info["note"], "reuse row offsets")
        self.assertEqual(len(self.measured), 6)  # first, then current+starter, then all three
        for command in self.measured[-3:]:
            self.assertEqual(command[3], str(third / "inputs/shapes-5x5.pgm"))
        self.assertTrue((third / "results.txt").exists())

    def test_failed_build_preserves_source_and_is_not_a_baseline(self):
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=ValueError("build failed")):
            with self.assertRaisesRegex(ValueError, "build failed"):
                bench.benchmark(self.root, self.workloads)
        failed = self.root / "experiments/001"
        self.assertTrue((failed / "source/src/saved.txt").exists())
        self.assertEqual(json.loads((failed / "experiment.json").read_text())["status"], "failed")
        self.measured = []
        good = self.run_experiment()
        self.assertEqual(good.name, "002")
        self.assertEqual(json.loads((good / "experiment.json").read_text())["comparisons"], {"current": "002"})

    def test_checks_run_on_saved_build_and_failure_stops_before_timing(self):
        self.measured = []
        commands = []

        def fail_result_check(command, log, cwd=None, env=None):
            commands.append((command, cwd))
            if "tests/test_results.py" in command:
                log.write_text("result check failed\n")
                raise ValueError("result check failed")
            return self.fake_run(command, log, cwd, env)

        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=fail_result_check):
            with self.assertRaisesRegex(ValueError, "result check failed"):
                bench.benchmark(self.root, self.workloads)
        failed = self.root / "experiments/001"
        self.assertEqual(commands[0][0][-3:],
                         ["../bin/pgm2svg", "../bin/dump_result", "../bin/test_edge_paths"])
        self.assertEqual(commands[1][0], [sys.executable, "tests/check_outputs.py", "../bin"])
        self.assertEqual(commands[2][0], [sys.executable, "tests/test_results.py", "../bin"])
        self.assertTrue(all(cwd == failed / "source" for _, cwd in commands))
        self.assertEqual(self.measured, [])
        self.assertEqual(json.loads((failed / "experiment.json").read_text())["status"], "failed")
        self.assertEqual((failed / "test_results.log").read_text(), "result check failed\n")

    def test_extra_versions_are_checked_and_rerun_without_duplicates(self):
        self.measured = []
        for _ in range(4):
            self.run_experiment()
        self.measured.clear()
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "compare", return_value=None) as check:
            current = bench.benchmark(self.root, self.workloads, compare_versions=" 002,3,002,001,004 ")
        self.assertEqual(check.call_count, 4)
        info = json.loads((current / "experiment.json").read_text())
        self.assertEqual(info["comparisons"], {
            "current": "005", "starter": "001", "previous": "004",
            "version-002": "002", "version-003": "003"})
        self.assertEqual([Path(command[0]).parents[1].name for command in self.measured],
                         ["005", "001", "004", "002", "003"])
        for command in self.measured:
            self.assertEqual(command[3], str(current / "inputs/shapes-5x5.pgm"))
        report = (current / "results.txt").read_text()
        for label in ("version-002", "version-003"):
            self.assertIn(label, report)
            self.assertIn("1.000x", next(line for line in report.splitlines() if line.startswith(label)))

    def test_invalid_extra_versions_fail_before_creating_experiment(self):
        self.measured = []
        first = self.run_experiment()
        (first / "experiment.json").write_text(json.dumps({"status": "failed"}))
        self.run_experiment()
        history = self.root / "experiments"
        for requested in ("001", "003", "999", "0", "-1", "../002", "2,,2", "2,", "two", "٢"):
            with self.subTest(requested=requested), self.assertRaisesRegex(ValueError, "COMPARE"):
                bench.benchmark(self.root, self.workloads, compare_versions=requested)
            self.assertEqual(sorted(path.name for path in history.iterdir() if path.is_dir()), ["001", "002"])

    def test_extra_version_mismatch_stops_before_any_timings(self):
        self.measured = []
        for _ in range(3):
            self.run_experiment()
        self.measured.clear()
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "compare", side_effect=[None, None, "pixel differs"]):
            with self.assertRaisesRegex(ValueError, "against version-002 002"):
                bench.benchmark(self.root, self.workloads, compare_versions="002")
        self.assertEqual(self.measured, [])
        info = json.loads((self.root / "experiments/004/experiment.json").read_text())
        self.assertEqual(info["status"], "failed")

    def test_main_passes_compare_option(self):
        with patch.dict(bench.os.environ, {"LAB_BENCH_COMPARE": "002,005"}, clear=True), \
                patch.object(bench, "select_workloads", return_value=self.workloads), \
                patch.object(bench, "benchmark") as run:
            self.assertEqual(bench.main(), 0)
        self.assertEqual(run.call_args.kwargs["compare_versions"], "002,005")

    def test_mismatch_stops_before_timing_and_preserves_failure(self):
        self.measured = []
        self.run_experiment()
        self.measured.clear()
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "compare", return_value="pixel differs"):
            with self.assertRaisesRegex(ValueError, "Correctness failed"):
                bench.benchmark(self.root, self.workloads)
        self.assertEqual(self.measured, [])
        info = json.loads((self.root / "experiments/002/experiment.json").read_text())
        self.assertEqual(info["status"], "failed")

    def test_second_benchmark_cannot_run_concurrently(self):
        history = self.root / "experiments"
        history.mkdir()
        with (history / ".lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(ValueError, "Another benchmark"):
                bench.benchmark(self.root, self.workloads)
        self.assertEqual(list(history.glob("[0-9]*")), [])

    def test_presets_validation_and_custom_input(self):
        for preset in ("directed", "quick", "texture", "vertical", "straight", "varied", "borders", "large"):
            self.assertTrue(bench.select_workloads(self.root, preset, ""))
        self.assertEqual(len(bench.select_workloads(self.root, "directed", "")), 3)
        with self.assertRaisesRegex(ValueError, "Unknown workload"):
            bench.select_workloads(self.root, "missing", "")
        (self.root / "custom.pgm").write_bytes(b"P5\n5 5\n255\n" + bytes(25))
        self.assertEqual(bench.select_workloads(self.root, "unused", "custom.pgm")[0]["name"], "custom")
        (self.root / "workloads.yaml").write_text("presets:\n  invalid:\n    - {pattern: shapes, width: 4, height: 5}\n")
        with self.assertRaisesRegex(ValueError, "Invalid workload"):
            bench.select_workloads(self.root, "invalid", "")

    def test_profile_runs_after_all_unprofiled_measurements(self):
        self.measured = []
        def check_profile(experiment, name):
            self.assertEqual(len(self.measured), 1)
            self.assertTrue((experiment / "results" / name / "current-timings.txt").exists())
            return experiment / "profiles" / name / "report/index.html"
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "collect_profile", side_effect=check_profile) as profiler:
            experiment = bench.benchmark(self.root, self.workloads, profile=True)
        profiler.assert_called_once()
        info = json.loads((experiment / "experiment.json").read_text())
        self.assertEqual(info["profiling"], {"runs": 40, "seconds": 10, "status": "passed"})

    def test_profile_failure_keeps_valid_benchmark_results(self):
        self.measured = []
        with redirect_stdout(io.StringIO()), patch.object(bench, "run_logged", side_effect=self.fake_run), \
                patch.object(bench, "collect_profile", side_effect=OSError("aperf missing")):
            with self.assertRaisesRegex(OSError, "aperf missing"):
                bench.benchmark(self.root, self.workloads, profile=True)
        experiment = self.root / "experiments/001"
        info = json.loads((experiment / "experiment.json").read_text())
        self.assertEqual(info["status"], "passed")
        self.assertEqual(info["profiling"]["status"], "failed")
        self.assertTrue(info["timings"])
        self.assertEqual(bench.saved_versions(self.root / "experiments"), [experiment])

    def test_profile_fixed_count_and_report_generation(self):
        experiment = self.root / "experiment"
        experiment.mkdir()
        def fake_logged(command, log, **kwargs):
            if str(command[0]) == "aperf":
                report = Path(command[-1])
                report.mkdir()
                (report / "index.html").write_text("report")
            log.write_text("ok")
            return "ok"
        with patch.object(bench.subprocess, "Popen") as popen, \
                patch.object(bench, "run_logged", side_effect=fake_logged) as logged:
            popen.return_value.wait.return_value = 0
            report = bench.collect_profile(experiment, "texture")
        self.assertTrue(report.is_file())
        self.assertIn("10", popen.call_args.args[0])
        self.assertEqual(logged.call_args_list[0].args[0][2], 40)
        self.assertEqual(str(logged.call_args_list[1].args[0][1]), "report")

    def test_timing_units_single_run_and_zero(self):
        self.assertAlmostEqual(bench.parse_timings(TABLE)["total"]["mean"], .002)
        self.assertEqual(bench.parse_timings("Stage Time (us)\ntotal 0\n")["total"]["stdev"], 0)
        with self.assertRaises(ValueError):
            bench.parse_timings("Stage Time (ms)\ntotal nan\n")
        with self.assertRaises(ValueError):
            bench.parse_timings("no timing table")


if __name__ == "__main__":
    unittest.main()
