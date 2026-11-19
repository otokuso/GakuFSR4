# Historical manual FG pacing measurement

GakuFSR4 now uses the AMD Native SwapChain for FG pacing. These measurements record the retired manual Present path. See the [current benchmarking guide](BENCHMARKING.md) for Native SwapChain measurements.

## RX 9070 XT result, 2026-11-19

The high-resolution timer prototype improved the CPU-side deadline but increased the tail of the actual display intervals in both runs.

| Run | Display FPS | p95 / p99 display interval (ms) | Longest display interval (ms) | Process CPU over ~25 s (ms) |
| --- | ---: | ---: | ---: | ---: |
| Current 1 ms sleep with spin tail, run 1 | 119.90 | 12.540 / 18.754 | 18.85 | 1765.6 |
| Waitable timer with 0.25 ms spin target, run 1 | 119.98 | 12.586 / 25.000 | 25.08 | 1046.9 |
| Waitable timer, run 2 | 119.94 | 12.587 / 24.995 | 25.03 | 1656.2 |
| Current wait, run 2 | 119.90 | 12.524 / 18.753 | 18.84 | 953.1 |

The two 600-wait samples per run reported mean deadline overshoots of 5.327–6.869 ms for the current wait and 0.068–0.082 ms for the timer. CPU totals overlapped across the two strategies.

Setup: Windows 11 build 26300, RX 9070 XT (32.0.32015.16001), 160 Hz desktop, RGBA8 960×540 composed DXGI test window, FG 4.0.2 and 1500 source frames target-paced at 60 Hz. The harness requests tearing and the bridge synchronizes the generated/real pair to vblank. PresentMon 2.6.0 display timestamps use TimeInMs plus MsUntilDisplayed for the dominant chain after a two-second warmup. This result applies to the stand-alone workload on this display. The in-game scene, VRR and native AMD SwapChain experiments have separate coverage.

To reproduce, build [the comparison commit](https://github.com/otokuso/GakuFSR4/commit/5bbf83f72a03836b15dfdabad2f8a8e6c69aafa4) twice. In the timer build, append /DGAKU_FG_TIMER_TEST to the baseline CMAKE_CXX_FLAGS so the Windows SDK platform definitions remain present. Run test_gpu.exe against each DLL with the SDK v2.4.0 runtime path and --fg-pacing. Capture test_gpu.exe in PresentMon and use the process CPU and Pace summaries printed by the harness.