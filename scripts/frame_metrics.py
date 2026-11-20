"""Frame-interval calculations shared by the capture reports."""

import math
import statistics


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def one_percent_low_fps(intervals):
    worst_count = max(1, math.ceil(len(intervals) * 0.01))
    worst_mean = statistics.fmean(sorted(intervals, reverse=True)[:worst_count])
    return 1000.0 / worst_mean
