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
