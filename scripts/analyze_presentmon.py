#!/usr/bin/env python3
"""Compare displayed frame pacing and software-visible input latency in PresentMon CSVs.

Metrics are derived from recorded display events. Input-to-photon fields are
PresentMon estimates, not optical measurements of the monitor's pixels.
"""

import argparse
import csv
import math
import statistics
import sys
from pathlib import Path

if __package__:
    from .frame_metrics import one_percent_low_fps, percentile
else:
    from frame_metrics import one_percent_low_fps, percentile


LATENCIES = {
    "input": "MsAllInputToPhotonLatency",
    "click": "MsClickToPhotonLatency",
    "pc": "MsPCLatency",
    "present_to_display": "MsUntilDisplayed",
}
DISPLAY_COLUMN = "MsBetweenDisplayChange"


def positive(row, name):
    value = (row.get(name) or "").strip()
    if not value or value.upper() == "NA":
        return None
    try:
        result = float(value)
    except ValueError as exc:
        raise ValueError(f"invalid {name} value {value!r}") from exc
    return result if math.isfinite(result) and result > 0 else None


def latency_stats(rows, name):
    values = [value for row in rows if (value := positive(row, name)) is not None]
    return (len(values), percentile(values, .5), percentile(values, .95)) if values else (0, None, None)


def select_display_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        columns = set(reader.fieldnames or ())
        required = {"ProcessID", "SwapChainAddress", DISPLAY_COLUMN}
        missing = required - columns
        if missing:
            raise ValueError(f"{path}: missing {', '.join(sorted(missing))}; export a PresentMon raw-frame CSV")
        groups = {}
        for index, row in enumerate(reader):
            if positive(row, DISPLAY_COLUMN) is None:
                continue  # not displayed: cannot contribute to input-to-display latency
            key = (row["ProcessID"], row["SwapChainAddress"])
            groups.setdefault(key, []).append((index, row))
    if not groups:
        raise ValueError(f"{path}: no positive displayed-frame intervals")
    chain, entries = max(groups.items(), key=lambda item: len(item[1]))
    return columns, chain, entries


def analyze(path, warmup=0, latency_frames="application", slow_factor=1.5):
    columns, (pid, chain), entries = select_display_rows(path)
    if warmup < 0 or warmup >= len(entries):
        raise ValueError(f"{path}: warmup {warmup} leaves no displayed frames (found {len(entries)})")
    entries = entries[warmup:]
    rows = [row for _, row in entries]
    intervals = [positive(row, DISPLAY_COLUMN) for row in rows]
    mean = statistics.fmean(intervals)
    median = percentile(intervals, .5)
    slow = sum(value > median * slow_factor for value in intervals)
    stddev = statistics.pstdev(intervals)
    presents = [v for row in rows if (v := positive(row, "MsBetweenPresents")) is not None]
    frame_types = [(row.get("FrameType") or "").strip().lower() for row in rows]
    application = [row for row, typ in zip(rows, frame_types) if typ in ("application", "app")]
    unknown = sum(typ == "unknown" for typ in frame_types)

    if latency_frames == "all":
        latency_rows, scope = rows, "all"
    elif "FrameType" not in columns:
        latency_rows, scope = rows, "all (FrameType unavailable)"
    elif application:
        latency_rows, scope = application, "application"
    else:
        latency_rows, scope = [], "unavailable (no Application frames)"

    result = {
        "capture": path.name,
        "process_id": pid,
        "swapchain": chain,
        "display_frames": len(intervals),
        "application_frames": len(application),
        "unknown_frames": unknown,
        "avg_display_fps": 1000 / mean,
        "one_percent_low_fps": one_percent_low_fps(intervals),
        "display_p50_ms": median,
        "display_p95_ms": percentile(intervals, .95),
        "display_p99_ms": percentile(intervals, .99),
        "display_max_ms": max(intervals),
        "display_stddev_ms": stddev,
        "display_cv_pct": stddev * 100 / mean,
        "slow_gap_pct": 100 * slow / len(intervals),
        "present_p99_ms": percentile(presents, .99) if presents else None,
        "latency_scope": scope,
    }
    for key, column in LATENCIES.items():
        # Present-to-display applies to every displayed frame, unlike the
        # keyboard/mouse metrics, which must not be polluted by FG-only rows.
        samples, p50, p95 = latency_stats(rows if key == "present_to_display" else latency_rows, column)
        result[f"{key}_samples"] = samples
        result[f"{key}_p50_ms"] = p50
        result[f"{key}_p95_ms"] = p95
    return result, entries


def write_trace(path, entries):
    columns = ["display_index", "source_row", "frame_type", DISPLAY_COLUMN,
               "MsBetweenPresents", *LATENCIES.values()]
    with path.open("x", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for display_index, (source_row, row) in enumerate(entries):
            data = {name: row.get(name, "") for name in columns[3:]}
            data.update(display_index=display_index, source_row=source_row, frame_type=row.get("FrameType", ""))
            writer.writerow(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+", help="PresentMon raw-frame CSV captures")
    parser.add_argument("--warmup", type=int, default=120, help="displayed frames to discard (default: 120)")
    parser.add_argument("--latency-frames", choices=("application", "all"), default="application",
                        help="sample input metrics on real Application frames, or all displayed frames")
    parser.add_argument("--slow-factor", type=float, default=1.5,
                        help="count display intervals longer than this times the median (default: 1.5)")
    parser.add_argument("--require-input", action="store_true", help="error if no recorded keyboard/mouse input samples")
    parser.add_argument("--trace-out", type=Path,
                        help="export per-display-frame intervals and input metrics to a new file (one capture only)")
    args = parser.parse_args(argv)
    if args.warmup < 0 or not math.isfinite(args.slow_factor) or args.slow_factor <= 1:
        parser.error("--warmup must be nonnegative and --slow-factor must be finite and above 1")
    if args.trace_out and len(args.files) != 1:
        parser.error("--trace-out requires exactly one input capture")
    try:
        reports = []
        for path in args.files:
            report, entries = analyze(path, args.warmup, args.latency_frames, args.slow_factor)
            if args.require_input and not (report["input_samples"] or report["click_samples"]):
                raise ValueError(f"{path}: no input latency samples; check PresentMon input tracking and real-frame selection")
            reports.append(report)
            if args.trace_out:
                write_trace(args.trace_out, entries)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    writer = csv.DictWriter(sys.stdout, fieldnames=reports[0].keys())
    writer.writeheader()
    for report in reports:
        writer.writerow({k: f"{v:.3f}" if isinstance(v, float) else ("" if v is None else v)
                         for k, v in report.items()})


if __name__ == "__main__":
    main()
