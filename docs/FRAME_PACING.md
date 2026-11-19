# Manual FG Present pacing measurement

The AMD-native FG swap-chain path is tracked in #23. Until that implementation has been verified, the manual Present path remains available. Its `Pace` helper sleeps while more than 1.5 ms remains, then busy-spins until the deadline.

This instrumentation logs aggregates after every **600 timed waits**:

- Average and maximum duration of the busy-spin tail, in milliseconds.
- Average and maximum deadline overshoot, in milliseconds. These values measure the current thread's timing, not the actual display scanout.

The instrumentation does **not** change the waiting algorithm. Collect the log under the same game scene, resolution, refresh rate and sync mode before introducing a different wait strategy.

## Repeatable on-device comparison

1. On a supported Radeon/Windows installation, enable FG and leave the game in a repeatable scene for at least 600 generated-frame waits.
2. Keep GPU driver, display refresh/VRR, Windows power profile, scene and graphics settings constant.
3. Record the bridge `FG manual Pace over 600 waits` lines, PresentMon frame intervals/1% lows, CPU usage for the game process and total package power if available.
4. Repeat for VSync ON/OFF and VRR ON/OFF; also capture input latency if equipment permits.
5. Compare with a waitable timer or `SwitchToThread` variant **only after** baseline results are available. Preserve the original path if reduced CPU usage causes worse present jitter or latency.

`Pace` is not invoked for all FG frames, so these numbers represent the timed-wait subset. They cannot substitute for observed display frame pacing.

## RX 9070 XT result, 2026-11-19

Keep the production wait. The high-resolution timer prototype improved the CPU-side deadline but increased the tail of the actual display intervals in both runs.

| Run | Display FPS | p95 / p99 display interval (ms) | Longest display interval (ms) | Process CPU over ~25 s (ms) |
| --- | ---: | ---: | ---: | ---: |
| Current 1 ms sleep with spin tail, run 1 | 119.90 | 12.540 / 18.754 | 18.85 | 1765.6 |
| Waitable timer with 0.25 ms spin target, run 1 | 119.98 | 12.586 / 25.000 | 25.08 | 1046.9 |
| Waitable timer, run 2 | 119.94 | 12.587 / 24.995 | 25.03 | 1656.2 |
| Current wait, run 2 | 119.90 | 12.524 / 18.753 | 18.84 | 953.1 |

The two 600-wait samples per run reported mean deadline overshoots of 5.327–6.869 ms for the current wait and 0.068–0.082 ms for the timer. CPU totals overlapped across the two strategies.

Setup: Windows 11 build 26300, RX 9070 XT (32.0.32015.16001), 160 Hz desktop, RGBA8 960×540 composed DXGI test window, FG 4.0.2 and 1500 source frames target-paced at 60 Hz. The harness requests tearing and the bridge synchronizes the generated/real pair to vblank. PresentMon 2.6.0 display timestamps use TimeInMs plus MsUntilDisplayed for the dominant chain after a two-second warmup. This result applies to the stand-alone workload on this display. The in-game scene, VRR and native AMD SwapChain experiments have separate coverage.

To reproduce, build [the comparison commit](https://github.com/otokuso/GakuFSR4/commit/5bbf83f72a03836b15dfdabad2f8a8e6c69aafa4) twice. In the timer build, append /DGAKU_FG_TIMER_TEST to the baseline CMAKE_CXX_FLAGS so the Windows SDK platform definitions remain present. Run test_gpu.exe against each DLL with the SDK v2.4.0 runtime path and --fg-pacing. Capture test_gpu.exe in PresentMon and use the process CPU and Pace summaries printed by the harness.