"""The findings reader and the visual report (UC-4.3, R10): a bad file is refused by name, never shown as clean, and the
HTML report carries every finding of every file it was given."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "unreal" / "MemorySentinel" / "Content" / "Python"))

from memsentinel import findings as F  # noqa: E402
from memsentinel.report import Report  # noqa: E402
from memsentinel.scanner import scan_text  # noqa: E402


class Reader(unittest.TestCase):
    def test_a_finding_without_how_known_is_refused(self):
        with self.assertRaises(ValueError):
            F.Finding(rule="heap-overrun", severity="error", title="t", where={}, why="w", fix="f", how_known="")

    def test_unknown_rule_in_a_file_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.json"
            p.write_text(json.dumps({"tool": "memsentinel", "schema": 1, "kind": "scan", "checked": {}, "blind_spots": [],
                                     "findings": [{"rule": "made-up", "severity": "error", "how_known": "READ"}]}))
            with self.assertRaisesRegex(ValueError, "made-up"):
                F.load(p)


class VisualReport(unittest.TestCase):
    def test_every_finding_reaches_the_page(self):
        with tempfile.TemporaryDirectory() as d:
            good = scan_text("UCLASS()\nclass AB : public AActor\n{\n GENERATED_BODY()\n UObject* P;\n};\n").write(Path(d) / "scan.json")
            bad = Path(d) / "broken.json"
            bad.write_text("{not json")
            rep = Report.from_files([good, bad])
            self.assertEqual(len(rep.files), 1)
            self.assertEqual(len(rep.findings), 1)
            self.assertEqual(len(rep.problems), 1, "the unreadable file must be reported, not dropped")
            out = rep.write_html(Path(d) / "report.html")
            html = out.read_text(encoding="utf-8")
            self.assertIn("window.MEMSENTINEL = ", html)
            self.assertIn("MemorySentinelViewer.mount", html)
            self.assertIn(rep.findings[0]["title"], html)
            self.assertIn("broken.json", html)


if __name__ == "__main__":
    unittest.main()
