import csv
import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

from scripts.analyze_presentmon import analyze, main


COLUMNS = ["ProcessID", "SwapChainAddress", "FrameType", "MsBetweenDisplayChange",
           "MsBetweenPresents", "MsAllInputToPhotonLatency", "MsClickToPhotonLatency",
           "MsPCLatency", "MsUntilDisplayed"]


class PresentMonAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "capture.csv"

    def write(self, rows, columns=COLUMNS):
        with self.path.open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)

    @staticmethod
    def row(interval, typ="Application", input_ms="NA", click="NA", pid="11", chain="0xAA", pc="NA"):
        return dict(ProcessID=pid, SwapChainAddress=chain, FrameType=typ,
                    MsBetweenDisplayChange=interval, MsBetweenPresents="16.7",
                    MsAllInputToPhotonLatency=input_ms, MsClickToPhotonLatency=click,
                    MsPCLatency=pc, MsUntilDisplayed="12.5")

    def test_mixed_fsr_fg_frames_keep_input_latency_on_application_frames(self):
        self.write([
            self.row("10", input_ms="100"),  # warmup
            self.row("8", input_ms="40"),
            self.row("8", "Unknown", input_ms="5", click="5"),  # AMD FG frame cannot bias latency
            self.row("8", input_ms="60", click="50", pc="17"),
            self.row("32", click="70", pc="20"),
            self.row("NA", input_ms="200"),  # not displayed
            self.row("8", pid="22", chain="0xBB"),
            self.row("8", pid="22", chain="0xBB"),
        ])
        summary, entries = analyze(self.path, warmup=1)
        self.assertEqual(len(entries), 4)
        self.assertEqual(summary["process_id"], "11")
        self.assertEqual(summary["swapchain"], "0xAA")
        self.assertEqual(summary["application_frames"], 3)
        self.assertEqual(summary["unknown_frames"], 1)
        self.assertAlmostEqual(summary["avg_display_fps"], 1000/14)
        self.assertAlmostEqual(summary["slow_gap_pct"], 25)
        self.assertEqual(summary["latency_scope"], "application")
        self.assertEqual(summary["input_samples"], 2)
        self.assertAlmostEqual(summary["input_p50_ms"], 50)
        self.assertAlmostEqual(summary["input_p95_ms"], 59)
        self.assertEqual(summary["click_samples"], 2)
        self.assertAlmostEqual(summary["click_p50_ms"], 60)
        self.assertEqual(summary["pc_samples"], 2)
        self.assertEqual(summary["present_to_display_samples"], 4)
        all_frames, _ = analyze(self.path, warmup=1, latency_frames="all")
        self.assertEqual(all_frames["input_samples"], 3)
        self.assertEqual(all_frames["input_p50_ms"], 40)

    def test_missing_frame_type_still_reports_observable_input(self):
        columns = [name for name in COLUMNS if name != "FrameType"]
        row = self.row("16.7", input_ms="18.3")
        row.pop("FrameType")
        self.write([row], columns=columns)
        summary, _ = analyze(self.path)
        self.assertEqual(summary["latency_scope"], "all (FrameType unavailable)")
        self.assertEqual(summary["input_samples"], 1)

    def test_unknown_only_frame_type_requires_explicit_override(self):
        self.write([self.row("8", typ="Unknown", input_ms="21")])
        filtered, _ = analyze(self.path)
        self.assertEqual(filtered["input_samples"], 0)
        self.assertIn("unavailable", filtered["latency_scope"])
        unfiltered, _ = analyze(self.path, latency_frames="all")
        self.assertEqual(unfiltered["input_samples"], 1)

    def test_missing_input_columns_are_unavailable_not_zero_latency(self):
        columns = ["ProcessID", "SwapChainAddress", "MsBetweenDisplayChange"]
        self.write([self.row("16.7")], columns=columns)
        report, _ = analyze(self.path)
        self.assertEqual(report["input_samples"], 0)
        self.assertIsNone(report["input_p50_ms"])
        self.assertEqual(report["present_to_display_samples"], 0)

    def test_reject_invalid_intervals_and_excess_warmup(self):
        self.write([self.row("not-a-number")])
        with self.assertRaisesRegex(ValueError, "invalid MsBetweenDisplayChange"):
            analyze(self.path)
        self.write([self.row("NA")])
        with self.assertRaisesRegex(ValueError, "no positive"):
            analyze(self.path)
        self.write([self.row("16.7")])
        with self.assertRaisesRegex(ValueError, "leaves no displayed frames"):
            analyze(self.path, warmup=1)

    def test_trace_output_cannot_overwrite_capture(self):
        self.write([self.row("8")])
        original = self.path.read_bytes()
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                main([str(self.path), "--warmup", "0", "--trace-out", str(self.path)])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.path.read_bytes(), original)

    def test_cli_emits_summary_and_per_frame_trace(self):
        self.write([self.row("10", input_ms="20", click="25"),
                    self.row("30", input_ms="NA")])
        trace = Path(self.temp.name) / "trace.csv"
        output = io.StringIO()
        with redirect_stdout(output):
            main([str(self.path), "--warmup", "0", "--require-input", "--trace-out", str(trace)])
        results = list(csv.DictReader(io.StringIO(output.getvalue())))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["display_frames"], "2")
        self.assertEqual(results[0]["input_samples"], "1")
        with trace.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        self.assertEqual(rows[0]["MsAllInputToPhotonLatency"], "20")
        self.assertEqual(rows[1]["MsBetweenDisplayChange"], "30")


if __name__ == "__main__":
    unittest.main()
