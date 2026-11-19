#!/usr/bin/env python3
"""Summarize frame-interval CSVs without assuming a particular capture-tool version."""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def summarize(times_ms):
    if not times_ms or any(not math.isfinite(t) or t <= 0 for t in times_ms):
        raise ValueError("At least one strictly positive, finite frame interval is required")
    mean_ms = statistics.fmean(times_ms)
    worst_count = max(1, math.ceil(len(times_ms) * 0.01))
    worst_mean_ms = statistics.fmean(sorted(times_ms, reverse=True)[:worst_count])
    return {
        "frames": len(times_ms),
        "avg_fps": 1000.0 / mean_ms,
        "one_percent_low_fps": 1000.0 / worst_mean_ms,
        "p95_ms": percentile(times_ms, 0.95),
        "p99_ms": percentile(times_ms, 0.99),
        "max_ms": max(times_ms),
    }


def read_intervals(path, column, warmup, primary_swapchain=False):
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        required = {column, "ProcessID", "SwapChainAddress"} if primary_swapchain else {column}
        missing = required.difference(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: missing columns {', '.join(sorted(missing))}; available: {reader.fieldnames}")
        groups = {}
        for row in reader:
            raw = (row.get(column) or "").strip()
            if not raw or raw.upper() == "NA":
                continue
            try:
                value = float(raw)
            except ValueError as exc:
                raise ValueError(f"{path}: invalid frame interval '{raw}'") from exc
            if math.isfinite(value) and value > 0:
                chain = (row["ProcessID"], row["SwapChainAddress"]) if primary_swapchain else None
                groups.setdefault(chain, []).append(value)
    intervals = max(groups.values(), key=len, default=[])
    if warmup >= len(intervals):
        raise ValueError(f"{path}: warmup {warmup} leaves no frames (read {len(intervals)})")
    return intervals[warmup:]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+", help="CSV captures of frame intervals in milliseconds")
    parser.add_argument("--column", default="MsBetweenPresents", help="CSV column with frame intervals in ms")
    parser.add_argument("--primary-swapchain", action="store_true",
                        help="use the process and swap chain with the most valid intervals per capture")
    parser.add_argument("--warmup", type=int, default=0, help="valid frames to discard at capture start")
    args = parser.parse_args(argv)
    if args.warmup < 0:
        parser.error("--warmup must be nonnegative")
    writer = csv.writer(sys.stdout)
    fields = ["capture", "frames", "avg_fps", "one_percent_low_fps", "p95_ms", "p99_ms", "max_ms"]
    writer.writerow(fields)
    try:
        for path in args.files:
            summary = summarize(read_intervals(path, args.column, args.warmup, args.primary_swapchain))
            writer.writerow([path.name] + [summary[f] if f == "frames" else f"{summary[f]:.3f}" for f in fields[1:]])
    except (OSError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
