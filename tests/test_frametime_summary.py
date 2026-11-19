import math
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from scripts.summarize_frametimes import percentile, read_intervals, summarize


class FrametimeTests(unittest.TestCase):
    def test_constant_60_hz(self):
        result = summarize([1000 / 60] * 100)
        self.assertEqual(result["frames"], 100)
        self.assertAlmostEqual(result["avg_fps"], 60)
        self.assertAlmostEqual(result["one_percent_low_fps"], 60)

    def test_stutter_reduces_one_percent_low(self):
        result = summarize([10] * 99 + [100])
        self.assertGreater(result["avg_fps"], result["one_percent_low_fps"])
        self.assertEqual(result["max_ms"], 100)
        self.assertAlmostEqual(result["one_percent_low_fps"], 10)
        self.assertEqual(percentile([10, 20, 30], 0.5), 20)

    def test_bad_input(self):
        with self.assertRaises(ValueError):
            summarize([])
        with self.assertRaises(ValueError):
            summarize([10, math.nan])

    def test_primary_swapchain_uses_displayed_intervals_from_one_process(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "capture.csv"
            path.write_text(
                "ProcessID,SwapChainAddress,MsBetweenDisplayChange\n"
                "1,0xa,NA\n2,0xb,NA\n2,0xb,NA\n2,0xb,NA\n2,0xb,NA\n"
                "2,0xa,20\n2,0xa,20\n1,0xa,8\n1,0xa,9\n1,0xa,10\n",
                encoding="utf-8",
            )
            self.assertEqual(read_intervals(path, "MsBetweenDisplayChange", 1, primary_swapchain=True), [9, 10])


if __name__ == "__main__":
    unittest.main()
