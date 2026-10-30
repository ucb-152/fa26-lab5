"""Complete-result comparisons; run by make benchmark on the saved build."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from compare_results import compare, fields

BUILD = Path(sys.argv.pop(1)).resolve()


class ResultTests(unittest.TestCase):
    def test_complete_result_round_trip_and_field_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, modified = root / "reference", root / "modified"
            subprocess.run([str(BUILD / "dump_result"), str(ROOT / "examples/valve.pgm"),
                            str(reference)], check=True)
            original = reference.read_bytes()
            modified.write_bytes(original)
            self.assertIsNone(compare(reference, modified))
            entries = list(fields(reference))
            # Exercise every kind of logical field, including data absent from SVGs.
            targets = {name for name, _ in entries if "[" not in name or "[0]" in name}
            for index, (name, _) in enumerate(entries):
                if name not in targets:
                    continue
                with self.subTest(field=name):
                    changed = bytearray(original)
                    changed[8 + 8 * index] ^= 1
                    modified.write_bytes(changed)
                    self.assertIn(name, compare(reference, modified))

    def test_parameters_and_invalid_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outputs = []
            for label, parameters in (("default", []), ("explicit", ["22", "45", "8", "0", "1"]),
                                      ("filtered", ["22", "45", "1000000", "0", "1"])):
                output = root / label
                subprocess.run([str(BUILD / "dump_result"), str(ROOT / "examples/valve.pgm"),
                                str(output), *parameters], check=True)
                outputs.append(output)
            self.assertIsNone(compare(outputs[0], outputs[1]))
            self.assertIsNotNone(compare(outputs[0], outputs[2]))
            bad = subprocess.run([str(BUILD / "dump_result"), str(ROOT / "examples/valve.pgm"),
                                  str(root / "bad"), "22", "45", "8", "0", "nan"], capture_output=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertFalse((root / "bad").exists())

    def test_truncated_result_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "truncated"
            path.write_bytes(b"L5OUT001")
            with self.assertRaisesRegex(ValueError, "Truncated"):
                compare(path, path)



if __name__ == "__main__":
    unittest.main()
