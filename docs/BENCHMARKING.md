# Reproducible FSR / FG performance comparisons

Use an independent frame presentation capture tool and keep a copy of each raw capture. The bridge's FSR/FG GPU time and VRAM counters do **not** include all D3D11↔D3D12 copies, fences, waits and frame presentation, so FPS must be measured separately. The `Runtime diagnostics` UI panel (PR #34) can be used for bridge-side statistics once integrated.

Test the same game build, live scene, camera, GPU driver, display, temperature/power profile and sampling duration. Run separate captures for:

| Render mode | 1920×1080 | 3840×2160 |
| --- | --- | --- |
| Game's built-in renderer | FG off | FG off |
| FSR NativeAA | FG off/on | FG off/on |
| FSR Quality | FG off/on | FG off/on |
| FSR Balanced | FG off/on | FG off/on |

Record refresh rate and VSync/VRR state with each capture. Where FG is used, distinguish *generated display frames* from *real game frames*. Exclude initial loading/camera transitions and warm up a scene before measuring. Repeat runs at least three times to reveal variability.

## Summarizing presentation intervals

Export capture data to CSV with a column of frame intervals measured in **milliseconds**. PresentMon versions differ in available columns, so pass the correct column name explicitly if it is not `MsBetweenPresents`:

```powershell
python scripts\summarize_frametimes.py --column MsBetweenPresents --warmup 120 baseline.csv quality.csv balanced.csv
python -m unittest discover -s tests
```

The script emits comparative CSV: average FPS (1000 / mean frame interval), 1% low FPS (1000 / mean of the slowest 1% of intervals), 95th/99th-percentile interval and longest interval. These are frame-presentation metrics, not a direct measure of input latency. Use the same definition of 1% low for every comparison.

Also record average/peak VRAM use, CPU load, `cpuMs`, `gpuMs`, `fgGpuMs`, `flushMs` where available and screenshots of moving detail, transparency, HUD, Bloom/DOF and camera pans. If the bridge reports zero for a counter, mark it **unavailable** rather than assuming the cost was zero.

No benchmark scores are included here: measurements must be collected on the target Windows/Radeon system.
