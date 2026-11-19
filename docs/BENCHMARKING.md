# Reproducible FSR / FG performance comparisons

## Live readout in GakuLoader (F1)

Open F1 → `GakuFSR4` → **Live frame pacing / input timing**. The panel updates about every two seconds and reports samples from the most recent ten seconds:

| UI field | What is actually measured |
| --- | --- |
| `Game Present gap` median/p95/p99 | The time between **successful real-game Present call entries** (manual and native FG paths). It excludes generated-frame display events. |
| `Pacing jitter (stddev)` | Standard deviation of those real-game Present intervals. This is CPU submission-side jitter, not displayed-frame jitter. |
| `Input event -> Present start` median/p95 | Elapsed CPU time from the **latest keyboard/mouse message removed from the Unity window's message queue** to the **next successful real-game Present call entry**, matched at most once per input event. This is only a *partial* input latency. |
| `n` | Number of observations in the corresponding rolling window. No observations are shown as `--`, never as zero latency. |

Input monitoring uses a `WH_GETMESSAGE` hook restricted to the Unity window's thread and process. It reads message types and timestamps only, not key codes, mouse coordinates, or user-entered text. Games that bypass this message queue can show `--` even when input works. Continuous mouse movement can shorten the input-to-Present value because the newest message is used. A hitch over one second is omitted from the rolling gap samples, and samples expire after ten seconds. The CPU-side timing is not measured display cadence, GPU/display queue latency, or physical click-to-photon latency; the CSV/PresentMon workflow below remains necessary for those comparisons.

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

For a PresentMon 2.x capture filtered to the game process, use the display interval and select the swap chain with the most valid intervals:

```powershell
python scripts\summarize_frametimes.py --column MsBetweenDisplayChange --primary-swapchain --warmup 120 live.csv
```

Check the selected swap chain against the raw CSV if the capture has multiple active windows or spans a resize. Compare `MsBetweenDisplayChange` across runs to measure displayed FPS.

The script emits comparative CSV: average FPS (1000 / mean frame interval), 1% low FPS (1000 / mean of the slowest 1% of intervals), 95th/99th-percentile interval and longest interval. These are frame-presentation metrics, not a direct measure of input latency. Use the same definition of 1% low for every comparison.

Also record average/peak VRAM use, CPU load, `cpuMs`, `gpuMs`, `fgGpuMs`, `flushMs` where available and screenshots of moving detail, transparency, HUD, Bloom/DOF and camera pans. If the bridge reports zero for a counter, mark it **unavailable** rather than assuming the cost was zero.

An initial live-scene capture on the Radeon RX 9070 XT measured 119.19 displayed FPS, 66.04 FPS for the 1% low, and 12.58 ms p95 after discarding the first 120 display intervals. The paired runs in the table are still needed to measure the effect of each render mode and frame generation.

## Measure frame pacing **and** input latency on the same PresentMon capture

Use the official [PresentMon 2.x console application](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md). It can capture displayed-frame timing and the software-visible interval from keyboard/mouse input to display. The plugin's own `Present FPS` counter measures calls/submissions, **not** the actual display cadence, so use the raw PresentMon CSV for this comparison.

Find the game's executable name in Task Manager → Details, then record a stable live sequence, for example in PowerShell (replace the executable name and PresentMon executable path with the real ones):

```powershell
.\PresentMon-2.x-x64.exe --process_name GAME.exe --track_frame_type --timed 45 --terminate_after_timed --output_file native-fg.csv
```

Do **not** pass `--no_track_input` or `--no_track_display`. Interact with the game using keyboard or mouse during the capture; clicking repeatedly in a controlled sequence allows `MsClickToPhotonLatency` to have useful samples. For a fair test, use identical input actions and live scenes in baseline (FG OFF), manual FG (`NativeSwapChain=0`) and AMD SwapChain FG (`NativeSwapChain=1`), repeat each three times, and keep VSync/VRR, driver, resolution and refresh rate constant.

```powershell
# Combined comparison: frame pacing, application-frame input latency and sample counts
python scripts\analyze_presentmon.py --warmup 120 --require-input baseline.csv manual-fg.csv native-fg.csv

# Single-capture per-frame trace for plotting in Excel or another CSV viewer
python scripts\analyze_presentmon.py --warmup 120 --trace-out native-trace.csv native-fg.csv
```

The analyzer selects the process/SwapChain with the most valid displayed intervals in each file and excludes the first 120 *displayed* frames. It reports:

| CSV output | Meaning |
| --- | --- |
| `display_p95_ms`, `display_p99_ms`, `display_cv_pct` | Long-tail displayed-frame intervals and frame-pacing variability. Smaller is generally smoother. |
| `slow_gap_pct` | Display intervals longer than **1.5× that run's median interval**. A relative long-gap indicator, not a formal stutter count; compare only matched refresh/FPS scenarios. |
| `present_p99_ms` | 99th-percentile CPU Present call gap, **not** the actual display interval. |
| `input_p50_ms`, `input_p95_ms`, `input_samples` | PresentMon's software-visible `MsAllInputToPhotonLatency` estimate from keyboard/mouse input. |
| `click_p50_ms`, `click_p95_ms`, `click_samples` | `MsClickToPhotonLatency` estimate for mouse-button input. |
| `pc_p50_ms`, `pc_p95_ms`, `pc_samples` | Instrumentation-dependent `MsPCLatency`; typically unavailable without a supported PC latency signal. |
| `present_to_display_p50_ms`, `present_to_display_p95_ms` | Time from Present call to display (`MsUntilDisplayed`); this alone is **not** input latency. |
| `application_frames`, `unknown_frames`, `latency_scope` | Number of real application frames, unclassified frames, and which frames contributed to input samples. |

By default, input latency uses rows marked `FrameType=Application` when available, to avoid treating generated/unknown FG frames as new game input responses. If `FrameType` is absent, the analyzer labels the latency scope as all/unclassified; if the capture contains only unknown frame types, input samples remain unavailable unless `--latency-frames all` is explicitly selected. `Unknown` is **not proof** a frame was generated. The displayed-frame pacing statistics always include every displayed frame on the selected SwapChain.

Missing/`NA` latency fields stay blank and have a sample count of zero; they must never be reported as zero-millisecond latency. `--require-input` makes the command fail if no keyboard/mouse latency samples were observed. For comparing latency percentiles, collect enough actual input samples per run and inspect `*_samples` first.

**Limitation:** PresentMon input-to-photon metrics are software-derived estimates. Without optical hardware they do not account for all input-device electronics, driver delay, display scanning or pixel response. They are useful for an A/B comparison under the same conditions, but cannot establish the full physical click-to-visible-pixel latency. See the [PresentMon metric definitions](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md#csv-columns) and [metric notes](https://github.com/GameTechDev/PresentMon/blob/main/Tools/generate/readme/metrics.awk).
