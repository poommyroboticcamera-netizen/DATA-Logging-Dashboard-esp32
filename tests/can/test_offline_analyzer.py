import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ANALYZER = ROOT / "scripts" / "analyze_can_csv.py"
HEADER = ["timestamp_us", "id", "extended", "dlc", *(f"d{i}" for i in range(8))]


class OfflineAnalyzerTest(unittest.TestCase):
    def test_sum_candidate_survives_validation_half(self):
        with tempfile.TemporaryDirectory(prefix="can-offline-") as directory:
            capture = Path(directory) / "capture.csv"
            with capture.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream)
                writer.writerow(HEADER)
                for sequence in range(64):
                    payload = [sequence, sequence * 3 & 0xFF, sequence * 7 & 0xFF]
                    payload.append(sum(payload) & 0xFF)
                    writer.writerow([sequence * 1000, "321", 0, 4, *payload, "", "", "", ""])

            result = subprocess.run(
                [sys.executable, str(ANALYZER), str(capture), "--id", "321", "--dlc", "4"],
                check=True,
                capture_output=True,
                text=True,
                encoding="utf-8",
            )

        self.assertIn("candidate byte=3 hypothesis=sum8 discovery=1.0000 validation=1.0000", result.stdout)
        self.assertIn("Candidates only; no physical meanings inferred.", result.stdout)


if __name__ == "__main__":
    unittest.main()
